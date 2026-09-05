# PLTS Monitoring — Firmware, Backend GAS, & Toolkit

**Firmware Version:** v1.9.3 · **Backend GAS:** WAVE-7+ · **License:** MIT
**Status:** Development (v1.9.x INA219 dynamic gain switching — hardware acceptance pending; v1.9.3 menutup REL-03/REL-04: build kini reproducible)
**Repositori kembar (PWA frontend):** [desvandi/PLTSMonitoring_PWA](https://github.com/desvandi/PLTSMonitoring_PWA)

---

## Daftar Isi

1. [Gambaran Proyek](#1-gambaran-proyek)
2. [Arsitektur Sistem](#2-arsitektur-sistem)
3. [Struktur Repositori](#3-struktur-repositori)
4. [Hardware Requirements](#4-hardware-requirements)
5. [Firmware v1.9.x — INA219 Dynamic Gain Switching](#5-firmware-v19x--ina219-dynamic-gain-switching)
6. [Panduan Deployment Lengkap](#6-panduan-deployment-lengkap)
7. [Konfigurasi Firmware](#7-konfigurasi-firmware)
8. [Backend Google Apps Script](#8-backend-google-apps-script)
9. [CI/CD Pipeline](#9-cicd-pipeline)
10. [Release Engineering](#10-release-engineering)
11. [Hardware Acceptance](#11-hardware-acceptance)
12. [Testing](#12-testing)
13. [Troubleshooting](#13-troubleshooting)
14. [Changelog](#14-changelog)

---

## 1. Gambaran Proyek

Proyek ini adalah sistem monitoring PLTS (Pembangkit Listrik Tenaga Surya) berbasis ESP32 untuk baterai 48V LiFePO4. Sistem mencakup telemetri lengkap (tegangan, arus, daya, SOC, suhu, BMS), alarm real-time, kontrol relay 8-channel, OTA update dengan signature verification, dan push notification.

### Dua Subsistem

| Subsistem | Fungsi | Komponen |
|-----------|--------|----------|
| **PLTS Monitor** (dasbor) | Telemetri → Google Apps Script → PWA Next.js; realtime MQTT opsional | `firmware/`, `firmware-generic/`, `code.gs/`, `scripts/` |
| **Push-Alarm MonitorIoT** | Alarm sensor via Web Push terenkripsi (VAPID + aes128gcm) | `push-alarm/` |

### Prinsip Inti

- **Never fabricate certainty** — setiap pengukuran membawa `value/unit/quality/source/timestamp`; sensor gagal → `null` (bukan `0`)
- **Fail-closed** — production build menolak konfigurasi tanpa signature/CA/allowlist
- **Immutable releases** — setiap release ditandatangani GPG + Ed25519, provenance binding machine-verifiable

---

## 2. Arsitektur Sistem

```
┌─────────────────────────────────────────────────────────────┐
│                     ESP32-WROOM-32                          │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ INA219   │  │ ADC GPIO │  │ SHT31    │  │ PCF8574  │   │
│  │ (Arus)   │  │ (Tegang) │  │ (Suhu)   │  │ (Relay)  │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘   │
│       │ I²C         │ ADC         │ I²C         │ I²C      │
│  ┌────┴─────────────┴─────────────┴─────────────┴────┐    │
│  │              Firmware v1.9.3                      │    │
│  │  • Dynamic PGA Switching (±80mV / ±160mV)        │    │
│  │  • Voltage Divider 190kΩ/10kΩ (20:1)             │    │
│  │  • 8-Channel Relay Controller (E-WAVE)           │    │
│  │  • Reproducible Build (deterministic identity)   │    │
│  │  • OTA Manager (Ed25519 + SHA-256)               │    │
│  │  • MQTT TLS / HTTPS telemetry                    │    │
│  └───────────────────┬──────────────────────────────┘    │
│                      │                                    │
└──────────────────────┼────────────────────────────────────┘
                       │
              ┌────────┴────────┐
              │   WiFi / MQTT   │
              └────────┬────────┘
                       │
          ┌────────────┴────────────┐
          │                         │
    ┌─────┴─────┐           ┌──────┴──────┐
    │  HTTPS    │           │   MQTT TLS  │
    │  POST     │           │  (opsional) │
    └─────┬─────┘           └──────┬──────┘
          │                        │
    ┌─────┴────────────────────────┴─────┐
    │     Google Apps Script (Backend)    │
    │  • Telemetry ingest (HMAC auth)     │
    │  • Google Sheets logging            │
    │  • Telegram alerts                 │
    │  • OTA command dispatch             │
    └─────────────┬───────────────────────┘
                  │
           ┌──────┴──────┐
           │  PWA (Vercel) │
           │  Next.js 16   │
           └───────────────┘
```

### Topologi Operasi

| Topologi | Transport | Backend | Use Case |
|----------|-----------|--------|----------|
| **A (Dasbor)** | HTTPS POST | Google Apps Script + Google Sheets | Standby/standalone tanpa internet real-time |
| **B (Realtime)** | MQTT TLS | Broker MQTT + PWA subscribe | Real-time dashboard dengan live updates |

---

## 3. Struktur Repositori

```
PLTSMonitoring_Firmware-Backend/
├── firmware/                    # Modular production firmware (v1.9.3)
│   ├── Core/                    #   Config.h, Types.h, Globals.h, Common.h
│   ├── Drivers/                 #   INA219, ADC, SHT31, ACS712, PCF8574, RTC
│   ├── Services/                #   OtaManager, RelayController, AlarmRegistry, dll
│   ├── Network/                 #   MQTT transport, telemetry publisher, OTA handler
│   ├── Web/                     #   HTTP server, REST handlers (status, OTA, config, dll)
│   ├── Storage/                 #   NVS config store, filesystem
│   ├── Utils/                   #   Crypto (Ed25519), CRC, I2C recovery, JSON
│   ├── AI/                      #   GasAdvisor (HMAC-signed AI insights)
│   ├── Comm/                    #   BMS protocols (CAN, Modbus RTU/TCP)
│   ├── firmware_v1.ino          #   Main entry point
│   ├── platformio.ini           #   Build configs: development, staging, production
│   └── partitions_ota_1mb5.csv  #   OTA partition table
│
├── firmware-generic/            # Generic firmware (ESP Web Tools flashing)
│   ├── src/
│   │   └── plts_firmware_v1.ino #   Single-file firmware (same features, simpler build)
│   ├── manifest.json            #   ESP Web Tools manifest (v1.9.3)
│   ├── platformio.ini
│   └── bin/                     #   CI-built binaries (gitignored)
│
├── code.gs/                     # Google Apps Script backend
│   └── Code.gs                  #   doPost handler, telemetry ingest, HMAC, OTA dispatch
│
├── push-alarm/                  # MonitorIoT Push-Alarm subsystem
│   ├── gas/                     #   GAS Web Push core (Code.gs, WebPushCore.gs)
│   ├── firmware/                #   Alarm firmware (MonitorIoT_Firmware.ino)
│   ├── tools/                   #   VAPID key gen, deploy config, audit tools
│   └── tests/                   #   Cross-audit, smoke, webpush-core tests
│
├── scripts/                     # Release engineering + testing
│   ├── sign_firmware.py         #   Ed25519 firmware signing
│   ├── generate_provenance.py   #   Release provenance (release.json, provenance.json)
│   ├── generate_canonical_manifest.py  # Schema 2.0 OTA manifest
│   ├── generate_provenance_binding.py  # Explicit provenance chain
│   ├── release_gate.py          #   Master release gate (strict mode)
│   ├── release_firmware_generic.py    # Generic tree release builder
│   ├── verify_tag_signature.py  #   GPG tag verification (P0-1)
│   ├── verify_hardware_acceptance.py  # HW acceptance gate (P0-2)
│   ├── verify_ota_evidence.py   #   OTA physical test gate
│   ├── verify_ina219_hardware_acceptance.py  # INA219 HW acceptance (v1.9.x)
│   ├── decode_ina219_config.py  #   INA219 register decoder (canonical)
│   ├── test_ina219_config_registers.py  # INA219 register unit test
│   ├── test_*.py / test_*.js    #   30+ test scripts (unit, property, contract, bench)
│   └── ...
│
├── docs/
│   ├── ARCHITECTURE.md          #   System architecture + relay subsystem (consolidated)
│   ├── DEPLOYMENT.md            #   Branch protection, GPG signing, binary distribution
│   ├── HARDWARE_ACCEPTANCE.md   #   All HW acceptance protocols (general + sensor + E-WAVE)
│   ├── AUDIT_2026_09_REMEDIATION.md  # Audit findings (P0/P1/P2 ID namespace)
│   ├── hardware-acceptance/     #   Version-specific HW acceptance templates + evidence
│   │   ├── v1.8.0.md            #     General HW acceptance (21 sections incl relay)
│   │   ├── v1.8.0.json          #     Filled-in evidence (verdict=PASS)
│   │   ├── v1.9.2.md            #     INA219-specific HW acceptance (12 criteria)
│   │   ├── v1.9.2.template.json #     Template for v1.9.2
│   │   ├── v1.9.3.md            #     INA219 HW acceptance for the v1.9.3 release line
│   │   └── v1.9.3.template.json #     Template for v1.9.3 (REL-03: SHA byte-exact evidence)
│   ├── ota-physical-test/       #   OTA physical test protocol (16 criteria)
│   │   ├── v1.8.0.md
│   │   └── v1.8.0.template.json
│   └── wiring/                  #   Wiring diagrams (DC, AC, emergency relay)
│
├── .github/workflows/
│   └── build-firmware.yml       #   CI: test → build → sign → gate → tag-verify → hw-verify → release
│
├── Panduan_Deploy_Production_MonitorIoT.pdf  # Legacy deploy guide (Edisi 4)
└── README.md                    # File ini
```

---

## 4. Hardware Requirements

### Komponen Utama

| Komponen | Spesifikasi | Fungsi |
|----------|-------------|--------|
| **ESP32-WROOM-32** | Dual-core 240MHz, 4MB Flash, WiFi+BT | Mikrokontroler utama |
| **INA219** | I²C 0x40, shunt 100A/75mV (R=0.75mΩ) | Pengukuran arus baterai (1A–150A) |
| **Shunt Resistor** | 100A/75mV (0.75mΩ) | Low-side current sensing |
| **Voltage Divider** | R1=190kΩ, R2=10kΩ (ratio 20:1) + 0.1µF cap | Pembacaan tegangan baterai 42–57.5V via ADC |
| **SHT31** | I²C, ±0.3°C, ±2%RH | Suhu + kelembaban ambient |
| **PCF8574** | I²C expander 8-bit | 8-channel relay control |
| **Relay Module** | 8-channel, active-LOW optocoupler | Kontrol beban PLTS |
| **Emergency Relay** | Active-LOW, normally-closed E-stop | Safety cutoff (hardware-level) |
| **ACS712-30A** (opsional) | ADC GPIO35 | Arus AC inverter |
| **PZEM-004T** (opsional) | Modbus RTU | Meter AC presisi |

### Wiring INA219 (Low-Side Sensing)

```
Baterai (+) ──────────────────────────────────────── Beban (+)
                                              │
                                         [Divider]
                                         R1=190kΩ
                                              │
Baterai (−) ──────┬──────────────────────┬───── ADC GPIO34
                  │                      │
              [Shunt 100A]              R2=10kΩ
              75mV, 0.75mΩ               │
                  │                   [0.1µF]
                  ├──── INA219 Vin+      │
                  │                      │
                  └──── INA219 Vin−      │
                  │                      │
                 GND ────────────────────┘
```

> **Penting:** R100 (0.1Ω) bawaan modul INA219 **wajib dicopot**. Gunakan shunt eksternal 100A/75mV. INA219 dipasang low-side (jalur GND) karena Vbus bisa 57.5V > 26V max INA219.

### Pin Assignment

| GPIO | Fungsi | Modul |
|------|--------|-------|
| 21 | I²C SDA | INA219, SHT31, PCF8574 |
| 22 | I²C SCL | INA219, SHT31, PCF8574 |
| 34 | ADC1 CH6 | Voltage divider (input-only, WiFi-safe) |
| 35 | ADC1 CH7 | ACS712 AC current (opsional) |
| 0 | BOOT button | Factory reset trigger |
| 2 | Built-in LED | Status indicator |

---

## 5. Firmware v1.9.x — INA219 Dynamic Gain Switching

### Masalah

Rentang arus PLTS sangat lebar: 1A (standby) hingga 150A (beban puncak). Single PGA range INA219 tidak bisa cover keduanya:
- **±80mV**: saturasi di ~106A (150A × 0.75mΩ = 112.5mV > 80mV)
- **±160mV**: resolusi buruk di arus standby 1–2A

### Solusi: Dynamic PGA Switching

Firmware v1.9.2+ (termasuk v1.9.3) secara otomatis beralih PGA berdasarkan besaran arus:

| Mode | Register | PGA Range | Max Current | Resolusi |
|------|----------|-----------|-------------|----------|
| Standby | `0x0FFF` | ±80mV (gain /2) | 106A | 10µV/bit (0.013A) |
| Peak | `0x17FF` | ±160mV (gain /4) | 213A | 10µV/bit |

### Hysteresis

```
|I| < 90A   → ±80mV  (high-resolution standby)
|I| ≥ 100A  → ±160mV (peak load, no saturation)
```
Gap 10A mencegah chattering saat beban hover di sekitar threshold.

### INA219 Config Register (TI Datasheet SBOS448G)

```
Bit 15    : RST (1 = reset)
Bit 14    : RESERVED (must be 0)
Bit 13    : BRNG (0 = 16V FSR, 1 = 32V FSR)
Bits 12:11: PG (00=±40mV, 01=±80mV, 10=±160mV, 11=±320mV)
Bits 10:7 : BADC (4-bit, 1111 = 12-bit/128-sample/68ms)
Bits 6:3  : SADC (4-bit, 1111 = 12-bit/128-sample/68ms)
Bits 2:0  : MODE (111 = shunt+bus continuous)
```

### Safety Features

1. **Register readback verification** — setiap PGA switch diverifikasi via I²C readback
2. **Sample discard** — sample pertama setelah PGA switch dibuang (settling time 68ms)
3. **Saturation detection** — di ±80mV mode, jika shunt > 78mV → emergency switch ke ±160mV
4. **pga_mode telemetry** — field `bat.pgaMode` ("80mV"/"160mV") dikirim ke backend + PWA

### Verifikasi Register

```bash
python3 scripts/decode_ina219_config.py 0x0FFF 0x17FF
python3 scripts/test_ina219_config_registers.py
```

---

## 6. Panduan Deployment Lengkap

### Prasyarat

- **PlatformIO Core** 6.1.18+ (`pip install platformio`)
- **Python** 3.11+ (untuk scripts)
- **Git** dengan GPG signing configured
- **Google Account** (untuk Apps Script + Sheets)
- **Vercel Account** (untuk PWA deployment — lihat repo PWA)

### Langkah 1: Clone Repositori

```bash
git clone https://github.com/desvandi/PLTSMonitoring_Firmware-Backend.git
cd PLTSMonitoring_Firmware-Backend
```

### Langkah 2: Build Firmware

#### Opsi A: Modular Production (Recommended)

```bash
cd firmware

# Set production secrets (WAJIB sebelum build production)
export MQTT_BROKER_HOST="your-broker.example.com"
export MQTT_BROKER_PORT="8883"
export MQTT_USERNAME="plts-device"
export MQTT_PASSWORD="your-password"
export MQTT_ROOT_CA="$(cat roots.pem)"
export OTA_ED25519_PUBLIC_KEY_HEX="your-64-hex-char-public-key"
export OTA_HTTPS_ROOT_CA="$(cat ota-ca.pem)"

# Build
pio run -e production

# Output: .pio/build/production/firmware.bin
```

#### Opsi B: Development (tanpa secrets)

```bash
cd firmware
pio run -e development
```

#### Opsi C: Generic (ESP Web Tools browser flashing)

```bash
cd firmware-generic
pio run -e esp32dev
# Output: bin/plts_firmware_v1.9.3.bin
```

### Langkah 3: Sign Firmware (Ed25519)

```bash
# Generate keypair (sekali saja)
python3 scripts/sign_firmware.py --gen-keys
# Output: firmware_signing_private.pem + firmware_signing_public.pem

# Sign binary
python3 scripts/sign_firmware.py --sign .pio/build/production/firmware.bin
# Output: firmware.bin.sig (128 hex chars = 64 bytes Ed25519)

# Verify
python3 scripts/sign_firmware.py --verify firmware.bin.sig firmware.bin
```

### Langkah 4: Flash ke ESP32

#### Via USB (PlatformIO)

```bash
cd firmware
pio run -e production -t upload
# Monitor:
pio device monitor -b 115200
```

#### Via ESP Web Tools (Browser)

1. Host `firmware-generic/bin/` di web server (GitHub Pages, Vercel, dll)
2. Buka PWA → Setup → "Install Firmware" button
3. Browser akan flash via Web Serial API

### Langkah 5: Deploy Backend Google Apps Script

1. Buka [script.google.com](https://script.google.com) → New Project
2. Copy seluruh isi `code.gs/Code.gs` ke editor
3. Buat Google Sheet baru, copy ID dari URL
4. Di Apps Script, jalankan `setupMasterTemplate()` (Run → function)
5. Set `AUTH_TOKEN` di Config sheet (minimal 16 karakter, tidak boleh "CHANGE_ME")
6. Deploy → New Deployment → Web App
   - Execute as: Me
   - Who has access: Anyone
7. Copy Web App URL → simpan untuk konfigurasi PWA

### Langkah 6: Generate OTA Signing Keys

```bash
# Generate Ed25519 keypair untuk OTA
python3 scripts/sign_firmware.py --gen-keys

# Public key hex (untuk firmware Config.h / OTA_ED25519_PUBLIC_KEY_HEX)
xxd -p -c 64 firmware_signing_public.pem | head -1
# Atau:
python3 -c "
from cryptography.hazmat.primitives import serialization
key = open('firmware_signing_public.pem','rb').read()
pub = serialization.load_pem_public_key(key)
raw = pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
print(raw.hex())
"
```

### Langkah 7: Konfigurasi ESP32 (via PWA)

1. Buka PWA di browser (deploy dari repo PWA)
2. Hubungkan ke WiFi AP ESP32 ("PLTS-Setup-XXXX")
3. PWA akan redirect ke setup page
4. Isi: WiFi SSID+password, GAS URL, AUTH_TOKEN, MQTT broker (opsional)
5. Submit → ESP32 reboot → connect ke WiFi

### Langkah 8: Verifikasi

1. Buka Serial Monitor: `pio device monitor -b 115200`
2. Verifikasi log INA219:
   ```
   [INA219 0x40] config readback OK: 0x0FFF (BRNG=0 16V, PGA=1 ±80mV, BADC=15, SADC=15, MODE=7)
   [INA219 0x40] init: shunt=0.7500 mΩ, cal=0x..., sign=-1.0, pga=80mV
   ```
3. Buka PWA → verifikasi telemetri muncul
4. Cek Google Sheet → data telemetri masuk

---

## 7. Konfigurasi Firmware

### Environment Variables (Production Build)

| Variable | Wajib | Description |
|----------|-------|-------------|
| `MQTT_BROKER_HOST` | Ya | MQTT broker hostname |
| `MQTT_BROKER_PORT` | Ya | MQTT port (8883 untuk TLS) |
| `MQTT_USERNAME` | Ya | MQTT username |
| `MQTT_PASSWORD` | Ya | MQTT password |
| `MQTT_ROOT_CA` | Ya | PEM-encoded Root CA untuk TLS |
| `OTA_ED25519_PUBLIC_KEY_HEX` | Ya | 64 hex char Ed25519 public key |
| `OTA_HTTPS_ROOT_CA` | Ya | PEM Root CA untuk HTTPS OTA download |

### Runtime Config (via PWA / NVS)

| Config | Default | Description |
|--------|---------|-------------|
| `deviceName` | "PLTS-XXXX" | Nama device |
| `siteName` | "Site" | Nama lokasi |
| `idleCurrentThreshold` | 0.5A | Deadband arus idle |
| `cfgBmsProtocol` | "auto" | Protokol BMS (auto/CAN/Modbus) |

### INA219 Constants (`firmware/Core/Config.h`)

```cpp
static constexpr float    INA219_SHUNT_OHM          = 0.00075f;   // 0.75 mΩ
static constexpr float    INA219_PGA_SWITCH_UP_A    = 100.0f;     // switch to 160mV
static constexpr float    INA219_PGA_SWITCH_DOWN_A  = 90.0f;      // switch back to 80mV
static constexpr uint16_t INA219_CONFIG_PGA_80MV   = 0x0FFF;     // ±80mV, 12b/128s, cont
static constexpr uint16_t INA219_CONFIG_PGA_160MV  = 0x17FF;     // ±160mV, 12b/128s, cont
```

### Voltage Divider Constants

```cpp
static constexpr float DIVIDER_R1       = 190000.0f;  // 190 kΩ
static constexpr float DIVIDER_R2       = 10000.0f;   // 10 kΩ
static constexpr float DIVIDER_RATIO    = 20.0f;      // (R1+R2)/R2
static constexpr float ADC_FINE_TUNE    = 1.000000f;  // kalibrasi multimeter
```

---

## 8. Backend Google Apps Script

### Struktur `code.gs/Code.gs`

| Function | Fungsi |
|----------|--------|
| `doPost(e)` | Entry point — parse JSON, HMAC auth, route action |
| `recordTelemetry_(body, deviceKey)` | Simpan telemetri ke Google Sheets |
| `latestTelemetry_(deviceKey)` | Baca telemetri terbaru |
| `otaGetManifest_(deviceKey)` | Serve OTA manifest ke device |
| `otaPublish_(body, deviceKey)` | Publish OTA command ke device |
| `emergencyCommand_(body, deviceKey)` | Dispatch emergency command |
| `setupMasterTemplate()` | Inisialisasi sheets + config (run sekali) |

### Google Sheets Schema

| Column | Field | Type |
|--------|-------|------|
| 0 | timestamp | DateTime (server) |
| 1 | device_key | String |
| 2 | sequence | Integer |
| 3 | event_time | DateTime (device) |
| 5 | v_bat | Float |
| 6 | i_bat_dc | Float |
| 7 | p_bat_dc | Float |
| 8 | soc | Float |
| ... | ... | ... |
| 39 | ina219_pga_mode | String ("80mV"/"160mV"/"") |

### HMAC Authentication

```
Authorization: HMAC <deviceKey>:<base64(HMAC-SHA256(authToken, canonicalString))>
```

---

## 9. CI/CD Pipeline

### Workflow: `.github/workflows/build-firmware.yml`

```
push/PR to main → [test] → [build-staging] → [build-generic-tree] → [build-production]
                                                                          ↓
                                                              [release-gate]
                                                                          ↓
                                                          [verify-tag-signature] (tag push only)
                                                                          ↓
                                                      [verify-hardware-acceptance] (tag push only)
                                                                          ↓
                                                          [release-publish] (tag push only)
```

### Jobs

| Job | Trigger | Fungsi |
|-----|---------|--------|
| Python tests | All pushes | 30+ unit/property/contract tests |
| Build staging | All pushes | PlatformIO staging env |
| Build generic | All pushes | PlatformIO generic tree |
| Build production | All pushes | PlatformIO production env (signed) |
| Reproducible build 2x | All pushes | REL-04: build A vs clean build B — SHA harus byte-identical (modular + generic) |
| Release gate | All pushes | Strict invariant check |
| Verify tag signature | Tag push (v*) | GPG tag + authorized signer verification |
| Verify HW acceptance | Tag push (v*) | Hardware acceptance JSON gate |
| Release publish | Tag push (v*) | Create GitHub Release with 20+ assets (requires reproducible-build PASS) |

### Branch Protection

- `main`: PR required, 0 approvals, 4 required status checks
- Tags `v*`: deletion + non-fast-forward + creation blocked

---

## 10. Release Engineering

### Release Chain

```
source → build → sign (Ed25519) → gate → tag-verify (GPG) → hw-verify → release
```

### GPG Tag Signing

```bash
# Generate GPG key (RSA 4096)
gpg --full-generate-key

# Configure git
git config --global user.signingkey <KEY_ID>
git config --global tag.gpgsign true

# Sign release tag
git tag -s -a v1.9.2 -m "Release v1.9.2 — INA219 dynamic gain (canonical fix)"
```

### Provenance Binding

Setiap release menyertakan `provenance-binding.json` yang mengikat:
```
tag → signed tag object → release commit → source commit
→ hardware-tested binary SHA → released binary SHA
→ GitHub Release → PWA canonical release identity
```

### Release Assets (20 files)

| Asset | Description |
|-------|-------------|
| `modular-firmware.bin` | Modular production firmware (signed) |
| `modular-firmware.bin.sig` | Ed25519 signature |
| `modular-firmware.bin.sha256` | SHA-256 hash |
| `modular-release.json` | Release metadata |
| `modular-provenance.json` | Build provenance |
| `modular-manifest-canonical.json` | Schema 2.0 OTA manifest |
| `generic-*.bin` | Generic tree binaries |
| `hardware-acceptance.json` | Physical test evidence |
| `provenance-binding.json` | Explicit provenance chain |

---

## 11. Hardware Acceptance

### General Hardware Acceptance (v1.8.0+)

Protocol 11-criteria: boot, sensors, alarms, OTA REST, OTA MQTT, rollback, emergency relay, config persistence, security, 24h soak, factory reset.

```bash
python3 scripts/verify_hardware_acceptance.py \
  --version 1.8.0 \
  --source-commit <SHA> \
  --release-json modular-release.json \
  --hw-dir docs/hardware-acceptance
```

### INA219 Hardware Acceptance (v1.9.2/v1.9.3)

Protocol 12-criteria khusus INA219: config readback, low/mid/peak current accuracy, PGA transitions, hysteresis, current sign, voltage divider, power calculation, telemetry pga_mode. Gunakan `v1.9.3.md` untuk release line v1.9.3 (measurement chain identik dengan v1.9.2; bukti SHA kini byte-exact karena build reproducible).

```bash
python3 scripts/verify_ina219_hardware_acceptance.py \
  --version 1.9.3 \
  --source-commit <SHA> \
  --release-json modular-release.json \
  --hw-dir docs/hardware-acceptance
```

### OTA Physical Test (v1.8.0+)

Protocol 16-criteria: PWA → GitHub Release → download → SHA verify → upload → device flash → reboot → version check → relay safe state → rollback.

```bash
python3 scripts/verify_ota_evidence.py \
  --version 1.8.0 \
  --canonical-release-json modular-release.json \
  --ota-dir docs/ota-physical-test
```

---

## 12. Testing

### Run All Tests

```bash
# Python unit + property tests
python3 scripts/test_ina219_config_registers.py
python3 scripts/test_version_identity.py
python3 scripts/test_voltage_calibration.py
python3 scripts/test_soc_calculation.py
python3 scripts/test_emergency_firmware_logic.py
# ... 30+ test scripts

# JavaScript tests
node scripts/test_wave1_integration.js
node scripts/test_wave2_data_honesty.js
node scripts/test_gas_contract.js
```

### Key Test Scripts

| Script | Fungsi |
|--------|--------|
| `test_ina219_config_registers.py` | Verifikasi PGA register constants per datasheet |
| `test_version_identity.py` | Version parity antara modular + generic + manifest |
| `test_wave12_contract_regression.py` | Cross-layer contract regression |
| `test_ed25519_interop.py` | Ed25519 signature interop (Python ↔ firmware) |
| `release_gate.py` | Master release gate (strict mode) |

---

## 13. Troubleshooting

### INA219: "config MISMATCH" di Serial Monitor

**Penyebab:** I²C write gagal atau register constant salah.

**Fix:**
1. Cek wiring I²C (SDA=GPIO21, SCL=GPIO22, pull-up 4.7kΩ)
2. Verifikasi constant: `python3 scripts/test_ina219_config_registers.py`
3. Jika constant benar tapi readback gagal → ganti modul INA219

### INA219: Arus terbaca 0A / NaN

**Penyebab:** R100 bawaan modul belum dicopot, atau shunt belum terpasang.

**Fix:**
1. Lepas R100 (0.1Ω) dari modul INA219
2. Pasang shunt 100A/75mV di jalur GND
3. Verifikasi: VIN+ dan VIN− terhubung ke baut shunt

### Voltage Divider: Tegangan tidak akurat

**Penyebab:** Toleransi resistor ±5% → error hingga ±2V di 57.5V.

**Fix:**
1. Ukur tegangan aktual dengan multimeter
2. Hitung `ADC_FINE_TUNE = tegangan_aktual / tegangan_PWA`
3. Update di `Config.h`: `static constexpr float ADC_FINE_TUNE = 1.004016f;`
4. Rebuild + reflash

### OTA: "signature header missing (PRODUCTION)"

**Penyebab:** PWA tidak mengirim header `X-Signature`.

**Fix:** Update PWA ke versi terbaru (commit `9c8510a`+) yang mengirim `X-Expected-SHA256`, `X-Signature`, `X-Firmware-Version`.

### MQTT: "TLS connection failed"

**Penyebab:** Root CA tidak ter-set atau broker tidak support TLS.

**Fix:**
1. Set `MQTT_ROOT_CA` environment variable sebelum build
2. Verifikasi broker menggunakan port 8883 (TLS) atau 8884 (TLS+auth)
3. Test: `openssl s_client -connect broker:8883 -showcerts`

---

## 14. Changelog

### v1.9.3 (Current)
- **Reproducible build (REL-03/REL-04 CLOSED)** — `FIRMWARE_BUILD_DATE` tidak lagi `__DATE__`/`__TIME__` (wall-clock); kini diturunkan dari `SOURCE_DATE_EPOCH` (timestamp commit HEAD git) via `firmware/scripts/set_build_date.py`. Dua build dari source yang sama = SHA-256 identik.
- **CI job `reproducible-build`** — build 2× (modular production + generic) dari clean tree, compare SHA; `release-publish` kini mewajibkan job ini (release tak bisa dipublish tanpa bukti determinisme).
- **buildDate semantics** — `/api/version` `buildDate` kini melaporkan tanggal SOURCE commit (identitas provenance), bukan wall-clock kompilasi.
- **HW acceptance v1.9.3 protocol** — `docs/hardware-acceptance/v1.9.3.md` + template (12 kriteria sama dengan v1.9.2; evidence `firmwareSha256` kini byte-exact vs released binary).
- **Version parity** — 1.9.3 di modular (Config.h) + generic (plts_firmware_v1.ino + manifest.json) + mirror PWA (`public/firmware/manifest.json`).
- Tidak ada perubahan measurement chain — INA219 PGA 0x0FFF/0x17FF, hysteresis 90A/100A, voltage divider 190k/10k, pga_mode telemetry: semuanya identik dengan v1.9.2.

### v1.9.2
- **INA219 dynamic gain switching** — PGA ±80mV/±160mV dengan hysteresis 90A/100A
- **Canonical register fix** — constants 0x0FFF/0x17FF per TI datasheet SBOS448G
- **Register readback verification** — setiap PGA switch diverifikasi via I²C readback
- **Sample discard** — sample pertama setelah PGA switch dibuang (settling time)
- **Voltage divider 190kΩ/10kΩ** (20:1 ratio) untuk tegangan baterai 42–57.5V
- **pga_mode telemetry** — field `bat.pgaMode` ("80mV"/"160mV") di JSON payload
- **CURRENT_SPIKE_REJECT_A** raised 120A → 160A (software ceiling, NOT hardware rating)
- **Mandatory measurement evidence** — 17 field observed wajib diisi di HW acceptance

### v1.8.0
- **8-channel relay integration** via PCF8574 I²C expander
- **RelayController** with safety supervisor (maxOnTime, minOnTime, antiChatter)
- **5-state lockout** (NORMAL→TRIPPED→ACKNOWLEDGED→CLEARED→ARMED)
- **Interlock engine** (mutual exclusion + dead time)
- **E-WAVE safety cascade** (trip → all relay channels OFF)
- **REST API**: `/api/relays`, `/api/relays/{id}/{on|off|pulse}`, `/api/relays/all_off`
- **PWA relay control view** with 3-tier state model

### v1.7.x
- OTA boot-health fix (`esp_ota_mark_app_valid_cancel_rollback`)
- Mixed-fleet manifest target self-check
- PZEM-004T real AC meter support
- Emergency relay + E-stop sense
- 2nd ACS712 channel (genset→inverter)
- Cross-layer contract regression tests (WAVE 7-13)

### v1.6.x
- External BMS/inverter comm (CAN, Modbus RTU/TCP)
- SOC provenance (BMS_DIRECT | SHUNT_COULOMB | OCV_ESTIMATED)
- Transaction journal for command dedup
- I²C bus recovery (NOISE-3)

---

## License

MIT — see [LICENSE](LICENSE)

## Kontak

- **GitHub Issues:** [github.com/desvandi/PLTSMonitoring_Firmware-Backend/issues](https://github.com/desvandi/PLTSMonitoring_Firmware-Backend/issues)
- **PWA Repo:** [github.com/desvandi/PLTSMonitoring_PWA](https://github.com/desvandi/PLTSMonitoring_PWA)
