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
//   acknowledge() already persist immediately (operator actions). raise() now
//   ALSO persists immediately on a MEANINGFUL existing-alarm update (severity
//   upgrade or message change) — a severity escalation is a safety-relevant
//   mutation and must not sit in a dirty flag for up to PERSIST_INTERVAL_MS.
//   Pure refreshes (same severity, no message) still ride the periodic
//   checkpoint to bound NVS wear.
//
// [AUDIT 2026-09 ROUND 6 / p.453 + p.455 — SINGLE-RECORD ATOMICITY] The
//   old layout wrote TWO separate NVS records ("hdr" + "arr") and checked
//   the array write with `w2 != 0` — a partial array write could still
//   report success, and a power loss between the two records left a
//   MIXED-GENERATION pair (new header + old/incomplete array) that only a
//   CRC rejection at boot could catch, discarding the whole alarm history.
//   The state is now ONE NVS record ("state"): magic + version + generation
//   + count + CRC + the alarm array, written with a strict full-length
//   check and verified by read-back. NVS writes one key atomically
//   (journaled), so a boot either sees the complete previous generation or
//   the complete new one — never a mix. The generation counter additionally
//   lets post-mortems prove which transaction produced a snapshot.
//
// [AUDIT 2026-09 ROUND 6 / p.454 — HONEST RAISE CONTRACT] raise() returning
//   true historically meant "accepted in the RAM registry" while the
//   immediate NVS persistence result was invisible to the caller. The bool
//   remains (all existing call sites ignore it), but its contract is now
//   DOCUMENTED as "accepted in RAM — NOT a durability claim"; callers that
//   need the durability outcome use raiseTracked(), which distinguishes
//   persisted / persist-failed / no-write-needed. Persist failures increment
//   persistFailures() and are logged as StorageError events; the periodic
//   checkpoint retries the dirty state.
//
// [AUDIT 2026-09 ROUND 6 / p.460 prerequisite — REACTIVATION] raise() on a
//   CLEARED entry re-ACTIVATES it (new raisedAt, clearedAt reset). The old
//   refresh path left lifecycle = Cleared forever, so any alarm code that
//   was cleared once could never become active again — a recurring condition
//   (overcurrent → clear → overcurrent) was invisible to the alarm center
//   for the rest of the boot.
//
// [AUDIT 2026-09 ROUND 7 / p.467 + p.468 + p.469 — CROSS-TASK
//   SERIALIZATION] The registry is a global singleton mutated from MANY
//   execution contexts: measurementTask (AcMeasurement), anomaly/health
//   paths (AnomalyDetector, HealthSupervisor), energyTask (SocStateMachine),
//   networkTask (WifiManager/MQTT), the web/async handler task
//   (AlarmHandlers, calibration), relayTask (RelayController),
//   emergencyTask (EmergencySupervisor) and persistenceTask (checkpoint) —
//   the round-6 fixes made each ALGORITHM atomic but left the REGISTRY
//   itself unsynchronized: two concurrent raise() calls could both read
//   _findIdx()==0xFF and both write the same append slot (lost alarm /
//   duplicated code), a clear() interleaved with an eviction could shift
//   the array under a writer's index, and two saveToNVS() calls could
//   build+write blobs from different RAM snapshots (mixed-generation
//   persisted state — the exact interleaving the round-6 single-record
//   atomicity fix assumed away on the WRITE side but never serialized on
//   the PRODUCER side).
//
//   Serialization now lives INSIDE the registry (the caller-side-mutex
//   approach the auditor rejected: call sites are too scattered to keep a
//   foreign lock correct). Architecture:
//
//     public API  →  lock  →  private *Unlocked() implementation
//       raise()            → raiseTrackedUnlocked()   → saveToNVSUnlocked()
//       clear()            → clearUnlocked()          → saveToNVSUnlocked()
//       acknowledge()      → acknowledgeUnlocked()    → saveToNVSUnlocked()
//       snapshotInto()     → (single coherent copy of the whole state)
//
//   - ONE non-recursive FreeRTOS mutex; taken ONLY in public entry points,
//     NEVER inside the Unlocked implementations — deadlock-free by
//     construction (no nested acquisition is even expressible).
//   - The immediate saves inside raise/clear/ack run while STILL HOLDING the
//     lock (p.468): the RAM mutation, blob build, write and read-back are
//     one serialized transaction — a concurrent mutator cannot slip
//     between the RAM update and the NVS write.
//   - READERS never see interior pointers: getAlarm()/getActiveAlarms()
//     (raw-pointer footguns) are REMOVED; find() copies the entry out;
//     snapshotInto() copies the entire registry state in one locked pass
//     (p.469: no countAll()+getAlarm() torn multi-step reads).
//   - clearIfActive() is an atomic test-and-clear for evaluator tick loops
//     (the find→check→clear TOCTOU window is closed; a re-raise racing the
//     clear is no longer lost between two separate acquisitions).
//   - Contract: task context ONLY (FreeRTOS mutex, priority inheritance).
//     No ISR may touch the registry — verified: every current call site is
//     a task/loop/web-handler context; the lock helper would be unusable
//     from interrupt context.
//
// Codes per brief §35 (PLTS-specific):
//   BATTERY_VOLTAGE_LOW, BATTERY_VOLTAGE_HIGH, BATTERY_VOLTAGE_INVALID,
//   BATTERY_OVERCURRENT_CHARGE, BATTERY_OVERCURRENT_DISCHARGE,
//   BATTERY_CURRENT_SENSOR_ERROR, BATTERY_CURRENT_SUSPECT,
//   BATTERY_SOC_LOW, TEMPERATURE_HIGH, TEMPERATURE_CRITICAL,
//   TEMPERATURE_RAPID_RISE, HUMIDITY_HIGH, CONDENSATION_RISK,
//   SHT31_FAILURE,
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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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

  // [p.469] Coherent whole-registry read — ONE lock acquisition copies the
  // array + every diagnostic counter together. Readers that need several
  // fields (REST GET /api/alarms, telemetry envelope) consume ONE snapshot
  // instead of a multi-call sequence (countAll()+getAlarm() loop) whose
  // steps could straddle a concurrent mutation. ~3.3 KB — provide storage
  // that is NOT a 4 KB task stack (static buffer or heap), like saveToNVS.
  struct Snapshot {
    Alarm    alarms[MAX_ALARMS];
    uint8_t  count;
    uint32_t overflowCount;
    uint32_t persistFailures;
    uint16_t generation;
    bool     dirty;
  };

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
  // [p.467] Atomic test-and-clear: clears ONLY if the entry exists and is
  // not already Cleared — the evaluator tick loops previously did
  // find()→check→clear() as two acquisitions, and a concurrent re-raise
  // between them was silently cleared. Returns true when a clear happened.
  bool clearIfActive(const char* code);
  // Mark as acknowledged (operator has seen it; alarm may still be active).
  void acknowledge(const char* code);
  void acknowledgeAll();

  // ---- Reads: every accessor takes the registry lock internally and
  // copies data out — none of them expose a pointer into the registry
  // (p.469). Each call is individually coherent; use snapshotInto() when
  // several fields must be mutually consistent.
  void snapshotInto(Snapshot& out) const;
  uint8_t countActive() const;
  uint8_t countAll() const;
  // [p.467] Copy-out find — the old `const Alarm*` return handed callers a
  // pointer that a concurrent eviction could invalidate (dangling read) and
  // whose fields could change mid-use. Returns true and fills `out` when
  // the code exists (a stable copy taken under the lock).
  bool find(const char* code, Alarm& out) const;
  Core::AlarmSeverity highestActiveSeverity() const;
  // Copy NON-CLEARED alarms (Active + Acknowledged) into a caller buffer
  // — [FW-23 REMEDIATION 2026-08] the old getActiveAlarms()/
  // getActiveAlarmCount() pair exposed the FULL array including CLEARED
  // entries; those raw-pointer accessors are now REMOVED (p.467).
  // Returns the number of entries copied. The copy loop runs under the
  // registry lock — a snapshot cannot be torn by a concurrent eviction.
  uint8_t copyActiveAlarms(Alarm* dst, uint8_t max) const;

  // [p.469] Snapshot-derived helpers — pure functions over a Snapshot the
  // caller already holds (no lock, no registry access). Envelope builders
  // that need severity AND the active list from ONE instant consume a single
  // snapshotInto() and derive both fields with these, instead of two
  // separately-locked instance calls that could straddle a mutation.
  static Core::AlarmSeverity highestSeverityIn(const Alarm* list, uint8_t count);
  static uint8_t copyActiveFrom(const Alarm* list, uint8_t count, Alarm* dst, uint8_t max);

  // [p.451] Registry saturation observability.
  uint32_t overflowCount() const;

  // [FW-23] Persistence — alarms survive reboot (versioned NVS blob + CRC).
  // raise()/clear()/acknowledge() set the dirty flag; persistenceTask
  // checkpoints dirty state; operator actions (clear/ack) save immediately.
  // [p.450] Severity-escalating raise() refreshes also save immediately.
  // [p.439-family] Returns false when the NVS write failed (begin/putBytes/
  // read-back) so the persistence task can raise STORAGE_ERROR — failure is
  // never silently "assumed persisted".
  // [p.453/p.455] ONE NVS record: {magic, version, generation, count, crc32,
  // alarms[]} — strict full-length write check + read-back verification.
  // [p.468] Public entry LOCKS the registry: the persistence-task
  // checkpoint and the immediate saves inside raise/clear/ack serialize
  // through the same mutex — two concurrent saveToNVS() calls can no longer
  // build+write blobs from different RAM snapshots (mixed-generation
  // persisted state).
  bool saveToNVS();
  void loadFromNVS();
  bool isDirty() const;
  uint32_t persistFailures() const;
  // [p.455] Generation of the last successfully written / loaded snapshot
  // (diagnostics: proves which transaction produced the persisted state).
  uint16_t generation() const;

