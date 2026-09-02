#!/usr/bin/env python3
# =============================================================================
# test_wave7_8_crosslayer.py — WAVE 7/8: MQTT/TLS security review + FULL
# cross-layer contract consistency test (PWA <-> GAS <-> firmware-generic).
# =============================================================================
# WAVE 7 (MQTT/TLS):
#   M1  PWA mqtt.ts must fail-closed on non-wss:// broker URLs (TLS-only)
#   M2  PWA must have NO public-broker fallback default in code
#   M3  Modular firmware: setInsecure() allowed ONLY on DEVELOPMENT_BUILD path
#   M4  PRODUCTION_BUILD #error guards for TLS/mqtt credentials present in
#       Config.h (port 8883/8884, username, password, root CA)
#   M5  MQTT secrets never printed in plaintext after generation (masked on
#       every boot; one-time reveal only at commissioning generation)
#
# WAVE 8 (cross-layer contract consistency — parsed from REAL sources):
#   C1  Emergency CONFIG schema: GAS EMERGENCY_CONFIG_FIELDS == PWA
#       EMERGENCY_CONFIG_FIELDS == firmware-generic EmergencyConfig struct
#       (field-for-field, range-for-range, default-for-default)
#   C2  Firmware emgApplyCommand range checks == schema ranges
#   C3  Telemetry flat path: every data["X"] the firmware writes is read by
#       GAS normalizeEnvelope_ (flat path) — no orphan writes
#   C4  Every flat field GAS reads has a header column (TELEMETRY_HEADER)
#   C5  MQTT topic contract: firmware suffixes {status,log,online,ack,config,ota}
#       — PWA subscribes to a subset; no unknown suffix
#   C6  Version identity: FIRMWARE_VERSION == manifest.json version ==
#       binary filename
#   C7  ACK contract: firmware ACK keys == GAS/PWA-documented ACK keys
# =============================================================================
import json
import re
import sys
from pathlib import Path

# Repo layout: this script lives in <fw-repo>/scripts/; the PWA repo is a
# sibling checkout by default (override with PLTS_PWA_REPO env var).
FWB = Path(__file__).resolve().parent.parent
PWA = Path(__file__).resolve().parent.parent.parent / "PLTSMonitoring_PWA"
import os
if os.environ.get("PLTS_PWA_REPO"):
    PWA = Path(os.environ["PLTS_PWA_REPO"])

GAS = FWB / "code.gs" / "Code.gs"
INO = FWB / "firmware-generic" / "src" / "plts_firmware_v1.ino"
MANIFEST = FWB / "firmware-generic" / "manifest.json"
CFG_H = FWB / "firmware" / "Core" / "Config.h"
MT_CPP = FWB / "firmware" / "Network" / "MqttTransport.cpp"
CS_CPP = FWB / "firmware" / "Storage" / "ConfigStore.cpp"
MQTT_TS = PWA / "src" / "lib" / "mqtt.ts"
EMERGENCY_TS = PWA / "src" / "lib" / "emergency.ts"
ENV_EXAMPLE = PWA / ".env.example"

passed, failed = [], []


def check(cid, desc, cond, detail=""):
    if cond:
        passed.append(cid)
        print(f"  [PASS] {cid}: {desc}")
    else:
        failed.append((cid, desc, detail))
        print(f"  [FAIL] {cid}: {desc}" + (f"  -> {detail}" if detail else ""))


# --- PWA availability: single-repo CI checkouts skip PWA-side checks ---------
PWA_AVAILABLE = PWA.exists() and (PWA / "src" / "lib" / "mqtt.ts").exists()
if not PWA_AVAILABLE:
    print("  [SKIP] PWA repo not present (single-repo CI checkout) — "
          "PWA-side checks (M1/M2/C1a/C1b/C5b) skipped; set PLTS_PWA_REPO "
          "to run the full cross-layer suite")


def check_or_skip(cid, desc, cond, detail=""):
    if not PWA_AVAILABLE:
        print(f"  [SKIP] {cid}: {desc} (PWA repo absent)")
        return
    check(cid, desc, cond, detail)

