// =============================================================================
// Services/RelayController.h — 8-channel relay state machine + safety + interlock
// -----------------------------------------------------------------------------
// [v1.8.0] Single authoritative relay control path.
//
// Architecture (NO BYPASS):
//   REST/MQTT → applyCommand() → safety.evaluate() → interlock.evaluate()
//     → applyChannelState() → RelayExpanderDriver.setChannel() → I²C → GPIO
//
// Safety features:
//   - maxOnTime (FORCE OFF, cannot be bypassed)
//   - minOnTime (protect inductive loads)
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
#if PLTS_ENABLE_RELAYS
#include "../Core/Config.h"
#include "../Core/Types.h"
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

class RelayController {
public:
  void begin();
  void tick();  // 5 Hz — safety checks, maxOnTime enforcement, lockout transitions

  /// Apply a relay command. Returns result + message.
  /// This is the SINGLE ENTRY POINT for all relay mutations.
  /// Commands: "on", "off", "pulse", "all_off", "config", "acknowledge", "clear"
  RelayCommandResult applyCommand(const String& command,
                                   uint8_t channel,
                                   bool desiredState,
                                   uint32_t pulseDurationMs,
                                   const String& source,
                                   String& messageOut);

  /// E-WAVE safety cascade — called from EmergencySupervisor::_trip().
  /// Forces ALL channels OFF immediately. Cannot be overridden.
  void emergencyAllOff();

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

  // Pulse tracking
  struct PulseEntry {
    uint8_t channel;
    uint32_t offAtMs;  // when to turn OFF
    bool active;
  };
  PulseEntry _pulses[4];  // max 4 concurrent pulses
  uint8_t _pulseWriteIdx = 0;

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
};

extern RelayController relaysController;

} // namespace Services

#endif // PLTS_ENABLE_RELAYS
#endif // PLTS_SERVICES_RELAY_CONTROLLER_H
