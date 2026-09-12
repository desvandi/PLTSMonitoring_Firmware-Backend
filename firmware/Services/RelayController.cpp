// =============================================================================
// Services/RelayController.cpp — 8-channel relay state machine + safety + interlock
// =============================================================================
#include "RelayController.h"
#if PLTS_ENABLE_RELAYS
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Services/LogService.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/HealthSupervisor.h"
#include <Preferences.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <ctime>

namespace Services {

RelayController relaysController;

void RelayController::begin() {
  // Load config from NVS
  _loadConfig();
  _loadLockoutStates();

  // Initialize pulse tracking — [P1-8] 8 slots, one per channel
  for (uint8_t i = 0; i < Core::RELAY_CHANNEL_COUNT; i++) {
    _pulses[i].active = false;
  }

  // [audit p.101-102] Native FreeRTOS queue replaces the custom volatile
  // ring buffer — cross-core producer (networkTask/core1) and consumer
  // (relayTask/core0) synchronization is now the kernel's responsibility.
  if (_commandQueueHandle == nullptr) {
    _commandQueueHandle = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(QueuedRelayCommand));
  }

  // [RG-RELAY-09] Clear the transaction result ring
  for (uint8_t i = 0; i < RESULT_RING_SIZE; i++) _resultRing[i] = RelayTransactionRecord{};
  _resultRingNext = 0;

  // Initialize interlock groups (all inactive by default — operator configures)
  for (uint8_t i = 0; i < 4; i++) {
    _interlockGroups[i].active = false;
    _interlockGroups[i].activeMember = 0xFF;
  }

  // Initialize PCF8574 driver (fail-safe: all OFF)
  _driverAvailable = Drivers::relayExpander.begin(_pcf8574Address);
  if (!_driverAvailable) {
    Services::Log.append(Core::LogType::Custom,
               "RELAY: PCF8574 not available — relay control disabled", 0);
    Services::alarms.raise(Core::AlarmCode::RELAY_FAULT,
                           Core::AlarmSeverity::Critical,
                           "PCF8574 I²C expander not responding");
  }

  // Apply boot policy: ALL OFF (fail-safe)
  // BootOff is the only policy in v1 — no RestoreLast (too dangerous without
  // physical verification). Lockout states are restored from NVS.
  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    _state[ch].desiredState = false;
    _state[ch].reportedState = false;
    _state[ch].physicalState = false;
    _state[ch].confidence = Core::RelayStateConfidence::Unknown;
    _state[ch].source = Core::RelaySource::Off;
    _state[ch].stateSequence = 0;
    _state[ch].lastChangedAtMs = 0;
    _state[ch].onSinceMs = 0;
    _state[ch].fault = false;

    // If channel was in TRIPPED/ACKNOWLEDGED lockout, keep it locked
    if (_state[ch].lockout == Core::RelayLockoutState::Tripped ||
        _state[ch].lockout == Core::RelayLockoutState::Acknowledged) {
      _state[ch].maxOnTimeForced = true;  // stay forced OFF
    }
  }

  Serial.printf("[RELAY] Controller initialized — %d channels, driver %s\n",
                Core::RELAY_CHANNEL_COUNT,
                _driverAvailable ? "OK" : "UNAVAILABLE");
}

void RelayController::tick() {
  // Record heartbeat for health supervisor
  _recordHeartbeat();

  // Drain the queue even when the driver is unavailable so pending commands
  // receive a FINAL result (Failed) instead of hanging without an outcome.
  if (!_driverAvailable) {
    processCommandQueue();
    return;
  }

  // [audit p.92-93] Automatic verified safe-recovery after a failed I²C
  // write: the driver refuses normal mutations while its shadow register is
  // unknown. Recovery drives 0xFF (ALL OFF — safe direction) and verifies by
  // readback, restoring the driver to a known state.
  //
  // Runs FIRST (before safety enforcement and the command queue) so the
  // executor works against a healthy driver in everything that follows
  // (audit p.407: commands dequeued while SHADOW_UNKNOWN must fail closed —
  // with recovery up front, the surviving window is one tick at most).
  if (Drivers::relayExpander.isShadowUnknown()) {
    if (Drivers::relayExpander.recoverWithAllOff()) {
      for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
        if (_state[ch].reportedState) {
          _state[ch].reportedState = false;
          _state[ch].desiredState = false;
          _state[ch].stateSequence++;
          _state[ch].lastChangedAtMs = millis();
          _state[ch].onSinceMs = 0;
          _state[ch].confidence = Core::RelayStateConfidence::SoftwareOnly;
        }
        if (_pulses[ch].active) {
          _annotatePulseOutcome(ch, "cancelled by I²C shadow recovery (ALL OFF)");
        }
        _pulses[ch].active = false;  // any pending pulse is moot — channel is OFF
      }
      Services::Log.append(Core::LogType::Custom,
                 "RELAY: I²C shadow recovered (verified ALL OFF) — normal mutations re-enabled", 0);
    }
    // Recovery failed — remain in refused state; fault alarms already raised.
  }

  // 1. [audit p.401] SAFETY ENFORCEMENT BEFORE THE COMMAND QUEUE —
  //    maxOnTime FORCE OFF is evaluated and applied FIRST, so any queued
  //    ON command is judged against an up-to-date maxOnTimeForced flag.
  //    This closes the previous one-tick (≤200 ms) window in which a normal
  //    ON could execute before the safety supervisor had refreshed the flag.
  _checkMaxOnTime();

  // 2. Process queued commands — single-threaded mutation authority.
  //    Safety + interlock are re-evaluated inside applyCommand immediately
  //    before each write.
  processCommandQueue();

  // 3. Process pending pulses (turn OFF after duration) — after safety, so
  //    a maxOnTime force-off in step 1 has already cancelled any pending
  //    pulse for the tripped channel (audit p.409 precedence).
  _processPulses();

  // 4. Process lockout state transitions (ARMED → NORMAL)
  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (_state[ch].lockout == Core::RelayLockoutState::Armed) {
      _state[ch].lockout = Core::RelayLockoutState::Normal;
      _saveLockoutStates();
    }
  }
}

