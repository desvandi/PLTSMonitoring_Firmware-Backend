# Checklist Hardware Acceptance — Firmware Modular v1.7.0 (HW-001..HW-025)

> **Provenance (jujur)**: daftar HW-001..HW-025 yang asli hidup di dokumen
> remediasi lama yang **tidak pernah di-commit ke git** (riwayat chat
> proyek). Daftar di bawah adalah **rekonstruksi baru** untuk permukaan
> perangkat keras firmware modular saat ini — disusun agar satu item = satu
> pemeriksaan terukur. Sesuaikan nomor/isi dengan unit fisik Anda; baris
> kosong = belum dieksekusi. **Eksekusi oleh operator dengan perangkat
> fisik — suite mekanis tidak bisa menggantikan item bertanda [HW].**

## A. Sensor internal (I2C/ADC)

| ID | Pemeriksaan | Alat | Kriteria lulus | [HW] | Lulus? |
|----|-------------|------|----------------|------|--------|
| HW-001 | INA219 terdeteksi (0x40) | i2cdetect / log boot | banner `[INA219] init` + `sensorHealth.ina219=ONLINE` | ☐ | |
| HW-002 | Arus DC INA219 nol + arah tanda | Beban DC diketahui (mis. lampu 12 V 1 A) | |I| masuk akal ±5%, charge positif saat mengisi | ☐ | |
| HW-003 | Pembagi tegangan ADC (34) | Multimeter vs `/api/status battery.voltage` | deviasi < ±1% setelah kalibrasi 3-titik | ☐ | |
| HW-004 | Plausibilitas V (30–60 V) | — | di luar rentang → `OUT_OF_RANGE` + null (bukan 0) | ☐ | |
| HW-005 | ACS712 zero-offset | tanpa beban | `rmsCurrent` ≈ 0.0 ± 0.1 A setelah zero | ☐ | |
| HW-006 | ACS712 sensitivitas (185 mV/A) | beban AC diketahui | arus sesuai clampmeter ±5% (bug 1.85× tidak kambuh) | ☐ | |
| HW-007 | SHT31 (0x44) suhu/kelembapan | termometer ruang | deviasi < ±2 °C / ±5% RH | ☐ | |
| HW-008 | Titik embun + risiko kondensasi | — | dewPoint konsisten Magnus, `condensationRisk` masuk akal | ☐ | |
| HW-009 | RTC (opsional) | — | `timeQuality=VALID` setelah NTP; `lastSync` maju | ☐ | |

## B. Komunikasi BMS/eksternal

| ID | Pemeriksaan | Alat | Kriteria lulus | [HW] | Lulus? |
|----|-------------|------|----------------|------|--------|
| HW-010 | Transceiver RS485 (16/17/4) | osiloskop/LED DE | `bms.state` mencapai Probing; DE toggle saat poll modbus_rtu | ☐ | |
| HW-011 | Pylontech CAN (25/26, 500 kbps) | baterai nyata | lock ≤ 2 poll valid; `bms.protocol=pylontech_can` | ☐ | |
| HW-012 | Register Modbus RTU vs manual | manual baterai + §2 PANDUAN_VALIDASI | tabel pemetaan final terlampir, satuan cocok | ☐ | |
| HW-013 | Gerbang plausibilitas BMS | — | SOC 150%/V 80/sampah suhu → field null, alarm sesuai | ☐ | |
| HW-014 | Cek silang arus BMS vs shunt | beban DC nyata | `BMS_CURRENT_MISMATCH` TIDAK aktif saat sehat | ☐ | |
| HW-015 | Modbus TCP (host LAN) | host gateway | lock sukses; host = IP privat (bukan publik) | ☐ | |
| HW-016 | Konsol capture RS485 pasif | mode `rs485_console` | ≥ 30 frame vendor tercatat via `/api/rs485/frames`, DE tetap LOW | ☐ | |

## C. Jaringan & keamanan

| ID | Pemeriksaan | Alat | Kriteria lulus | [HW] | Lulus? |
|----|-------------|------|----------------|------|--------|
| HW-017 | WiFi STA + reconnect | matikan/nyalakan AP | `wifiReconnectCount` naik, telemetri lanjut | ☐ | |
| HW-018 | MQTT TLS (8883) | broker staging | `mqttConnected=true`, ws:// ditolak (guard W7-1) | ☐ | |
| HW-019 | NTP + kanal HMAC GAS | — | POST GAS SUCCESS (nonce diterima); `timeQuality=VALID` | ☐ | |
| HW-020 | Auth web perangkat | peramban | login + CSRF + rate-limit aktif; logout memutus | ☐ | |

## D. Lapisan darurat E-WAVE (v1.7.0) — WAJIB sebelum daya tinggi

| ID | Pemeriksaan | Alat | Kriteria lulus | [HW] | Lulus? |
|----|-------------|------|----------------|------|--------|
| HW-021 | Relai aktif-LOW GPIO 27 + isolasi boot | multimeter kontaktor | B1 lulus: terbuka sejak reset, sebelum WiFi | ☐ | |
| HW-022 | E-stop NC GPIO 14 | tombol fisik | B3 lulus: putus seketika; lepas tidak menyalakan ulang | ☐ | |
| HW-023 | Sembilan skenario bench B1–B9 | PROTOKOL_BENCH_DARURAT.md | semua ☑; gagal satu = stop komisioning | ☐ | |
| HW-024 | LED status GPIO 2 | mata | RUN solid; EMERGENCY kedip 2 Hz | ☐ | |

## E. Persistensi & OTA

| ID | Pemeriksaan | Alat | Kriteria lulus | [HW] | Lulus? |
|----|-------------|------|----------------|------|--------|
| HW-025 | NVS (`plts`, `plts_batt`, `plts_emg`) + OTA rollback | reboot + uji OTA | config/emg/trips bertahan; OTA gagal → rollback otomatis; boot-loop → chain naik | ☐ | |

## Catat hasil

Simpan tabel terisi + bukti (foto/foto layar/log serial) di dokumen situs.
Deployment produksi dinyatakan sah hanya bila **semua** baris [HW] bercentang
LULUS — persis seperti aslinya: *eksekusi operator, bukan klaim*.
