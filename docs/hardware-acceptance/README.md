# Hardware Acceptance Gate (P1-10)

> **Every release candidate MUST pass this checklist on real hardware before
> being tagged `stable`.** A release without a completed checklist is
> `pre-release` / `beta` only — never promoted to fleet OTA.

## Scope

This checklist validates firmware behavior on real ESP32 hardware. CI cannot
run these checks — they require physical sensors, relays, and a battery. The
checklist is filled in by the engineer who ran the test, signed off by a
second engineer, and attached to the GitHub Release as a PDF/Markdown file.

## Required Hardware

| Item | Spec | Purpose |
|------|------|---------|
| ESP32 DevKit | 4 MB flash, ESP-WROOM-32 | Target board |
| Voltage sensor | ZMPT101B or voltage divider | Voltage telemetry |
| Current sensor | ACS712 (±20A) or INA219 | Current telemetry |
| Temperature sensor | DS18B20 (1-Wire) | Battery temp |
| LiFePO4 battery | 48V nominal, 15S | Real load |
| Relay module | 5V coil, opto-isolated | Emergency disconnect |
| PZEM-004T v3 | AC power meter (optional) | AC gen telemetry |
| USB-UART | CP2102 / CH340 | Serial console for diagnostics |

## Pre-test Setup

1. Flash the **release candidate** firmware via:
   ```
   pio run -e production -t upload
   ```
   (NOT `development` — production env exercises the real secrets + assert
   gates.)
2. Connect serial console at 115200 baud.
3. Confirm `FIRMWARE_VERSION` in boot banner matches the release tag.
4. Confirm MQTT connection to the **staging** broker (NOT production yet).

---

## Checklist

### 1. Boot & First-Run Behavior

- [ ] Device boots within 5 s of power-on
- [ ] Boot banner prints correct `FIRMWARE_VERSION` and `BUILD_ID`
- [ ] NVS mounts cleanly (no corruption warnings)
- [ ] WiFi connects to configured AP within 30 s
- [ ] NTP syncs within 60 s (`timeQuality: VALID` in telemetry)
- [ ] MQTT connects to broker (TLS handshake OK)
- [ ] Boot health check passes after 60 s (`OTA image ACTIVATED` log line)

**Result:** PASS / FAIL — Notes: _______________

### 2. Sensor Telemetry

- [ ] Voltage reading within ±0.2 V of multimeter reference
- [ ] Current reading within ±0.2 A of multimeter reference (no-load = 0)
- [ ] Temperature reading within ±2 °C of ambient
- [ ] Telemetry envelope published to MQTT at configured cadence
- [ ] GAS `TELEMETRY` action returns `SUCCESS` for the device
- [ ] PWA status page shows live data within 2 s of publish

**Result:** PASS / FAIL — Notes: _______________

### 3. Alarm Lifecycle

- [ ] Trigger a low-voltage alarm (drop battery below `BATTERY_LOW_V`)
- [ ] Alarm appears in `GET /api/alarms` `active` list within 5 s
- [ ] PWA shows alarm banner
- [ ] `POST /api/alarms/{alarmCode}/acknowledge` returns 200
- [ ] Alarm moves to `history` list
- [ ] GAS records the alarm event

**Result:** PASS / FAIL — Notes: _______________

### 4. OTA Update (REST)

- [ ] Upload a signed binary via `POST /api/ota` (PWA → device)
- [ ] Progress events appear in serial console
- [ ] SHA-256 verification passes
- [ ] Ed25519 signature verification passes (P0-1 — this used to fail!)
- [ ] Device reboots into new image
- [ ] New `FIRMWARE_VERSION` reported on boot
- [ ] Boot health check passes → image ACTIVATED
- [ ] **Lifecycle events emitted to MQTT** (P1-6): `ACCEPTED`, `DOWNLOADING`,
      `VERIFIED`, `FLASHED`, `ACTIVATED` — verify each appears in GAS OtaEvents

**Result:** PASS / FAIL — Notes: _______________

### 5. OTA Rollback (Negative Test)

- [ ] Upload a binary that fails the boot health check (e.g., panics on boot)
- [ ] After 3 boot attempts (or single bootloader revert), device rolls back
- [ ] Old `FIRMWARE_VERSION` reported after rollback
- [ ] **`ROLLBACK` lifecycle event emitted** (P1-6) — verify in GAS OtaEvents

**Result:** PASS / FAIL — Notes: _______________

### 6. Emergency Relay (if `PLTS_ENABLE_EMERGENCY=1`)

- [ ] `POST /api/bms` returns valid state (or honest 503 if compiled out)
- [ ] GAS `EMERGENCY_COMMAND ARM` → device acknowledges within 2 s
- [ ] Relay physically clicks (audible)
- [ ] `EMERGENCY_ACK` received by GAS
- [ ] `EMERGENCY_COMMAND DISARM` → relay releases
- [ ] E-stop button trip → `EMERGENCY_EVENT` logged + Telegram alert sent

**Result:** PASS / FAIL — Notes: _______________

### 7. Configuration Persistence

- [ ] `POST /api/config` with new `deviceName`
- [ ] Reboot device
- [ ] `GET /api/config` returns the new `deviceName` (NVS persisted)
- [ ] `POST /api/calibration` with offset → telemetry reflects new offset

**Result:** PASS / FAIL — Notes: _______________

### 8. Security

- [ ] Unauthenticated `GET /api/status` returns 401
- [ ] CSRF token required for all POST endpoints
- [ ] `factory_reset` requires 2-step prepare+confirm with valid token
- [ ] MQTT credentials are NOT printed in serial console (production env)
- [ ] `assert_production_secrets.py` passes (CI gate equivalent on local build)

**Result:** PASS / FAIL — Notes: _______________

### 9. Long-Run Stability (24 h soak test)

- [ ] Device stays connected for 24 h without reboot
- [ ] No memory leak (heap free stays > 50 KB after 24 h)
- [ ] No task watchdog triggers
- [ ] Telemetry continuity: no gaps > 2 cadence intervals
- [ ] GAS `SEQ_LEDGER` shows no unexplained sequence gaps

**Result:** PASS / FAIL — Notes: _______________

---

## Sign-off

| Role | Name | Date | Signature |
|------|------|------|-----------|
| Test Engineer | __________ | ______ | __________ |
| Reviewer | __________ | ______ | __________ |
| Release Manager | __________ | ______ | __________ |

## Release Decision

- [ ] **RELEASE** — All checks PASS. Tag as `v<X.Y.Z>` and promote to fleet OTA.
- [ ] **PRE-RELEASE** — Some checks FAILED but core functionality works. Tag as
      `v<X.Y.Z>-beta.N` and restrict OTA to test devices.
- [ ] **BLOCKED** — Critical checks FAILED. Do NOT tag. File issues for each
      failure and re-run after fixes.

Notes / known issues: _________________________________________________

---

## Template Usage

1. Copy this file to `docs/hardware-acceptance/v<X.Y.Z>.md`.
2. Fill in all checklist items with actual results (not just checked boxes —
   include observed values where the check says "within X").
3. Attach the completed file to the GitHub Release as documentation.
4. Reference it in the release notes: "Hardware acceptance: PASSED — see
   docs/hardware-acceptance/v1.7.2.md".
