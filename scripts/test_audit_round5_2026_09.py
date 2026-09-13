#!/usr/bin/env python3
"""
test_audit_round5_2026_09.py — [AUDIT 2026-09 ROUND 5] telemetry durability,
energy/SOC persistence failure propagation, BMS↔shunt authority interlock,
alarm-registry safety policy, and credential-boundary observability.

Auditor round-5 findings this gate pins (p.437–p.452):
  p.437  Regular telemetry spool lost on reboot (RAM-only)
  p.438  spool()==true hides eviction of the oldest record
  p.439  EnergyCounters NVS failure not propagated to caller
  p.440  Legacy energy fallback may load a mixed-generation snapshot
  p.441  Wall-clock/timeQuality contract (firmware-side evidence enumerated)
  p.442  Sequence-gap vs spool-overflow vs reboot-gap distinguishability
  p.443  Credentials plaintext at app level (provisioning-order enforcement)
  p.444  One-time plaintext UART credential reveal (boundary must be explicit)
  p.445  SOC lastSyncTs mixes Unix epoch and uptime-seconds domains
  p.446  OCV→SOC rough curve lacks rejection evidence gates
  p.447  Full-charge declares 100% from V+I only
  p.448  BMS authoritative gate ignores faultFlags
  p.449  BMS/shunt cross-check is a detector, not an interlock
  p.450  Existing-alarm update not immediately durable
  p.451  Alarm registry can evict ACTIVE safety alarms when saturated
  p.452  SOC NVS write failure not reported

What this gate enforces (static + Python mirrors, on every push):
  A. TelemetrySpool persists the regular ring to LittleFS (atomic, CRC),
     restores it at boot, clears it when drained; SpoolResult semantics
     (STORED / EVICTED_OLDEST / REJECTED) replace the ambiguous bool.
  B. EnergyCounters::saveToNVS and SocStateMachine::saveToNVS return bool,
     verify the write (size + read-back), count failures; persistenceTask
     converts failure into the STORAGE_ERROR alarm (fail-closed, never
     silently assumed).
  C. Legacy energy fallback is migration-only and self-normalizing.
  D. SOC sync timestamp is UNIX-epoch on every path; implausible legacy
     values are sanitized to 0 on load.
  E. BMS authority gate requires faultFlags == 0 AND no active mismatch;
     the mismatch demotes shunt integration quality to Suspect (interlock);
     the BMS_SYNC re-baseline is unreachable during mismatch.
  F. Full-charge confirmation and OCV resolution defer on contradicting
     evidence (BMS disagreement / bad cells / temperature window); unknown
     evidence keeps the documented V+I fallback.
  G. AlarmRegistry NEVER evicts non-cleared alarms; rejection is counted,
     logged (rate-limited), returned false; severity-escalating refreshes
     persist immediately.
  H. Credential boundary: DEFAULT_CREDENTIALS_ACTIVE alarm + credProv flag
     lifecycle + audit-log event; production-unencrypted raises
     SECURITY_PROVISIONING; spoolDrops in the telemetry envelope.
  I. Python mirrors: alarm eviction policy decision table, spool result
     semantics, mismatch interlock state transitions, full-charge defer
     decision table, legacy-timestamp sanitization.

Run: python3 scripts/test_audit_round5_2026_09.py   (exit 0 = PASS)
"""
import re
import subprocess
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
print("AUDIT ROUND 5 — telemetry durability / persistence / authority / alarms")
print("=" * 78)

spool_h = read(FW / "Services/TelemetrySpool.h")
spool_c = read(FW / "Services/TelemetrySpool.cpp")
energy_c = read(FW / "Services/EnergyCounters.cpp")
energy_h = read(FW / "Services/EnergyCounters.h")
soc_c = read(FW / "Services/SocStateMachine.cpp")
soc_h = read(FW / "Services/SocStateMachine.h")
bms_c = read(FW / "Comm/BatteryCommManager.cpp")
alarm_c = read(FW / "Services/AlarmRegistry.cpp")
alarm_h = read(FW / "Services/AlarmRegistry.h")
ino = read(FW / "firmware_v1.ino")
types_h = read(FW / "Core/Types.h")
globals_h = read(FW / "Core/Globals.h")
serializer_h = read(FW / "Web/BatteryStatusSerializer.h")
security_h_c = read(FW / "Web/SecurityHandlers.cpp")
diags_c = read(FW / "Web/DiagnosticsHandlers.cpp")
alarms_api_c = read(FW / "Web/AlarmHandlers.cpp")
configstore_c = read(FW / "Storage/ConfigStore.cpp")
extrah_c = read(FW / "Web/ExtraHandlers.cpp")

