#!/usr/bin/env python3
# =============================================================================
# test_wave13_ota_mixedfleet.py — WAVE 13: mixed-fleet OTA compatibility +
# end-to-end OTA review regression (firmware-generic flat + firmware modular
# in ONE GAS deployment, OTA trust chain, rollback semantics).
# =============================================================================
# Background (W13 findings fixed by this wave — these checks lock them in):
#   W13-1  Modular OTA rollback protection was INERT: markBootHealthy() never
#          called esp_ota_mark_app_valid_cancel_rollback() (so every modular
#          OTA silently reverted on the first post-update reset —
#          CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y makes the bootloader
#          revert unconfirmed PENDING_VERIFY images), and the boot-attempt
#          counter was incremented at UPLOAD time (once per upload, never
#          per boot) — the >= 3 threshold was unreachable dead code, and the
#          download path never touched the counter at all.
#   W13-2  The GAS OTA manifest had no firmware-tree discrimination: ONE
#          active manifest was served to EVERY device. A generic image
#          (1.1 MB, fits both trees' OTA slots) cross-flashed onto a modular
#          device through the shared HMAC trust domain; a modular image on a
#          generic device failed Update.begin and retried hourly forever.
#   W13-3  OtaEvents was a write-only sheet: firmware-generic has reported
#          ACTIVATED/ROLLBACK/DOWNLOAD_FAILED/REFUSED since WAVE-6, but no
#          action could read them back, and the modular state machine's
#          documented VERIFICATION_FAILED event was rejected by the GAS
#          word list.
#
# Checks (parsed from REAL sources — no fixtures):
#   X1   Modular OtaManager calls esp_ota_mark_app_valid_cancel_rollback()
#        inside markBootHealthyIfPending() (the missing W13-1 confirm call)
#   X2   begin() counts a boot attempt ONLY under PENDING_VERIFY; the
#        upload-time pre-increment is gone (dead semantics removed)
#   X3   firmware_v1.ino drives markBootHealthyIfPending() from the 1 s main
#        loop and NO LONGER unconditionally calls markBootHealthy() in setup
#   X4   Core::OTA_HEALTHY_AFTER_MS exists and matches generic's 60 s window
#        (one activation criterion across both trees)
#   X5   Generic tree still has its (correct, pre-W13) rollback pair:
#        handleOtaRollback (tries++ per PENDING_VERIFY boot) +
#        markOtaHealthyIfPending (60 s + PENDING_VERIFY gate) — no drift
#   X6   GAS OTA_HEADER gains column 7 'target'; OTA_TARGETS whitelist
#        ('' / generic / modular); publish validates + persists it
#   X7   GAS otaGetManifest_ serves the newest manifest whose target matches
#        the device's declared DEVICES!firmware_type (fail-closed: an
#        undeclared device sees only fleet-wide manifests); response echoes
#        the target; DEVICES_HEADER gains column 6 'firmware_type';
#        migrateAppendHeader_ extends old sheets in place
#   X8   BEHAVIORAL simulation of latestManifestForTarget_ + the device-side
#        gates: generic refuses target != 'generic', modular refuses
#        target != 'modular', '' passes for both (fleet-wide compat)
#   X9   Generic fetchOtaManifest refuses a foreign target BEFORE any
#        download (honest REFUSED OTA_STATUS event)
#   X10  Modular MQTT ota.start target self-check + ota.check manifest target
#        self-check + CommandCanonicalizer whitelists the 'target' field
#   X11  GAS OTA_STATUS word list accepts VERIFICATION_FAILED (modular state
#        machine vocabulary)
#   X12  GAS OTA_LOG read action exists, is routed in doPost, and reads the
#        OtaEvents sheet per-device (newest-first, bounded)
#   X13  PWA OTA history prefers real GAS OTA_LOG events with mock fallback;
#        OtaHistoryEntry carries the raw event verb + message
#   X14  PWA publish panel offers the target selector and sends
#        manifest.target
#   X15  Version identity: both trees at 1.7.1; generic manifest.json ==
#        PWA manifest.json == 1.7.1; the synced v1.7.1 binary exists in BOTH
#        repos and the stale v1.7.0 binary is gone
#   X16  GAS Code.gs parses as JavaScript (node --check) — deployment guard
# =============================================================================
import json
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
OTA_MGR_CPP = FWB / "firmware" / "Services" / "OtaManager.cpp"
OTA_MGR_H = FWB / "firmware" / "Services" / "OtaManager.h"
FW1 = FWB / "firmware" / "firmware_v1.ino"
CFG_H = FWB / "firmware" / "Core" / "Config.h"
CANON_CPP = FWB / "firmware" / "Services" / "CommandCanonicalizer.cpp"
MQTT_OTA_CPP = FWB / "firmware" / "Network" / "MqttOtaHandler.cpp"
OTA_VIEW = PWA / "src" / "components" / "ota" / "ota-view.tsx"
SIGN_PANEL = PWA / "src" / "components" / "settings" / "ota-signing-panel.tsx"
TYPES_TS = PWA / "src" / "lib" / "types.ts"

