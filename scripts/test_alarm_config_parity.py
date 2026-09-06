#!/usr/bin/env python3
"""
test_alarm_config_parity.py — [PARITY-4 2026-09-06] cross-layer contract test.

Verifies the operator-configurable two-tier alarm thresholds are a REAL
cross-layer feature (audit P0: the PWA Configuration Center displayed an
`alarmThresholds` schema no firmware ever served — a "feature ghost"):

  PWA AlarmThresholds type
      -> REST POST /api/config (flat fields, requestId, canonical path)
      -> MQTT config.update (same fields, same validation)
      -> CommandCanonicalizer whitelist (fail-closed)
      -> NVS "plts_alarm" persistence (ConfigStore, defaults-on-read)
      -> runtime consumer: AnomalyDetector (SINGLE evaluator, two-tier +
         hysteresis)
      -> readback: GET /api/config flat + nested alarmThresholds object

Also guards the PARITY-4 hardening:
  - config import carries transaction identity (X-Request-Id header +
    TransactionJournal; body stays CRC32-pure)
  - password change joins the canonical transaction path
  - BMS comm fields are whitelisted (was: silently rejected by the
    canonicalizer on BOTH REST and MQTT)
  - the duplicate voltage-alarm evaluator in firmware_v1.ino is gone

Run:  python3 scripts/test_alarm_config_parity.py
Exit: 0 = PASS, 1 = FAIL
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

ALARM_FIELDS = [
    "voltageLowWarn", "voltageLowCritical", "voltageHighWarn",
    "voltageHighCritical", "currentHighWarn", "currentHighCritical",
    "temperatureHighWarn", "temperatureHighCritical", "humidityHighWarn",
    "socLowWarn", "socLowCritical",
]

CFG_ALARM_GLOBALS = [
    "cfgAlarmVoltageLowWarnV", "cfgAlarmVoltageLowCriticalV",
    "cfgAlarmVoltageHighWarnV", "cfgAlarmVoltageHighCriticalV",
    "cfgAlarmCurrentHighWarnA", "cfgAlarmCurrentHighCriticalA",
    "cfgAlarmTemperatureHighWarnC", "cfgAlarmTemperatureHighCriticalC",
    "cfgAlarmHumidityHighWarnPct", "cfgAlarmSocLowWarnPct",
    "cfgAlarmSocLowCriticalPct",
]

failures = []


def check(name, ok, detail=""):
    status = "PASS" if ok else "FAIL"
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not ok else ""))
    if not ok:
        failures.append(name)


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def must_cont(fname, patterns):
    src = read(FW / fname)
    for pat in patterns:
        check(f"{fname} :: {pat}",
              re.search(pat, src) is not None,
              f"pattern not found in {fname}")


def must_not_cont(fname, patterns):
    src = read(FW / fname)
    for pat in patterns:
        check(f"{fname} MUST NOT match {pat}",
              re.search(pat, src) is None,
              f"forbidden pattern found in {fname}")


# --- 1. Authoritative storage layer -----------------------------------------
must_cont("Core/Config.h", [rf"#define|static constexpr float\s+ALARM_{'|'.join(ALARM_FIELDS[0].split('_')[:1])}"][:0] + [
    r"static constexpr float\s+ALARM_(VOLTAGE|CURRENT|TEMP|HUMIDITY|SOC)",          # defaults exist
    r"ALARM_VOLTAGE_LOW_WARN_V\s*=\s*46", r"ALARM_SOC_LOW_CRITICAL_PCT\s*=\s*10",
])
cfg_globals_src = read(FW / "Core/Globals.h")
for g in CFG_ALARM_GLOBALS:
    check(f"Globals.h extern {g}", rf"extern\s+float\s+{g}\s*;" in cfg_globals_src or
          f"extern float    {g}" in cfg_globals_src)

ino_src = read(FW / "firmware_v1.ino")
for g in CFG_ALARM_GLOBALS:
    check(f"firmware_v1.ino defines {g}", re.search(rf"float\s+{g}\s*=", ino_src) is not None)
check("setup() loads alarm config",
      re.search(r"Storage::config\.loadAlarmConfig\(\)", ino_src) is not None)
# Count only non-comment call sites (prose comments legitimately mention it).
_calls = [ln for ln in ino_src.splitlines()
          if "Storage::config.loadAlarmConfig()" in ln and not ln.strip().startswith("//")]
check("loadAlarmConfig called exactly once",
      len(_calls) == 1, f"found {len(_calls)} code call sites")

cs_src = read(FW / "Storage/ConfigStore.cpp")
check("ConfigStore: NVS namespace plts_alarm", 'p.begin("plts_alarm"' in cs_src)
check("ConfigStore: loadAlarmConfig sanitizes (isfinite clamp)",
      "std::isfinite" in cs_src and "clampf" in cs_src)
check("ConfigStore: saveAlarmConfig persists all 11 keys",
      all(k in cs_src for k in ["vLoW", "vLoC", "vHiW", "vHiC", "iW", "iC", "tW", "tC", "hW", "socW", "socC"]))
check("ConfigStore.h declares load/saveAlarmConfig",
      "loadAlarmConfig" in read(FW / "Storage/ConfigStore.h"))
check("exportAll carries alarmConfig backup",
      re.search(r'createNestedObject\("alarmConfig"\)', cs_src) is not None)
check("importAll restores + re-sanitizes alarmConfig",
      re.search(r'containsKey\("alarmConfig"\)', cs_src) is not None and
      re.search(r'saveAlarmConfig\(\);\s*\n\s*loadAlarmConfig\(\);', cs_src) is not None)

# --- 2. REST surface (GET/POST /api/config) ----------------------------------
ch_src = read(FW / "Web/ConfigHandlers.cpp")
check("GET /api/config: nested alarmThresholds object",
      'createNestedObject("alarmThresholds")' in ch_src)
for f in ALARM_FIELDS:
    check(f"GET /api/config emits flat {f}", f'doc["{f}"]' in ch_src)
    check(f"POST /api/config validates {f}", f'containsKey("{f}")' in ch_src)
check("POST /api/config persists via saveAlarmConfig",
      "saveAlarmConfig()" in ch_src)
check("POST /api/config enforces tier order",
      "voltageLowCritical must be < voltageLowWarn" in ch_src and
      "socLowCritical must be < socLowWarn" in ch_src)

# --- 3. Canonical command whitelist (fail-closed) -----------------------------
cc_src = read(FW / "Services/CommandCanonicalizer.cpp")
check("canonicalizer field slots >= 32", "fields[32]" in cc_src)
for f in ALARM_FIELDS:
    check(f"whitelist config.update :: {f}", f'"{f}"' in cc_src)
for f in ["bmsProtocol", "bmsPollIntervalMs", "bmsModbusSlaveId",
          "bmsModbusTcpHost", "bmsModbusTcpPort"]:
    check(f"whitelist config.update :: {f} (latent REST/MQTT reject fixed)", f'"{f}"' in cc_src)
check("whitelist config.password command",
      re.search(r'\{"config",\s*"password"', cc_src) is not None)

# --- 4. MQTT path (identical validation) --------------------------------------
mq_src = read(FW / "Network/MqttConfigReceiver.cpp")
for f in ["voltageLowWarn", "voltageHighCritical", "currentHighCritical",
          "humidityHighWarn", "socLowCritical"]:
    check(f"MQTT config.update applies {f}", f'containsKey("{f}")' in mq_src)
check("MQTT applies BMS comm fields", 'containsKey("bmsProtocol")' in mq_src)
check("MQTT live-reconfigures BMS comm", "batteryComm.reconfigure()" in mq_src)
check("MQTT persists alarm config", "saveAlarmConfig()" in mq_src)

# --- 5. Runtime consumer — SINGLE evaluator, two-tier + hysteresis -------------
ad_src = read(FW / "Services/AnomalyDetector.cpp")
for g in ["cfgAlarmVoltageLowCriticalV", "cfgAlarmVoltageHighWarnV",
          "cfgAlarmCurrentHighCriticalA", "cfgAlarmTemperatureHighWarnC",
          "cfgAlarmHumidityHighWarnPct", "cfgAlarmSocLowCriticalPct"]:
    check(f"AnomalyDetector consumes {g}", f"Core::{g}" in ad_src)
check("AnomalyDetector has clear-side hysteresis", "VOLTAGE_ALARM_HYST_V" in ad_src)
# Legacy hardcoded evaluation constants must be RETIRED from evaluation:
for legacy in [r"Core::BATTERY_HIGH_V\b", r"Core::OVERCURRENT_CHARGE_A",
               r"Core::TEMP_CRIT_THRESHOLD_C", r"Core::HUMIDITY_HIGH_PCT",
               r"Core::cfgFullVoltage\s*\*\s*1\.02f"]:
    check(f"AnomalyDetector retired {legacy}",
          re.search(legacy, ad_src) is None, "legacy threshold still evaluated")
# The duplicate evaluator in the .ino is gone:
must_not_cont("firmware_v1.ino", [r"lowAlarmActive", r"highAlarmActive"])
check("SOC discontinuity uses its own code",
      "BATTERY_SOC_DISCONTINUITY" in read(FW / "Core/Types.h") and
      "BATTERY_SOC_DISCONTINUITY" in ad_src)
check("REAL SOC-low evaluation present (finite-gated)",
      re.search(r"std::isfinite\(ctx\.soc\)", ad_src) is not None and
      "Battery SOC below critical threshold" in ad_src)

# --- 6. Transaction identity: import + password --------------------------------
ex_src = read(FW / "Web/ExtraHandlers.cpp")
check("password joins canonical path (config.password)",
      re.search(r'doc\["type"\]\s*=\s*"config";\s*\n\s*doc\["action"\]\s*=\s*"password";', ex_src) is not None)
check("password journaled after mutation",
      re.search(r'journal\.storeTransaction\(canon\.transactionId.*Password changed', ex_src, re.S) is not None)
check("import reads X-Request-Id header",
      'http.header("X-Request-Id")' in ex_src)
check("import journal: sha256(raw) dedup hash", "sha256Hex(raw)" in ex_src)
check("import replays stored ACK on duplicate",
      "requestId reuse with different import payload" in ex_src)
check("HttpServer collects X-Request-Id",
      '"X-Request-Id"' in read(FW / "Web/HttpServer.cpp"))
check("CORS allows X-Request-Id",
      'Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token, X-Request-Id"' in
      read(FW / "Web/HttpServer.cpp"))

# --- summary -------------------------------------------------------------------
print()
if failures:
    print(f"ALARM CONFIG PARITY = FAIL ({len(failures)} failure(s))")
    sys.exit(1)
print("ALARM CONFIG PARITY = PASS (all checks green)")
sys.exit(0)
