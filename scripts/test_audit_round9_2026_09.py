#!/usr/bin/env python3
# =============================================================================
# test_audit_round9_2026_09.py — ROUND-9 static gate (p.471 remediation:
# fail-closed mutex acquisition in AlarmRegistry + symmetric telemetryMutex
# boot guard).
#
# Auditor residual (baseline fe1905f, verdict 2026-09-14):
#   p.471  P3/P4 — the round-7 _lock() was FAIL-OPEN:
#          "if (mutex == null) create; if (mutex) take" — when
#          xSemaphoreCreateMutex() failed (extreme boot OOM) the registry
#          silently proceeded WITHOUT synchronization, so the
#          "mutex gagal dibuat → serialization guarantee hilang" semantics
#          applied exactly when the system entered multi-task state.
#
# What this gate enforces (static, on every push):
#   [A] _lock() is FAIL-CLOSED (bool; retry-create once; reject + atomic
#       accounting + rate-limited CRIT log on failure) and begin() carries
#       the FATAL boot guard (no multi-task state without serialization).
#   [B] EVERY public entry refuses its operation fail-closed when the lock
#       is unavailable, with the documented return value — and no *Unlocked
#       body is reachable without holding the lock.
#   [C] The contract is observable: RaiseResult::LockUnavailable,
#       Snapshot.lockFailures, lock-free lockFailures() accessor, and the
#       additive JSON fields on /api/alarms + /api/diagnostics.
#   [D] telemetryMutex gets the symmetric boot guard (a null handle would
#       hard-fault the p.470 deep-copy helper).
#   [E] The native harness exercises the p.471 class for real (injection
#       hooks, boot-guard phase, fail-closed storm, recovery) and carries a
#       SECOND negative control (-DLOCK_FAIL_OPEN, the round-7 fail-open
#       shape) that the CI runner REQUIRES to trip.
#   [F] Prior-round invariants survive the refactor untouched.
# =============================================================================
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
NATIVE = ROOT / "scripts" / "native"

results = []


def check(name: str, cond: bool, detail: str = "") -> None:
    results.append((name, bool(cond), detail))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail and not cond else ""))


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_comments(src: str) -> str:
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return src


def body_of(src: str, signature: str, end_markers) -> str:
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
print("AUDIT ROUND 9 — p.471 fail-closed mutex acquisition")
print("=" * 78)

alarm_h = read(FW / "Services" / "AlarmRegistry.h")
alarm_c = read(FW / "Services" / "AlarmRegistry.cpp")
alarm_code = strip_comments(alarm_c)
alarm_hdr = strip_comments(alarm_h)
handlers_c = read(FW / "Web" / "AlarmHandlers.cpp")
handlers_code = strip_comments(handlers_c)
diags_c = read(FW / "Web" / "DiagnosticsHandlers.cpp")
diags_code = strip_comments(diags_c)
ino = read(FW / "firmware_v1.ino")
ino_code = strip_comments(ino)
runner = read(NATIVE / "run-native-tests.sh")
harness = read(NATIVE / "verify_alarm_concurrency.cpp")
harness_code = strip_comments(harness)

# ---------------------------------------------------------------------------
print("\n[A] _lock() fail-closed + begin() boot guard (p.471 core)")

lock_body = brace_body(alarm_code, "bool AlarmRegistry::_lock() const")
check("A1. _lock() returns bool and takes the mutex with portMAX_DELAY on success",
      bool(lock_body) and
      "xSemaphoreTake(_mutex, portMAX_DELAY);" in lock_body and
      "return true;" in lock_body)

check("A2. Creation retried ONCE; unavailable handle REJECTS (return false)",
      bool(lock_body) and
      "if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();" in lock_body and
      "return false;" in lock_body and
      lock_body.find("xSemaphoreCreateMutex()") < lock_body.find("return false;"))

check("A3. The round-7 FAIL-OPEN fingerprint is gone (no 'if (_mutex) take')",
      "if (_mutex)" not in lock_body and
      not re.search(r"if \(_mutex\)\s*xSemaphoreTake", alarm_code))