# ---------------------------------------------------------------------------
print("== WAVE 7: MQTT/TLS security review ==")

# --- M1: PWA mqtt.ts wss:// scheme guard (fail-closed TLS) ----------------
mqtt_src = MQTT_TS.read_text(encoding="utf-8") if PWA_AVAILABLE else ""
# Strip comments so a 'wss://' mention in a comment cannot fake a pass.
mqtt_code = re.sub(r"//.*", "", mqtt_src)
mqtt_code = re.sub(r"/\*.*?\*/", "", mqtt_code, flags=re.S)
has_scheme_guard = bool(re.search(
    r'protocol\s*===?\s*[\'"]wss:|startsWith\(\s*[\'"]wss://|'
    r'scheme\s*===?\s*[\'"]wss',
    mqtt_code))
check_or_skip("M1", "PWA connectMqtt() enforces wss:// (TLS-only broker URL) in CODE",
      has_scheme_guard,
      "no wss:// scheme validation in connectMqtt() — a ws:// (plaintext) "
      "broker URL is silently accepted in production builds")

# --- M2: no public-broker default ------------------------------------------
public_brokers = ["broker.hivemq.com", "broker.emqx.io", "test.mosquitto.org",
                  "public.mqtthq.com"]
mqtt_defaults = re.findall(r"MQTT_BROKER_URL\s*=\s*([^;]+);", mqtt_src)
leaked = [pb for pb in public_brokers if any(pb in d for d in mqtt_defaults)]
check_or_skip("M2", "PWA has no public-broker fallback default", not leaked,
      f"public broker(s) present as default: {leaked}")

# --- M3: setInsecure only on DEVELOPMENT_BUILD path ------------------------
mt_src = MT_CPP.read_text(encoding="utf-8")
mt_code = re.sub(r"//.*", "", mt_src)
mt_code = re.sub(r"/\*.*?\*/", "", mt_code, flags=re.S)
insecure_idx = mt_code.find("_tls.setInsecure()")
guard_ok = insecure_idx == -1
if not guard_ok:
    pre = mt_code[:insecure_idx]
    dev_guard = pre.rfind("#elif defined(DEVELOPMENT_BUILD)")
    ca_guard = pre.rfind("#ifdef MQTT_ROOT_CA")
    guard_ok = dev_guard > ca_guard and dev_guard != -1
check("M3", "Modular firmware: setInsecure() reachable ONLY via DEVELOPMENT_BUILD",
      guard_ok, "setInsecure() not gated behind #elif defined(DEVELOPMENT_BUILD)")

# --- M4: PRODUCTION_BUILD TLS guards in Config.h ---------------------------
cfg_src = CFG_H.read_text(encoding="utf-8")
guards = {
    "port 8883/8884": r"MQTT_BROKER_PORT != 8883 && MQTT_BROKER_PORT != 8884",
    "username/password": r"requires MQTT_USERNAME and MQTT_PASSWORD",
    "root CA": r"requires MQTT_ROOT_CA",
}
for name, pat in guards.items():
    check("M4-" + name.split()[0],
          f"Config.h PRODUCTION_BUILD #error: {name}",
          bool(re.search(pat, cfg_src)))

# --- M5: MQTT password masked on every boot after generation ---------------
cs_src = CS_CPP.read_text(encoding="utf-8")
masked_boot = "maskSecret_(Core::mqttPassword)" in cs_src
plain_prints = re.findall(
    r'Serial\.printf\([^)]*mqttPassword[^)]*\)', cs_src)
gen_reveal_only = len(plain_prints) == 1 and "BARU" in cs_src
check("M5", "MQTT password: masked every boot, plaintext ONLY at generation",
      masked_boot and gen_reveal_only,
      f"plain mqttPassword printf count = {len(plain_prints)} (expected 1, "
      "the generation-time reveal)")

# ---------------------------------------------------------------------------
print("== WAVE 8: cross-layer contract consistency ==")

