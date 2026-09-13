// =============================================================================
// Services/AlarmRegistry.h — Central alarm engine (brief §35, §60)
// -----------------------------------------------------------------------------
// Idempotent raise() — same code raised twice doesn't duplicate (refreshes).
// INFO / WARNING / CRITICAL severity.
// ACTIVE / ACKNOWLEDGED / CLEARED lifecycle (ACK ≠ CLEAR).
// Max 24 alarms. Auto-evict cleared (oldest) when full.
//
// [AUDIT 2026-09 ROUND 5 / p.451 — CRITICAL POLICY] When the registry is full:
//   - Eviction candidates are CLEARED entries ONLY (oldest clearedAt first,
//     raisedAt as tie-break).
//   - ACTIVE / ACKNOWLEDGED safety state is NEVER sacrificed to make room for
//     a new alarm. If no cleared slot exists the new entry is REJECTED
//     (raise() returns false, overflowCount++ persists across reads, and a
//     rate-limited log line records it). Rejection is recoverable — alarm
//     evaluators re-raise every tick, so the alarm is admitted as soon as a
//     slot is freed; an evicted ACTIVE alarm would have been a PERMANENT,
//     silent loss of safety state.
//   - Saturation is observable: /api/alarms + /api/diagnostics expose
//     overflowCount so the operator can see the registry is full.
//
// [AUDIT 2026-09 ROUND 5 / p.450 — durability symmetry] clear() and
// acknowledge() already persist immediately (operator actions). raise() now
// ALSO persists immediately on a MEANINGFUL existing-alarm update (severity
// upgrade or message change) — a severity escalation is a safety-relevant
// mutation and must not sit in a dirty flag for up to PERSIST_INTERVAL_MS.
// Pure refreshes (same severity, no message) still ride the periodic
// checkpoint to bound NVS wear.
//
// Codes per brief §35 (PLTS-specific):
//   BATTERY_VOLTAGE_LOW, BATTERY_VOLTAGE_HIGH, BATTERY_VOLTAGE_INVALID,
//   BATTERY_OVERCURRENT_CHARGE, BATTERY_OVERCURRENT_DISCHARGE,
//   BATTERY_CURRENT_SENSOR_ERROR, BATTERY_CURRENT_SUSPECT,
//   BATTERY_SOC_LOW, TEMPERATURE_HIGH, TEMPERATURE_CRITICAL,
//   HUMIDITY_HIGH, CONDENSATION_RISK, SHT31_FAILURE,
//   AC_OVERCURRENT, AC_CURRENT_SENSOR_ERROR, AC_CURRENT_STALE,
//   DEVICE_OFFLINE, TELEMETRY_STALE, TIME_UNSYNCED,
//   STORAGE_ERROR, NETWORK_DEGRADED, OTA_FAILURE,
//   CONFIGURATION_ERROR, CALIBRATION_ERROR
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_ALARM_REGISTRY_H
#define PLTS_SERVICES_ALARM_REGISTRY_H

#include <Arduino.h>
#include <cstdint>
#include "../Core/Types.h"

namespace Services {

struct Alarm {
  static constexpr uint8_t CODE_LEN = 32;
  char     code[CODE_LEN];
  Core::AlarmSeverity severity;
  Core::AlarmLifecycle lifecycle;
  uint32_t raisedAt;       // Unix epoch
  uint32_t acknowledgedAt;
  uint32_t clearedAt;
  uint32_t lastUpdatedAt;
  char     message[80];
};

class AlarmRegistry {
public:
  static constexpr uint8_t MAX_ALARMS = 24;

  void begin();
  // Idempotent raise — refreshes existing alarm instead of duplicating.
  // [p.451] Returns FALSE only when the registry is saturated with
  // non-cleared alarms and the new entry was rejected (never silently).
  bool raise(const char* code, Core::AlarmSeverity sev, const char* message = "");
  // Clear an alarm (active → cleared).
  void clear(const char* code);
  // Mark as acknowledged (operator has seen it; alarm may still be active).
  void acknowledge(const char* code);
  void acknowledgeAll();

  uint8_t countActive() const;
  uint8_t countAll() const;
  const Alarm* getAlarm(uint8_t idx) const;
  const Alarm* find(const char* code) const;
  Core::AlarmSeverity highestActiveSeverity() const;
  const Alarm* getActiveAlarms() const { return _alarms; }
  uint8_t getActiveAlarmCount() const { return _count; }

  // [FW-23 REMEDIATION 2026-08] Copy NON-CLEARED alarms (Active +
  // Acknowledged) into a caller buffer. The old getActiveAlarms()/
  // getActiveAlarmCount() pair exposed the FULL array including CLEARED
  // entries — every telemetry envelope published cleared alarms as active.
  // Returns the number of entries copied.
  uint8_t copyActiveAlarms(Alarm* dst, uint8_t max) const;

  // [p.451] Registry saturation observability.
  uint32_t overflowCount() const { return _overflowCount; }

  // [FW-23] Persistence — alarms survive reboot (versioned NVS blob + CRC).
  // raise()/clear()/acknowledge() set the dirty flag; persistenceTask
  // checkpoints dirty state; operator actions (clear/ack) save immediately.
  // [p.450] Severity-escalating raise() refreshes also save immediately.
  // [p.439-family] Returns false when the NVS write failed (begin/putBytes)
  // so the persistence task can raise STORAGE_ERROR — failure is never
  // silently "assumed persisted".
  bool saveToNVS();
  void loadFromNVS();
  bool isDirty() const { return _dirty; }
  uint32_t persistFailures() const { return _persistFailures; }

private:
  Alarm  _alarms[MAX_ALARMS] = {};
  uint8_t _count = 0;
  bool    _dirty = false;
  uint32_t _overflowCount = 0;   // [p.451] rejected-by-saturation events (this boot)
  uint32_t _persistFailures = 0; // [p.439-family] failed NVS writes (this boot)
  uint8_t _findIdx(const char* code) const;
};

extern AlarmRegistry alarms;

} // namespace Services

#endif // PLTS_SERVICES_ALARM_REGISTRY_H