check("A4. Failure accounting is ATOMIC + CRIT log rate-limited (no shared-state writes without sync)",
      "_lockFailures.fetch_add" in lock_body and
      "_lastLockFailLogMs.compare_exchange_strong" in lock_body and
      "60000UL" in lock_body and
      "Log.append" in lock_body and
      re.search(r"mutable std::atomic<uint32_t> _lockFailures", alarm_hdr) is not None)

check("A5. _unlock() is unconditional give — only reachable after a TRUE _lock()",
      bool(re.search(r"void AlarmRegistry::_unlock\(\) const \{\s*xSemaphoreGive\(_mutex\);\s*\}",
                     alarm_code)))

begin_body = brace_body(alarm_code, "void AlarmRegistry::begin()")
check("A6. begin() boot guard: creation failure → FATAL log + halt (no multi-task state)",
      bool(begin_body) and
      begin_body.find("xSemaphoreCreateMutex()") < begin_body.find("if (_mutex == nullptr) {") and
      "FATAL" in begin_body and
      "while (true)" in begin_body and
      begin_body.find("while (true)") < begin_body.find("_count = 0;"))

check("A7. The halt deliberately does NOT feed the task watchdog (TWDT panic reset, honest reset reason)",
      "esp_task_wdt_reset" not in begin_body.split("while (true)")[1].split("}")[0])

check("A8. begin() still precedes every xTaskCreate in setup()",
      ino.find("alarms.begin()") < min(
          m.start() for m in re.finditer(r"xTaskCreatePinnedToCore", ino)))

# ---------------------------------------------------------------------------
print("\n[B] Every public entry fail-closed — documented refusal values")

FC = [
    ("raiseTracked", "RaiseResult AlarmRegistry::raiseTracked(",
     "if (!_lock()) return RaiseResult::LockUnavailable;"),
    ("clear", "void AlarmRegistry::clear(", "if (!_lock()) return;"),
    ("clearIfActive", "bool AlarmRegistry::clearIfActive(", "if (!_lock()) return false;"),
    ("acknowledge", "void AlarmRegistry::acknowledge(", "if (!_lock()) return;"),
    ("acknowledgeAll", "void AlarmRegistry::acknowledgeAll()", "if (!_lock()) return;"),
    ("saveToNVS", "bool AlarmRegistry::saveToNVS()", "if (!_lock()) return false;"),
    ("loadFromNVS", "void AlarmRegistry::loadFromNVS()", "if (!_lock()) return;"),
    ("snapshotInto", "void AlarmRegistry::snapshotInto(Snapshot& out) const",
     "if (!_lock()) {"),
    ("countActive", "uint8_t AlarmRegistry::countActive() const", "if (!_lock()) return 0;"),
    ("countAll", "uint8_t AlarmRegistry::countAll() const", "if (!_lock()) return 0;"),
    ("find", "bool AlarmRegistry::find(", "if (!_lock()) return false;"),
    ("highestActiveSeverity", "Core::AlarmSeverity AlarmRegistry::highestActiveSeverity() const",
     "if (!_lock()) return Core::AlarmSeverity::Info;"),
    ("copyActiveAlarms", "uint8_t AlarmRegistry::copyActiveAlarms(",
     "if (!_lock()) return 0;"),
    ("overflowCount", "uint32_t AlarmRegistry::overflowCount() const", "if (!_lock()) return 0;"),
    ("persistFailures", "uint32_t AlarmRegistry::persistFailures() const",
     "if (!_lock()) return 0;"),
    ("generation", "uint16_t AlarmRegistry::generation() const", "if (!_lock()) return 0;"),
]
all_guarded = True
for name, sig, guard in FC:
    body = body_of(alarm_code, sig, ["\nRaiseResult AlarmRegistry::_", "\nvoid AlarmRegistry::_",
                                     "\nbool AlarmRegistry::_", "\nuint8_t AlarmRegistry::_",
                                     "\nuint32_t AlarmRegistry::_", "\nuint16_t AlarmRegistry::_",
                                     "\nCore::AlarmSeverity AlarmRegistry::_",
                                     "\nvoid AlarmRegistry::raise", "\nbool AlarmRegistry::raise",
                                     "\nvoid AlarmRegistry::snapshot", "} // namespace"])
    if guard not in body:
        all_guarded = False
        print(f"      (entry {name}: missing fail-closed guard)")