# --- C1: Emergency CONFIG schema parity (GAS <-> PWA <-> firmware) ---------
gas_src = GAS.read_text(encoding="utf-8")
emg_ts_src = EMERGENCY_TS.read_text(encoding="utf-8") if PWA_AVAILABLE else ""
ino_src = INO.read_text(encoding="utf-8")

# GAS fields: ['name', min, max, dflt]
gas_m = re.search(r"const EMERGENCY_CONFIG_FIELDS = \[(.*?)\n\];", gas_src, re.S)
gas_fields = {}
for f in re.findall(
        r"\['([A-Za-z0-9_]+)',\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)\]",
        gas_m.group(1)):
    gas_fields[f[0]] = (float(f[1]), float(f[2]), float(f[3]))

# PWA fields: { key, min, max, dflt }
pwa_m = re.search(
    r"export const EMERGENCY_CONFIG_FIELDS[^=]+= \[(.*?)\n\];", emg_ts_src, re.S)
pwa_fields = {}
if pwa_m:   # absent PWA repo (single-repo CI) → C1a/C1b report SKIP
    for f in re.findall(
            r'key:\s*"([A-Za-z0-9_]+)",\s*min:\s*(-?[\d.]+),\s*max:\s*(-?[\d.]+),'
            r'\s*dflt:\s*(-?[\d.]+)', pwa_m.group(1)):
        pwa_fields[f[0]] = (float(f[1]), float(f[2]), float(f[3]))

# Firmware struct defaults + ranges from comments [a..b]
fw_struct_m = re.search(
    r"struct EmergencyConfig \{(.*?)\n\};", ino_src, re.S)
fw_defaults, fw_ranges = {}, {}
for line in fw_struct_m.group(1).splitlines():
    d = re.match(
        r"\s*\w+\s+(\w+)\s*=\s*(-?[\d.]+)f?\s*;.*\[\s*(-?[\d.]+)\s*\.\.\s*(-?[\d.]+)\s*\]",
        line)
    if d:
        fw_defaults[d.group(1)] = float(d.group(2))
        fw_ranges[d.group(1)] = (float(d.group(3)), float(d.group(4)))

check_or_skip("C1a", "Emergency schema: GAS / PWA / firmware all define 13 fields",
      len(gas_fields) == len(pwa_fields) == len(fw_defaults) == 13,
      f"GAS={len(gas_fields)} PWA={len(pwa_fields)} FW={len(fw_defaults)}")

mismatch = []
for name, (mn, mx, df) in gas_fields.items():
    p = pwa_fields.get(name)
    f_d = fw_defaults.get(name)
    f_r = fw_ranges.get(name)
    if p is None or f_d is None:
        mismatch.append(f"{name}: missing in {'PWA' if p is None else 'firmware'}")
        continue
    if (mn, mx, df) != p:
        mismatch.append(f"{name}: GAS{mn,mx,df} != PWA{p}")
    if df != f_d:
        mismatch.append(f"{name}: default GAS {df} != FW {f_d}")
    if f_r and (mn, mx) != f_r:
        mismatch.append(f"{name}: range GAS{(mn, mx)} != FW {f_r}")
check_or_skip("C1b", "Emergency schema: ranges + defaults identical across 3 layers",
      not mismatch, "; ".join(mismatch))

# --- C2: firmware emgApplyCommand range checks == schema ---------------------
# Zone: the CONFIG branch inside emgApplyCommand (runtime command path) --
# the same checks exist in loadConfig() for persisted JSON.
apply_m = re.search(r"void emgApplyCommand\(.*?\{(.*?)\n\}", ino_src, re.S)
apply_zone = apply_m.group(1) if apply_m else ""
checks_found = {}
for name in gas_fields:
    pat = (rf'(?:emg|cfg)\["{name}"\][^\n]*?'
           r'>=\s*(-?[\d.]+)f?\s*&&\s*\w+\s*<=\s*(-?[\d.]+)f?')
    m = re.search(pat, apply_zone)
    if m:
        checks_found[name] = (float(m.group(1)), float(m.group(2)))
    else:  # boolean 0/1 policy fields use == 0 || == 1
        if re.search(
                rf'(?:emg|cfg)\["{name}"\][^\n]*?==\s*0[^\n]*?\|\|[^\n]*?==\s*1',
                apply_zone):
            checks_found[name] = gas_fields[name][:2]