# ---------------------------------------------------------------------------
# A. p.437 + p.438 — regular spool persistence + explicit spool semantics
# ---------------------------------------------------------------------------
print("\n[A] Regular telemetry spool survives reboot (p.437) + explicit spool() semantics (p.438)")

check("A1. SpoolResult enum defined with three outcomes",
      "enum class SpoolResult" in spool_h and "EvictedOldest" in spool_h and "Rejected" in spool_h and "Stored" in spool_h)

check("A2. spool() returns SpoolResult (not bool)",
      bool(re.search(r"SpoolResult\s+spool\s*\(", spool_h)))

check("A3. Eviction path returns EvictedOldest and counts the drop",
      "result = SpoolResult::EvictedOldest" in spool_c and spool_c.count("_dropCount++") >= 2)

spool_code = strip_comments(spool_c)
check("A4. Regular ring flushed to LittleFS (atomic .tmp -> rename)",
      "_flushRegularToFs" in spool_c and 'LittleFS.rename(tmp, REGULAR_SNAPSHOT_PATH)' in spool_code)

check("A5. Boot restores the regular ring from the snapshot",
      "_loadRegularFromFs" in spool_c and "_loadRegularFromFs();" in spool_c.split("void TelemetrySpool::begin()")[1].split("}")[0])

check("A6. Flush is wear-bounded (interval constant, first-record immediate flush)",
      "REGULAR_FLUSH_INTERVAL_MS = 30000" in spool_h and "if (_count == 1)" in spool_code)

check("A7. Snapshot CRC32-verified; corrupt snapshot discarded, per-record CRC drops counted",
      "_clearRegularFs" in spool_code and "crc != hdr.crc32" in spool_code)

check("A8. Ring drained -> snapshot cleared (no resurrection of replayed records)",
      bool(re.search(r"if \(_count == 0\) \{\s*_clearRegularFs\(\);", spool_code)))

check("A9. Restore evidence exposed (fsRestoredCount, fsWriteFailures)",
      "fsRestoredCount()" in spool_h and "fsWriteFailures()" in spool_h and
      '"spoolFsRestored"' in diags_c and '"spoolFsWriteFailures"' in diags_c)

check("A10. Caller handles the explicit result (EvictedOldest logged, not silent)",
      "SpoolResult sr = Services::telemetrySpool.spool" in ino and "EvictedOldest" in ino)

# ---------------------------------------------------------------------------
# B. p.439 + p.452 — persistence failure propagation
# ---------------------------------------------------------------------------
print("\n[B] Persistence failures are reported, not assumed (p.439, p.452)")

check("B1. EnergyCounters::saveToNVS returns bool",
      bool(re.search(r"bool\s+EnergyCounterService::saveToNVS\s*\(", energy_c)))

check("B2. Energy save checks begin() failure, short write, and read-back mismatch",
      "Energy NVS begin() failed" in energy_c and "short write" in energy_c and "read-back mismatch" in energy_c)

check("B3. SocStateMachine::saveToNVS returns bool with begin/putBytes checks",
      bool(re.search(r"bool\s+SocStateMachine::saveToNVS\s*\(", soc_c)) and "putBytes" in soc_c.split("bool SocStateMachine::saveToNVS")[1][:800])

check("B4. Failure counters exposed (persistFailures getters + diagnostics keys)",
      "persistFailures()" in energy_h and "persistFailures()" in soc_h and
      '"energyPersistFailures"' in diags_c and '"socPersistFailures"' in diags_c)

