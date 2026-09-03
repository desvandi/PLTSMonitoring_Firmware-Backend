#!/usr/bin/env python3
"""
test_alarm_ack_contract.py — P1-3 cross-layer contract test.

Verifies that the canonical alarm ACK endpoint is:
  POST /api/alarms/{alarmId}/acknowledge

in BOTH:
  - the PWA (src/lib/deviceApi.ts)
  - the firmware (firmware/Web/AlarmHandlers.cpp)

The audit found the firmware had two endpoints (POST /api/alarms/acknowledge
with body {code} AND POST /api/alarms/{code}/acknowledge), and only the
former was actually registered. The PWA called the latter — every ACK 404'd.

Run:  python3 scripts/test_alarm_ack_contract.py
Exit: 0 = PASS, 1 = FAIL
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PWA_ROOT = ROOT.parent / "PLTSMonitoring_PWA"

PATTERNS = {
    "pwa_canonical": (
        r"`/api/alarms/\$\{[^}]+\}/acknowledge`",
        "PWA deviceApi.ts must use canonical /api/alarms/{id}/acknowledge",
    ),
    "fw_canonical_registered": (
        r'http\.on\(\s*"/api/alarms/"\s*,\s*HTTP_POST',
        "Firmware must register a catch-all at /api/alarms/ for canonical ACK",
    ),
    "fw_canonical_handler": (
        r'void handleAcknowledge\(\)',
        "Firmware must have handleAcknowledge() that parses /api/alarms/{code}/acknowledge",
    ),
    "fw_404_on_missing": (
        r'sendError\(404,\s*"Alarm not found"\)',
        "Firmware must 404 when alarm code not found (was: silently OK)",
    ),
}

# Forbidden patterns (regression guards)
FORBIDDEN = {
    "fw_noncanonical_route": (
        r'http\.on\(\s*"/api/alarms/acknowledge"\s*,\s*HTTP_POST',
        "Firmware must NOT register the non-canonical /api/alarms/acknowledge "
        "route (P1-3 — one endpoint, one contract)",
    ),
}

failures = []

# ---- PWA: deviceApi.ts --------------------------------------------------
pwa_device_api = PWA_ROOT / "src" / "lib" / "deviceApi.ts"
if not pwa_device_api.is_file():
    failures.append(f"PWA deviceApi.ts missing: {pwa_device_api}")
else:
    text = pwa_device_api.read_text()
    m = re.search(PATTERNS["pwa_canonical"][0], text)
    if not m:
        failures.append(PATTERNS["pwa_canonical"][1])
    else:
        print(f"PASS pwa_canonical: {m.group(0)}")

# ---- Firmware: AlarmHandlers.cpp ---------------------------------------
fw_alarms = ROOT / "firmware" / "Web" / "AlarmHandlers.cpp"
if not fw_alarms.is_file():
    failures.append(f"Firmware AlarmHandlers.cpp missing: {fw_alarms}")
else:
    text = fw_alarms.read_text()
    for key in ("fw_canonical_registered", "fw_canonical_handler", "fw_404_on_missing"):
        pat, msg = PATTERNS[key]
        if re.search(pat, text):
            print(f"PASS {key}")
        else:
            failures.append(f"{key}: {msg}")

# ---- Forbidden patterns -------------------------------------------------
for key, (pat, msg) in FORBIDDEN.items():
    targets = [
        ROOT / "firmware" / "Web" / "AlarmHandlers.cpp",
        ROOT / "firmware" / "Web" / "ExtraHandlers.cpp",
    ]
    for t in targets:
        if t.is_file() and re.search(pat, t.read_text()):
            failures.append(f"{key} (in {t.name}): {msg}")

# ---- PWA: api.ts (legacy façade) must also use canonical ---------------
pwa_api = PWA_ROOT / "src" / "lib" / "api.ts"
if pwa_api.is_file():
    text = pwa_api.read_text()
    if "/api/alarms/${alarmId}/acknowledge" in text or "/api/alarms/${alarmId}" in text:
        print("PASS pwa_api_canonical: legacy api.ts delegates to deviceApi (canonical)")
    else:
        # It might delegate via deviceApi.acknowledgeAlarm — that's fine.
        if "deviceApi.acknowledgeAlarm" in text or "acknowledgeAlarm:  deviceApi" in text:
            print("PASS pwa_api_canonical: legacy api.ts delegates to deviceApi.acknowledgeAlarm")
        else:
            failures.append("pwa_api_canonical: legacy api.ts must delegate ACK to deviceApi")

if failures:
    print("\nFAIL — alarm ACK contract violations:")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)
print("\nPASS — alarm ACK contract consistent across PWA + firmware")