RelayCommandResult RelayController::applyCommand(
    const String& command,
    uint8_t channel,
    bool desiredState,
    uint32_t pulseDurationMs,
    const String& source,
    String& messageOut,
    const char* transactionId) {

  if (!_driverAvailable) {
    messageOut = "Relay driver unavailable (PCF8574 not responding)";
    return RelayCommandResult::Failed;
  }

  // Handle "all_off" command — [audit p.77] all_off is a normal command and
  // follows the SAME queue/task lifecycle as per-channel commands.
  if (command == "all_off") {
    AllOffResult result = allOffWithResult();
    messageOut = "All channels OFF: " + String(result.success) + " ok, " +
                 String(result.failed) + " failed" +
                 (result.unknown > 0 ? (", " + String(result.unknown) + " unknown") : "");
    Services::Log.append(Core::LogType::Custom,
               "RELAY: all_off command executed (" + messageOut + ")", 0);
    return (result.failed == 0 && result.unknown == 0)
           ? RelayCommandResult::Applied
           : RelayCommandResult::Failed;
  }

  // Handle "config" command
  if (command == "config") {
    if (!_validChannel(channel)) {
      messageOut = "Invalid channel";
      return RelayCommandResult::Rejected;
    }
    // Config is set via setChannelConfig() — just return OK
    messageOut = "Config updated";
    return RelayCommandResult::Applied;
  }

  // Handle "acknowledge" command
  if (command == "acknowledge") {
    if (!_validChannel(channel)) {
      messageOut = "Invalid channel";
      return RelayCommandResult::Rejected;
    }
    if (acknowledgeSafetyAlarm(channel)) {
      messageOut = "Safety alarm acknowledged";
      return RelayCommandResult::Applied;
    }
    messageOut = "Channel not in TRIPPED state";
    return RelayCommandResult::Rejected;
  }

  // Handle "clear" command
  if (command == "clear") {
    if (!_validChannel(channel)) {
      messageOut = "Invalid channel";
      return RelayCommandResult::Rejected;
    }
    if (clearSafetyLockout(channel)) {
      messageOut = "Safety lockout cleared";
      return RelayCommandResult::Applied;
    }
    messageOut = "Channel not in ACKNOWLEDGED state or fault not resolved";
    return RelayCommandResult::Rejected;
  }

  // Validate channel for on/off/pulse commands
  if (!_validChannel(channel)) {
    messageOut = "Invalid channel (must be 0-7)";
    return RelayCommandResult::Rejected;
  }

  // Check if channel is enabled
  if (!_config[channel].enabled) {
    messageOut = "Channel is disabled in config";
    return RelayCommandResult::Rejected;
  }

  // Check lockout state
  if (_state[channel].lockout != Core::RelayLockoutState::Normal &&
      _state[channel].lockout != Core::RelayLockoutState::Armed) {
    messageOut = "Channel in lockout state: ";
    messageOut += Core::relayLockoutStateToStr(_state[channel].lockout);
    return RelayCommandResult::Blocked;
  }

  // Check maxOnTimeForced (safety override — cannot be bypassed)
  if (desiredState && _state[channel].maxOnTimeForced) {
    messageOut = "Channel FORCE OFF (maxOnTime exceeded) — acknowledge + clear required";
    return RelayCommandResult::Blocked;
  }

  // [audit p.401] DIRECT maxOnTime evaluation — defense-in-depth. The
  // maxOnTimeForced flag above is refreshed by _checkMaxOnTime() at the
  // START of each executor tick (safety runs BEFORE the command queue),
  // but the executor must ALSO verify the hard limit from raw timestamps
  // so the invariant holds even if this ever executes outside the tick
  // ordering contract (future call sites, harnesses). A channel whose hard
  // ON-limit is already exceeded is force-OFF NOW and the ON is blocked.
  if (desiredState && _config[channel].maxOnTimeSec > 0 &&
      _state[channel].reportedState && _state[channel].onSinceMs > 0) {
    uint32_t onDuration = (millis() - _state[channel].onSinceMs) / 1000;
    if (onDuration >= _config[channel].maxOnTimeSec) {
      if (_pulses[channel].active) {
        _annotatePulseOutcome(channel, "cancelled by maxOnTime FORCE OFF");
        _pulses[channel].active = false;  // [audit p.409] maxOnTime outranks pulse
      }
      _applyChannelState(channel, false, Core::RelaySource::Safety);
      _state[channel].maxOnTimeForced = true;
      _state[channel].lockout = Core::RelayLockoutState::Tripped;
      _saveLockoutStates();
      String msg = "Channel " + String(channel) + " maxOnTime exceeded (" +
                   String(onDuration) + "s >= " + String(_config[channel].maxOnTimeSec) +
                   "s) — ON blocked, FORCE OFF";
      Services::Log.append(Core::LogType::Custom, "RELAY: " + msg, 0);
      Services::alarms.raise(Core::AlarmCode::RELAY_MAX_ON_TIME,
                   Core::AlarmSeverity::Critical, msg.c_str());
      messageOut = msg;
      return RelayCommandResult::Blocked;
    }
  }

  // Determine source
  Core::RelaySource src = Core::RelaySource::Manual;
  if (source == "SCHEDULE") src = Core::RelaySource::Schedule;
  else if (source == "AUTOMATION") src = Core::RelaySource::Automation;
  else if (source == "SAFETY") src = Core::RelaySource::Safety;
  else if (source == "SYSTEM") src = Core::RelaySource::System;

  // Handle "on" command
  if (command == "on") {
    // Safety evaluation
    SafetyDecision sd = _evaluateSafety(channel, true);
    if (sd == SafetyDecision::InhibitMinOff) {
      messageOut = "Blocked by minOffTime (cooling period)";
      return RelayCommandResult::Blocked;
    }
    if (sd == SafetyDecision::InhibitChatter) {
      messageOut = "Blocked by antiChatter (min switch interval)";
      return RelayCommandResult::Blocked;
    }

    // Interlock evaluation
    String interlockReason;
    if (!_evaluateInterlock(channel, true, interlockReason)) {
      messageOut = "Blocked by interlock: " + interlockReason;
      Services::alarms.raise(Core::AlarmCode::RELAY_INTERLOCK_VIOLATION,
                   Core::AlarmSeverity::Warning,
                   interlockReason.c_str());
      return RelayCommandResult::Blocked;
    }

    _applyChannelState(channel, true, src);
    // [audit p.407] An unverified I²C write must NEVER be reported as
    // Applied/EXECUTED — _applyChannelState flags the channel on failure
    // without reverting the contract; surface it as Failed (the executor
    // promotes it to Unknown when the driver shadow is unknown).
    if (_state[channel].fault) {
      messageOut = "Channel " + String(channel) + " ON — I²C write FAILED, state UNKNOWN";
      return RelayCommandResult::Failed;
    }
    messageOut = "Channel " + String(channel) + " ON";
    return RelayCommandResult::Applied;
  }

  // Handle "off" command
  if (command == "off") {
    // Safety evaluation
    SafetyDecision sd = _evaluateSafety(channel, false);
    if (sd == SafetyDecision::InhibitMinOn) {
      messageOut = "Blocked by minOnTime (protect inductive load)";
      return RelayCommandResult::Blocked;
    }

    _applyChannelState(channel, false, src);
    // [audit p.407] Same honesty contract as the ON path — see above.
    if (_state[channel].fault) {
      messageOut = "Channel " + String(channel) + " OFF — I²C write FAILED, state UNKNOWN";
      return RelayCommandResult::Failed;
    }
    messageOut = "Channel " + String(channel) + " OFF";
    return RelayCommandResult::Applied;
  }

  // Handle "pulse" command (momentary ON for duration, then OFF)
  if (command == "pulse") {
    if (pulseDurationMs == 0 || pulseDurationMs > 60000) {
      messageOut = "Invalid pulse duration (1-60000 ms)";
      return RelayCommandResult::Rejected;
    }

    // Same safety + interlock checks as "on"
    SafetyDecision sd = _evaluateSafety(channel, true);
    if (sd == SafetyDecision::InhibitMinOff || sd == SafetyDecision::InhibitChatter) {
      messageOut = "Blocked by safety: " + String(sd == SafetyDecision::InhibitMinOff ? "minOffTime" : "antiChatter");
      return RelayCommandResult::Blocked;
    }

    String interlockReason;
    if (!_evaluateInterlock(channel, true, interlockReason)) {
      messageOut = "Blocked by interlock: " + interlockReason;
      return RelayCommandResult::Blocked;
    }

    // [P1-9] Reject pulse shorter than minOnTime
    if (_config[channel].minOnTimeSec > 0 && pulseDurationMs < _config[channel].minOnTimeSec * 1000) {
      messageOut = "Pulse duration " + String(pulseDurationMs) + "ms < minOnTime " +
                   String(_config[channel].minOnTimeSec * 1000) + "ms — rejected";
      return RelayCommandResult::Rejected;
    }

    // Turn ON
    _applyChannelState(channel, true, src);
    // [audit p.407] If the ON write failed, do NOT schedule the pulse and
    // do NOT report Applied — the transaction must land Failed/Unknown.
    if (_state[channel].fault) {
      messageOut = "Channel " + String(channel) + " PULSE — I²C write FAILED, state UNKNOWN";
      return RelayCommandResult::Failed;
    }

    // [audit p.403] Overwrite semantics are now explicit and auditable: a
    // newer pulse on the same channel supersedes the pending one — the
    // superseded transaction is annotated in the result ring first.
    if (_pulses[channel].active) {
      _annotatePulseOutcome(channel, "superseded by newer pulse");
    }

    // [P1-8 + audit p.403] Schedule pulse OFF — one slot per channel,
    // deterministic, owned by this transaction.
    strncpy(_pulses[channel].transactionId,
            transactionId ? transactionId : "",
            sizeof(_pulses[channel].transactionId) - 1);
    _pulses[channel].transactionId[sizeof(_pulses[channel].transactionId) - 1] = '\0';
    _pulses[channel].generation = _safetyGeneration;
    _pulses[channel].offAtMs = millis() + pulseDurationMs;
    _pulses[channel].active = true;

    messageOut = "Channel " + String(channel) + " PULSE " + String(pulseDurationMs) + "ms";
    return RelayCommandResult::Applied;
  }

  messageOut = "Unknown command: " + command;
  return RelayCommandResult::Rejected;
}

