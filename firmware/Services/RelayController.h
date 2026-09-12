// =============================================================================
// Services/RelayController.h — 8-channel relay state machine + safety + interlock
// -----------------------------------------------------------------------------
// [v1.8.0] Single authoritative relay control path.
//
// [PRODUCTION-GRADE 2026-09] — audit p.34-59, p.284-294 remediation:
//
//   SINGLE PHYSICAL EXECUTION AUTHORITY (RG-RELAY-01):
//     REST ─────┐
//     MQTT ─────┤→ queueCommand() → FreeRTOS Queue → relayTask → applyCommand()
//     all_off ──┘                                            │
//                                                            ▼
//                                              RelayExpanderDriver (I²C guard)
//     SAFETY OVERRIDE (separate authority, RG-RELAY-08):
//     EmergencySupervisor → emergencyAllOff()  (never waits for the queue)
//
//   TRANSACTION IDENTITY SURVIVES THE QUEUE (RG-RELAY-02):
//     QueuedRelayCommand carries transactionId/requestId/commandHash plus
//     the full envelope; the final execution result is recorded and queryable
//     via getTransactionResult() / GET /api/relays/transactions/{id}.
//
//   SAFETY GENERATION (audit p.366-369):
//     emergencyAllOff() increments _safetyGeneration; every command queued
//     BEFORE the emergency is re-validated at EXECUTION time and BLOCKED as
//     BLOCKED_STALE_SAFETY — a queued "ON" can never re-activate a load
//     after an E-WAVE trip.
//
//   EXECUTION-TIME VALIDATION (audit p.375-377):
//     Freshness (expiresAt), safety, and interlock are re-evaluated when the
//     command is DEQUEUED, immediately before the hardware mutation —
//     ingress validation and execution validation are two boundaries.
//
//   SAFETY DECISION MATRIX (audit p.409 — precedence, highest first):
//     1. EMERGENCY (E-WAVE cascade)   — bypasses minOnTime, lockout, queue;
//                                      executed by the safety authority on
//                                      its own execution context, bumps the
//                                      safety generation (stale queued
//                                      commands are BLOCKED).
//     2. maxOnTime FORCE OFF          — hard limit; tick() enforces it BEFORE
//                                      the command queue is drained, and
//                                      applyCommand() re-evaluates it
//                                      directly from millis()-onSinceMs as
//                                      defense-in-depth (audit p.401).
//     3. pulse expiry (auto-OFF)      — bounded by minOnTime: the auto-OFF is
//                                      DEFERRED until minOnTime is
//                                      satisfied, never dropped; cancelled
//                                      by 1/2 above.
//     4. normal OFF / minOnTime /
//        interlock / antiChatter     — lowest; evaluated per command.
//
//   EXECUTOR TICK ORDERING CONTRACT (audit p.401):
//     I²C shadow recovery → maxOnTime enforcement → command queue →
//     pulse expiry → lockout transitions. Safety state is refreshed BEFORE
//     normal commands are evaluated, so no queued ON can execute inside
//     the window between "limit exceeded" and "flag updated".
//
// Safety features:
//   - maxOnTime (FORCE OFF, cannot be bypassed)
//   - minOnTime (protect inductive loads; bypassed ONLY by safety/emergency)
//   - minOffTime (cooling period)
//   - antiChatter (min switch interval)
//   - 5-state lockout (NORMAL→TRIPPED→ACKNOWLEDGED→CLEARED→ARMED→NORMAL)
//   - NVS-persisted lockout (prevents bypass via power-cycle)
//
// Interlock:
//   - Declarative mutual-exclusion groups
//   - Dead time between OFF one member and ON another
//
// State model (3-tier, honest):
//   desiredState  — what operator/automation requested
//   reportedState — what device ACK'd (software GPIO state)
//   physicalState — null (no aux feedback) = UNKNOWN
//   stateConfidence — SOFTWARE_ONLY (default, no physical verification)
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_RELAY_CONTROLLER_H
#define PLTS_SERVICES_RELAY_CONTROLLER_H