passed, failed = [], []


def check(cid, desc, cond, detail=""):
    if cond:
        passed.append(cid)
        print(f"  [PASS] {cid}: {desc}")
    else:
        failed.append((cid, desc, detail))
        print(f"  [FAIL] {cid}: {desc}" + (f"  -> {detail}" if detail else ""))


PWA_AVAILABLE = PWA.exists() and (PWA / "src" / "lib" / "types.ts").exists()
if not PWA_AVAILABLE:
    print("  [SKIP] PWA repo not present (single-repo CI checkout) — "
          "PWA-side checks degrade to SKIP")


def check_or_skip(cid, desc, cond, detail=""):
    if not PWA_AVAILABLE:
        print(f"  [SKIP] {cid}: {desc} (PWA repo absent)")
        return
    check(cid, desc, cond, detail)


gas_src = GAS.read_text()
ino_src = INO.read_text()
ota_cpp = OTA_MGR_CPP.read_text()
ota_h = OTA_MGR_H.read_text()
fw1_src = FW1.read_text()
cfg_h = CFG_H.read_text()
canon_cpp = CANON_CPP.read_text()
mqtt_ota_cpp = MQTT_OTA_CPP.read_text()

print("== W13-1: modular OTA rollback semantics (bootloader contract) ==")

# --- X1: the missing confirm call now exists in the right place -----------
mbhi = ota_cpp[ota_cpp.find("void OtaManager::markBootHealthyIfPending()"):
               ota_cpp.find("void OtaManager::markBootHealthy()")]
check("X1", "OtaManager::markBootHealthyIfPending() calls "
      "esp_ota_mark_app_valid_cancel_rollback() (the W13-1 fix)",
      "esp_ota_mark_app_valid_cancel_rollback()" in mbhi and len(mbhi) > 0,
      "confirm call missing from markBootHealthyIfPending body")

# --- X2: boot counting semantics -------------------------------------------
begin_zone = ota_cpp[ota_cpp.find("void OtaManager::begin()"):
                     ota_cpp.find("bool OtaManager::shouldRollback()")]
upload_zone = ota_cpp[ota_cpp.find("bool OtaManager::beginUpload("):
                      ota_cpp.find("bool OtaManager::feedChunk(")]
check("X2a", "begin() increments boot_att only under PENDING_VERIFY "
      "(per-boot semantics, not per-upload)",
      "_pendingVerify" in begin_zone and "_bootAttempts++" in begin_zone,
      "begin() must count PENDING_VERIFY boots")
check("X2b", "beginUpload() no longer pre-increments boot_att "
      "(dead code removed)",
      "putUInt(\"boot_att\"" not in upload_zone,
      "upload-time increment must be gone")
check("X2c", "markBootHealthyIfPending is declared in the header and driven "
      "with a one-shot flag",
      "markBootHealthyIfPending" in ota_h and "_markedValid" in ota_h,
      "header declaration missing")

# --- X3: call sites --------------------------------------------------------
loop_zone = fw1_src[fw1_src.find("void loop()"):
                    fw1_src.find("void loop()") + 400]
setup_zone = fw1_src[fw1_src.find("void setup()"):
                     fw1_src.find("void loop()")]
check("X3a", "modular loop() drives markBootHealthyIfPending() each tick",
      "markBootHealthyIfPending" in loop_zone,
      "loop() must confirm the OTA image after the stable window")