// [audit p.363-365] Emergency ALL OFF — the SAFETY AUTHORITY path.
// - Attempts the OFF write on EVERY channel regardless of reportedState:
//   after an I²C fault the physical state is unknown, so "software thinks
//   it's off" is NOT a reason to skip the write.
// - NOT blocked by minOnTime (safety hierarchy, audit p.378).
// - Bumps the safety generation → every command still queued from BEFORE the
//   emergency is re-validated at execution and BLOCKED (audit p.366-369).
void RelayController::emergencyAllOff() {
  // Invalidate pending normal commands — they belong to a dead safety epoch.
  _safetyGeneration++;

  if (!_driverAvailable) {
    Services::Log.append(Core::LogType::Custom,
               "RELAY: E-WAVE cascade — driver UNAVAILABLE, cannot force OFF", 0);
    Services::alarms.raise(Core::AlarmCode::RELAY_FAULT,
                           Core::AlarmSeverity::Critical,
                           "E-WAVE: relay driver unavailable — physical state NOT verifiable");
    return;
  }

  AllOffResult result;
  result.requested = Core::RELAY_CHANNEL_COUNT;
  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (_pulses[ch].active) {
      _annotatePulseOutcome(ch, "cancelled by E-WAVE emergency cascade");
    }
    _pulses[ch].active = false;  // cancel pending pulses first

    // Attempt OFF regardless of reportedState (audit p.364).
    _applyChannelState(ch, false, Core::RelaySource::Safety);

    if (_state[ch].fault) {
      // _applyChannelState could not verify the write — outcome unknown
      // (the write may or may not have reached the expander).
      result.unknown++;
      result.detail += "CH" + String(ch) + ":UNKNOWN ";
    } else if (!_state[ch].reportedState) {
      result.success++;
    } else {
      result.failed++;
      result.detail += "CH" + String(ch) + ":STILL_ON ";
    }
  }

  Services::alarms.raise(Core::AlarmCode::RELAY_FAULT,
                         Core::AlarmSeverity::Critical,
                         ("E-WAVE cascade executed: " + String(result.success) + " off, " +
                          String(result.unknown) + " unknown").c_str());
  Services::Log.append(Core::LogType::Custom,
             "RELAY: E-WAVE cascade — all channels OFF attempted (" +
             String(result.success) + " ok, " + String(result.unknown) + " unknown, safetyGen=" +
             String(_safetyGeneration) + ")", 0);
}

