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
// [AUDIT 2026-09 ROUND 6 / p.453 + p.455 — SINGLE-RECORD ATOMICITY] The
// old layout wrote TWO separate NVS records ("hdr" + "arr") and checked
// the array write with `w2 != 0` — a partial array write could still
// report success, and a power loss between the two records left a
// MIXED-GENERATION pair (new header + old/incomplete array) that only a
// CRC rejection at boot could catch, discarding the whole alarm history.
// The state is now ONE NVS record ("state"): magic + version + generation
// + count + CRC + the alarm array, written with a strict full-length
// check and verified by read-back. NVS writes one key atomically
// (journaled), so a boot either sees the complete previous generation or
// the complete new one — never a mix. The generation counter additionally
// lets post-mortems prove which transaction produced a snapshot.
//
// [AUDIT 2026-09 ROUND 6 / p.454 — HONEST RAISE CONTRACT] raise() returning
// true historically meant "accepted in the RAM registry" while the
// immediate NVS persistence result was invisible to the caller. The bool
// remains (all existing call sites ignore it), but its contract is now
// DOCUMENTED as "accepted in RAM — NOT a durability claim"; callers that
// need the durability outcome use raiseTracked(), which distinguishes
// persisted / persist-failed / no-write-needed. Persist failures increment
// persistFailures() and are logged as StorageError events; the periodic
// checkpoint retries the dirty state.
//
// [AUDIT 2026-09 ROUND 6 / p.460 prerequisite — REACTIVATION] raise() on a
// CLEARED entry re-ACTIVATES it (new raisedAt, clearedAt reset). The old
// refresh path left lifecycle = Cleared forever, so any alarm code that
// was cleared once could never become active again — a recurring condition
// (overcurrent → clear → overcurrent) was invisible to the alarm center
// for the rest of the boot.
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

// [p.454] Durability-aware outcome of an alarm submission.
enum class RaiseResult : uint8_t {
  Rejected = 0,               // registry saturated with non-cleared alarms
  AcceptedRam = 1,           // in the RAM registry; no immediate NVS write needed (pure refresh)
  AcceptedPersisted = 2,     // in RAM AND the immediate NVS write verified OK
  AcceptedPersistFailed = 3,  // in RAM; the immediate NVS write FAILED (checkpoint retries)
};

class AlarmRegistry {
public:
  static constexpr uint8_t MAX_ALARMS = 24;

  void begin();
  // [p.454] Durability-aware submission — same registry semantics as
  // raise() plus the persistence outcome. See RaiseResult.
  RaiseResult raiseTracked(const char* code, Core::AlarmSeverity sev,
                           const char* message = "");
  // Idempotent raise — refreshes existing alarm instead of duplicating.
  // [p.451] Returns FALSE only when the registry is saturated with
  // non-cleared alarms and the new entry was rejected (never silently).
  // [p.454] TRUE means "accepted in the RAM registry" — NOT a durability
  // claim. Durability outcomes: raiseTracked() / persistFailures() /
  // STORAGE_ERROR raised by the persistence task on continued failure.
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
  // [p.439-family] Returns false when the NVS write failed (begin/putBytes/
  // read-back) so the persistence task can raise STORAGE_ERROR — failure is
  // never silently "assumed persisted".
  // [p.453/p.455] ONE NVS record: {magic, version, generation, count, crc32,
  // alarms[]} — strict full-length write check + read-back verification.
  bool saveToNVS();
  void loadFromNVS();
  bool isDirty() const { return _dirty; }
  uint32_t persistFailures() const { return _persistFailures; }
  // [p.455] Generation of the last successfully written / loaded snapshot
  // (diagnostics: proves which transaction produced the persisted state).
  uint16_t generation() const { return _generation; }

private:
  Alarm  _alarms[MAX_ALARMS] = {};
  uint8_t _count = 0;
  bool    _dirty = false;
  uint32_t _overflowCount = 0;   // [p.451] rejected-by-saturation events (this boot)
  uint32_t _persistFailures = 0; // [p.439-family] failed NVS writes (this boot)
  uint16_t _generation = 0;       // [p.455] persisted-snapshot generation
  uint8_t _findIdx(const char* code) const;
};

extern AlarmRegistry alarms;

} // namespace Services

#endif // PLTS_SERVICES_ALARM_REGISTRY_H