bad = [n for n, r in checks_found.items() if gas_fields[n][:2] != r]
missing = [n for n in gas_fields if n not in checks_found]
check("C2", "Firmware CONFIG apply range-checks match the 13-field schema",
      not bad and not missing,
      f"mismatch={bad} missing-range-check={missing}")

# --- C3: telemetry flat fields firmware writes <-> GAS reads ---------------
# Only the telemetry serialization (sendTelemetry) -- other JsonDocuments in
# the .ino (OTA manifest, pending commands) are NOT telemetry contract.
st_start = ino_src.find("void sendTelemetry()")
st_zone = ino_src[st_start:st_start + 4000] if st_start != -1 else ""
fw_writes = set(re.findall(r'data\["([a-z0-9_]+)"\]', st_zone))
# Flat path inside normalizeEnvelope_ (the '// Legacy flat' comment marks it)
ne_start = gas_src.rfind("// Legacy flat (firmware-generic)")
ne_end = gas_src.find("function measVal_")
ne_zone = gas_src[ne_start:ne_end] if ne_start != -1 else ""
flat_zone_reads = set(re.findall(r"data\.([A-Za-z0-9_]+)", ne_zone))
orphans = {w for w in fw_writes if w not in flat_zone_reads}
check("C3", "Every telemetry field the firmware writes is consumed by GAS "
      "(flat path)", not orphans,
      f"orphan writes (written by FW, not read by GAS): {sorted(orphans)}")

# --- C4: flat fields read by GAS map to TELEMETRY_HEADER columns ----------
hdr_m = re.search(r"const TELEMETRY_HEADER = \[(.*?)\n\];", gas_src, re.S)
hdr_cols = set(re.findall(r"'([a-z0-9_]+)'", hdr_m.group(1)))
read_to_col = {
    "v_bat": "v_bat", "i_bat_dc": "i_bat_dc", "p_bat_dc": "p_bat_dc",
    "i_ac_load": "i_ac_load", "i_ac_gen": "i_ac_gen",
    "emg_state": "emg_state", "emg_reason": "emg_reason",
    "emg_estop": "emg_estop", "emg_trips": "emg_trips",
    "ina219_ok": "ina219_ok", "free_heap": "free_heap", "rssi": "rssi",
    "fw_version": "fw_version", "temp_celsius": "temp_celsius",
    "sequence": "sequence", "soc": "soc", "humidity": "humidity",
}
unmapped = [c for r, c in read_to_col.items()
            if r in flat_zone_reads and c not in hdr_cols]
check("C4", "Every flat-path field GAS reads has a TELEMETRY_HEADER column",
      not unmapped, f"missing columns: {unmapped}")

# --- C5: MQTT topic contract ------------------------------------------------
fw_suffixes = set(re.findall(r'getDeviceTopic\("([a-z]+)"\)',
    (MT_CPP.read_text() + "\n" +
     (FWB / "firmware" / "Network" / "MqttConfigReceiver.cpp").read_text() + "\n" +
     (FWB / "firmware" / "Network" / "MqttOtaHandler.cpp").read_text() + "\n" +
     (FWB / "firmware" / "Network" / "MqttTelemetryPublisher.cpp").read_text())))
pwa_subs = set(re.findall(r"`\$\{baseTopic\}/([a-z]+)`", mqtt_src))
allowed = {"status", "log", "online", "ack", "config", "ota"}
check("C5a", "Firmware MQTT topic suffixes within documented contract",
      fw_suffixes <= allowed, f"unknown suffixes: {fw_suffixes - allowed}")
check_or_skip("C5b", "PWA subscribes only to monitoring topics (no command topics)",
      bool(pwa_subs) and pwa_subs <= {"status", "log", "online"},
      f"PWA subscription set: {pwa_subs}")

