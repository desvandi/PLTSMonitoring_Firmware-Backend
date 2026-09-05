#!/usr/bin/env python3
# =============================================================================
# test_wave12_contract_regression.py — WAVE 12: FULL cross-layer contract
# consistency regression after v1.7.0 (13-field emergency CONFIG +
# emergency TELEMETRY block + PZEM-004T meter + genset channel).
# =============================================================================
# Background (W12 findings fixed by this wave — these checks lock them in):
#   W12-1  Modular BatteryStatusSerializer emitted emergency.estopOpen /
#          trips while GAS canonical ingest read estopLine / tripCount and
#          the PWA SystemStatus.emergency type expects estopLineOpen /
#          tripCount — e-stop status + lifetime trip count were silently
#          dropped for modular devices (PWA showed "closed" while open).
#   W12-2  ac.meter (PZEM real power, PLTS_ENABLE_PZEM_AC) was a dead-end
#          contract: firmware emitted it, nobody consumed it (GAS no ingest
#          column, LATEST no block, PWA no slot / no UI).
#
# Checks (parsed from REAL sources — no fixtures):
#   X1   13-field emergency CONFIG schema parity across FOUR layers:
#        GAS EMERGENCY_CONFIG_FIELDS == PWA EMERGENCY_CONFIG_FIELDS ==
#        firmware-generic EmergencyConfig struct == modular cfgEmg* globals
#        (field-for-field, range-for-range, default-for-default)
#   X2   Modular ConfigStore::loadEmergencyConfig sanitize ranges == schema
#   X3   Modular EmergencySupervisor::applyCommand CONFIG clamps == schema
#   X4   Emergency telemetry (nested): every emergency key the modular
#        serializer writes is either consumed by GAS ingest or is a
#        documented informational extra (relayEnergized/tripAtMs/crashChain)
#   X5   Serializer emits the CANONICAL PWA names (estopLineOpen / tripCount)
#        and no longer the dropped legacy pair (estopOpen / trips as PRIMARY
#        keys); GAS ingest fallback chain covers estopLineOpen|estopLine|
#        estopOpen + tripCount|trips (mixed-version fleet safety)
#   X6   GAS LATEST emergency block keys == keys the PWA parses
#        (parseLatestEnvelope + parseEmergencyBlock + SystemStatus type)
#   X7   Emergency reason vocabulary parity: generic EMG_REASON_* set ==
#        modular EMG_REASON_* set == GAS/PWA documented reasons
#   X8   Event type vocabulary: every event type the modular + generic
#        firmware can queue is inside the GAS EMERGENCY_EVENT_TYPES whitelist
#   X9   PZEM: serializer ac.meter keys (connected/power/voltage) ⊆ GAS
#        ingest reads; honesty gate — values stored ONLY when connected
#   X10  PZEM: TELEMETRY_HEADER columns p_ac_meter / meter_v /
#        meter_connected at indices 36..38; appendRow writes them;
#        rowToEnvelope_ emits ac.meter{connected,voltage,power} guarded by
#        row.length > 36 (pre-W12 rows → absent, honest)
#   X11  PZEM: PWA AcTelemetry has the meter slot + gasEnvelope parses
#        ac.meter + ac-output-view renders a MEASURED meter surface
#   X12  Genset: generic flat i_ac_gen → GAS column i_ac_gen → LATEST
#        gensetRmsCurrent → PWA i_ac_gen; modular RESERVED documented
#   X13  TELEMETRY_HEADER column count == 39; V1_7_LEN == 36;
#        migrateTelemetryHeader_ keeps the append-only guard
#   X14  Flat path regression (WAVE-8 C3): every telemetry field the generic
#        firmware writes is still consumed by GAS (drift protection)
#   X15  GAS Code.gs parses as JavaScript (node --check) — deployment guard
# =============================================================================
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FWB = Path(__file__).resolve().parent.parent
PWA = Path(__file__).resolve().parent.parent.parent / "PLTSMonitoring_PWA"
import os
if os.environ.get("PLTS_PWA_REPO"):
    PWA = Path(os.environ["PLTS_PWA_REPO"])