bool RelayController::acknowledgeSafetyAlarm(uint8_t channel) {
  if (!_validChannel(channel)) return false;
  if (_state[channel].lockout != Core::RelayLockoutState::Tripped) return false;

  _state[channel].lockout = Core::RelayLockoutState::Acknowledged;
  _saveLockoutStates();
  Services::Log.append(Core::LogType::Custom,
             "RELAY: Channel " + String(channel) + " safety alarm acknowledged", 0);
  return true;
}

bool RelayController::clearSafetyLockout(uint8_t channel) {
  if (!_validChannel(channel)) return false;
  if (_state[channel].lockout != Core::RelayLockoutState::Acknowledged) return false;

  // Check if fault condition is resolved
  // For maxOnTime: the channel is OFF, so the condition is resolved
  // For driver fault: check driver availability
  bool faultResolved = !_state[channel].maxOnTimeForced || !_state[channel].reportedState;
  if (!faultResolved) return false;

  _state[channel].maxOnTimeForced = false;
  // [audit p.380-382] CLEARED is recorded before ARMING so the lifecycle is
  // observable in logs; ARMED → NORMAL happens on the next executor tick.
  _state[channel].lockout = Core::RelayLockoutState::Cleared;
  _state[channel].lockout = Core::RelayLockoutState::Armed;
  _saveLockoutStates();
  Services::Log.append(Core::LogType::Custom,
             "RELAY: Channel " + String(channel) + " safety lockout cleared (CLEARED→ARMED)", 0);
  return true;
}

bool RelayController::setChannelConfig(uint8_t ch, const RelayChannelConfig& cfg) {
  if (!_validChannel(ch)) return false;
  _config[ch] = cfg;
  _saveConfig();
  return true;
}

void RelayController::serializeStatus(JsonArray& arr) const {
  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    JsonObject o = arr.createNestedObject();
    o["channel"] = ch;
    o["name"] = _config[ch].name;
    o["desiredState"] = _state[ch].desiredState;
    o["reportedState"] = _state[ch].reportedState;
    o["physicalState"] = nullptr;  // null = no aux feedback
    o["stateConfidence"] = Core::relayStateConfidenceToStr(_state[ch].confidence);
    o["fault"] = _state[ch].fault;
    o["lockoutState"] = Core::relayLockoutStateToStr(_state[ch].lockout);
    o["source"] = Core::relaySourceToStr(_state[ch].source);
    o["enabled"] = _config[ch].enabled;
    o["lastChangedAt"] = _state[ch].lastChangedAtMs;
    o["maxOnTimeForced"] = _state[ch].maxOnTimeForced;
    o["stateSequence"] = _state[ch].stateSequence;  // [audit p.293] readback authority
  }
}