check("B5. persistenceTask converts failure into STORAGE_ERROR alarm (fail-closed)",
      not re.search(r"^\s*Services::energyCounters\.saveToNVS\(\);\s*$", ino, re.M) and
      "bool energyOk = Services::energyCounters.saveToNVS();" in ino and
      'raise(Core::AlarmCode::STORAGE_ERROR' in ino)

check("B6. AlarmRegistry::saveToNVS returns bool and counts failures",
      bool(re.search(r"bool\s+AlarmRegistry::saveToNVS\s*\(", alarm_c)) and "_persistFailures++" in alarm_c)

# ---------------------------------------------------------------------------
# C. p.440 — legacy energy path is migration-only
# ---------------------------------------------------------------------------
print("\n[C] Legacy energy fallback normalizes immediately (p.440)")

check("C1. Legacy load detects actual key presence (isKey, not default-vs-zero)",
      'p.isKey("chAh")' in energy_c)

check("C2. Legacy state is rewritten as the atomic blob on acceptance",
      bool(re.search(r"if \(anyLegacyKey\) \{[^}]*_legacyLoadUsed = true;[^}]*saveToNVS\(\);", strip_comments(energy_c), re.S)))

check("C3. loadedFromLegacy() exposed for fleet visibility",
      "loadedFromLegacy" in energy_h and '"energyLoadedFromLegacyKeys"' in diags_c)

# ---------------------------------------------------------------------------
# D. p.445 — single time domain for SOC sync timestamps
# ---------------------------------------------------------------------------
print("\n[D] SOC lastSyncTs is UNIX epoch everywhere (p.445)")

check("D1. Full-charge path uses rtc.getUnixTime() (uptime-seconds assignment removed)",
      "Drivers::rtc.getUnixTime();  // [p.445]" in soc_c and "monotonicMs / 1000;  // store as seconds" not in soc_c)

check("D2. No remaining uptime-seconds writes into _lastSyncTs",
      "_lastSyncTs = monotonicMs" not in strip_comments(soc_c))

check("D3. Load sanitizes implausible (pre-2020) legacy timestamps to 0",
      "1577836800u" in soc_c and "wrong time domain" in soc_c)

# ---------------------------------------------------------------------------
# E. p.448 + p.449 — authority gate + interlock
# ---------------------------------------------------------------------------
print("\n[E] BMS authority + shunt interlock (p.448, p.449)")

bms_code = strip_comments(bms_c)
check("E1. socAuthoritative() requires faultFlags == 0",
      bool(re.search(r"_data\.faultFlags == 0", bms_code.split("socAuthoritative")[1].split("}")[0] if "socAuthoritative" in bms_code else "")) or
      "_data.faultFlags == 0" in bms_code.split("bool BatteryCommManager::socAuthoritative() const {")[1].split("}\n")[0])

check("E2. socAuthoritative() refuses authority during active mismatch",
      "!_mismatchActive" in bms_code.split("bool BatteryCommManager::socAuthoritative() const {")[1].split("}\n")[0])

ino_code = strip_comments(ino)
check("E3. energyTask demotes shunt current to Suspect while mismatch active (interlock)",
      "isMismatchActive()" in ino and
      bool(re.search(r"if \(mismatchActive\) \{\s*snap\.batteryCurrent\.quality = Core::MeasurementQuality::Suspect;", ino_code)))

check("E4. Cross-check runs BEFORE the integration ticks (same-cycle gate)",
      ino_code.find("crossCheckShunt") < ino_code.find("energyCounters.tick"))

check("E5. BMS_SYNC re-baseline gated behind bmsAuthoritative (unreachable during mismatch)",
      bool(re.search(r"if \(bmsAuthoritative\) \{[^}]*setSoc\(bms\.soc", ino_code, re.S)))

check("E6. Cross-check refuses STALE BMS data (no false interlock while BMS absent)",
      "_data.isFresh(millis()" in bms_code.split("crossCheckShunt")[1][:1500] and
      "stale last-reading is not a second opinion" in bms_c)

# ---------------------------------------------------------------------------
# F. p.446 + p.447 — evidence gates for OCV + full-charge
# ---------------------------------------------------------------------------
print("\n[F] OCV / full-charge evidence gates (p.446, p.447)")