check("X3b", "setup() no longer unconditionally calls markBootHealthy() "
      "(crash-looping images must not be marked healthy at boot)",
      "Services::ota.markBootHealthy();" not in setup_zone,
      "setup() still calls markBootHealthy directly")

# --- X4: one activation criterion across both trees ------------------------
check("X4", "Core::OTA_HEALTHY_AFTER_MS == 60000 (matches generic's 60 s)",
      re.search(r"OTA_HEALTHY_AFTER_MS\s*=\s*60000", cfg_h) is not None,
      "Config.h must define the shared 60 s window")

# --- X5: generic tree rollback pair unchanged (no drift) -------------------
check("X5a", "generic handleOtaRollback counts tries under PENDING_VERIFY",
      "ESP_OTA_IMG_PENDING_VERIFY" in ino_src and "nvsSetBootTries(tries)" in ino_src,
      "generic rollback accounting drifted")
check("X5b", "generic markOtaHealthyIfPending marks valid + reports ACTIVATED",
      "esp_ota_mark_app_valid_cancel_rollback()" in ino_src and
      'reportOtaStatus("ACTIVATED"' in ino_src,
      "generic activation reporting drifted")

print("== W13-2: mixed-fleet manifest targeting ==")

# --- X6: schema -------------------------------------------------------------
check("X6a", "GAS OTA_HEADER has column 7 'target'",
      re.search(r"OTA_HEADER\s*=\s*\[[^\]]*'published_at',\s*'target'\s*\]",
                gas_src) is not None,
      "target column missing from OTA_HEADER")
check("X6b", "GAS OTA_TARGETS whitelist ('' / generic / modular)",
      re.search(r"OTA_TARGETS\s*=\s*\[\s*'',\s*'generic',\s*'modular'\s*\]",
                gas_src) is not None,
      "whitelist missing")
pub_zone = gas_src[gas_src.find("function otaPublishManifest_("):
                   gas_src.find("function otaPublishManifest_(") + 2000]
check("X6c", "publish validates target against the whitelist and persists it",
      "OTA_TARGETS.indexOf(target)" in pub_zone and "target\n  ]);" in pub_zone,
      "publish must validate + store target")

# --- X7: serve-side filtering ----------------------------------------------
check("X7a", "otaGetManifest_ filters by device firmware_type "
      "(latestManifestForTarget_ + deviceFirmwareType_)",
      "latestManifestForTarget_(rows, deviceType)" in gas_src and
      "deviceFirmwareType_(" in gas_src,
      "serve-side filter missing")
check("X7b", "manifest response echoes the target for device-side self-check",
      re.search(r"target:\s*String\(row\[6\]", gas_src) is not None,
      "target not echoed in OTA_MANIFEST response")
check("X7c", "DEVICES_HEADER gains optional column 6 'firmware_type'",
      re.search(r"DEVICES_HEADER\s*=\s*\[[^\]]*'last_ts',\s*'firmware_type'\s*\]",
                gas_src) is not None,
      "firmware_type column missing from DEVICES_HEADER")
check("X7d", "migrateAppendHeader_ extends old Ota/Devices sheets in place "
      "(append-only discipline)",
      "function migrateAppendHeader_(" in gas_src and
      "migrateAppendHeader_(sheet, header, 6)" in gas_src and
      "migrateAppendHeader_(sheet, header, 5)" in gas_src,
      "append-only migration missing")