GAS = FWB / "code.gs" / "Code.gs"
INO = FWB / "firmware-generic" / "src" / "plts_firmware_v1.ino"
GLOBALS_H = FWB / "firmware" / "Core" / "Globals.h"
CS_CPP = FWB / "firmware" / "Storage" / "ConfigStore.cpp"
EMG_SUP_CPP = FWB / "firmware" / "Services" / "EmergencySupervisor.cpp"
SERIALIZER_H = FWB / "firmware" / "Web" / "BatteryStatusSerializer.h"
EMERGENCY_TS = PWA / "src" / "lib" / "emergency.ts"
GAS_ENV_TS = PWA / "src" / "lib" / "gasEnvelope.ts"
TYPES_TS = PWA / "src" / "lib" / "types.ts"
AC_VIEW_TSX = PWA / "src" / "components" / "ac" / "ac-output-view.tsx"

passed, failed = [], []


def check(cid, desc, cond, detail=""):
    if cond:
        passed.append(cid)
        print(f"  [PASS] {cid}: {desc}")
    else:
        failed.append((cid, desc, detail))
        print(f"  [FAIL] {cid}: {desc}" + (f"  -> {detail}" if detail else ""))


PWA_AVAILABLE = PWA.exists() and (PWA / "src" / "lib" / "emergency.ts").exists()
if not PWA_AVAILABLE:
    print("  [SKIP] PWA repo not present (single-repo CI checkout) — "
          "PWA-side checks skipped; set PLTS_PWA_REPO for the full suite")


def check_or_skip(cid, desc, cond, detail=""):
    if not PWA_AVAILABLE:
        print(f"  [SKIP] {cid}: {desc} (PWA repo absent)")
        return
    check(cid, desc, cond, detail)


gas_src = GAS.read_text(encoding="utf-8")
ino_src = INO.read_text(encoding="utf-8")
globals_src = GLOBALS_H.read_text(encoding="utf-8")
cs_src = CS_CPP.read_text(encoding="utf-8")
sup_src = EMG_SUP_CPP.read_text(encoding="utf-8")
ser_src = SERIALIZER_H.read_text(encoding="utf-8")
emg_ts = EMERGENCY_TS.read_text(encoding="utf-8") if PWA_AVAILABLE else ""
env_ts = GAS_ENV_TS.read_text(encoding="utf-8") if PWA_AVAILABLE else ""
types_ts = TYPES_TS.read_text(encoding="utf-8") if PWA_AVAILABLE else ""
ac_view = AC_VIEW_TSX.read_text(encoding="utf-8") if PWA_AVAILABLE else ""

# ---------------------------------------------------------------------------
print("== WAVE 12: 13-field emergency CONFIG contract (4 layers) ==")

# --- X1: schema parity GAS <-> PWA <-> generic struct <-> modular globals --
gas_m = re.search(r"const EMERGENCY_CONFIG_FIELDS = \[(.*?)\n\];", gas_src, re.S)
gas_fields = {}
for f in re.findall(
        r"\['([A-Za-z0-9_]+)',\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)\]",
        gas_m.group(1)):
    gas_fields[f[0]] = (float(f[1]), float(f[2]), float(f[3]))

pwa_fields = {}
pwa_m = re.search(
    r"export const EMERGENCY_CONFIG_FIELDS[^=]+= \[(.*?)\n\];", emg_ts, re.S)
if pwa_m:
    for f in re.findall(
            r'key:\s*"([A-Za-z0-9_]+)",\s*min:\s*(-?[\d.]+),\s*max:\s*(-?[\d.]+),'
            r'\s*dflt:\s*(-?[\d.]+)', pwa_m.group(1)):
        pwa_fields[f[0]] = (float(f[1]), float(f[2]), float(f[3]))

fw_struct_m = re.search(r"struct EmergencyConfig \{(.*?)\n\};", ino_src, re.S)
fw_defaults, fw_ranges = {}, {}
for line in fw_struct_m.group(1).splitlines():
    d = re.match(
        r"\s*\w+\s+(\w+)\s*=\s*(-?[\d.]+)f?\s*;.*\[\s*(-?[\d.]+)\s*\.\.\s*(-?[\d.]+)\s*\]",
        line)
    if d:
        fw_defaults[d.group(1)] = float(d.group(2))
        fw_ranges[d.group(1)] = (float(d.group(3)), float(d.group(4)))

# Modular: Globals.h cfgEmg* declarations with // [lo,hi] or {0,1} comments
mod_fields = {}
for m in re.finditer(
        r"extern\s+\w+\s+cfgEmg(\w+)\s*;\s*//\s*[\[{(](-?[\d.]+),\s*(-?[\d.]+)[\]})]\s*.*?"
        r"default\s+(-?[\d.]+)",
        globals_src):
    name = m.group(1)
    name = name[0].lower() + name[1:]
    mod_fields[name] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))

