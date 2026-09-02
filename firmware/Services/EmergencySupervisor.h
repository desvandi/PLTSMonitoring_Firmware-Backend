// =============================================================================
// Services/EmergencySupervisor.h — E-WAVE v1.6.0 emergency state machine
// -----------------------------------------------------------------------------
// PORT of the firmware-generic emergency layer (README §13 limitation #7)
// into the modular service pattern. LOCAL-FIRST: tick() never touches the
// network — a safety function must never depend on WiFi/MQTT/GAS being up
// (Brief §75). Remote ARM/DISARM/CONFIG commands arrive via
// Network::GasEmergencyChannel (EMERGENCY_PENDING poll, HMAC envelope).
//
// STATE MACHINE (fail-safe by construction):
//   Every boot re-enters EMERGENCY (RAM-only state — no persistence of RUN).
//   RUN is reachable ONLY via a validated operator ARM command:
//     crash-chain < 3  AND  all trigger sensors valid (sensorFailPolicy=1,
//     fail-closed)  AND  no live threshold violation  AND  recovery window
//     (recoverySec, default 60 s) elapsed since the last clear state.
//   TRIP is LATCHED: any violation while RUN -> EMERGENCY immediately (relay
//   ISOLATED within the same tick, local GPIO write, no network hop).
//   E-stop (physical, normally-closed) is AUTHORITATIVE IN HARDWARE — the
//   sense line only latches the state; releasing it never re-energizes.
//
// PERSISTED (NVS "plts_emg"): lifetime trip counter, crash chain, healthy-boot
// flag, plus the 13-field trigger config (via Storage::ConfigStore).
//
// PORT DELTAS vs firmware-generic (documented, deliberate):
//   * Sensor inputs come from the canonical measurement pipeline
//     (latestStatus, quality-gated) instead of raw 10-sample moving averages
//     — the modular firmware already runs plausibility/stale machinery.
//   * The genset current channel (iGen / i_ac_gen) is RESERVED: the modular
//     board has one ACS712. The channel never trips and is EXCLUDED from the
//     sensor-loss policy until a second ACS712 lands (PLTS_ENABLE_AC_GEN).
//   * Events retry up to 20 attempts with a 5 s rate limit (generic dropped
//     the pending event after one attempt — dead code fixed here).
//   * ARMED/DISARMED events are emitted (GAS whitelist already accepts them).
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_EMERGENCY_SUPERVISOR_H
#define PLTS_SERVICES_EMERGENCY_SUPERVISOR_H

#if PLTS_ENABLE_EMERGENCY

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../Core/Config.h"

namespace Services {

enum class EmgState : uint8_t {
  Run,          // relay ENERGIZED, kontaktor path CLOSED
  Emergency     // relay ISOLATED (latched until operator ARM)
};

// Reason vocabulary — wire-compatible with GAS EmergencyEvents + PWA.
constexpr const char* EMG_REASON_BOOT          = "BOOT";
constexpr const char* EMG_REASON_VBAT_LOW      = "VBAT_LOW";
constexpr const char* EMG_REASON_VBAT_HIGH     = "VBAT_HIGH";
constexpr const char* EMG_REASON_I_DC_OVER     = "I_DC_OVER";
constexpr const char* EMG_REASON_I_AC_LOAD_OVER= "I_AC_LOAD_OVER";
constexpr const char* EMG_REASON_I_AC_GEN_OVER = "I_AC_GEN_OVER";
constexpr const char* EMG_REASON_SENSOR_LOSS   = "SENSOR_LOSS";
constexpr const char* EMG_REASON_ESTOP         = "ESTOP";
constexpr const char* EMG_REASON_OPERATOR      = "OPERATOR";
constexpr const char* EMG_REASON_CRASHLOOP     = "CRASHLOOP";

// Sensor snapshot consumed by triggers + the ARM gate. NaN = invalid/absent.
struct EmgSensors {
  float vbat;           // battery pack voltage [V]
  float idc;            // DC battery current [A, +charging]
  float iac;            // AC load RMS current [A]
  float igen;           // RESERVED: genset AC current [A] (NaN until wired)
  bool  ina219Present;  // DC current sensor detected
};

class EmergencySupervisor {
public:
  EmergencySupervisor() = default;