# --- X8: BEHAVIORAL simulation of the target contract -----------------------
# Extract latestManifestForTarget_ from Code.gs and run it against scenarios.
fn_src = gas_src[gas_src.find("function latestManifestForTarget_"):]
fn_src = fn_src[:fn_src.find("\n}\n") + 3]
js = """
function run(rows, deviceType) {
%s
  return JSON.stringify(latestManifestForTarget_(rows, deviceType));
}
""" % fn_src
scenarios = [
    # rows mirror getDataRange().getValues(): row 0 is the HEADER, data from 1.
    # data row: [version, url, sha256, hmac, size, published_at, target]
    ("fleet-wide only, undeclared device", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.1", "https://a", "s1", "h1", 1, 0, ""],
    ], "", "1.7.1"),
    ("fleet-wide only, generic device", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.1", "https://a", "s1", "h1", 1, 0, ""],
    ], "generic", "1.7.1"),
    ("targeted generic row newest — generic device takes it", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.0", "https://a", "s1", "h1", 1, 0, ""],
        ["1.7.1", "https://b", "s2", "h2", 1, 1, "generic"],
    ], "generic", "1.7.1"),
    ("targeted generic row — modular device falls back to fleet-wide", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.0", "https://a", "s1", "h1", 1, 0, ""],
        ["1.7.1", "https://b", "s2", "h2", 1, 1, "generic"],
    ], "modular", "1.7.0"),
    ("only targeted generic rows — modular device sees NOTHING", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.1", "https://b", "s2", "h2", 1, 1, "generic"],
    ], "modular", None),
    ("undeclared device never sees a tree-specific image (fail-closed)", [
        ["version", "url", "sha256", "hmac", "size", "published_at", "target"],
        ["1.7.1", "https://b", "s2", "h2", 1, 1, "modular"],
    ], "", None),
]
x8_ok, x8_detail = True, []
for name, rows, dtype, want in scenarios:
    with tempfile.NamedTemporaryFile(suffix=".js", delete=False, mode="w") as tf:
        tf.write(js + f"console.log(run({json.dumps(rows)}, {json.dumps(dtype)}));")
        tmp = tf.name
    r = subprocess.run(["node", tmp], capture_output=True, text=True)
    Path(tmp).unlink(missing_ok=True)
    if r.returncode != 0:
        x8_ok = False
        x8_detail.append(f"{name}: node error")
        continue
    got = json.loads(r.stdout.strip())
    got_v = None if got is None else got[0]
    if got_v != want:
        x8_ok = False
        x8_detail.append(f"{name}: got {got_v}, want {want}")
check("X8", "latestManifestForTarget_ behavioral simulation "
      "(6 scenarios incl. fail-closed undeclared device)",
      x8_ok, "; ".join(x8_detail))

# --- X9: generic device-side gate -------------------------------------------
fetch_zone = ino_src[ino_src.find("OtaManifest fetchOtaManifest()"):
                     ino_src.find("bool applyOta(")]
check("X9", "generic fetchOtaManifest refuses a foreign target BEFORE download "
      "(honest REFUSED OTA_STATUS event)",
      'm.target != "generic"' in fetch_zone and
      'reportOtaStatus("REFUSED"' in fetch_zone,
      "device-side target gate missing")

# --- X10: modular device-side gates ------------------------------------------
check("X10a", "MqttOtaHandler ota.start refuses target != 'modular'",
      'target != "modular"' in mqtt_ota_cpp,
      "MQTT ota.start target gate missing")
check("X10b", "OtaManager tickManifestCheck refuses a foreign manifest target",
      'manifestTarget != "modular"' in ota_cpp,
      "ota.check manifest target gate missing")
check("X10c", "CommandCanonicalizer whitelists 'target' on ota.start",
      re.search(r'\{"ota",\s*"start",\s*\{[^}]*"target"', canon_cpp) is not None,
      "'target' not in the ota.start field whitelist")

print("== W13-3: OTA event visibility (read side + vocabulary) ==")

# --- X11: vocabulary ---------------------------------------------------------
check("X11", "GAS OTA_STATUS word list accepts VERIFICATION_FAILED",
      re.search(r"validEvents\s*=\s*\[[^\]]*'VERIFICATION_FAILED'", gas_src)
      is not None,
      "VERIFICATION_FAILED must join the word list")

# --- X12: OTA_LOG read action ------------------------------------------------
check("X12a", "doPost routes the OTA_LOG action (auth + registration gated)",
      "action === 'OTA_LOG'" in gas_src,
      "OTA_LOG route missing")
check("X12b", "otaReadEvents_ reads OtaEvents per-device, newest-first, bounded",
      "function otaReadEvents_(" in gas_src and
      "events.length < limit" in gas_src,
      "read implementation missing")