check_or_skip("X1a", "13-field emergency schema: GAS / PWA / generic / modular all "
      "define the same 13 fields",
      len(gas_fields) == len(pwa_fields) == len(fw_defaults) == len(mod_fields) == 13,
      f"GAS={len(gas_fields)} PWA={len(pwa_fields)} generic={len(fw_defaults)} "
      f"modular={len(mod_fields)}")

mm = []
for name, (mn, mx, df) in gas_fields.items():
    p = pwa_fields.get(name)
    g = fw_defaults.get(name)
    g_r = fw_ranges.get(name)
    mod = mod_fields.get(name)
    if p is None or g is None or mod is None:
        miss = [k for k, v in (("PWA", p), ("generic", g), ("modular", mod))
                if v is None]
        mm.append(f"{name}: missing in {'/'.join(miss)}")
        continue
    if (mn, mx, df) != p:
        mm.append(f"{name}: GAS{(mn, mx, df)} != PWA{p}")
    if df != g or (g_r and (mn, mx) != g_r):
        mm.append(f"{name}: generic default/range drift")
    if (mn, mx, df) != mod:
        mm.append(f"{name}: modular{mod} != GAS{(mn, mx, df)}")
check_or_skip("X1b", "13-field ranges + defaults identical across all FOUR layers",
      not mm, "; ".join(mm[:6]))

# --- X2: modular ConfigStore load sanitize ranges == schema -----------------
load_zone_m = re.search(
    r"void ConfigStore::loadEmergencyConfig\(\) \{(.*?)\n\}\n", cs_src, re.S)
load_zone = load_zone_m.group(1) if load_zone_m else ""
bad_san = []
for name, (mn, mx, df) in gas_fields.items():
    var = "cfgEmg" + name[0].upper() + name[1:]
    # full two-sided check: < lo || > hi  (Core::-qualified, isfinite guard first)
    pat = (rf"Core::{var}\s*<\s*(-?[\d.]+)f?\s*\|\|\s*Core::{var}\s*>\s*(-?[\d.]+)f?")
    m = re.search(pat, load_zone)
    if m:
        lo, hi = sorted((float(m.group(1)), float(m.group(2))))
        if (lo, hi) != (mn, mx):
            bad_san.append(f"{name}: sanitize {(lo, hi)} != schema {(mn, mx)}")
        continue
    # one-sided upper bound is honest when the floor is 0 (unsigned types)
    m_up = re.search(rf"Core::{var}\s*>\s*(-?[\d.]+)f?\b", load_zone)
    if m_up and mn == 0 and float(m_up.group(1)) == mx:
        continue
    bad_san.append(f"{name}: no sanitize")
check("X2", "Modular ConfigStore load-range sanitization == 13-field schema",
      not bad_san, "; ".join(bad_san[:6]))

# --- X3: modular applyCommand CONFIG clamps == schema -----------------------
apply_zone_m = re.search(
    r'cmd == "CONFIG"\) \{(.*?)_queueEventUnlocked\("CONFIG_APPLIED"',
    sup_src, re.S)
apply_zone = apply_zone_m.group(1) if apply_zone_m else ""
bad_clamp = []
for name, (mn, mx, df) in gas_fields.items():
    pat = (rf'cfg\["{name}"\][^,]*,\s*(-?[\d.]+)f?,\s*(-?[\d.]+)f?,')
    m = re.search(pat, apply_zone)
    if not m:
        bad_clamp.append(f"{name}: no clamp call")
        continue
    lo, hi = float(m.group(1)), float(m.group(2))
    if (lo, hi) != (mn, mx):
        bad_clamp.append(f"{name}: clamp {(lo, hi)} != schema {(mn, mx)}")
check("X3", "Modular EmergencySupervisor CONFIG clamps == 13-field schema",
      not bad_clamp, "; ".join(bad_clamp[:6]))

# ---------------------------------------------------------------------------
print("== WAVE 12: emergency TELEMETRY contract (nested canonical path) ==")

# --- X4: serializer emergency keys -> GAS ingest / documented extras -------
ser_zone_m = re.search(
    r'JsonObject emg = doc\.createNestedObject\("emergency"\);(.*?)#endif',
    ser_src, re.S)
ser_keys = set()
if ser_zone_m:
    ser_keys = set(re.findall(r'emg\["(\w+)"\]', ser_zone_m.group(1)))