#include <Arduino.h>
#include "../Core/Config.h"   // [CI fix] MUST be before #if PLTS_ENABLE_RELAYS
#include "../Core/Types.h"
#if PLTS_ENABLE_RELAYS
#include "../Drivers/RelayExpanderDriver.h"
#include <ArduinoJson.h>

namespace Services {

// Per-channel relay state (runtime, RAM only — recomputed on boot)
struct RelayChannelState {
  bool desiredState = false;           // what was requested
  bool reportedState = false;          // what GPIO was set to (software state)
  bool physicalState = false;          // always false — no aux feedback
  Core::RelayStateConfidence confidence = Core::RelayStateConfidence::Unknown;
  Core::RelaySource source = Core::RelaySource::Off;
  Core::RelayLockoutState lockout = Core::RelayLockoutState::Normal;
  uint32_t stateSequence = 0;          // monotonic per-channel
  uint32_t lastChangedAtMs = 0;        // millis() of last state change
  uint32_t onSinceMs = 0;             // millis() when turned ON (0 = not ON)
  bool maxOnTimeForced = false;        // FORCE OFF active
  bool fault = false;
};

// Per-channel configuration (persistent, NVS)
struct RelayChannelConfig {
  char name[Core::RELAY_MAX_NAME_LEN] = "";
  uint32_t maxOnTimeSec = Core::RELAY_DEFAULT_MAX_ON_TIME_SEC;
  uint32_t minOnTimeSec = Core::RELAY_DEFAULT_MIN_ON_TIME_SEC;
  uint32_t minOffTimeSec = Core::RELAY_DEFAULT_MIN_OFF_TIME_SEC;
  uint32_t minSwitchIntervalSec = Core::RELAY_DEFAULT_MIN_SWITCH_INTERVAL_SEC;
  bool enabled = true;                 // channel is usable
  uint8_t interlockGroup = 0;          // 0 = no interlock; 1-4 = group ID
};

// Interlock group definition
struct RelayInterlockGroup {
  bool active = false;                 // is this group configured?
  uint8_t members[4] = {0};            // channel indices (max 4 per group)
  uint8_t memberCount = 0;
  uint16_t deadTimeMs = 1000;          // min time between OFF one and ON another
  uint8_t activeMember = 0xFF;         // 0xFF = none active; else channel index
  uint32_t lastOffMs = 0;             // millis() of last OFF in this group
};

// Command result
enum class RelayCommandResult {
  Applied,
  Rejected,
  Blocked,
  Failed
};

// [audit p.50] Canonical terminal results — surfaced to REST/MQTT/PWA.
// UNKNOWN is a FIRST-CLASS outcome: an I²C timeout does not mean the write
// did not reach the expander, so it must never be reported as FAILED.
enum class RelayTerminalResult {
  Executed,
  Blocked,
  Rejected,
  Failed,
  Unknown
};

// [P1-7] Result for all_off — per-channel breakdown
struct AllOffResult {
  uint8_t requested = 0;
  uint8_t success = 0;
  uint8_t failed = 0;
  uint8_t unknown = 0;   // [audit p.365] write attempted, outcome unverified
  String detail;         // per-channel failures
};

// [RG-RELAY-02] Queued command with FULL transaction identity — the queue is
// the durability hand-off point between ingress (networkTask) and the single
// executor (relayTask). Identity must survive it for final-result correlation.
struct QueuedRelayCommand {
  char transactionId[65] = {0};
  char requestId[65] = {0};
  char commandHash[65] = {0};
  char command[16] = {0};        // "on" | "off" | "pulse" | "all_off"
  uint8_t channel = 0;
  bool desiredState = false;
  uint32_t pulseDurationMs = 0;
  char source[16] = {0};
  uint32_t issuedAt = 0;
  uint32_t expiresAt = 0;
  uint32_t safetyGeneration = 0;  // snapshot at enqueue time
  uint32_t enqueuedAtMs = 0;
};

// [RG-RELAY-09] Final transaction record — bounded ring of the most recent
// relay command outcomes, queryable by transactionId (PWA reconciliation).
struct RelayTransactionRecord {
  char transactionId[65] = {0};
  RelayTerminalResult result = RelayTerminalResult::Unknown;
  uint8_t channel = 0;
  bool desiredState = false;
  bool reportedState = false;
  uint32_t stateSequence = 0;
  char message[96] = {0};
  uint32_t completedAtMs = 0;
  bool valid = false;
};

class RelayController {
public:
  void begin();
  void tick();  // 5 Hz — safety checks, maxOnTime enforcement, lockout transitions