  // Boot init: driver pins from cfg, NVS counters, crash-chain evaluation,
  // BOOT/CRASHLOOP event, LED pin. Idempotent.
  void begin();

  // 10 Hz local tick (emergencyTask). NO network I/O. Order per tick:
  // E-stop poll -> trigger evaluation (RUN only) -> recovery clock ->
  // runtime-healthy mark -> LED -> latestStatus publish.
  void tick();

  // Operator command entry point (from GasEmergencyChannel). Commands are
  // uppercase; unknown commands REJECTED. Returns the ACK result
  // ("APPLIED" | "REJECTED") and human-readable message (Indonesian, parity
  // with firmware-generic).
  String applyCommand(const String& commandId, const String& command,
                      JsonVariantConst cfg, String& messageOut);

  // ---- Event queue (single latest unsent event; channel retries) ----
  bool peekPendingEvent(String& typeOut, String& reasonOut) const;
  void consumePendingEvent();       // called by the channel AFTER a 200 ACK
  void queueEvent(const char* type, const String& reason);

  // ---- Status accessors (thread-safe snapshot under _mutex) ----
  const char* stateStr() const;     // "RUN" | "EMERGENCY"
  const char* reasonStr() const;
  bool isEstopOpen() const;
  bool relayEnergized() const;
  uint32_t trips() const;
  uint8_t  crashChain() const;

  // Publishes the emergency block into latestStatus (telemetryMutex).
  void publishStatus();

  // Static range validation shared by CONFIG apply + ConfigStore load.
  // Returns the clamped/defaulted value (field dropped -> default, exactly
  // like firmware-generic CONFIG: out-of-range fields are silently dropped
  // and keep their previous/default value).
  static float clampEmgFloat(float v, float lo, float hi, float fallback);
  static long clampEmgInt(long v, long lo, long hi, long fallback);

  static constexpr uint8_t  EMG_CRASH_CHAIN_LIMIT   = 3;      // >=3 unhealthy reboots -> hold
  static constexpr uint32_t EMG_HEALTHY_RUNTIME_MS  = 300000; // 5 stable minutes
  static constexpr uint8_t  EMG_DEBOUNCE_SLOTS      = 6;      // vbatLo,vbatHi,iDc,iAc,iAcGen,sensorLoss

private:
  void _trip(const char* reason, const char* eventType = "TRIP",
             const String& eventReason = "");
  void _queueEventUnlocked(const char* type, const String& reason);
  void _arm(const char* source);
  String _armBlockReason(const EmgSensors& s);
  void _evaluateTriggers(const EmgSensors& s);
  void _trackClear(const EmgSensors& s);
  void _pollEstop();
  void _markRuntimeHealthy();
  EmgSensors _readSensors();
  void _ledTick();
  void _nvsSetU32(const char* key, uint32_t v);

  mutable SemaphoreHandle_t _mutex = nullptr;
  EmgState  _state       = EmgState::Emergency;   // RAM-only: boot = EMERGENCY
  const char* _reason    = EMG_REASON_BOOT;
  uint32_t _tripAtMs     = 0;
  uint32_t _trips        = 0;
  uint8_t  _crashChain   = 0;
  bool     _estopOpen    = false;
  bool     _runOkMarked  = false;
  uint8_t  _debounce[EMG_DEBOUNCE_SLOTS] = {0, 0, 0, 0, 0, 0};
  uint32_t _clearAtMs    = 0;
  String   _pendingEventType;
  String   _pendingEventReason;
  bool     _hasPendingEvent = false;
  bool     _ledState       = false;
  uint32_t _lastLedToggleMs = 0;
};

extern EmergencySupervisor emergency;

} // namespace Services

#endif // PLTS_ENABLE_EMERGENCY
#endif // PLTS_SERVICES_EMERGENCY_SUPERVISOR_H