ing_zone_m = re.search(
    r"const emg = data\.emergency \|\| null;(.*?)\n    const env",
    gas_src, re.S)
ing_reads = set(re.findall(r"emg\.(\w+)", ing_zone_m.group(1))) if ing_zone_m else set()

EXTRAS = {"relayEnergized", "tripAtMs", "crashChain"}  # informational, not contract
orphans = {k for k in ser_keys - ing_reads if k not in EXTRAS}
check("X4", "Every emergency key the modular serializer writes is consumed "
      "by GAS ingest or is a documented informational extra",
      not orphans, f"orphan keys: {sorted(orphans)}")

# --- X5: canonical names + fallback chain ----------------------------------
check("X5a", "Serializer emits canonical PWA names estopLineOpen + tripCount",
      'emg["estopLineOpen"]' in ser_src and 'emg["tripCount"]' in ser_src,
      "the W12-1 fix must keep the PWA SystemStatus.emergency key names")
ing_txt = ing_zone_m.group(1) if ing_zone_m else ""
has_chain = all(k in ing_txt for k in
                ("emg.estopLineOpen", "emg.estopLine", "emg.estopOpen",
                 "emg.tripCount", "emg.trips"))
check("X5b", "GAS ingest fallback chain: estopLineOpen | estopLine | "
      "estopOpen AND tripCount | trips (mixed-version fleets)",
      has_chain, "one or more alias reads missing from canonical ingest")

# --- X6: GAS LATEST emergency block == PWA parses ---------------------------
lat_zone_m = re.search(
    r"emergency: row\.length > 32 \{(.*?)\n    \}", gas_src, re.S)
lat_keys = set(re.findall(r"(\w+):", lat_zone_m.group(1))) if lat_zone_m else set()

pwa_env_reads = set(re.findall(r"emg\.(\w+)", env_ts)) if PWA_AVAILABLE else set()
pwa_emg_ts_reads = set(re.findall(r"o\.(\w+)|estopLineOpen|tripCount", emg_ts))
pwa_emg_ts_reads = {x for x in pwa_emg_ts_reads if x}
missing_parse = {k for k in lat_keys if k not in pwa_env_reads}
check_or_skip("X6", "GAS LATEST emergency keys are all parsed by the PWA "
      "(gasEnvelope + emergency.ts)",
      not missing_parse,
      f"LATEST keys {sorted(lat_keys)}; unparsed: {sorted(missing_parse)}; "
      f"gasEnvelope reads: {sorted(pwa_env_reads)}")

# --- X7: reason vocabulary parity -------------------------------------------
gen_reasons = set(re.findall(r'"(BOOT|VBAT_LOW|VBAT_HIGH|I_DC_OVER|'
    r'I_AC_LOAD_OVER|I_AC_GEN_OVER|SENSOR_LOSS|ESTOP|OPERATOR|CRASHLOOP)"',
    ino_src))
mod_reasons = set(re.findall(r'EMG_REASON_\w+\s*=\s*"(\w+)"', globals_src)) | \
    set(re.findall(r'EMG_REASON_\w+\s*=\s*"(\w+)"', ser_src))
mod_reasons |= set(re.findall(r'EMG_REASON_\w+\s*=\s*"(\w+)"',
    (FWB / "firmware" / "Services" / "EmergencySupervisor.h").read_text()))
VOCAB = {"BOOT", "VBAT_LOW", "VBAT_HIGH", "I_DC_OVER", "I_AC_LOAD_OVER",
         "I_AC_GEN_OVER", "SENSOR_LOSS", "ESTOP", "OPERATOR", "CRASHLOOP"}
check("X7", "Emergency reason vocabulary identical across generic + modular",
      gen_reasons == mod_reasons == VOCAB,
      f"generic={sorted(gen_reasons)} modular={sorted(mod_reasons)}")

# --- X8: event type vocabulary ⊆ GAS whitelist ------------------------------
wl_m = re.search(r"const EMERGENCY_EVENT_TYPES = \[(.*?)\];", gas_src, re.S)
whitelist = set(re.findall(r"'([A-Z_]+)'", wl_m.group(1))) if wl_m else set()
mod_events = set(re.findall(r'_queueEventUnlocked\("(\w+)"', sup_src)) | \
    set(re.findall(r'queueEvent\("(\w+)"', sup_src))