  /// Apply a relay command. Returns result + message.
  /// EXECUTOR-ONLY: called from relayTask (processCommandQueue). Ingress
  /// (REST/MQTT) must use queueCommand() — never this method.
  /// Commands: "on", "off", "pulse", "all_off", "config", "acknowledge", "clear"
  /// transactionId (optional): identity of the queued command — used to
  /// attribute pulse lifecycle events (audit p.403).
  RelayCommandResult applyCommand(const String& command,
                                   uint8_t channel,
                                   bool desiredState,
                                   uint32_t pulseDurationMs,
                                   const String& source,
                                   String& messageOut,
                                   const char* transactionId = nullptr);

  /// E-WAVE safety cascade — called from EmergencySupervisor (safety
  /// authority, separate execution context). Attempts OFF on EVERY channel
  /// regardless of reportedState (audit p.363-365: after an I²C fault the
  /// physical state is unknown — safety must attempt the write anyway).
  /// Increments the safety generation, invalidating queued normal commands.
  /// Cannot be overridden, and is not blocked by minOnTime.
  void emergencyAllOff();

  /// [P1-7] all_off with per-channel result tracking.
  /// EXECUTOR-ONLY: invoked from applyCommand("all_off") inside relayTask.
  AllOffResult allOffWithResult();

  /// [RG-RELAY-02/06] Queue a command for execution in relayTask context.
  /// ALL relay mutations from REST/MQTT go through this queue — the FreeRTOS
  /// queue provides the cross-core synchronization the old volatile ring
  /// buffer lacked (audit p.101-102). Returns false if the queue is full.
  bool queueCommand(const QueuedRelayCommand& cmd);

  /// [P1-10] Process queued commands — called from relayTask tick().
  /// Re-validates freshness + safety generation at EXECUTION time, executes,
  /// and records the final per-transaction result.
  void processCommandQueue();

  /// Current safety generation — incremented by every emergency trip.
  uint32_t safetyGeneration() const { return _safetyGeneration; }

  /// [RG-RELAY-09] Look up the final outcome of a relay transaction.
  /// Returns true + fills `out` when a record exists for this transactionId.
  bool getTransactionResult(const String& transactionId,
                            RelayTransactionRecord& out) const;

  /// Acknowledge safety alarm for a channel (TRIPPED → ACKNOWLEDGED).
  bool acknowledgeSafetyAlarm(uint8_t channel);

  /// Clear safety lockout for a channel (ACKNOWLEDGED → CLEARED).
  /// Requires fault condition resolved.
  bool clearSafetyLockout(uint8_t channel);

  // --- Accessors ---
  const RelayChannelState& getChannelState(uint8_t ch) const { return _state[ch]; }
  const RelayChannelConfig& getChannelConfig(uint8_t ch) const { return _config[ch]; }
  bool isAvailable() const { return _driverAvailable; }
  uint8_t getChannelCount() const { return Core::RELAY_CHANNEL_COUNT; }

  /// Set channel config (from REST/MQTT config command). Saves to NVS.
  bool setChannelConfig(uint8_t ch, const RelayChannelConfig& cfg);

  /// Serialize relay status into a JSON array for telemetry.
  void serializeStatus(JsonArray& arr) const;

private:
  RelayChannelState _state[Core::RELAY_CHANNEL_COUNT];
  RelayChannelConfig _config[Core::RELAY_CHANNEL_COUNT];
  RelayInterlockGroup _interlockGroups[4];  // max 4 interlock groups
  bool _driverAvailable = false;
  uint8_t _pcf8574Address = Core::PCF8574_I2C_ADDRESS_DEFAULT;