// ============================================================================
// QUEUE — single physical mutation authority (audit p.284-289, p.589-602)
// ============================================================================

bool RelayController::queueCommand(const QueuedRelayCommand& cmd) {
  if (_commandQueueHandle == nullptr) return false;
  // Producer side: networkTask (core 1) / MQTT context. xQueueSend is
  // cross-core safe; timeout 0 — ingress must never block on a full queue
  // (the caller reports 503 and the operator retries with the SAME
  // transactionId, which the journal deduplicates).
  return xQueueSend((QueueHandle_t)_commandQueueHandle, &cmd, 0) == pdTRUE;
}

void RelayController::processCommandQueue() {
  if (_commandQueueHandle == nullptr) return;

  QueuedRelayCommand cmd;
  while (xQueueReceive((QueueHandle_t)_commandQueueHandle, &cmd, 0) == pdTRUE) {
    // ---- EXECUTION-TIME VALIDATION (audit p.375-377) --------------------
    // Ingress validation proves the command COULD enter the system; this
    // proves it can STILL safely execute NOW.

    // 1. Freshness re-check — the command may have expired while queued.
    if (cmd.expiresAt > 0) {
      uint32_t now = (uint32_t)::time(nullptr);
      if (now != 0 && cmd.expiresAt < now) {
        _recordTransactionResult(cmd, RelayTerminalResult::Rejected,
                                 "command expired while queued");
        continue;
      }
    }

    // 2. Safety generation — commands queued BEFORE an emergency trip are
    //    stale and must never re-activate a load after E-WAVE (audit
    //    p.366-369). No automatic restoration of lower-priority intent.
    if (cmd.safetyGeneration != _safetyGeneration) {
      _recordTransactionResult(cmd, RelayTerminalResult::Blocked,
                               "BLOCKED_STALE_SAFETY — safety epoch changed (emergency)");
      continue;
    }

    // 3. Execute through the single mutation path (safety + interlock are
    //    re-evaluated inside applyCommand immediately before the write).
    //    transactionId is passed through so pulse lifecycle events are
    //    attributed to the owning transaction (audit p.403).
    String messageOut;
    RelayCommandResult r = applyCommand(String(cmd.command), cmd.channel,
                                        cmd.desiredState, cmd.pulseDurationMs,
                                        String(cmd.source), messageOut,
                                        cmd.transactionId);

    RelayTerminalResult terminal;
    switch (r) {
      case RelayCommandResult::Applied: terminal = RelayTerminalResult::Executed; break;
      case RelayCommandResult::Blocked: terminal = RelayTerminalResult::Blocked; break;
      case RelayCommandResult::Rejected: terminal = RelayTerminalResult::Rejected; break;
      default:                          terminal = RelayTerminalResult::Failed; break;
    }
    // Honest UNKNOWN propagation: when the hardware write failed AND the
    // driver shadow is unknown, the outcome is not deterministic FAILED.
    if (terminal == RelayTerminalResult::Failed &&
        Drivers::relayExpander.isShadowUnknown()) {
      terminal = RelayTerminalResult::Unknown;
    }
    _recordTransactionResult(cmd, terminal, messageOut);
  }
}

bool RelayController::getTransactionResult(const String& transactionId,
                                           RelayTransactionRecord& out) const {
  if (transactionId.length() == 0) return false;
  for (uint8_t i = 0; i < RESULT_RING_SIZE; i++) {
    if (_resultRing[i].valid &&
        transactionId.equals(_resultRing[i].transactionId)) {
      out = _resultRing[i];
      return true;
    }
  }
  return false;
}

void RelayController::_recordTransactionResult(const QueuedRelayCommand& cmd,
                                               RelayTerminalResult result,
                                               const String& message) {
  RelayTransactionRecord& rec = _resultRing[_resultRingNext];
  strncpy(rec.transactionId, cmd.transactionId, sizeof(rec.transactionId) - 1);
  rec.transactionId[sizeof(rec.transactionId) - 1] = '\0';
  rec.result = result;
  rec.channel = cmd.channel;
  rec.desiredState = cmd.desiredState;
  rec.reportedState = _validChannel(cmd.channel) ? _state[cmd.channel].reportedState : false;
  rec.stateSequence = _validChannel(cmd.channel) ? _state[cmd.channel].stateSequence : 0;
  strncpy(rec.message, message.c_str(), sizeof(rec.message) - 1);
  rec.message[sizeof(rec.message) - 1] = '\0';
  rec.completedAtMs = millis();
  rec.valid = true;
  _resultRingNext = (_resultRingNext + 1) % RESULT_RING_SIZE;
}

