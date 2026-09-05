# Hardware Acceptance — Comprehensive Guide

> **Consolidated from:** `hardware-acceptance/README.md` + `bench/PANDUAN_VALIDASI.md` + `bench/PROTOKOL_BENCH_DARURAT.md`
> **Applies to:** All firmware versions (version-specific templates in `docs/hardware-acceptance/v*.md`)

---

## Daftar Isi

1. [General Hardware Acceptance Process](#1-general-hardware-acceptance-process)
2. [Required Hardware](#2-required-hardware)
3. [Pre-Test Setup](#3-pre-test-setup)
4. [General Checklist (9 Sections)](#4-general-checklist-9-sections)
5. [Sensor Validation Procedures](#5-sensor-validation-procedures)
6. [E-WAVE Emergency Layer Bench Protocol (B1–B9)](#6-e-wave-emergency-layer-bench-protocol-b1b9)
7. [Sign-Off & Release Decision](#7-sign-off--release-decision)
8. [Version-Specific Protocols](#8-version-specific-protocols)

---

## 1. General Hardware Acceptance Process

Every release candidate MUST pass hardware acceptance on real ESP32 hardware before being tagged `stable`. A release without a completed checklist is `pre-release` / `beta` only — never promoted to fleet OTA.

CI cannot run these checks — they require physical sensors, relays, and a battery. The checklist is filled in by the engineer who ran the test, signed off by a second engineer, and attached to the GitHub Release.

### CI Verifier Scripts

| Script | Protocol | Scope |
|--------|----------|-------|
| `scripts/verify_hardware_acceptance.py` | `docs/hardware-acceptance/v{version}.json` | General platform (boot, sensors, alarms, OTA, relay, 24h soak) |
| `scripts/verify_ina219_hardware_acceptance.py` | `docs/hardware-acceptance/v1.9.2.json` | INA219-specific (PGA register, measurement accuracy) |
| `scripts/verify_ota_evidence.py` | `docs/ota-physical-test/v{version}.json` | OTA physical test (PWA → device end-to-end) |

---

## 2. Required Hardware

| Item | Spec | Purpose |
|------|------|---------|
| ESP32 DevKit | 4 MB flash, ESP-WROOM-32 | Target board |
| INA219 + Shunt | 100A/75mV shunt, I²C 0x40 | Current measurement (1A–150A) |
| Voltage Divider | R1=190kΩ, R2=10kΩ (20:1) + 0.1µF cap | Battery voltage (42–57.5V) |
| SHT31 | I²C 0x44, ±0.3°C | Temperature + humidity |
| PCF8574 | I²C 0x20, 8-channel | Relay control |
| Relay Module | 5V, active-LOW optocoupler, 8-channel | Load control |
| Emergency Relay | Active-LOW, NC E-stop | Safety cutoff (GPIO 27 + GPIO 14) |
| LiFePO4 battery | 48V nominal, 15S | Real load |
| PZEM-004T v3 | AC power meter (optional) | AC measurement |
| Reference multimeter | ±0.1V, ±0.5A | Calibration cross-check |
| Load bank | Adjustable 1A–150A | Current testing |
| USB-UART | CP2102 / CH340 | Serial console |

---

## 3. Pre-Test Setup

1. Flash the **release candidate** firmware via:
   ```
   pio run -e production -t upload
   ```
   (NOT `development` — production env exercises the real secrets + assert gates.)
2. Connect serial console at 115200 baud.
3. Confirm `FIRMWARE_VERSION` in boot banner matches the release tag.
4. Confirm MQTT connection to the **staging** broker (NOT production yet).
5. Verify INA219 config readback in Serial Monitor:
   ```
   [INA219 0x40] config readback OK: 0x0FFF (BRNG=0 16V, PGA=1 ±80mV, BADC=15, SADC=15, MODE=7)
   ```
   (For v1.9.2+ — see `docs/hardware-acceptance/v1.9.2.md` for details)

---

## 4. General Checklist (9 Sections)

> **Version-specific templates:** `docs/hardware-acceptance/v1.8.0.md` (21 sections including relay) and `docs/hardware-acceptance/v1.9.2.md` (INA219 12 criteria). This is the base 9-section checklist; version-specific templates extend it.

### 4.1 Boot & First-Run Behavior

- [ ] Device boots within 5 s of power-on
- [ ] Boot banner prints correct `FIRMWARE_VERSION` and `BUILD_ID`
- [ ] NVS mounts cleanly (no corruption warnings)
- [ ] WiFi connects to configured AP within 30 s
- [ ] NTP syncs within 60 s (`timeQuality: VALID` in telemetry)
- [ ] MQTT connects to broker (TLS handshake OK)
- [ ] Boot health check passes after 60 s (`OTA image ACTIVATED` log line)

### 4.2 Sensor Telemetry

- [ ] Voltage reading within ±0.2 V of multimeter reference
- [ ] Current reading within ±0.2 A of multimeter reference (no-load = 0)
- [ ] Temperature reading within ±2 °C of ambient
- [ ] Telemetry envelope published to MQTT at configured cadence
- [ ] GAS `TELEMETRY` action returns `SUCCESS` for the device
- [ ] PWA status page shows live data within 2 s of publish

### 4.3 Alarm Lifecycle

- [ ] Trigger a low-voltage alarm (drop battery below `BATTERY_LOW_V`)
- [ ] Alarm appears in `GET /api/alarms` `active` list within 5 s
- [ ] PWA shows alarm banner
- [ ] `POST /api/alarms/{alarmCode}/acknowledge` returns 200
- [ ] Alarm moves to `history` list
- [ ] GAS records the alarm event

### 4.4 OTA Update (REST)

- [ ] Upload a signed binary via `POST /api/ota` (PWA → device)
- [ ] Progress events appear in serial console
- [ ] SHA-256 verification passes
- [ ] Ed25519 signature verification passes
- [ ] Device reboots into new image
- [ ] New `FIRMWARE_VERSION` reported on boot
- [ ] Boot health check passes → image ACTIVATED
- [ ] Lifecycle events emitted to MQTT: `ACCEPTED`, `DOWNLOADING`, `VERIFIED`, `FLASHED`, `ACTIVATED`

### 4.5 OTA Rollback (Negative Test)

- [ ] Upload a binary that fails the boot health check (e.g., panics on boot)
- [ ] After 3 boot attempts (or single bootloader revert), device rolls back
- [ ] Old `FIRMWARE_VERSION` reported after rollback
- [ ] `ROLLBACK` lifecycle event emitted

### 4.6 Emergency Relay (if `PLTS_ENABLE_EMERGENCY=1`)

- [ ] `POST /api/bms` returns valid state (or honest 503 if compiled out)
- [ ] GAS `EMERGENCY_COMMAND ARM` → device acknowledges within 2 s
- [ ] Relay physically clicks (audible)
- [ ] `EMERGENCY_ACK` received by GAS
- [ ] `EMERGENCY_COMMAND DISARM` → relay releases
- [ ] E-stop button trip → `EMERGENCY_EVENT` logged + Telegram alert sent

### 4.7 Configuration Persistence

- [ ] `POST /api/config` with new `deviceName`
- [ ] Reboot device
- [ ] `GET /api/config` returns the new `deviceName` (NVS persisted)
- [ ] `POST /api/calibration` with offset → telemetry reflects new offset

### 4.8 Security

- [ ] Unauthenticated `GET /api/status` returns 401
- [ ] CSRF token required for all POST endpoints
- [ ] `factory_reset` requires 2-step prepare+confirm with valid token
- [ ] MQTT credentials are NOT printed in serial console (production env)
- [ ] `assert_production_secrets.py` passes

### 4.9 Long-Run Stability (24h Soak Test)

- [ ] Device stays connected for 24 h without reboot
- [ ] No memory leak (heap free stays > 50 KB after 24 h)
- [ ] No task watchdog triggers
- [ ] Telemetry continuity: no gaps > 2 cadence intervals
- [ ] GAS `SEQ_LEDGER` shows no unexplained sequence gaps

---

## 5. Sensor Validation Procedures

> **Source:** `bench/PANDUAN_VALIDASI.md` (version-agnostic operator procedures)

### 5.1 PZEM-004T Validation (24h)

> **Source code reference:** `firmware/Drivers/Pzem004tDriver.h` — enable only after this validation passes

**Prasyarat:** PZEM-004T v3 terhubung ke UART1 (GPIO 18/19), `PLTS_ENABLE_PZEM_AC=1` di `platformio.ini`.

**Langkah:**
1. Flash firmware dengan PZEM enabled
2. Verifikasi Serial log: `[PZEM-004T] init OK` (no timeout)
3. Bandingkan pembacaan PZEM (V/A/W/Hz/PF) dengan referensi selama 24 jam
4. Tolerance: ±2V, ±0.1A, ±5W, ±0.5Hz, ±0.05 PF
5. Verifikasi energy counter (Wh) akumulasi konsisten

**Jika gagal:** PZEM flag tetap OFF (`PLTS_ENABLE_PZEM_AC=0`), AC power tetap estimasi (ACS712 × 220V × PF 0.9).

### 5.2 RS485 Pylontech Vendor Frame Capture

Slot klien `PylontechRs485` RESERVED sampai frame vendor asli terdokumentasi. Konsol capture adalah alatnya — **pasif total**.

**Langkah:**
1. Kabel: A/B RS485 baterai → terminal A/B MAX3485 (GPIO 16 TX / 17 RX)
2. Aktifkan mode console: `bmsProto = "rs485_console"` (NVS), reboot
3. Log harus menampilkan: `RS485_CONSOLE_ACTIVE — passive vendor-frame capture started`
4. Tangkap minimal **30 frame berturut-turut** via `GET /api/rs485/frames`
5. Catat periode antar-frame
6. Serahkan hasil (hex mentah + kondisi baterai) untuk parser development

**Yang TIDAK boleh:** Jangan mengaktifkan `rs485_console` bersamaan dengan `modbus_rtu`. Jangan men-interpretasi hex secara manual lalu meng-hardcode.

### 5.3 Modbus Register Map Verification

Peta default (`Comm/ModbusMap.h`, register 100–112) adalah **CONTOH** sampai diverifikasi terhadap manual baterai.

**Langkah:**
1. Buka manual baterai, cari tabel Modbus register
2. Bandingkan setiap register (address, type, scale, unit) dengan `ModbusMap.h`
3. Jika ada mismatch: update `ModbusMap.h`, rebuild, test baca ulang
4. Verifikasi: V, I, SOC, temperature, cell voltages, alarm flags sesuai manual

### 5.4 Modbus TCP Network Hardening

Jika menggunakan Modbus TCP (bukan RTU):

1. Bind ke interface LAN saja (jangan 0.0.0.0)
2. Firewall: port 502 hanya dari IP lokal
3. Auth: Modbus TCP tidak punya auth native — gunakan VPN/VLAN isolation
4. Logging: log semua connection attempt yang ditolak

### 5.5 OTA Rollback Physical Test

> **Also see:** `docs/ota-physical-test/v1.8.0.md` (16-criteria protocol, more detailed)

**Happy path:**
1. Flash firmware A (older version)
2. OTA upload firmware B (newer, signed)
3. Device reboots into B → ACTIVATED after 60s stable
4. Verify `FIRMWARE_VERSION` = B

**Rollback path:**
1. Flash firmware B (working)
2. OTA upload firmware C (broken — panics on boot)
3. Device boots C → fails health check → bootloader reverts to B
4. Verify `FIRMWARE_VERSION` = B (rolled back)
5. Verify `ROLLBACK` lifecycle event emitted

---

## 6. E-WAVE Emergency Layer Bench Protocol (B1–B9)

> **Source:** `bench/PROTOKOL_BENCH_DARURAT.md` (version-agnostic — applies to all releases with `PLTS_ENABLE_EMERGENCY=1`)

### Prasyarat

1. ESP32 dengan firmware production (`PLTS_ENABLE_EMERGENCY=1`)
2. Relay 5V optocoupler **active-LOW** pada GPIO 27
3. Tombol E-stop NC pada GPIO 14
4. LED status GPIO 2
5. Kontaktor pada BEBAN (bukan sumber DC utama)
6. ADMIN_TOKEN terisi, perangkat online (WiFi + NTP sync)
7. PWA menu **Kontrol Darurat** terbuka
8. **Amankan dulu:** B1 harus lulus SEBELUM kontaktor dihubungkan ke beban nyata

### Sembilan Skenario

| ID | Skenario | Langkah | Hasil yang Diharapkan |
|----|----------|---------|----------------------|
| **B1** | Boot isolasi | Cabut daya 10s, sambungkan kembali, amati 1 menit | Relay isolasi sejak reset (GPIO Hi-Z); `emergency.state=EMERGENCY`, `reason=BOOT`, `relayEnergized=false` sebelum WiFi; LED kedip 2 Hz; kontaktor TERBUKA |
| **B2** | Trip otomatis ambang | Naikkan `vbatLowV` di atas tegangan aktual; tunggu ≥3s; kembalikan | Trip dengan `reason=VBAT_LOW`; LED 2 Hz; kejadian TRIP di GAS; ARM diterima setelah histeresis + 60s |
| **B3** | E-stop fisik | Tekan E-stop NC; amati; putar untuk melepas; tekan ARM | TERISOLASI seketika (hardware memutus catu relay); `estopOpen=true`; melepas TIDAK menyalakan ulang — hanya ARM |
| **B4** | ARM ditolak | Dengan pemicu aktif, tekan ARM; perbaiki kondisi; ARM lagi setelah 60s | ARM pertama DITOLAK dengan alasan; ARM kedua diterima setelah kondisi aman |
| **B5** | ARM diterima | Dari kondisi sehat, tekan ARM | Sistem RUN; LED solid; `relayEnergized=true`; kontaktor tertutup; kejadian ARMED |
| **B6** | EMERGENCY STOP jarak jauh | Dari PWA, tekan EMERGENCY STOP + konfirmasi STOP | TERISOLASI ≤15s; `reason=OPERATOR`; alert Telegram; idempoten |
| **B7** | Perintah basi (TTL) | Matikan WiFi; kirim EMERGENCY STOP; tunggu 11 menit; nyalakan WiFi | Perintah EXPIRED (TTL 10 menit); tidak diterapkan; sistem tetap RUN |
| **B8** | Crash-loop hold | Reboot paksa 3× berturut-turut <5 menit; coba ARM | ARM DITOLAK; `crashChain=3`; setelah 5 menit stabil + reboot, chain reset |
| **B9** | CONFIG persisten | Kirim GAS CONFIG; reboot; cek | ACK `APPLIED`; config bertahan di NVS; `trips` tidak hilang |

### Aturan Lolos/Gagal

B3 dan B4 menguji **filosofi fail-safe**, bukan hanya fungsi. Jika **satu saja** skenario gagal: **hentikan komisioning**, putus kontaktor, periksa wiring relay (polaritas active-LOW!), posisi E-stop NC, dan konfigurasi pin.

### Pemetaan ke Uji Mekanis

| Skenario | Grup asersi `test_emergency_modular_logic.py` | Invarian |
|----------|----------------------------------------------|----------|
| B1 | E1/E2/E8c | Isolasi fail-safe sebelum WiFi |
| B2 | A2–A7 | Debounce + latching + histeresis |
| B3 | C1–C3 | E-stop latch; lepas ≠ re-energize |
| B4 | B1–B9, S1–S3 | Gerbang ARM fail-closed |
| B5 | B6/B4 | ARM operator-only |
| B6 | D1–D2 | DISARM aman + idempoten |
| B7 | R-grup | Perintah basi tidak diterapkan |
| B8 | E10/B8 | Rantai crash NVS |
| B9 | R7a–R7d + E14c | Rentang & default lintas GAS-firmware |

---

## 7. Sign-Off & Release Decision

### Sign-Off Table

| Role | Name | Date | Signature |
|------|------|------|-----------|
| Test Engineer | __________ | ______ | __________ |
| Reviewer | __________ | ______ | __________ |
| Release Manager | __________ | ______ | __________ |

### Release Decision

- [ ] **RELEASE** — All checks PASS. Tag as `v<X.Y.Z>` and promote to fleet OTA.
- [ ] **PRE-RELEASE** — Some checks FAILED but core functionality works. Tag as `v<X.Y.Z>-beta.N` and restrict OTA to test devices.
- [ ] **BLOCKED** — Critical checks FAILED. Do NOT tag. File issues for each failure and re-run after fixes.

---

## 8. Version-Specific Protocols

### General Platform Acceptance

| Version | Protocol | Evidence | Status |
|---------|----------|----------|--------|
| v1.8.0 | `docs/hardware-acceptance/v1.8.0.md` (21 sections) | `v1.8.0.json` | ✅ PASS (2026-09-04) |
| v1.9.2 | `docs/hardware-acceptance/v1.8.0.md` (as regression) | _(pending)_ | 🔴 PENDING |

### INA219-Specific Acceptance (v1.9.2+)

| Version | Protocol | Evidence | Status |
|---------|----------|----------|--------|
| v1.9.2 | `docs/hardware-acceptance/v1.9.2.md` (12 criteria) | _(pending)_ | 🔴 PENDING |

### OTA Physical Test

| Version | Protocol | Evidence | Status |
|---------|----------|----------|--------|
| v1.8.0 | `docs/ota-physical-test/v1.8.0.md` (16 criteria) | _(pending)_ | 🔴 PENDING |

### Template Usage

1. Copy the version-specific template to `docs/hardware-acceptance/v<X.Y.Z>.json`
2. Fill in all checklist items with actual observed values
3. Set `verdict` to `PASS` or `BLOCKED`
4. Run the CI verifier:
   ```bash
   python3 scripts/verify_hardware_acceptance.py --version <X.Y.Z> ...
   python3 scripts/verify_ina219_hardware_acceptance.py --version <X.Y.Z> ...
   python3 scripts/verify_ota_evidence.py --version <X.Y.Z> ...
   ```
5. Attach the completed JSON to the GitHub Release as evidence
