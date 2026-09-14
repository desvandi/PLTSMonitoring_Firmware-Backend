#!/usr/bin/env python3
"""
test_audit_round7_2026_09.py — [AUDIT 2026-09 ROUND 7] AlarmRegistry
cross-task serialization, persistence race, and coherent reader snapshots.

Auditor round-7 findings this gate pins (p.467–p.469):
  p.467  P1 — AlarmRegistry is global mutable state mutated from many
         execution contexts (measurement/anomaly/health/energy/network/
         web/relay/emergency/MQTT) with no internal serialization:
         concurrent raise() both see _findIdx()==0xFF → double-append /
         lost alarm; clear() vs eviction shifts the array under a writer.
  p.468  P1 — saveToNVS() is reachable from every immediate-save path AND
         the persistence-task checkpoint with no serialization of the
         RAM-snapshot → blob → write → read-back transaction
         (mixed-generation persisted state).
  p.469  P2 — Reader sequences (countAll()+getAlarm() loop,
         highestActiveSeverity()+copyActiveAlarms()) are multi-call reads
         with no coherent snapshot; interior pointers (find()/getAlarm())
         hand callers references a concurrent eviction can invalidate.

What this gate enforces (static, on every push):
  A. Serialization lives INSIDE the registry (auditor-rejected alternative:
     caller-side mutexes — call sites too scattered to keep correct):
     one mutable FreeRTOS mutex, created in begin() (pre-scheduler),
     lazy-create fallback like AuthManager/LogService.
  B. Every public mutator is a LOCKED wrapper delegating to a private
     *Unlocked() implementation; no Unlocked implementation takes the lock
     (deadlock structurally impossible — non-recursive mutex, single
     nesting level). The immediate NVS saves run INSIDE the lock: RAM
     mutation + blob build + write + read-back is one serialized
     transaction (p.468).
  C. Reads copy data out under the lock: Snapshot + snapshotInto(); find()
     is copy-out; getAlarm()/getActiveAlarms()/getActiveAlarmCount()
     raw-pointer accessors are REMOVED; REST GET /api/alarms and the
     telemetry envelope consume ONE snapshot each (p.469).
  D. Atomic clearIfActive() for evaluator tick loops (find→check→clear
     TOCTOU closed); AnomalyDetector delegates to it.
  E. The native concurrency harness exists, runs REAL threads
     (writers/operator/persistence/readers), is built with TSAN AND
     ASAN/UBSAN in CI, and carries a REGISTRY_NO_LOCK negative control
     that the runner REQUIRES to produce TSAN reports (blindness check).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
NATIVE = ROOT / "scripts" / "native"

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


def body_of(src: str, signature: str, end_markers) -> str:
    """Extract a function body from `signature` up to the first of end_markers."""
    i = src.find(signature)
    if i < 0:
        return ""
    rest = src[i:]
    for m in end_markers:
        j = rest.find(m, len(signature))
        if j > 0:
            return rest[:j]
    return rest


def brace_body(src: str, signature: str) -> str:
    """Balanced-brace extraction of the function starting at `signature`."""
    i = src.find(signature)
    if i < 0:
        return ""
    j = src.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
    return src[i:]


print("=" * 78)
print("AUDIT ROUND 7 — AlarmRegistry serialization / persistence race / snapshots")
print("=" * 78)

alarm_h = read(FW / "Services/AlarmRegistry.h")
alarm_c = read(FW / "Services/AlarmRegistry.cpp")
alarm_code = strip_comments(alarm_c)
alarm_hdr = strip_comments(alarm_h)
handlers_c = read(FW / "Web/AlarmHandlers.cpp")
handlers_code = strip_comments(handlers_c)
mqtt_c = read(FW / "Network/MqttConfigReceiver.cpp")
anom_c = read(FW / "Services/AnomalyDetector.cpp")
ino = read(FW / "firmware_v1.ino")
ino_code = strip_comments(ino)
runner = read(NATIVE / "run-native-tests.sh")
harness = read(NATIVE / "verify_alarm_concurrency.cpp")

# ---------------------------------------------------------------------------
print("\n[A] Serialization lives INSIDE the registry (p.467)")

check("A1. Internal FreeRTOS mutex member (mutable, lazy-init pattern)",
      "mutable SemaphoreHandle_t _mutex = nullptr;" in alarm_h and
      "#include <freertos/FreeRTOS.h>" in alarm_h and
      "#include <freertos/semphr.h>" in alarm_h)

check("A2. Mutex created in begin() BEFORE loadFromNVS (pre-scheduler creation)",
      bool(re.search(r"void AlarmRegistry::begin\(\) \{[^}]*"
                     r"if \(_mutex == nullptr\) _mutex = xSemaphoreCreateMutex\(\);",
                     alarm_code, re.S)))

# [ROUND-9 UPDATE 2026-09-15 / p.471] _lock() is now FAIL-CLOSED and
# returns bool: it retries creation once and returns false (operation
# refused) when the mutex is unavailable — the round-7 "if (_mutex) take"
# fail-open shape is gone. begin() gained a FATAL boot guard. The gate pins
# the NEW shape; the serialization semantics it has always protected are
# unchanged (and are re-pinned harder by test_audit_round9_2026_09.py).
check("A3. _lock() fail-closed (p.471): bool return, retry-create, reject + CRIT log on failure",
      bool(re.search(r"bool AlarmRegistry::_lock\(\) const \{[^}]*"
                     r"if \(_mutex == nullptr\) _mutex = xSemaphoreCreateMutex\(\);[^}]*"
                     r"xSemaphoreTake\(_mutex, portMAX_DELAY\);[^}]*return true;",
                     alarm_code, re.S)) and
      "return false;" in body_of(alarm_code, "bool AlarmRegistry::_lock() const",
                                 ["void AlarmRegistry::_unlock"]) and
      "_lockFailures.fetch_add" in alarm_code and
      "if (_mutex)" not in body_of(alarm_code, "bool AlarmRegistry::_lock() const",
                                   ["void AlarmRegistry::_unlock"]))

check("A3b. begin() boot guard (p.471): mutex creation failure is FATAL pre-multi-task",
      bool(re.search(r"void AlarmRegistry::begin\(\) \{[^}]*"
                     r"if \(_mutex == nullptr\) _mutex = xSemaphoreCreateMutex\(\);[^}]*"
                     r"if \(_mutex == nullptr\) \{[^}]*FATAL[^}]*while \(true\)",
                     alarm_code, re.S)))

check("A4. begin() precedes every xTaskCreate in setup() (mutex exists before tasks)",
      ino.find("alarms.begin()") < min(
          m.start() for m in re.finditer(r"xTaskCreatePinnedToCore", ino)))

# ---------------------------------------------------------------------------
print("\n[B] Locked public wrappers → private *Unlocked implementations (p.467/p.468)")

MUTATORS = [
    ("raiseTracked", "RaiseResult AlarmRegistry::raiseTracked(",
     "_raiseTrackedUnlocked(code, sev, message)"),
    ("clear", "void AlarmRegistry::clear(", "_clearUnlocked(code)"),
    ("clearIfActive", "bool AlarmRegistry::clearIfActive(", "_clearIfActiveUnlocked(code)"),
    ("acknowledge", "void AlarmRegistry::acknowledge(", "_acknowledgeUnlocked(code)"),
    ("acknowledgeAll", "void AlarmRegistry::acknowledgeAll()", "_acknowledgeAllUnlocked()"),
    ("saveToNVS", "bool AlarmRegistry::saveToNVS()", "_saveToNVSUnlocked()"),
    ("loadFromNVS", "void AlarmRegistry::loadFromNVS()", "_loadFromNVSUnlocked()"),
]
all_wrapped = True
for name, sig, call in MUTATORS:
    body = body_of(alarm_code, sig, ["\nRaiseResult AlarmRegistry::_", "\nvoid AlarmRegistry::_",
                                     "\nbool AlarmRegistry::_", "\nuint8_t AlarmRegistry::",
                                     "\nvoid AlarmRegistry::raise", "\nbool AlarmRegistry::raise",
                                     "} // namespace"])
    # [ROUND-9 UPDATE 2026-09-15 / p.471] wrappers now GUARD the lock result
    # before delegating ("if (!_lock()) return <fail-closed value>;") — the
    # locked-wrapper semantics are unchanged, the guard is what p.471 adds.
    ok = "if (!_lock())" in body and "_unlock();" in body and call in body and \
         body.find("if (!_lock())") < body.find(call)
    if not ok:
        all_wrapped = False
        print(f"      (mutator {name}: wrapper missing lock guard/unlock or delegation)")
check("B1. Every public mutator: if (!_lock()) refuse → *Unlocked() → _unlock() (p.467/p.471)", all_wrapped)

unlocked_bodies = []
for sig in ["AlarmRegistry::_raiseTrackedUnlocked(", "AlarmRegistry::_clearUnlocked(",
            "AlarmRegistry::_clearIfActiveUnlocked(", "AlarmRegistry::_acknowledgeUnlocked(",
            "AlarmRegistry::_acknowledgeAllUnlocked()", "AlarmRegistry::_saveToNVSUnlocked()",
            "AlarmRegistry::_loadFromNVSUnlocked()"]:
    unlocked_bodies.append((sig, brace_body(alarm_code, sig)))
check("B2. No *Unlocked implementation ever takes the registry lock (deadlock-free by construction)",
      all(b for _, b in unlocked_bodies) and
      all("_lock();" not in b for _, b in unlocked_bodies) and len(unlocked_bodies) == 7)

check("B3. Immediate saves INSIDE the lock call the Unlocked form (p.468 transaction)",
      alarm_code.count("_saveToNVSUnlocked();") >= 6 and
      "bool persisted = _saveToNVSUnlocked();" in alarm_code)

check("B4. Legacy migration save runs Unlocked (already under the load lock)",
      bool(re.search(r"atomic record\", -1\);\s*_saveToNVSUnlocked\(\);", alarm_code, re.S)))

check("B5. Persistence-task checkpoint goes through the PUBLIC locked entries",
      bool(re.search(r"if \(Services::alarms\.isDirty\(\)\) \{\s*"
                     r"if \(!Services::alarms\.saveToNVS\(\)", ino_code)))

check("B6. The registry log-rate-limit statics are only touched under the lock",
      bool(re.search(r"_overflowCount\+\+;\s*static uint32_t lastOverflowLogMs = 0;",
                     alarm_code)) and
      "lastOverflowLogMs" not in body_of(alarm_code, "bool AlarmRegistry::saveToNVS()",
                                         ["} // namespace"]))

# ---------------------------------------------------------------------------
print("\n[C] Coherent read snapshots — no interior pointers, no multi-call reads (p.469)")

check("C1. Snapshot struct + snapshotInto() exist with the full state copy",
      "struct Snapshot {" in alarm_h and "void snapshotInto(Snapshot& out) const;" in alarm_h and
      bool(re.search(r"void AlarmRegistry::snapshotInto\(Snapshot& out\) const \{[^}]*if \(!_lock\(\)\)",
                     alarm_code, re.S)) and
      "out = Snapshot{};" in alarm_code)

check("C2. Raw-pointer accessors REMOVED (getAlarm / getActiveAlarms / getActiveAlarmCount)",
      "getAlarm(" not in alarm_hdr and "getActiveAlarms" not in alarm_hdr and
      "getActiveAlarmCount" not in alarm_hdr)

check("C3. find() is copy-out (bool + out-param), never a pointer into the registry",
      "bool find(const char* code, Alarm& out) const;" in alarm_h and
      "const Alarm* find(" not in alarm_h)

check("C4. REST GET /api/alarms consumes ONE snapshot (no countAll()+getAlarm() loop)",
      "snapshotInto(*snap)" in handlers_code and
      "countAll()" not in handlers_code and "getAlarm(" not in handlers_code and
      "malloc(sizeof(Services::AlarmRegistry::Snapshot))" in handlers_code)

check("C5. Telemetry envelope derives severity + active list from ONE snapshot",
      "Services::alarms.snapshotInto(s_alarmSnap);" in ino_code and
      bool(re.search(r"latestStatus\.health\.highestAlarmSeverity =\s*"
                     r"Services::AlarmRegistry::highestSeverityIn\(s_alarmSnap\.alarms, s_alarmSnap\.count\);",
                     ino_code)) and
      "copyActiveFrom(\n        s_alarmSnap.alarms, s_alarmSnap.count," in ino_code)

check("C6. copyActiveAlarms() locks internally (single coherent copy)",
      bool(re.search(r"uint8_t AlarmRegistry::copyActiveAlarms\(Alarm\* dst, uint8_t max\) const \{"
                     r"\s*if \(!_lock\(\)\) return 0;", alarm_code)))

check("C7. MQTT + Web ack paths use the copy-out find()",
      "Services::alarms.find(code, a)" in strip_comments(mqtt_c) and
      "Services::alarms.find(code.c_str(), a)" in handlers_code)

# ---------------------------------------------------------------------------
print("\n[D] Atomic clearIfActive for evaluator tick loops (p.467 TOCTOU)")

check("D1. clearIfActive() is a single locked test-and-clear",
      bool(re.search(r"bool AlarmRegistry::clearIfActive\(const char\* code\) \{\s*"
                     r"if \(!_lock\(\)\) return false;", alarm_code)) and
      "_clearIfActiveUnlocked(code);" in alarm_code and
      bool(re.search(r"AlarmRegistry::_clearIfActiveUnlocked\(const char\* code\) \{[^}]*"
                     r"if \(a\.lifecycle == Core::AlarmLifecycle::Cleared\) return false;",
                     alarm_code, re.S)))

check("D2. AnomalyDetector._clearIfActive delegates to the atomic registry primitive",
      bool(re.search(r"bool AnomalyDetector::_clearIfActive\(const char\* code\) \{\s*"
                     r"return alarms\.clearIfActive\(code\);\s*\}",
                     strip_comments(anom_c))))

# ---------------------------------------------------------------------------
print("\n[E] Native concurrency harness — real interleaving, sensitivity proven")

check("E1. Harness runs REAL threads: 2 writers + operator + persistence + 2 readers + watchdog",
      all(s in harness for s in
          ["std::thread wA(writerThread", "std::thread wB(writerThread",
           "std::thread op(operatorThread", "std::thread per(persistenceThread)",
           "std::thread rA(readerThread", "std::thread rB(readerThread",
           "std::thread wd(watchdog)"]))

check("E2. Persistence thread mirrors the firmware checkpoint loop (isDirty → saveToNVS)",
      bool(re.search(r"if \(Services::alarms\.isDirty\(\)\) \{\s*"
                     r"bool ok = Services::alarms\.saveToNVS\(\);", harness)))

check("E3. Readers validate snapshot coherence invariants (count/dupes/termination/monotonic)",
      all(s in harness for s in
          ["No duplicate codes among", "code is terminated and non-empty",
           "generation monotonic per reader", "overflowCount monotonic"]))

check("E4. Negative control compiles the ROUND-6 unlocked registry (REGISTRY_NO_LOCK)",
      "#ifdef REGISTRY_NO_LOCK" in harness and
      "bool _lock() const { return true; }" in harness and
      "#elif defined(LOCK_FAIL_OPEN)" in harness)

check("E5. Runner builds TSAN + ASAN/UBSAN treatment AND requires BOTH negative controls to FAIL",
      "-fsanitize=thread" in runner and "-DREGISTRY_NO_LOCK" in runner and
      "-DLOCK_FAIL_OPEN" in runner and
      "NEGATIVE CONTROL FAILED" in runner and "harness sensitivity proven" in runner and
      "P471 NEGATIVE CONTROL TRIPPED" in runner)

check("E6. Saturation/rejection + transient-NV-failure retry are exercised in-harness",
      "failNextWrite" in harness and "AcceptedPersistFailed" in harness and
      "checkpoint retried the failed save while idle" in harness)

# ---------------------------------------------------------------------------
print("\n[F] Round-6 invariants preserved through the refactor (no regression)")

save_fn = alarm_code.split("bool AlarmRegistry::saveToNVS()")[1].split("void AlarmRegistry::loadFromNVS")[0]
check("F1. Single-record persistence + strict length + read-back still intact",
      save_fn.count('putBytes("state"') == 1 and "w != blobLen" in save_fn and
      "memcmp(rb, blob, blobLen)" in save_fn and
      "_generation = (uint16_t)(_generation + 1);" in save_fn.split("memcmp")[1])

check("F2. Eviction policy untouched (CLEARED-only, tie-break) inside the Unlocked body",
      "AlarmLifecycle::Cleared" in alarm_code.split("_count >= MAX_ALARMS")[1].split("} else {")[0])

check("F3. Honest saturation rejection + append-slot assignment still present",
      "_overflowCount++" in alarm_code and
      "return RaiseResult::Rejected;" in alarm_code and
      "if (idx == 0xFF) idx = _count;" in alarm_code and
      "if (idx == _count) _count++;" in alarm_code)

check("F4. Reactivation path still present (raise-on-cleared → Active)",
      bool(re.search(r"a\.lifecycle == Core::AlarmLifecycle::Cleared\) \{[^}]*"
                     r"a\.lifecycle = Core::AlarmLifecycle::Active;[^}]*a\.clearedAt = 0;",
                     alarm_code, re.S)))

# ---------------------------------------------------------------------------
print()
failed = [r for r in results if not r[1]]
print(f"RESULT: {len(results) - len(failed)}/{len(results)} checks passed")
if failed:
    print("FAILED CHECKS:")
    for name, _, detail in failed:
        print(f"  - {name}" + (f" — {detail}" if detail else ""))
    sys.exit(1)
print("AUDIT ROUND 7 GATE: ALL CHECKS PASSED")