check("F1. SocSyncEvidence struct exists (plain data, no Comm dependency)",
      "struct SocSyncEvidence" in soc_h and "#include" not in soc_h.split("struct SocSyncEvidence")[0].split("struct SocBaselineCorrectionEvent")[0].split("namespace Services {")[1])

check("F2. OCV resolution blocked on bad cells / temperature window",
      "cellsBad" in soc_c and "OCV_TEMP_MIN_C" in soc_c and "OCV_TEMP_MAX_C" in soc_c and "ocvBlocked" in soc_c)

check("F3. Full-charge confirmation defers on BMS disagreement (BMS_FULL_AGREE_PCT)",
      "BMS_FULL_AGREE_PCT" in read(FW / "Core/Config.h") and "bmsBlocks" in soc_c and "bmsAgreesFull" in soc_c)

check("F4. Full-charge defers during active mismatch (no new SOC basis in dispute)",
      "mismatchBlocks" in soc_c and "_evidence.mismatchActive" in soc_c)

check("F5. Defer is stay-in-candidate (no 100% write, no saveToNVS on defer)",
      bool(re.search(r"if \(bmsBlocks \|\| mismatchBlocks \|\| tempBlocks\) \{", strip_comments(soc_c))))

check("F6. energyTask populates evidence every cycle before tick()",
      "setSyncEvidence" in ino and ino_code.find("setSyncEvidence") < ino_code.find("socStateMachine.tick"))

check("F7. Unknown evidence keeps the documented V+I fallback (cells absent != bad)",
      "unknown cells" in soc_h or "(unknown cells" in soc_h)

check("F8. OCV provenance reported with LOW confidence + OCV_AT_REST method",
      '"OCV_AT_REST"' in ino and '"LOW"' in ino)

# ---------------------------------------------------------------------------
# G. p.450 + p.451 — alarm registry safety policy
# ---------------------------------------------------------------------------
print("\n[G] Alarm registry never sacrifices active safety state (p.451, p.450)")

alarm_code = strip_comments(alarm_c)
check("G1. Old drop-lowest-severity-active fallback is GONE",
      "drop the lowest-severity" not in alarm_c and "lowest-severity active" not in alarm_code)

check("G2. Eviction candidates are CLEARED only (clearedAt + raisedAt tie-break)",
      "AlarmLifecycle::Cleared" in alarm_code.split("_count >= MAX_ALARMS")[1].split("} else {")[0])

check("G3. Saturation rejects the new alarm and returns false",
      "REJECTED (not stored)" in alarm_c and
      bool(re.search(r"_overflowCount\+\+;.*?return false;", alarm_c, re.S)))

check("G4. Rejection counted (overflowCount) and rate-limited logged",
      "_overflowCount++" in alarm_code and "60000UL" in alarm_code)

check("G5. raise() returns bool; overflowCount exposed on /api/alarms + /api/diagnostics",
      "bool raise(" in alarm_h and '"overflowCount"' in alarms_api_c and '"alarmRegistryOverflow"' in diags_c)

check("G6. Severity-escalating refresh persists immediately (durability symmetry)",
      "meaningful" in alarm_c and
      bool(re.search(r"// \[p\.450\] Severity escalation.*?if \(meaningful\) \{\s*_dirty = true;\s*saveToNVS\(\);", alarm_c, re.S)))

check("G7. Pure refresh does NOT persist every tick (wear bound)",
      bool(re.search(r"if \(meaningful\) \{[^}]*saveToNVS\(\);[^}]*\}", alarm_code)) and
      alarm_code.count("saveToNVS();") <= 5)

# ---------------------------------------------------------------------------
# H. p.443 + p.444 + p.442 — credential boundary + posture + envelope evidence
# ---------------------------------------------------------------------------
print("\n[H] Credential boundary + provisioning posture + envelope evidence (p.442-444)")

check("H1. credentialsProvisioned flag exists with extern + definition",
      "extern bool credentialsProvisioned;" in globals_h and "credentialsProvisioned = true;" in ino)