# --- X13: PWA OTA history source ---------------------------------------------
check_or_skip("X13a", "PWA ota-view fetches real GAS OTA_LOG events "
              "(fetchGasOtaHistory)",
              OTA_VIEW.exists() and "fetchGasOtaHistory" in
              (OTA_VIEW.read_text() if OTA_VIEW.exists() else ""),
              "GAS source missing from ota-view")
ota_view_src = OTA_VIEW.read_text() if OTA_VIEW.exists() else ""
check_or_skip("X13b", "PWA OTA history falls back to the mock when GAS is "
              "unconfigured/unreachable",
              "return api.otaHistory()" in ota_view_src,
              "mock fallback missing")
types_src = TYPES_TS.read_text() if TYPES_TS.exists() else ""
check_or_skip("X13c", "OtaHistoryEntry carries the raw event verb + message",
              "event?: string" in types_src and "message?: string" in types_src,
              "optional fields missing from OtaHistoryEntry")

# --- X14: PWA publish panel target selector -----------------------------------
panel_src = SIGN_PANEL.read_text() if SIGN_PANEL.exists() else ""
check_or_skip("X14", "PWA publish panel offers the target selector and sends "
              "manifest.target",
              "ota-input-target" in panel_src and "target: target.trim()" in panel_src,
              "target selector / payload missing")

print("== W13 release integrity ==")

# --- X15: version identity + synced binaries ---------------------------------
gen_v = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', ino_src)
mod_v = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', cfg_h)
gen_manifest = json.loads((FWB / "firmware-generic" / "manifest.json").read_text())
pwa_manifest = json.loads((PWA / "public" / "firmware" / "manifest.json").read_text()) \
    if (PWA / "public" / "firmware" / "manifest.json").exists() else {}
check("X15a", "both firmware trees at 1.7.1 (W13 bump, line parity)",
      gen_v and gen_v.group(1) == "1.7.1" and mod_v and mod_v.group(1) == "1.7.1",
      f"generic={gen_v and gen_v.group(1)}, modular={mod_v and mod_v.group(1)}")
check("X15b", "generic manifest.json version == 1.7.1",
      gen_manifest.get("version") == "1.7.1",
      f"got {gen_manifest.get('version')}")
check_or_skip("X15c", "PWA public/firmware/manifest.json == 1.7.1 (synced)",
              pwa_manifest.get("version") == "1.7.1",
              f"got {pwa_manifest.get('version')}")
fw_bin = FWB / "firmware-generic" / "bin" / "plts_firmware_v1.7.1.bin"
pwa_bin = PWA / "public" / "firmware" / "plts_firmware_v1.7.1.bin"
check("X15d", "v1.7.1 binary exists in the firmware repo bin/",
      fw_bin.exists(), str(fw_bin))
check_or_skip("X15e", "v1.7.1 binary synced to PWA public/firmware/",
              pwa_bin.exists(), str(pwa_bin))
old_gen = (FWB / "firmware-generic" / "bin" / "plts_firmware_v1.7.0.bin").exists()
old_pwa = (PWA / "public" / "firmware" / "plts_firmware_v1.7.0.bin").exists()
check("X15f", "stale v1.7.0 binaries removed (one active version)",
      not old_gen and not old_pwa,
      f"firmware repo stale={old_gen}, PWA stale={old_pwa}")

# --- X16: GAS parses as JavaScript --------------------------------------------
try:
    with tempfile.NamedTemporaryFile(suffix=".js", delete=False) as tf:
        tf.write(GAS.read_bytes())
        tmp = tf.name
    r = subprocess.run(["node", "--check", tmp], capture_output=True, text=True)
    Path(tmp).unlink(missing_ok=True)
    check("X16", "code.gs/Code.gs parses as JavaScript (node --check)",
          r.returncode == 0, (r.stderr or "")[:200])
except FileNotFoundError:
    print("  [SKIP] X16: node not available in this environment")

# ---------------------------------------------------------------------------
print()
total = len(passed) + len(failed)
print(f"RESULT: {len(passed)}/{total} passed, {len(failed)} failed")
if failed:
    for cid, desc, detail in failed:
        print(f"  FAILED {cid}: {desc}" + (f" -> {detail[:300]}" if detail else ""))
    sys.exit(1)
print("ALL WAVE 13 MIXED-FLEET OTA REGRESSION CHECKS PASSED")