// [audit p.403] Attribute a pulse lifecycle event (auto-OFF, supersede,
// cancellation) to the owning transaction by annotating its record in the
// result ring. The terminal result is NOT rewritten — this only enriches
// the audit trail so the physical OFF can be correlated to the pulse
// transaction that scheduled it.
void RelayController::_annotatePulseOutcome(uint8_t ch, const char* note) {
  if (!_validChannel(ch)) return;
  const char* txId = _pulses[ch].transactionId;
  if (txId == nullptr || txId[0] == '\0') return;  // legacy/internal pulse

  for (uint8_t i = 0; i < RESULT_RING_SIZE; i++) {
    RelayTransactionRecord& rec = _resultRing[i];
    if (rec.valid && strncmp(rec.transactionId, txId, sizeof(rec.transactionId)) == 0) {
      String m = String(rec.message) + " | " + note;
      strncpy(rec.message, m.c_str(), sizeof(rec.message) - 1);
      rec.message[sizeof(rec.message) - 1] = '\0';
      rec.completedAtMs = millis();
      break;  // most recent record wins — ring lookup returns first match
    }
  }
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

void RelayController::_applyChannelState(uint8_t ch, bool newState, Core::RelaySource source) {
  // SINGLE GPIO MUTATION PATH — the ONLY function that calls relayExpander.setChannel()
  bool oldState = _state[ch].reportedState;

  // Write to hardware — [P0-2 FIX] check return value!
  bool hwOk = Drivers::relayExpander.setChannel(ch, newState);

  if (!hwOk) {
    // I²C write FAILED — do NOT update reportedState.
    // The physical relay state is UNKNOWN. Mark fault + raise alarm.
    // The operator must reconcile after investigating the I²C issue.
    _state[ch].fault = true;
    _state[ch].confidence = Core::RelayStateConfidence::Fault;
    // desiredState reflects what was requested (for state-drift detection)
    _state[ch].desiredState = newState;
    _state[ch].source = source;
    Services::alarms.raise(Core::AlarmCode::RELAY_FAULT,
                           Core::AlarmSeverity::Critical,
                           ("I²C write failed for channel " + String(ch)).c_str());
    Services::Log.append(Core::LogType::Custom,
                         "RELAY: I²C write FAILED for channel " + String(ch) +
                         " — state NOT updated (reportedState unchanged, fault=true)", 0);
    return;
  }

  // Hardware write succeeded — update state
  _state[ch].reportedState = newState;
  _state[ch].desiredState = newState;
  _state[ch].confidence = Core::RelayStateConfidence::SoftwareOnly;
  _state[ch].source = source;
  _state[ch].stateSequence++;
  _state[ch].lastChangedAtMs = millis();
  _state[ch].fault = false;

  if (newState && !oldState) {
    // Turned ON
    _state[ch].onSinceMs = millis();
  } else if (!newState && oldState) {
    // Turned OFF
    _state[ch].onSinceMs = 0;
  }

  // Update interlock group
  if (_config[ch].interlockGroup > 0 && _config[ch].interlockGroup <= 4) {
    RelayInterlockGroup& group = _interlockGroups[_config[ch].interlockGroup - 1];
    if (group.active) {
      if (newState) {
        group.activeMember = ch;
      } else {
        if (group.activeMember == ch) {
          group.activeMember = 0xFF;
        }
        group.lastOffMs = millis();
      }
    }
  }
}

RelayController::SafetyDecision RelayController::_evaluateSafety(uint8_t ch, bool desired) {
  uint32_t now = millis();

  if (desired) {
    // Check minOffTime (cooling period before re-enabling)
    if (_config[ch].minOffTimeSec > 0 && !_state[ch].reportedState) {
      uint32_t offDuration = (now - _state[ch].lastChangedAtMs) / 1000;
      if (offDuration < _config[ch].minOffTimeSec) {
        return SafetyDecision::InhibitMinOff;
      }
    }

    // Check antiChatter (min switch interval)
    if (_config[ch].minSwitchIntervalSec > 0) {
      uint32_t sinceLastChange = (now - _state[ch].lastChangedAtMs) / 1000;
      if (sinceLastChange < _config[ch].minSwitchIntervalSec) {
        return SafetyDecision::InhibitChatter;
      }
    }
  } else {
    // Check minOnTime (protect inductive load from premature OFF)
    if (_config[ch].minOnTimeSec > 0 && _state[ch].reportedState && _state[ch].onSinceMs > 0) {
      uint32_t onDuration = (now - _state[ch].onSinceMs) / 1000;
      if (onDuration < _config[ch].minOnTimeSec) {
        return SafetyDecision::InhibitMinOn;
      }
    }
  }

  return SafetyDecision::Allow;
}

bool RelayController::_evaluateInterlock(uint8_t ch, bool desired, String& reasonOut) {
  if (!desired) return true;  // OFF is always allowed

  uint8_t groupIdx = _config[ch].interlockGroup;
  if (groupIdx == 0 || groupIdx > 4) return true;  // no interlock group

  RelayInterlockGroup& group = _interlockGroups[groupIdx - 1];
  if (!group.active) return true;

  // Check mutual exclusion
  if (group.activeMember != 0xFF && group.activeMember != ch) {
    reasonOut = "mutual exclusion — channel " + String((int)group.activeMember) + " is active in group " + String(groupIdx);
    return false;
  }

  // Check dead time
  if (group.lastOffMs > 0) {
    uint32_t sinceOff = millis() - group.lastOffMs;
    if (sinceOff < group.deadTimeMs) {
      reasonOut = "dead time — " + String(group.deadTimeMs - sinceOff) + "ms remaining";
      return false;
    }
  }

  return true;
}

void RelayController::_checkMaxOnTime() {
  uint32_t now = millis();

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (!_state[ch].reportedState) continue;
    if (_state[ch].onSinceMs == 0) continue;
    if (_config[ch].maxOnTimeSec == 0) continue;  // 0 = unlimited

    uint32_t onDuration = (now - _state[ch].onSinceMs) / 1000;
    if (onDuration >= _config[ch].maxOnTimeSec && !_state[ch].maxOnTimeForced) {
      // FORCE OFF — safety authority, cannot be overridden.
      // [audit p.409] Safety decision matrix: maxOnTime outranks pulse
      // expiry — cancel the pending pulse so the two timers never race.
      if (_pulses[ch].active) {
        _annotatePulseOutcome(ch, "cancelled by maxOnTime FORCE OFF");
        _pulses[ch].active = false;
      }
      _applyChannelState(ch, false, Core::RelaySource::Safety);
      _state[ch].maxOnTimeForced = true;
      _state[ch].lockout = Core::RelayLockoutState::Tripped;
      _saveLockoutStates();

      String msg = "RELAY: Channel " + String(ch) + " maxOnTime exceeded (" +
                   String(onDuration) + "s >= " + String(_config[ch].maxOnTimeSec) + "s) — FORCE OFF";
      Services::Log.append(Core::LogType::Custom, msg, 0);
      Services::alarms.raise(Core::AlarmCode::RELAY_MAX_ON_TIME,
                   Core::AlarmSeverity::Critical,
                   msg.c_str());
    }
  }
}

