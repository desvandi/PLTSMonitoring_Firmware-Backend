#!/usr/bin/env python3
# =============================================================================
# test_audit_round8_2026_09.py — ROUND-8 static gate (auditor verification
# of PR #38 + the p.470 status-snapshot remediation).
#
# Round-8 scope (this file):
#   [A] The p.470 fix: Web::serializeLatestStatusLocked() deep-copies the
#       active-alarm list under telemetryMutex before serializing.
#   [B] Both affected consumers migrated (handleStatus, GasAdvisor);
#       publishTelemetry documented as the single-writer exemption.
#   [C] No remaining "struct copy under mutex + serialize aliased pointer
#       after release" shapes anywhere in the firmware.
#   [D] Native harness + negative control wired into the CI runner.
#   [E] Round-7 registry invariants untouched by the fix.
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


def brace_body(src: str, signature: str) -> str:
    """Body (inside the outermost braces) of the function starting at
    `signature` — brace counting, robust against stripped trailing comments."""
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
                return src[j + 1:k]
    return src[j + 1:]


print("=" * 78)
print("AUDIT ROUND 8 — p.470 status-snapshot detach + PR #38 verification")
print("=" * 78)

ser_h = read(FW / "Web" / "BatteryStatusSerializer.h")
ser_code = strip_comments(ser_h)
status_c = read(FW / "Web" / "StatusHandlers.cpp")
status_code = strip_comments(status_c)
gas_c = read(FW / "AI" / "GasAdvisor.cpp")
gas_code = strip_comments(gas_c)
ino = read(FW / "firmware_v1.ino")
ino_code = strip_comments(ino)
runner = read(NATIVE / "run-native-tests.sh")
detach_h = read(NATIVE / "verify_status_snapshot_detach.cpp")
alarm_h = read(FW / "Services" / "AlarmRegistry.h")
alarm_c = read(FW / "Services" / "AlarmRegistry.cpp")
alarm_code = strip_comments(alarm_c)

# ---------------------------------------------------------------------------
print("\n[A] serializeLatestStatusLocked(): deep-copy under the mutex (p.470)")

check("A1. Helper exists in the shared serializer header",
      "inline String serializeLatestStatusLocked()" in ser_code)

helper_body = brace_body(ser_code, "inline String serializeLatestStatusLocked()")
check("A2. Helper takes telemetryMutex and copies latestStatus inside it",
      bool(helper_body) and
      "xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100))" in helper_body and
      "snap = latestStatus;" in helper_body)

check("A3. Alarm list DEEP-COPIED under the mutex (malloc + memcpy + pointer re-pointed)",
      bool(helper_body) and
      "malloc(snap.activeAlarmCount * sizeof(Services::Alarm))" in helper_body and
      "memcpy(listCopy, snap.activeAlarms," in helper_body and
      "snap.activeAlarms = listCopy;" in helper_body)

check("A4. Mutex RELEASED before serialize; copy freed after",
      bool(helper_body) and
      helper_body.find("xSemaphoreGive(telemetryMutex)") < helper_body.find("serialize(snap)") and
      helper_body.find("serialize(snap)") < helper_body.find("free(listCopy)"))

check("A5. OOM omits the list honestly (count=0, pointer=null) — never a torn read",
      bool(helper_body) and bool(re.search(
          r"snap\.activeAlarmCount = 0;\s*snap\.activeAlarms = nullptr;", helper_body)))

check("A6. Deep copy is bounded by the copied count (no MAX_ALARMS-sized over-copy)",
      bool(helper_body) and
      "snap.activeAlarmCount * sizeof(Services::Alarm)" in helper_body)

# ---------------------------------------------------------------------------
print("\n[B] Consumers migrated; single-writer exemption documented")

check("B1. handleStatus serializes via the locked helper (no local struct-copy aliasing)",
      "Web::serializeLatestStatusLocked()" in status_code and
      "Web::serialize(snap)" not in status_code and
      "snap = latestStatus" not in status_code)

check("B2. handleStatus still maps mutex timeout to 503",
      'sendError(503, "Telemetry mutex timeout")' in status_code)

check("B3. GasAdvisor builds its signed payload via the locked helper",
      "Web::serializeLatestStatusLocked()" in gas_code and
      "Web::serialize(snap)" not in gas_code)