check("H2. DEFAULT_CREDENTIALS_ACTIVE alarm evaluated by healthTask single-owner",
      "DEFAULT_CREDENTIALS_ACTIVE" in types_h and
      'raise(Core::AlarmCode::DEFAULT_CREDENTIALS_ACTIVE' in ino and
      'clear(Core::AlarmCode::DEFAULT_CREDENTIALS_ACTIVE)' in ino)

check("H3. Flag persisted (credProv) in config.json + closed on password change",
      '"credProv"' in configstore_c and "credentialsProvisioned = true;" in extrah_c)

check("H4. Legacy config.json without flag treated as provisioned (no retroactive nag)",
      'doc["credProv"] | true' in configstore_c)

check("H5. UART reveal logs an auditable security event",
      "revealed via UART" in configstore_c)

check("H6. /api/security exposes credentialBoundary block",
      "credentialBoundary" in security_h_c and "defaultCredentialActive" in security_h_c)

check("H7. Production build raises SECURITY_PROVISIONING on unencrypted flash",
      "SECURITY_PROVISIONING" in types_h and
      bool(re.search(r"#ifdef PRODUCTION_BUILD\s*\n\s*if \(!Services::securityPosture\.flashEncryptionEnabled\(\)\)", ino)))

check("H8. spoolDrops field in the telemetry envelope (struct + serializer + writer)",
      "spoolDrops" in globals_h and '"spoolDrops"' in serializer_h and "latestStatus.health.spoolDrops" in ino)

# ---------------------------------------------------------------------------
# I. Python mirrors — decision tables the firmware implements
# ---------------------------------------------------------------------------
print("\n[I] Python mirrors — decision tables (executable specification)")

# I1. Alarm eviction policy mirror (p.451)
def alarm_raise_mirror(registry, new_code, new_sev):
    """registry: list of dicts(code, severity, lifecycle, clearedAt, raisedAt).
    Returns (accepted, overflow, remaining_registry) per the firmware policy."""
    registry = [dict(r) for r in registry]
    overflow = 0
    if not any(r["code"] == new_code for r in registry):
        if len(registry) >= 24:
            cleared = [i for i, r in enumerate(registry) if r["lifecycle"] == "CLEARED"]
            if cleared:
                idx = min(cleared, key=lambda i: (registry[i]["clearedAt"], registry[i]["raisedAt"]))
                registry.pop(idx)
            else:
                overflow += 1
                return False, overflow, registry
        registry.append({"code": new_code, "severity": new_sev, "lifecycle": "ACTIVE",
                         "clearedAt": 0, "raisedAt": 100})
    return True, overflow, registry

cases = [
    ("slot free -> accepted", [], "NEW", True, 0),
    ("cleared evicted when full", [{"code": f"C{i}", "severity": 1, "lifecycle": "ACTIVE", "clearedAt": 0, "raisedAt": i} for i in range(23)] +
                                   [{"code": "CL", "severity": 1, "lifecycle": "CLEARED", "clearedAt": 5, "raisedAt": 1}], "NEW", True, 0),
    ("all active -> REJECTED (even lower severity exists)",
     [{"code": f"A{i}", "severity": 1 if i < 23 else 0, "lifecycle": "ACTIVE", "clearedAt": 0, "raisedAt": i} for i in range(24)], "NEW", False, 1),
    ("all active, new is CRITICAL -> still rejected (no active sacrifice)",
     [{"code": f"A{i}", "severity": 0, "lifecycle": "ACTIVE", "clearedAt": 0, "raisedAt": i} for i in range(24)], "CRIT", False, 1),
]
for name, reg, code, want_ok, want_ov in cases:
    ok, ov, _ = alarm_raise_mirror(reg, code, 2)
    check(f"I1 mirror: {name}", ok == want_ok and ov == want_ov, f"got ok={ok} ov={ov}")

# I2. Full-charge defer decision mirror (p.447)
def full_charge_confirm(bmsHealthy, bmsAgreesFull, cellsBad, mismatchActive, tempValid, tempC):
    bmsBlocks = bmsHealthy and (not bmsAgreesFull or cellsBad)
    tempBlocks = tempValid and (tempC < 0.0 or tempC > 45.0)
    return not (bmsBlocks or mismatchActive or tempBlocks)

