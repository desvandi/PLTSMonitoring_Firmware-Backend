#!/usr/bin/env python3
"""
test_audit_round6_2026_09.py — [AUDIT 2026-09 ROUND 6] safety-grade measurement
eligibility, anomaly quality gates, alarm persistence atomicity, honest raise
semantics, and the emergency mutex-starvation watchdog.

Auditor round-6 findings this gate pins (p.453–p.466):
  p.453  AlarmRegistry save: two NVS records + `w2 != 0` short-write acceptance
  p.454  raise() reports success even when the immediate persist failed
  p.455  Alarm persistence has no transactional generation marker
  p.456  (E2E live acceptance — operator checklist, docs §12; not source-pinnable)
  p.457  EmergencySupervisor accepts Derived/Estimated as safety inputs
  p.458  AnomalyDetector has no quality gate for temperature/humidity/SOC
  p.459  Anomaly baselines updated from ineligible samples (phantom jumps)
  p.460  Numerical alarms stay ACTIVE when the sensor becomes not assessable
  p.461  TELEMETRY_STALE detects gaps only, never a stalled stream
  p.462-464  (verified CLOSED by the auditor — no gate needed)
  p.465  Mutex starvation can skip emergency evaluation unboundedly
  p.466  isValid() is too generic as a safety predicate (API-level hardening)

What this gate enforces (static, on every push):
  A. Measurement::isSafetyUsable() exists (Valid + Measured + finite) and the
     emergency _readSensors() uses it on all three safety inputs; isValid()
     keeps its dashboard contract (Derived/Estimated accepted).
  B. AnomalyContext carries per-quantity quality (temperatureQ/humidityQ/socQ);
     the call site populates them; T/H evaluations are Valid-gated; SOC is
     "known"-gated (Estimated eligible — the normal shunt path).
  C. Anomaly baselines only update from ELIGIBLE samples, carry their own
     timestamps (rate = Δ/elapsed-between-eligible-samples); the stuck window
     resets across a quality break.
  D. Not-assessable quality clears the numerical alarms (clear-if-active);
     raise() REACTIVATES a cleared entry (prerequisite fix — recurring
     conditions were invisible after the first clear).
  E. TELEMETRY_STALE: gap raises; consecutive +1 clears; the measurement task
     raises the STALLED variant on its queue-timeout path.
  F. AlarmRegistry persistence is ONE NVS record (magic+version+generation+
     count+CRC), strict full-length write check, read-back verification, and
     a one-time legacy two-record migration (read-only legacy path).
  G. RaiseResult distinguishes Rejected / AcceptedRam / AcceptedPersisted /
     AcceptedPersistFailed; bool raise() documented as NOT a durability claim;
     persist failures are logged as StorageError.
  H. The emergency tick() mutex-starvation watchdog is bounded (consecutive
     starved ticks → force relay ISOLATED without the mutex → reconcile into
     a latched INTERNAL trip on the next locked cycle).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

results = []


def check(name: str, cond: bool, detail: str = "") -> None:
    results.append((name, cond, detail))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail and not cond else ""))


def read(p: Path) -> str:
    return p.read_text(errors="replace")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


print("=" * 78)
print("AUDIT ROUND 6 — safety eligibility / anomaly gates / alarm atomicity")
print("=" * 78)

types_h = read(FW / "Core/Types.h")
emg_h = read(FW / "Services/EmergencySupervisor.h")
emg_c = read(FW / "Services/EmergencySupervisor.cpp")
anom_h = read(FW / "Services/AnomalyDetector.h")
anom_c = read(FW / "Services/AnomalyDetector.cpp")
alarm_c = read(FW / "Services/AlarmRegistry.cpp")
alarm_h = read(FW / "Services/AlarmRegistry.h")
ino = read(FW / "firmware_v1.ino")

types_code = strip_comments(types_h)
emg_code = strip_comments(emg_c)
anom_code = strip_comments(anom_c)
anom_h_code = strip_comments(anom_h)
alarm_code = strip_comments(alarm_c)
alarm_h_code = strip_comments(alarm_h)
ino_code = strip_comments(ino)

# ---------------------------------------------------------------------------
print("\n[A] Safety-grade measurement eligibility (p.457 + p.466)")

check("A1. Measurement::isSafetyUsable() defined: Valid quality AND Measured source AND isfinite",
      "bool isSafetyUsable() const" in types_h and
      bool(re.search(r"isSafetyUsable\(\) const \{\s*return quality == MeasurementQuality::Valid\s*"
                     r"&& source\s*== MeasurementSource::Measured\s*&& std::isfinite\(value\);", types_code)))

check("A2. isValid() still accepts Derived/Estimated (dashboard contract preserved)",
      bool(re.search(r"isValid\(\) const \{\s*return \(quality == MeasurementQuality::Valid \|\|\s*"
                     r"quality == MeasurementQuality::Derived \|\|\s*quality == MeasurementQuality::Estimated\)",
                     types_code, re.S)))

read_sensors = emg_code.split("EmgSensors EmergencySupervisor::_readSensors()")[1].split("return s;")[0]
check("A3. Emergency _readSensors uses isSafetyUsable() on vbat/idc/iac",
      "isSafetyUsable()" in read_sensors and read_sensors.count("isValid()") == 0)

check("A4. No isValid() remains in EmergencySupervisor safety paths",
      "isValid()" not in emg_code)

# ---------------------------------------------------------------------------
print("\n[B] Per-quantity quality gates in AnomalyDetector (p.458)")

check("B1. AnomalyContext carries temperatureQ / humidityQ / socQ",
      all(f in anom_h for f in ("temperatureQ", "humidityQ", "socQ")))

check("B2. Call site populates the new quality fields",
      all(f"actx.{f}" in ino for f in ("temperatureQ", "humidityQ", "socQ")) and
      "getSocQuality()" in ino)

check("B3. T/H evaluations gated on envEligible() (Valid-only)",
      "envEligible(ctx.temperatureQ)" in anom_code and "envEligible(ctx.humidityQ)" in anom_code)

soc_fn = anom_h_code.split("static bool socEligible")[1].split("}")[0]
check("B4. SOC gate is 'known' (Estimated eligible — normal shunt path not silenced)",
      all(q in soc_fn for q in ("NotAvailable", "SensorError", "Invalid", "OutOfRange")) and
      "Estimated" not in soc_fn and
      "socEligible(ctx.socQ)" in anom_code)

# ---------------------------------------------------------------------------
print("\n[C] Baseline eligibility (p.459)")

check("C1. No unconditional end-of-tick baseline assignment block (the p.459 poison path)",
      not bool(re.search(r"_lastVoltage = ctx\.voltage;\s*_lastCurrent = ctx\.current;\s*"
                         r"_lastTemp = ctx\.temperatureC;", anom_code)))

check("C2. Baselines updated ONLY inside eligible branches (count == priming + eligible branch)",
      anom_code.count("_lastVoltage = ctx.voltage;") == 2 and
      anom_code.count("_lastCurrent = ctx.current;") == 2 and
      anom_code.count("_lastTemp = ctx.temperatureC;") == 2)

check("C3. Per-quantity baseline timestamps exist and feed the rate detectors",
      all(v in anom_h for v in ("_lastVoltageSec", "_lastCurrentSec", "_lastTempSec", "_lastSocSec")) and
      "nowSec - _lastVoltageSec" in anom_code and "nowSec - _lastCurrentSec" in anom_code)

check("C4. Stuck window resets on a quality break",
      bool(re.search(r"_stuckIdx = 0;\s*_stuckFull = false;", anom_code)))

# ---------------------------------------------------------------------------
print("\n[D] Honest not-assessable semantics (p.460 + reactivation prerequisite)")

check("D1. clear-if-active helper exists and is used for numerical suspensions",
      "_clearIfActive" in anom_h and anom_code.count("_clearIfActive(") >= 10)

clears = anom_code
for code in ("BATTERY_VOLTAGE_LOW", "BATTERY_VOLTAGE_HIGH", "BATTERY_OVERCURRENT_CHARGE",
             "BATTERY_OVERCURRENT_DISCHARGE", "BATTERY_CURRENT_SUSPECT"):
    if f"_clearIfActive(Core::AlarmCode::{code})" not in clears and f'_clearIfActive("{code}")' not in clears:
        clears = ""
        break
check("D2. V/I numerical alarms cleared when not assessable", bool(clears))

check("D3. T/H/SOC numerical alarms cleared when not assessable",
      "_clearIfActive(Core::AlarmCode::TEMPERATURE_HIGH)" in anom_code and
      "_clearIfActive(Core::AlarmCode::HUMIDITY_HIGH)" in anom_code and
      "_clearIfActive(Core::AlarmCode::BATTERY_SOC_LOW)" in anom_code)

check("D4. raise() REACTIVATES a CLEARED entry (lifecycle → Active, clearedAt reset)",
      bool(re.search(r"a\.lifecycle == Core::AlarmLifecycle::Cleared\) \{[^}]*"
                     r"a\.lifecycle = Core::AlarmLifecycle::Active;[^}]*a\.clearedAt = 0;",
                     alarm_code, re.S)))

# ---------------------------------------------------------------------------
print("\n[E] TELEMETRY_STALE semantics (p.461)")

check("E1. Gap still raises; consecutive +1 clears honestly",
      bool(re.search(r"ctx\.telemetrySeq > _lastTelemetrySeq \+ 1\) \{[^}]*TELEMETRY_STALE", anom_code)) and
      bool(re.search(r"ctx\.telemetrySeq == _lastTelemetrySeq \+ 1\) \{\s*_clearIfActive\(\"TELEMETRY_STALE\"\)",
                     anom_code)))

check("E2. Stalled-stream detection lives on the measurement queue-timeout path",
      "MEAS_STALL_ALARM_MS" in ino and "lastSampleMonotonicMs" in ino and
      "sensor pipeline stalled" in ino)

# ---------------------------------------------------------------------------
print("\n[F] Single-record alarm persistence (p.453 + p.455)")

save_fn = alarm_code.split("bool AlarmRegistry::saveToNVS()")[1].split("void AlarmRegistry::loadFromNVS")[0]
check("F1. saveToNVS writes ONE record ('state') with a STRICT full-length check",
      save_fn.count('putBytes("state"') == 1 and "w != blobLen" in save_fn and
      'putBytes("hdr"' not in alarm_code and 'putBytes("arr"' not in alarm_code)

check("F2. AlarmStateBlob: magic + version + generation + count + crc32, packed 16-byte header",
      "struct AlarmStateBlob" in alarm_c and "uint16_t generation" in alarm_c and
      'static_assert(sizeof(AlarmStateBlob) == 16' in alarm_c)

check("F3. Read-back verification (getBytes + memcmp)",
      "memcmp(rb, blob, blobLen)" in save_fn)

check("F4. Generation committed only after a verified write; load adopts it",
      "_generation = (uint16_t)(_generation + 1);" in save_fn.split("memcmp")[1] and
      "_generation = h.generation;" in alarm_code)

# [ROUND-7 UPDATE 2026-09-14] The migration write runs as _saveToNVSUnlocked()
# — loadFromNVS() already holds the registry lock (p.467/p.468); calling the
# public saveToNVS() there would double-lock the non-recursive mutex. The
# one-time-migration behavior is unchanged.
check("F5. Legacy two-record layout is READ-ONLY (migration) and triggers a one-time v2 save",
      'getBytes("hdr"' in alarm_code and 'getBytes("arr"' in alarm_code and
      bool(re.search(r"_saveToNVSUnlocked\(\);.*?one-time migration", alarm_c, re.S)))

# ---------------------------------------------------------------------------
print("\n[G] Honest raise contract (p.454)")

check("G1. RaiseResult enum with the four durability outcomes",
      "enum class RaiseResult" in alarm_h and
      all(v in alarm_h for v in ("Rejected", "AcceptedRam", "AcceptedPersisted", "AcceptedPersistFailed")))

check("G2. raiseTracked() implemented; bool raise() is a documented thin wrapper",
      "RaiseResult AlarmRegistry::raiseTracked(" in alarm_c and
      "!= RaiseResult::Rejected;" in alarm_c and
      "NOT a durability" in alarm_h)

check("G3. Immediate-persist failure logged as StorageError (never silent)",
      alarm_c.count("accepted in RAM but the immediate") == 2)

# ---------------------------------------------------------------------------
print("\n[I] Self-review hardening (post-merge verification 2026-09-14)")

check("I1. Temp rate-detector uses the DEDICATED code (no raise+clear collision)",
      "TEMPERATURE_RAPID_RISE" in types_h and
      'alarms.raise(Core::AlarmCode::TEMPERATURE_RAPID_RISE' in anom_code and
      'alarms.clear(Core::AlarmCode::TEMPERATURE_RAPID_RISE' not in anom_code,
      "the old code raised TEMPERATURE_HIGH for a rapid rise and the level block "
      "cleared it in the same tick below warn-hyst — the early warning was invisible")

check("I2. Not-assessable path also clears the dedicated rate code (symmetry)",
      "_clearIfActive(Core::AlarmCode::TEMPERATURE_RAPID_RISE)" in anom_code)

check("I3. V/I baselines update inside the isfinite guard (NaN cannot poison)",
      bool(re.search(r"isfinite\(ctx\.voltage\)\) \{[^}]*_lastVoltage = ctx\.voltage;",
                     anom_code, re.S)) and
      bool(re.search(r"isfinite\(ctx\.current\)\) \{[^}]*_lastCurrent = ctx\.current;",
                     anom_code, re.S)))

check("I4. Temperature priming stamps its baseline timestamp",
      bool(re.search(r"_lastTemp = ctx\.temperatureC; _lastTempSec = nowSec;", anom_code)))

# ---------------------------------------------------------------------------
print("\n[H] Mutex-starvation watchdog (p.465)")

tick_fn = emg_code.split("void EmergencySupervisor::tick()")[1].split("EmgSensors EmergencySupervisor::_readSensors")[0]

check("H1. Consecutive-starvation counter with a bounded limit",
      "_lockStarveCycles++" in tick_fn and "EMG_LOCK_STARVE_LIMIT" in emg_h and
      "EMG_LOCK_STARVE_LIMIT   = 20" in emg_h)

check("H2. Watchdog forces the relay ISOLATED WITHOUT taking the mutex",
      bool(re.search(r"_lockStarveCycles >= EMG_LOCK_STARVE_LIMIT && !_lockStarveEscalated\) \{[^}]*"
                     r"setEnergized\(false\);", tick_fn, re.S)))

check("H3. Reconcile on the next locked cycle converts to a latched INTERNAL trip",
      "EMG_REASON_INTERNAL" in emg_c and "EMG_REASON_INTERNAL" in emg_h and
      bool(re.search(r"if \(_lockStarveEscalated\) \{[^}]*if \(_state == EmgState::Run\) \{[^}]*"
                     r"_trip\(EMG_REASON_INTERNAL", tick_fn, re.S)))

check("H4. Watchdog state resets when the lock is acquired (transient contention tolerated)",
      bool(re.search(r"_lockStarveCycles = 0;\s*if \(_lockStarveEscalated\)", tick_fn)))

# ---------------------------------------------------------------------------
total = len(results)
bad = [r for r in results if not r[1]]
print("\n" + "=" * 78)
print(f"ROUND 6 GATE: {total - len(bad)}/{total} checks passed"
      + ("" if not bad else " — FAILURES:"))
for name, _, _ in bad:
    print(f"  ✗ {name}")
sys.exit(1 if bad else 0)