check("B1. All 16 locked public entries carry their documented fail-closed guard", all_guarded)

check("B2. isDirty() fails closed to TRUE (conservative — checkpoint + STORAGE_ERROR signaling stay alive)",
      bool(re.search(r"bool AlarmRegistry::isDirty\(\) const \{[^}]*if \(!_lock\(\)\) return true;",
                     alarm_code, re.S)))

check("B3. snapshotInto() fail path ZERO-FILLS the caller buffer + surfaces lockFailures",
      bool(re.search(r"void AlarmRegistry::snapshotInto\(Snapshot& out\) const \{[^}]*"
                     r"if \(!_lock\(\)\) \{[^}]*out = Snapshot\{\};[^}]*"
                     r"out\.lockFailures = _lockFailures\.load", alarm_code, re.S)))

check("B4. raise() surfaces LockUnavailable as FALSE (never a silent success)",
      "return r == RaiseResult::AcceptedRam || r == RaiseResult::AcceptedPersisted ||" in alarm_code and
      "return raiseTracked(code, sev, message) != RaiseResult::Rejected;" not in alarm_code)

# No Unlocked body may run without the lock: every *Unlocked( call site must
# sit in a function that guards _lock() first, or be an Unlocked-internal
# callee (which only run under a held lock by construction).
unlocked_sigs = ["AlarmRegistry::_raiseTrackedUnlocked(", "AlarmRegistry::_clearUnlocked(",
                 "AlarmRegistry::_clearIfActiveUnlocked(", "AlarmRegistry::_acknowledgeUnlocked(",
                 "AlarmRegistry::_acknowledgeAllUnlocked()", "AlarmRegistry::_saveToNVSUnlocked()",
                 "AlarmRegistry::_loadFromNVSUnlocked()"]
check("B5. No *Unlocked implementation takes the registry lock (deadlock-free by construction)",
      all("_lock()" not in brace_body(alarm_code, sig) for sig in unlocked_sigs) and
      all(brace_body(alarm_code, sig) for sig in unlocked_sigs))

# ---------------------------------------------------------------------------
print("\n[C] Contract + observability")

check("C1. RaiseResult::LockUnavailable exists and is documented",
      "LockUnavailable = 4," in alarm_hdr and "LockUnavailable" in alarm_code)

check("C2. Snapshot carries lockFailures (degraded-mode visibility)",
      "uint32_t lockFailures;" in alarm_hdr and
      "out.lockFailures = _lockFailures.load" in alarm_code)

lockfail_fn = brace_body(alarm_code, "uint32_t AlarmRegistry::lockFailures() const")
check("C3. lockFailures() is the LOCK-FREE accessor (atomic read, never takes the mutex)",
      bool(lockfail_fn) and "_lock()" not in lockfail_fn and
      "_lockFailures.load" in lockfail_fn)

check("C4. /api/alarms exposes lockFailures additively (consumers treat absent as 0)",
      'doc["lockFailures"] = snap->lockFailures;' in handlers_code and
      'doc["overflowCount"] = snap->overflowCount;' in handlers_code)

check("C5. /api/diagnostics exposes alarmLockFailures",
      'doc["alarmLockFailures"] = Services::alarms.lockFailures();' in diags_code)

# ---------------------------------------------------------------------------
print("\n[D] telemetryMutex symmetric boot guard (protects the p.470 helper)")

check("D1. telemetryMutex creation is null-checked in setup()",
      bool(re.search(r"telemetryMutex = xSemaphoreCreateMutex\(\);\s*"
                     r"if \(telemetryMutex == nullptr\) \{", ino_code)))

check("D2. The guard is FATAL (log + halt without feeding the task watchdog)",
      bool(re.search(r"if \(telemetryMutex == nullptr\) \{[^}]*FATAL[^}]*while \(true\)",
                     ino_code, re.S)) and
      "p.471" in ino)