void RelayController::_processPulses() {
  uint32_t now = millis();
  // [P1-8] 8 slots — one per channel, deterministic
  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (!_pulses[ch].active) continue;
    // [audit p.402] Rollover-safe deadline check. millis() wraps every
    // ~49.7 days; the absolute comparison `now >= offAtMs` misjudges across
    // the wrap (a just-scheduled pulse whose deadline wrapped is seen as
    // already expired; a deadline that passed just before the wrap is seen
    // as not-yet-reached for another ~49.7 days). The signed-difference
    // pattern in Core::deadlineReached is correct for any deadline less
    // than 2^31 ms in the future — pulse durations are bounded to 60 s.
    if (Core::deadlineReached(now, _pulses[ch].offAtMs)) {
      uint32_t onDuration = (now - _state[ch].onSinceMs) / 1000;
      if (_config[ch].minOnTimeSec > 0 && onDuration < _config[ch].minOnTimeSec) {
        // [audit p.409] Safety matrix: pulse expiry WAITS for minOnTime
        // (deferred, never skipped) — the OFF is rescheduled, not dropped.
        _pulses[ch].offAtMs = _state[ch].onSinceMs + (_config[ch].minOnTimeSec * 1000);
      } else {
        _applyChannelState(ch, false, Core::RelaySource::Manual);
        if (_state[ch].fault) {
          // The auto-OFF write itself failed — the pending I²C shadow
          // recovery will drive ALL OFF (safe direction) within one tick.
          // Attribute honestly; do not report the pulse as completed.
          _annotatePulseOutcome(ch, "auto-OFF write FAILED — deferred to I²C recovery");
        } else {
          _annotatePulseOutcome(ch, "pulse completed (auto-OFF)");
        }
        _pulses[ch].active = false;
      }
    }
  }
}

void RelayController::_loadConfig() {
  Preferences p;
  if (!p.begin(Core::RELAY_NVS_NAMESPACE, true)) return;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    String prefix = "ch" + String(ch) + "_";
    String name = p.getString((prefix + "name").c_str(), "");
    strncpy(_config[ch].name, name.c_str(), Core::RELAY_MAX_NAME_LEN - 1);
    _config[ch].name[Core::RELAY_MAX_NAME_LEN - 1] = '\0';
    _config[ch].maxOnTimeSec = p.getULong((prefix + "maxOn").c_str(), Core::RELAY_DEFAULT_MAX_ON_TIME_SEC);
    _config[ch].minOnTimeSec = p.getULong((prefix + "minOn").c_str(), Core::RELAY_DEFAULT_MIN_ON_TIME_SEC);
    _config[ch].minOffTimeSec = p.getULong((prefix + "minOff").c_str(), Core::RELAY_DEFAULT_MIN_OFF_TIME_SEC);
    _config[ch].minSwitchIntervalSec = p.getULong((prefix + "minInt").c_str(), Core::RELAY_DEFAULT_MIN_SWITCH_INTERVAL_SEC);
    _config[ch].enabled = p.getBool((prefix + "en").c_str(), true);
    _config[ch].interlockGroup = p.getUChar((prefix + "ilk").c_str(), 0);
  }

  // Load PCF8574 address
  _pcf8574Address = p.getUChar("i2c_addr", Core::PCF8574_I2C_ADDRESS_DEFAULT);

  // Load interlock groups — [P1-11] validate members (reject invalid/duplicate/out-of-range)
  for (uint8_t g = 0; g < 4; g++) {
    String prefix = "ilk" + String(g) + "_";
    _interlockGroups[g].active = p.getBool((prefix + "act").c_str(), false);
    _interlockGroups[g].deadTimeMs = p.getUShort((prefix + "dead").c_str(), 1000);
    _interlockGroups[g].memberCount = 0;
    _interlockGroups[g].activeMember = 0xFF;
    if (_interlockGroups[g].active) {
      String membersKey = prefix + "mem";
      String members = p.getString(membersKey.c_str(), "");
      // Parse comma-separated channel indices with validation
      int start = 0;
      while (start < (int)members.length() && _interlockGroups[g].memberCount < 4) {
        int comma = members.indexOf(',', start);
        String num = (comma < 0) ? members.substring(start) : members.substring(start, comma);
        num.trim();
        if (num.length() > 0) {
          int val = num.toInt();
          // [P1-11] Validate: must be 0-7
          if (val < 0 || val >= Core::RELAY_CHANNEL_COUNT) {
            Services::Log.append(Core::LogType::Custom,
              "RELAY: Interlock group " + String(g) + " has invalid member " + num + " — skipped", 0);
          } else {
            // Check for duplicate
            bool dup = false;
            for (uint8_t m = 0; m < _interlockGroups[g].memberCount; m++) {
              if (_interlockGroups[g].members[m] == val) { dup = true; break; }
            }
            if (dup) {
              Services::Log.append(Core::LogType::Custom,
                "RELAY: Interlock group " + String(g) + " has duplicate member " + String(val) + " — skipped", 0);
            } else {
              _interlockGroups[g].members[_interlockGroups[g].memberCount++] = val;
            }
          }
        }
        if (comma < 0) break;
        start = comma + 1;
      }
      // If no valid members, deactivate the group
      if (_interlockGroups[g].memberCount < 2) {
        Services::Log.append(Core::LogType::Custom,
          "RELAY: Interlock group " + String(g) + " has <2 valid members — deactivated", 0);
        _interlockGroups[g].active = false;
      }
    }
  }

  p.end();
}