  // [audit p.366-369] Safety epoch — bumped on every emergency trip. Queued
  // commands snapshot the generation at enqueue; a mismatch at execution
  // time BLOCKS the command as stale (no post-emergency reactivation).
  uint32_t _safetyGeneration = 0;

  // [P1-8 + audit p.403] Pulse tracking — one slot per channel (deterministic,
  // no overflow). Each entry carries the identity of the transaction that
  // scheduled it, so the eventual auto-OFF (or its cancellation / supersede)
  // is correlated back to the owning transaction in the result ring.
  struct PulseEntry {
    bool active = false;
    uint32_t offAtMs = 0;               // when to turn OFF (deadline — checked
                                        // rollover-safe via deadlineReached)
    char transactionId[65] = {0};        // owning TX ("" for legacy internal calls)
    uint32_t generation = 0;            // safety generation at schedule time
  };
  PulseEntry _pulses[Core::RELAY_CHANNEL_COUNT];  // 8 slots — one per channel

  // [audit p.101-102] Native FreeRTOS queue — thread-safe across cores.
  // The old custom ring buffer (volatile head/tail, array) had NO
  // synchronization primitive between the networkTask producer and the
  // relayTask consumer.
  void* _commandQueueHandle = nullptr;  // QueueHandle_t (kept opaque in header)
  static const uint8_t COMMAND_QUEUE_SIZE = 8;

  // [RG-RELAY-09] Recent final results — small ring for PWA reconciliation.
  static const uint8_t RESULT_RING_SIZE = 8;
  RelayTransactionRecord _resultRing[RESULT_RING_SIZE];
  uint8_t _resultRingNext = 0;

  // --- Internal methods ---

  /// SINGLE GPIO MUTATION PATH — the only function that calls relayExpander.setChannel().
  /// Updates state, sequence, timestamp, confidence, safety records, interlock records.
  void _applyChannelState(uint8_t ch, bool newState, Core::RelaySource source);

  /// Safety evaluation — returns Allow/Inhibit/ForceOff
  enum class SafetyDecision { Allow, InhibitMinOn, InhibitMinOff, InhibitChatter, ForceOffMaxOn };
  SafetyDecision _evaluateSafety(uint8_t ch, bool desired);

  /// Interlock evaluation — returns true if transition allowed
  bool _evaluateInterlock(uint8_t ch, bool desired, String& reasonOut);

  /// Check maxOnTime for all channels — FORCE OFF if exceeded
  void _checkMaxOnTime();

  /// Process pending pulses (turn OFF after duration)
  void _processPulses();

  /// Load config from NVS
  void _loadConfig();

  /// Save config to NVS
  void _saveConfig();

  /// Load lockout states from NVS (prevents power-cycle bypass)
  void _loadLockoutStates();

  /// Save lockout states to NVS
  void _saveLockoutStates();

  /// Record heartbeat for health supervisor
  void _recordHeartbeat();

  /// Validate channel index
  bool _validChannel(uint8_t ch) const { return ch < Core::RELAY_CHANNEL_COUNT; }

  /// Record a final transaction outcome into the result ring.
  void _recordTransactionResult(const QueuedRelayCommand& cmd,
                                RelayTerminalResult result,
                                const String& message);

  /// [audit p.403] Attribute a pulse lifecycle event (auto-OFF, supersede,
  /// cancellation) to the owning transaction by annotating its record in
  /// the result ring. The terminal result is NOT rewritten — this enriches
  /// the audit trail so the physical OFF can be correlated to the pulse
  /// transaction that scheduled it.
  void _annotatePulseOutcome(uint8_t ch, const char* note);
};

extern RelayController relaysController;

} // namespace Services

#endif // PLTS_ENABLE_RELAYS
#endif // PLTS_SERVICES_RELAY_CONTROLLER_H