# --- C6: version identity ----------------------------------------------------
ino_ver = re.search(r'FIRMWARE_VERSION\s*=\s*"([\d.]+)"', ino_src).group(1)
man = json.loads(MANIFEST.read_text())
man_ver = man["version"]
bin_part = next(p["path"] for p in man["builds"][0]["parts"] if "plts_firmware" in p["path"])
bin_ver = re.search(r"v([\d.]+)\.bin", bin_part).group(1)
bin_exists = (FWB / "firmware-generic" / bin_part).exists()
check("C6", f"Version identity: ino={ino_ver} manifest={man_ver} bin={bin_ver}",
      ino_ver == man_ver == bin_ver and bin_exists,
      f"drift or missing binary: {bin_part} exists={bin_exists}")

# --- C7: ACK contract --------------------------------------------------------
# Firmware emgAck() body keys must be a superset of what GAS emergencyAck_
# consumes (command_id, result, message, state) -- both parsed from code.
emg_ack_zone = ino_src[ino_src.find("void emgAck("):ino_src.find("void emgApplyCommand")]
fw_ack_keys = set(re.findall(r'ack\["([A-Za-z0-9_]+)"\]', emg_ack_zone))
gas_ack_reads = set()
for f in ("command_id", "result", "message", "state"):
    if re.search(
            rf"payload\.{f}\s*!==\s*undefined\s*\?\s*payload\.{f}\s*:\s*body\.{f}",
            gas_src):
        gas_ack_reads.add(f)
check("C7", "Firmware emergency ACK keys == keys GAS consumes "
      "(command_id/result/message/state)",
      bool(gas_ack_reads) and gas_ack_reads <= fw_ack_keys,
      f"missing in firmware ACK: {gas_ack_reads - fw_ack_keys} "
      f"(firmware sends: {sorted(fw_ack_keys)})")

# ---------------------------------------------------------------------------
print("== WAVE 9: HMAC canonicalization + nonce/replay cross-layer ==")

# --- H1: canonical string ordering identical (firmware <-> GAS) -------------
GA_CPP = FWB / "firmware" / "AI" / "GasAdvisor.cpp"
ga_src = GA_CPP.read_text(encoding="utf-8")
fw_canon = re.search(
    r'String canonical = String\("HMAC-SHA256"\) \+ "\\n" \+\s*'
    r'String\(action\) \+ "\\n" \+\s*String\(timestamp\) \+ "\\n" \+\s*'
    r'String\(nonce\) \+ "\\n" \+\s*String\(deviceId\) \+ "\\n" \+\s*dataDigest',
    ga_src)
gas_canon = re.search(
    r"const canonical = 'HMAC-SHA256\\n' \+ action \+ '\\n' \+ auth\.timestamp \+ '\\n' \+\s*"
    r"auth\.nonce \+ '\\n' \+ deviceKey \+ '\\n' \+ dataDigest", gas_src)
check("H1", "HMAC canonical string component order identical (FW GasAdvisor "
      "<-> GAS verifyHmac_)", bool(fw_canon and gas_canon),
      f"fw_match={bool(fw_canon)} gas_match={bool(gas_canon)}")

# --- H2: nonce cache TTL >= replay window ------------------------------------
ttl = int(re.search(r"cache\.put\(nonceKey, '1', (\d+)\)", gas_src).group(1))
window = int(re.search(r"skew > (\d+)", gas_src).group(1))
check("H2", f"Nonce cache TTL ({ttl}s) >= replay window ({window}s) — a "
      "timestamp still in-window always has a cached nonce",
      ttl >= window, f"TTL={ttl} window={window}")

# --- H3: ADMIN_TOKEN constant-time comparison --------------------------------
ct = re.search(r"diff \|= g\.charCodeAt\(i\) \^ expected\.charCodeAt\(i\)", gas_src)
check("H3", "verifyAdminToken_ uses constant-time comparison", bool(ct))

# --- H4: OTA per-device HMAC derivation parameter order parity ---------------
fw_derive = re.search(
    r"hmacSha256Hex\(config\.token, config\.deviceKey\)", ino_src)