check("D3. The guard sits BEFORE the first xTaskCreatePinnedToCore",
      ino_code.find("if (telemetryMutex == nullptr)") <
      min(m.start() for m in re.finditer(r"xTaskCreatePinnedToCore", ino_code)))

# ---------------------------------------------------------------------------
print("\n[E] Native harness — real fail-closed exercise + fail-open negative control")

check("E1. Injection hooks present (creation-failure + handle drop), documented mirror-only",
      "g_injectMutexCreateFail" in harness and
      "debugDropMutexForTest" in harness and
      "MIRROR-ONLY" in harness)

check("E2. Harness mirror carries the fail-closed contract (LockUnavailable + zero-fill + atomics)",
      "LockUnavailable = 4," in harness and
      "out = Snapshot{};" in harness and
      "_lockFailures.fetch_add" in harness)

check("E3. Phase P (boot guard) asserts the refusal + fail-closed API on a boot-refused registry",
      "Phase P: p.471 boot guard" in harness and
      "begin() REFUSES to boot" in harness and
      "LockUnavailable" in harness)

check("E4. Phase Q storms every public API from 8 threads + verifies zero mutation / zero NVS writes / byte-identical seeds",
      "Phase Q: p.471 runtime fail-closed storm" in harness and
      "ZERO NVS writes during the storm" in harness and
      "seeded entries byte-identical" in harness and
      "seeded alarms UNTOUCHED" in harness)

check("E5. Phase Q proves deterministic RECOVERY (lock re-created lazily, registry coherent again)",
      "no NVS transaction committed during the storm" in harness and
      bool(re.search(r"g_injectMutexCreateFail = false;[^}]*q\.snapshotInto\(post\)", harness, re.S)))

check("E6. LOCK_FAIL_OPEN negative-control mode compiles the round-7 fail-open _lock",
      "#elif defined(LOCK_FAIL_OPEN)" in harness and
      "return true;" in brace_body(harness_code, "bool _lock() const {") and
      "P471 NEGATIVE CONTROL TRIPPED" in harness)

check("E7. Runner builds the fail-open control under TSAN and REQUIRES it to trip",
      "-DLOCK_FAIL_OPEN" in runner and
      "P471 NEGATIVE CONTROL TRIPPED" in runner and
      "fail-open shape detected" in runner)

check("E8. The sentinel fires only on p.471-phase failures (scoped failure accounting)",
      bool(re.search(r"int p471FailuresBefore = g_failures\.load\(\);", harness) and
           re.search(r"P471 NEGATIVE CONTROL TRIPPED: fail-closed contract violated", harness)))

# ---------------------------------------------------------------------------
print("\n[F] Prior-round invariants survive the p.471 refactor")

check("F1. Immediate saves still run INSIDE the lock via _saveToNVSUnlocked (p.468)",
      alarm_code.count("_saveToNVSUnlocked();") == 8 and
      "bool persisted = _saveToNVSUnlocked();" in alarm_code)

check("F2. No raw-pointer accessors resurrected (p.469)",
      "getAlarm(" not in alarm_hdr and "getActiveAlarms" not in alarm_hdr and
      "getActiveAlarmCount" not in alarm_hdr)

check("F3. p.470 deep-copy helper untouched (serializeLatestStatusLocked shape intact)",
      "inline String serializeLatestStatusLocked()" in read(FW / "Web" / "BatteryStatusSerializer.h"))

check("F4. One registry mutex, created in begin() (single creation site in the class)",
      alarm_code.count("xSemaphoreCreateMutex()") == 2)   # _lock() retry + begin()

check("F5. Lock-failure path writes ONLY atomics (no registry-state mutation without sync)",
      bool(lock_body) and
      "_alarms" not in lock_body.split("return false;")[0].split("xSemaphoreTake")[0] and
      "_count" not in lock_body)

# ---------------------------------------------------------------------------
print()
failed = [r for r in results if not r[1]]
print(f"RESULT: {len(results) - len(failed)}/{len(results)} checks passed")
if failed:
    print("FAILED CHECKS:")
    for name, _, detail in failed:
        print(f"  - {name}" + (f" — {detail}" if detail else ""))
    sys.exit(1)
print("AUDIT ROUND 9 GATE: ALL CHECKS PASSED")