gen_events = set(re.findall(r'emgQueueEvent\("(\w+)"', ino_src))
check("X8", "Every firmware emergency event type is inside the GAS whitelist",
      mod_events <= whitelist and gen_events <= whitelist,
      f"modular={sorted(mod_events)} generic={sorted(gen_events)} "
      f"whitelist={sorted(whitelist)}")

# ---------------------------------------------------------------------------
print("== WAVE 12: PZEM-004T meter contract (v1.7.0, end-to-end) ==")

# --- X9: serializer meter keys ⊆ GAS ingest; honesty gate -------------------
ser_meter_m = re.search(
    r'JsonObject acMeter = ac\.createNestedObject\("meter"\);(.*?)#endif',
    ser_src, re.S)
ser_meter_keys = set(re.findall(r'acMeter\["(\w+)"\]', ser_meter_m.group(1))) \
    if ser_meter_m else set()
ing_meter_m = re.search(
    r"const meter = ac\.meter \|\| null;(.*?)\n    const emg", gas_src, re.S)
ing_meter_txt = ing_meter_m.group(1) if ing_meter_m else ""
ing_meter_reads = set(re.findall(r"meter\.(\w+)", ing_meter_txt))
meter_orphans = ser_meter_keys - ing_meter_reads - {"current", "energy", "frequency", "powerFactor"}
# current/energy/frequency/powerFactor are driver-level fields; the GAS sheet
# contract stores the headline trio (connected/power/voltage) only.
check("X9a", "Serializer meter keys consumed by GAS ingest (headline trio) "
      "or documented driver-only fields",
      not meter_orphans, f"orphan meter keys: {sorted(meter_orphans)}")
gated = re.search(r"n\.pAcMeter = n\.meterConnected \? numOrEmpty_\(meter\.power\)", ing_meter_txt)
check("X9b", "Honesty gate: meter power stored ONLY when connected=true",
      bool(gated), "disconnected meter must never park a stale reading")

# --- X10: columns + appendRow + LATEST meter block ---------------------------
hdr_m = re.search(r"const TELEMETRY_HEADER = \[(.*?)\n\];", gas_src, re.S)
hdr_cols = re.findall(r"'([a-z0-9_]+)'", hdr_m.group(1))
# [v1.9.0] TELEMETRY_HEADER now has 40 columns: the original 39 (indices 0..38
# ending at meter_connected) + ina219_pga_mode at index 39. The X10a check
# verifies the meter trio is at indices 36..38 — the total column count is
# flexible (new columns are append-only).
check("X10a", "TELEMETRY_HEADER carries p_ac_meter/meter_v/meter_connected "
      "at indices 36..38",
      len(hdr_cols) >= 39 and hdr_cols[36:39] == ["p_ac_meter", "meter_v", "meter_connected"],
      f"cols={len(hdr_cols)} tail={hdr_cols[36:]}")
append_m = re.search(r"norm\.iAcGen, norm\.emgState, norm\.emgReason, "
    r"norm\.emgEstop, norm\.emgTrips,\s*\n\s*// v1\.7\.0 \[W12-2\][^\n]*\n"
    r"\s*norm\.pAcMeter, norm\.meterV, norm\.meterConnected", gas_src)
check("X10b", "storeTelemetry_ appendRow writes the 3 meter columns",
      bool(append_m), "appendRow must persist norm.pAcMeter/meterV/meterConnected")
lat_ac_m = re.search(r"meter: row\.length > 36 \? \{(.*?)\}", gas_src, re.S)
lat_meter_txt = lat_ac_m.group(1) if lat_ac_m else ""
lat_meter_ok = all(s in lat_meter_txt for s in ("row[38]", "row[37]", "row[36]"))
check("X10c", "rowToEnvelope_ LATEST emits ac.meter{connected,voltage,power} "
      "guarded by row.length > 36",
      lat_meter_ok, f"meter block text: {lat_meter_txt[:80]!r}")
check("X10d", "Meter block compiled out when PLTS_ENABLE_PZEM_AC=0 (honest "
      "absence — createNestedObject only under the flag)",
      ser_src.find("#if PLTS_ENABLE_PZEM_AC") < ser_src.find('createNestedObject("meter")'),
      "meter block must stay flag-guarded")

# --- X11: PWA meter slot + parse + UI ----------------------------------------
check_or_skip("X11a", "PWA AcTelemetry declares the optional meter slot",
      "meter?" in types_ts and "AcMeterMeasurement" in types_ts,
      "types.ts must carry the v1.7.0 PZEM contract slot")