fw_verify = re.search(
    r"hmacSha256Hex\(otaKey, m\.version \+ \"\|\" \+ m\.url \+ \"\|\" \+ m\.sha256\)", ino_src)
gas_derive = re.search(
    r"computeHmacSha256Signature\(\s*String\(deviceKey\), authToken,", gas_src)
gas_verify = re.search(
    r"computeHmacSha256Signature\(\s*message, derived,", gas_src)
fw_sig_order = "hmacSha256Hex(const String& secret, const String& message)" \
    in ino_src and "mbedtls_md_hmac(info,\n                  (const uint8_t*)secret.c_str()" in ino_src
check("H4", "OTA per-device HMAC derivation byte-identical (key/message order "
      "parity FW <-> GAS)", all([fw_derive, fw_verify, gas_derive, gas_verify,
                                 fw_sig_order]),
      f"fw_derive={bool(fw_derive)} fw_verify={bool(fw_verify)} "
      f"gas_derive={bool(gas_derive)} gas_verify={bool(gas_verify)} "
      f"fw_sig_order={fw_sig_order}")

# --- H5: nonce reserved only AFTER signature verification --------------------
sig_pos = gas_src.find("signature mismatch")
put_pos = gas_src.find("cache.put(nonceKey")
check("H5", "Nonce cache put AFTER signature check (failed sigs never burn "
      "a nonce)", -1 < sig_pos < put_pos,
      f"sig_pos={sig_pos} put_pos={put_pos}")

# ---------------------------------------------------------------------------
print("== WAVE 10: GAS security — queue rotation + lock atomicity ==")

# --- GQ1: EmergencyQueue rotation exists and is wired -------------------------
rot_def = re.search(r"function rotateEmergencyQueue_\(\)", gas_src) is not None
rot_cfg = "EMERGENCY_QUEUE_MAX_ROWS" in gas_src and "['EMERGENCY_QUEUE_MAX_ROWS', '200']" in gas_src
rot_call = re.search(r"rotateEmergencyQueue_\(\);", gas_src) is not None
check("GQ1", "EmergencyQueue growth bounded (rotator defined, config key, "
      "invoked after append)", rot_def and rot_cfg and rot_call,
      f"def={rot_def} cfg={rot_cfg} call={rot_call}")

# --- GQ2: emergencyCommand_ critical section under lock -----------------------
cmd_lock = re.search(
    r"function emergencyCommand_\(.*?tryLock\(10000\).*?"
    r"emergencyCommandLocked_\(.*?releaseLock\(\)", gas_src, re.S) is not None
check("GQ2", "emergencyCommand_ wraps check-then-append in tryLock/finally",
      cmd_lock, "no lock around the emergency command critical section")

# --- GQ3: emergencyAck_ critical section under lock ---------------------------
ack_lock = re.search(
    r"function emergencyAck_\(.*?tryLock\(10000\).*?"
    r"emergencyAckLocked_\(.*?releaseLock\(\)", gas_src, re.S) is not None
check("GQ3", "emergencyAck_ wraps scan+settle+event in tryLock/finally",
      ack_lock, "no lock around the emergency ACK critical section")

# --- GQ4: every mutating sheet has a rotation bound ---------------------------
rotating = {"Telemetry": "rotateLogs_", "Ota": "rotateOtaManifests_",
            "OtaEvents": "rotateOtaEvents_", "Calibration": "rotateCalibrationHistory_",
            "EmergencyEvents": "EMERGENCY_EVENTS_MAX_ROWS",
            "EmergencyQueue": "rotateEmergencyQueue_"}
missing_rot = [s for s, marker in rotating.items() if marker not in gas_src]
check("GQ4", "Every growth-bearing sheet has a rotation bound", not missing_rot,
      f"sheets without rotation: {missing_rot}")

# ---------------------------------------------------------------------------
total = len(passed) + len(failed)
print(f"\n== RESULT: {len(passed)}/{total} PASS, {len(failed)} FAIL ==")
for cid, desc, detail in failed:
    print(f"   FAIL {cid}: {desc}\n        {detail}")
sys.exit(1 if failed else 0)