private:
  // [p.467] ONE registry mutex. Created in begin() (setup(), before any
  // task exists — see firmware_v1.ino: alarms.begin() at setup time, task
  // creation afterwards); the lazy-create fallback in _lock() only serves
  // pre-scheduler single-threaded callers, same pattern as
  // AuthManager/LogService/BatteryCommManager. NOT recursive by design: the
  // Unlocked implementations must never be reachable while the lock is
  // held, making nested acquisition structurally impossible.
  mutable SemaphoreHandle_t _mutex = nullptr;

  Alarm  _alarms[MAX_ALARMS] = {};
  uint8_t _count = 0;
  bool    _dirty = false;
  uint32_t _overflowCount = 0;   // [p.451] rejected-by-saturation events (this boot)
  uint32_t _persistFailures = 0; // [p.439-family] failed NVS writes (this boot)
  uint16_t _generation = 0;      // [p.455] persisted-snapshot generation

  // ---- [p.467] lock discipline: public API locks, private *Unlocked runs
  // under it. Nothing below may take the mutex (deadlock-free by
  // construction). _findIdx is Unlocked-context only.
  void _lock() const;
  void _unlock() const;
  uint8_t _findIdx(const char* code) const;

  RaiseResult _raiseTrackedUnlocked(const char* code, Core::AlarmSeverity sev,
                                     const char* message);
  void _clearUnlocked(const char* code);
  bool _clearIfActiveUnlocked(const char* code);
  void _acknowledgeUnlocked(const char* code);
  void _acknowledgeAllUnlocked();
  bool _saveToNVSUnlocked();
  void _loadFromNVSUnlocked();
};

extern AlarmRegistry alarms;

} // namespace Services

#endif // PLTS_SERVICES_ALARM_REGISTRY_H