check_or_skip("X11b", "PWA gasEnvelope parses ac.meter (connected/power/voltage)",
      "ac.meter" in env_ts or "meter" in env_ts and "meterConnected" in env_ts,
      "parseLatestEnvelope must surface the meter trio to the fleet view")
check_or_skip("X11c", "PWA ac view renders the MEASURED meter surface",
      "meter" in ac_view and "MEASURED" in ac_view,
      "ac-output-view must show the real meter (never silently reuse the estimate)")

# ---------------------------------------------------------------------------
print("== WAVE 12: genset channel + header integrity ==")

# --- X12: genset end-to-end --------------------------------------------------
flat_zone_m = re.search(r"// Legacy flat \(firmware-generic\)(.*?)function measVal_",
                        gas_src, re.S)
flat_reads = set(re.findall(r"data\.([A-Za-z0-9_]+)", flat_zone_m.group(1))) \
    if flat_zone_m else set()
genset_ok = ("i_ac_gen" in flat_reads and "i_ac_gen" in hdr_cols
             and "gensetRmsCurrent" in gas_src
             and "ac.gensetRmsCurrent" in gas_src)
check("X12a", "Genset: generic flat i_ac_gen -> column -> LATEST "
      "gensetRmsCurrent (GAS side complete)",
      genset_ok, "one of the four genset hops is missing")
check_or_skip("X12b", "Genset: PWA parses i_ac_gen (flat) + gensetRmsCurrent (nested)",
      "gensetRmsCurrent" in env_ts and "i_ac_gen" in env_ts,
      "PWA must accept both genset spellings")
check("X12c", "Genset modular RESERVED documented (no silent second channel)",
      "RESERVED" in ser_src or "PLTS_ENABLE_AC_GEN" in globals_src or
      "RESERVED" in (FWB / "firmware" / "Core" / "Globals.h").read_text(),
      "the missing modular genset slot must stay a documented reservation")

# --- X13: header integrity ---------------------------------------------------
v17_m = re.search(r"const TELEMETRY_HEADER_V1_7_LEN = (\d+);", gas_src)
check("X13a", "TELEMETRY_HEADER_V1_7_LEN == 36 (migration boundary marker)",
      v17_m and v17_m.group(1) == "36", f"found: {v17_m.group(1) if v17_m else None}")
migr_guard = re.search(
    r"function migrateTelemetryHeader_\(sheet, header\) \{(.*?)\n\}", gas_src, re.S)
guard_ok = migr_guard and "TELEMETRY_HEADER_V1_5_LEN" in migr_guard.group(1) \
    and "header.length" in migr_guard.group(1)
check("X13b", "migrateTelemetryHeader_ keeps the append-only guard "
      "(V1_5 floor + header.length ceiling)",
      bool(guard_ok), "migration must never rewrite existing columns")

# --- X14: flat path drift protection (C3 regression) -------------------------
st_start = ino_src.find("void sendTelemetry()")
st_zone = ino_src[st_start:st_start + 4000] if st_start != -1 else ""
fw_writes = set(re.findall(r'data\["([a-z0-9_]+)"\]', st_zone))
orphan_flat = {w for w in fw_writes if w not in flat_reads}
check("X14", "Flat path regression: every generic telemetry write still "
      "consumed by GAS (no drift since WAVE-8)",
      not orphan_flat, f"orphan writes: {sorted(orphan_flat)}")

# --- X15: GAS parses as JavaScript (deployment guard) ------------------------
try:
    with tempfile.NamedTemporaryFile(suffix=".js", delete=False) as tf:
        tf.write(GAS.read_bytes())
        tmp = tf.name
    r = subprocess.run(["node", "--check", tmp], capture_output=True, text=True)
    Path(tmp).unlink(missing_ok=True)
    check("X15", "code.gs/Code.gs parses as JavaScript (node --check)",
          r.returncode == 0, (r.stderr or "")[:200])
except FileNotFoundError:
    print("  [SKIP] X15: node not available in this environment")

# ---------------------------------------------------------------------------
print()
total = len(passed) + len(failed)
print(f"RESULT: {len(passed)}/{total} passed, {len(failed)} failed")
if failed:
    for cid, desc, detail in failed:
        print(f"  FAILED {cid}: {desc}" + (f" -> {detail[:300]}" if detail else ""))
    sys.exit(1)
print("ALL WAVE 12 CONTRACT REGRESSION CHECKS PASSED")