fc_cases = [
    ("no BMS, no temp evidence -> confirm (documented fallback)", dict(bmsHealthy=False, bmsAgreesFull=False, cellsBad=False, mismatchActive=False, tempValid=False, tempC=25.0), True),
    ("healthy BMS agrees -> confirm", dict(bmsHealthy=True, bmsAgreesFull=True, cellsBad=False, mismatchActive=False, tempValid=True, tempC=25.0), True),
    ("healthy BMS says 73% -> DEFER", dict(bmsHealthy=True, bmsAgreesFull=False, cellsBad=False, mismatchActive=False, tempValid=True, tempC=25.0), False),
    ("cells overvoltage -> DEFER", dict(bmsHealthy=True, bmsAgreesFull=True, cellsBad=True, mismatchActive=False, tempValid=True, tempC=25.0), False),
    ("mismatch active -> DEFER", dict(bmsHealthy=False, bmsAgreesFull=False, cellsBad=False, mismatchActive=True, tempValid=True, tempC=25.0), False),
    ("temp -5C -> DEFER", dict(bmsHealthy=False, bmsAgreesFull=False, cellsBad=False, mismatchActive=False, tempValid=True, tempC=-5.0), False),
    ("faulty BMS (not healthy) disagrees -> still confirm (V+I fallback)", dict(bmsHealthy=False, bmsAgreesFull=False, cellsBad=False, mismatchActive=False, tempValid=True, tempC=25.0), True),
    ("cells unknown (no cell data) -> not cellsBad, confirm", dict(bmsHealthy=True, bmsAgreesFull=True, cellsBad=False, mismatchActive=False, tempValid=False, tempC=25.0), True),
]
for name, kw, want in fc_cases:
    check(f"I2 mirror: {name}", full_charge_confirm(**kw) == want)

# I3. Mismatch interlock state mirror (p.449)
def mismatch_interlock(prev_streak, mismatch_sustained_polls, delta, threshold):
    streak = prev_streak + 1 if delta > threshold else 0
    active = streak >= mismatch_sustained_polls
    return streak, active

st, act = mismatch_interlock(0, 3, 160.0, 0.5)
check("I3 mirror: 160A delta sustained -> active after 3 polls", st == 1 and act is False)
st, act = mismatch_interlock(2, 3, 160.0, 0.5)
check("I3 mirror: third consecutive -> interlock ACTIVE", st == 3 and act is True)
st, act = mismatch_interlock(3, 3, 0.2, 0.5)
check("I3 mirror: agreement resets the interlock", st == 0 and act is False)

# I4. Legacy timestamp sanitization mirror (p.445)
def sanitize_last_sync(ts):
    return 0 if (ts != 0 and ts < 1577836800) else ts

check("I4 mirror: uptime-seconds value (43812) -> 0", sanitize_last_sync(43812) == 0)
check("I4 mirror: epoch 2026 value preserved", sanitize_last_sync(1760000000) == 1760000000)
check("I4 mirror: 0 stays 0 (honest unknown)", sanitize_last_sync(0) == 0)

# I5. SpoolResult semantics mirror (p.438)
def spool_mirror(count, capacity, seq, last_seq, valid_payload):
    if seq == last_seq:
        return "REJECTED", count, 0
    if not valid_payload:
        return "REJECTED", count, 1
    if count < capacity:
        return "STORED", count + 1, 0
    return "EVICTED_OLDEST", count, 1

r = spool_mirror(8, 8, 5, 4, True)
check("I5 mirror: full ring stores but reports EVICTED_OLDEST + drop", r == ("EVICTED_OLDEST", 8, 1))
r = spool_mirror(0, 8, 5, 4, True)
check("I5 mirror: empty ring reports STORED, no drop", r == ("STORED", 1, 0))
r = spool_mirror(3, 8, 5, 5, True)
check("I5 mirror: duplicate sequence REJECTED", r == ("REJECTED", 3, 0))

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print("\n" + "=" * 78)
failed = [r for r in results if not r[1]]
print(f"RESULT: {len(results) - len(failed)}/{len(results)} PASS" +
      (f" — FAILED: {[r[0] for r in failed]}" if failed else ""))
sys.exit(1 if failed else 0)
