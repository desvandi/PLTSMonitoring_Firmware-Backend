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

  // [P1-10] Initialize command queue
  _cmdQueueHead = 0;
  _cmdQueueTail = 0;

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

  // [P1-10] Process queued commands FIRST — single-threaded mutation
  processCommandQueue();

  if (!_driverAvailable) return;

  // 1. Check maxOnTime for all channels — FORCE OFF if exceeded
  _checkMaxOnTime();

  // 2. Process pending pulses (turn OFF after duration)
  _processPulses();

  // 3. Process lockout state transitions (ARMED → NORMAL)
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
    String& messageOut) {

  if (!_driverAvailable) {
    messageOut = "Relay driver unavailable (PCF8574 not responding)";
    return RelayCommandResult::Failed;
  }

  // Handle "all_off" command
  if (command == "all_off") {
    for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
      _applyChannelState(ch, false, Core::RelaySource::System);
    }
    messageOut = "All channels OFF";
    Services::Log.append(Core::LogType::Custom, "RELAY: all_off command executed", 0);
    return RelayCommandResult::Applied;
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

    // [P1-8] Schedule pulse OFF — one slot per channel, deterministic
    _pulses[channel].offAtMs = millis() + pulseDurationMs;
    _pulses[channel].active = true;

    messageOut = "Channel " + String(channel) + " PULSE " + String(pulseDurationMs) + "ms";
    return RelayCommandResult::Applied;
  }

  messageOut = "Unknown command: " + command;
  return RelayCommandResult::Rejected;
}

void RelayController::emergencyAllOff() {
  if (!_driverAvailable) return;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    if (_state[ch].reportedState) {
      _applyChannelState(ch, false, Core::RelaySource::System);
    }
  }
  Services::Log.append(Core::LogType::Custom,
             "RELAY: E-WAVE cascade — all channels OFF", 0);
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
  _state[channel].lockout = Core::RelayLockoutState::Cleared;
  // Will transition to ARMED on next tick, then NORMAL
  _state[channel].lockout = Core::RelayLockoutState::Armed;
  _saveLockoutStates();
  Services::Log.append(Core::LogType::Custom,
             "RELAY: Channel " + String(channel) + " safety lockout cleared", 0);
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
      // FORCE OFF — safety authority, cannot be overridden
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
    if (now >= _pulses[ch].offAtMs) {
      // [P1-9] Check minOnTime before turning OFF
      uint32_t onDuration = (now - _state[ch].onSinceMs) / 1000;
      if (_config[ch].minOnTimeSec > 0 && onDuration < _config[ch].minOnTimeSec) {
        // Delay the OFF until minOnTime is satisfied
        _pulses[ch].offAtMs = _state[ch].onSinceMs + (_config[ch].minOnTimeSec * 1000);
      } else {
        _applyChannelState(ch, false, Core::RelaySource::Manual);
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

// [P1-7] all_off with per-channel result tracking
AllOffResult RelayController::allOffWithResult() {
  AllOffResult result;
  result.requested = Core::RELAY_CHANNEL_COUNT;

  for (uint8_t ch = 0; ch < Core::RELAY_CHANNEL_COUNT; ch++) {
    bool wasOn = _state[ch].reportedState;
    if (wasOn) {
      _applyChannelState(ch, false, Core::RelaySource::System);
      // Check if the write succeeded (fault flag set by _applyChannelState on failure)
      if (_state[ch].fault) {
        result.failed++;
        result.detail += "CH" + String(ch) + ":I2C_FAIL ";
      } else {
        result.success++;
      }
      // Cancel any pending pulse for this channel
      _pulses[ch].active = false;
    } else {
      result.success++;  // already OFF = success
    }
  }
  return result;
}

// [P1-10] Command queue — single-threaded mutation via relayTask
bool RelayController::queueCommand(const String& command, uint8_t channel,
                                    bool desiredState, uint32_t pulseDurationMs,
                                    const String& source) {
  uint8_t nextTail = (_cmdQueueTail + 1) % COMMAND_QUEUE_SIZE;
  if (nextTail == _cmdQueueHead) {
    // Queue full
    return false;
  }
  QueuedCommand& cmd = _commandQueue[_cmdQueueTail];
  strncpy(cmd.command, command.c_str(), sizeof(cmd.command) - 1);
  cmd.command[sizeof(cmd.command) - 1] = '\0';
  cmd.channel = channel;
  cmd.desiredState = desiredState;
  cmd.pulseDurationMs = pulseDurationMs;
  strncpy(cmd.source, source.c_str(), sizeof(cmd.source) - 1);
  cmd.source[sizeof(cmd.source) - 1] = '\0';
  cmd.valid = true;
  _cmdQueueTail = nextTail;
  return true;
}

void RelayController::processCommandQueue() {
  while (_cmdQueueHead != _cmdQueueTail) {
    QueuedCommand& cmd = _commandQueue[_cmdQueueHead];
    if (cmd.valid) {
      String messageOut;
      applyCommand(String(cmd.command), cmd.channel, cmd.desiredState,
                   cmd.pulseDurationMs, String(cmd.source), messageOut);
      cmd.valid = false;
    }
    _cmdQueueHead = (_cmdQueueHead + 1) % COMMAND_QUEUE_SIZE;
  }
}

} // namespace Services

#endif // PLTS_ENABLE_RELAYS