void RelayController::_saveConfig() {
  Preferences p;
  if (!p.begin(Core::RELAY_NVS_NAMESPACE, false)) return;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    String prefix = "ch" + String(ch) + "_";
    p.putString((prefix + "name").c_str(), _config[ch].name);
    p.putULong((prefix + "maxOn").c_str(), _config[ch].maxOnTimeSec);
    p.putULong((prefix + "minOn").c_str(), _config[ch].minOnTimeSec);
    p.putULong((prefix + "minOff").c_str(), _config[ch].minOffTimeSec);
    p.putULong((prefix + "minInt").c_str(), _config[ch].minSwitchIntervalSec);
    p.putBool((prefix + "en").c_str(), _config[ch].enabled);
    p.putUChar((prefix + "ilk").c_str(), _config[ch].interlockGroup);
  }

  p.putUChar("i2c_addr", _pcf8574Address);

  for (uint8_t g = 0; g < 4; g++) {
    String prefix = "ilk" + String(g) + "_";
    p.putBool((prefix + "act").c_str(), _interlockGroups[g].active);
    p.putUShort((prefix + "dead").c_str(), _interlockGroups[g].deadTimeMs);
    String members = "";
    for (uint8_t m = 0; m < _interlockGroups[g].memberCount; m++) {
      if (m > 0) members += ",";
      members += String(_interlockGroups[g].members[m]);
    }
    p.putString((prefix + "mem").c_str(), members);
  }

  p.end();
}

void RelayController::_loadLockoutStates() {
  Preferences p;
  if (!p.begin(Core::RELAY_NVS_NAMESPACE, true)) return;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    String key = "lock_" + String(ch);
    _state[ch].lockout = (Core::RelayLockoutState)p.getUChar(key.c_str(), 0);
    String forcedKey = "frc_" + String(ch);
    _state[ch].maxOnTimeForced = p.getBool(forcedKey.c_str(), false);
  }
  p.end();
}

void RelayController::_saveLockoutStates() {
  Preferences p;
  if (!p.begin(Core::RELAY_NVS_NAMESPACE, false)) return;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    String key = "lock_" + String(ch);
    p.putUChar(key.c_str(), (uint8_t)_state[ch].lockout);
    String forcedKey = "frc_" + String(ch);
    p.putBool(forcedKey.c_str(), _state[ch].maxOnTimeForced);
  }
  p.end();
}

void RelayController::_recordHeartbeat() {
  // [CI fix] Use Services::health.recordHeartbeat() — the canonical API.
  // The previous 'extern void recordHeartbeat(Core::TaskId)' declaration
  // looked for a free function that doesn't exist — recordHeartbeat is a
  // member of HealthSupervisor class.
  Services::health.recordHeartbeat(Core::TaskId::Relay);
}

// [P1-7] all_off with per-channel result tracking.
// EXECUTOR-ONLY — invoked from applyCommand("all_off") inside relayTask.
// Writes OFF on every channel regardless of reportedState (audit p.364 —
// channels whose physical state is unknown after a fault still get the
// safe-direction write attempted).
AllOffResult RelayController::allOffWithResult() {
  AllOffResult result;
  result.requested = Core::RELAY_CHANNEL_COUNT;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (_pulses[ch].active) {
      _annotatePulseOutcome(ch, "cancelled by all_off");
    }
    _pulses[ch].active = false;  // cancel any pending pulse for this channel
    _applyChannelState(ch, false, Core::RelaySource::Manual);

    if (_state[ch].fault) {
      // The write outcome is unverified — count separately from hard failures
      // so the ACK can report UNKNOWN honestly (audit p.50, p.365).
      result.unknown++;
      result.detail += "CH" + String(ch) + ":UNKNOWN ";
    } else {
      result.success++;
    }
  }
  return result;
}

} // namespace Services

#endif // PLTS_ENABLE_RELAYS