check("B4. publishTelemetry documented as the single-writer self-read exemption",
      "p.470" in ino and "single-writer self-read" in ino and
      "serializeLatestStatusLocked()" in ino)

# ---------------------------------------------------------------------------
print("\n[C] No aliased-pointer serialization shape remains anywhere")

# Every Web::serialize( call site must be publishTelemetry (the buffer OWNER,
# serializing its own static buffer — single-writer self-read). The helper's
# internal call is unprefixed serialize(snap) inside namespace Web.
call_sites = []
for path in (FW / "Web" / "StatusHandlers.cpp", FW / "AI" / "GasAdvisor.cpp",
             FW / "Web" / "BatteryStatusSerializer.h", FW / "firmware_v1.ino"):
    for mm in re.finditer(r"Web::serialize\(", strip_comments(read(path))):
        call_sites.append((path.name, mm.start()))
check("C1. The only Web::serialize() caller left is publishTelemetry (single-writer)",
      len(call_sites) == 1 and call_sites[0][0] == "firmware_v1.ino")

check("C2. GasAdvisor no longer takes telemetryMutex for the body build (helper owns it)",
      "xSemaphoreTake(telemetryMutex" not in gas_code.split("serializeLatestStatusLocked")[0].split("tick()")[-1]
      if "tick()" in gas_code else "xSemaphoreTake(telemetryMutex" not in gas_code)

# ---------------------------------------------------------------------------
print("\n[D] Native harness + negative control wired into CI")

check("D1. verify_status_snapshot_detach.cpp: real threads (writer + >=2 readers + watchdog)",
      all(s in detach_h for s in
          ["std::thread w(writerTask)", "std::thread rA(readerTask", "std::thread wd(watchdog)"]))

check("D2. Harness models BOTH shapes (treatment deep-copy vs NO_DETACH negative control)",
      "#ifdef NO_DETACH" in detach_h and "memcpy(listCopy" in detach_h)

check("D3. Runner builds treatment (TSAN + ASAN/UBSAN) AND requires the negative control to report races",
      "verify_status_snapshot_detach" in runner and
      "-DNO_DETACH" in runner and
      "aliased-pointer read — harness sensitivity proven" in runner)

check("D4. Harness torn-entry detector is generation-consistency based (tagA==tagB per entry)",
      "entryConsistent" in detach_h and "a.tagA == a.tagB" in detach_h)

# ---------------------------------------------------------------------------
print("\n[E] Round-7 registry invariants untouched by the round-8 fix")

# [ROUND-9 UPDATE 2026-09-15 / p.471] The registry wrappers now GUARD the
# lock result before delegating ("if (!_lock()) return <fail-closed>;") —
# the locked-entry semantics this gate protects are unchanged (see
# test_audit_round9_2026_09.py for the fail-closed contract itself).
check("E1. AlarmRegistry source: locked wrappers + Unlocked discipline intact",
      bool(re.search(r"bool AlarmRegistry::saveToNVS\(\) \{\s*if \(!_lock\(\)\) return false;\s*"
                     r"bool ok = _saveToNVSUnlocked\(\);\s*_unlock\(\);", alarm_code)))

check("E2. snapshotInto still one-lock whole-state copy",
      bool(re.search(r"void AlarmRegistry::snapshotInto\(Snapshot& out\) const \{\s*"
                     r"if \(!_lock\(\)\)", alarm_code)) and
      "memcpy(out.alarms, _alarms, sizeof(_alarms));" in alarm_code)

check("E3. No raw-pointer accessors resurrected",
      "getAlarm(" not in strip_comments(alarm_h) and
      "getActiveAlarms(" not in strip_comments(alarm_h) and
      "getActiveAlarmCount(" not in strip_comments(alarm_h))

check("E4. Registry log-rate-limit statics still only touched under the lock",
      bool(re.search(r"_overflowCount\+\+;\s*static uint32_t lastOverflowLogMs = 0;", alarm_code)))

# ---------------------------------------------------------------------------
print()
failed = [r for r in results if not r[1]]
print(f"RESULT: {len(results) - len(failed)}/{len(results)} checks passed")
if failed:
    print("FAILED CHECKS:")
    for name, _, detail in failed:
        print(f"  - {name}" + (f" — {detail}" if detail else ""))
    sys.exit(1)
print("AUDIT ROUND 8 GATE: ALL CHECKS PASSED")
