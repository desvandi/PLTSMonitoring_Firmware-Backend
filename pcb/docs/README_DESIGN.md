# README_DESIGN — PLTS Monitoring Controller PCB

> Dokumen desain otoritatif PCB `plts-monitoring-controller` (13 sheet
> schematic, 2-layer FR-4 1.6 mm 2 oz, 140 × 100 mm).
> Dokumen terkait: HARDWARE_REVISION.md (perubahan), TEST_PLAN.md
> (validasi), BOM.csv / PickAndPlace.csv (fab S10).

## 1. Arsitektur Sistem

```
BATERAI 48V (15S LiFePO4, 45–54V, 100A shunt path)
   │ J_BAT → F1 0.5A → D1 US1M (reverse) → PROTECTED_VBAT
   │           D2 SMBJ58A TVS, C50 2.2uF/100V, C57 47uF/100V
   ▼
M1 LM2596HVS module (60V/3A buck) ──→ +5V (C52 47uF, F2 0.5A → FB50 → +5V_RELAY)
   │                                      │
   │                                      └→ F4 1.1A → FB2 → +5V_RELAY_EXP (sheet 12)
   ▼
U11 AP2112K-3.3 ──→ +3V3 (C60/C61 in, C62/C63 out)

U1 ESP32-WROOM-32E (pusat: 11 task FreeRTOS — lihat firmware/)
   ├── I²C 3V3 (GPIO21/22): INA219 0x40 (VBAT shunt), SHT31 0x44,
   │   DS3231 0x68 (RTC + backup CR2032), J_I2C_EXP
   │   └── Q22/Q23 BSS138 shifter → I²C 5V → J_RELAY_EXP (PCF8574 0x20 modul eksternal)
   ├── UART2 (GPIO16/17) → MAX3485 RS485 (Modbus RTU: PZEM + BMS)
   ├── TWAI (GPIO25/26) → SN65HVD230 CAN (Pylontech)
   ├── UART0 + CP2102N + USB-C (prog/debug), Q1/Q2 auto-EN/IO0
   ├── ADC: VBAT divider (GPIO34), ACS712×2 30A (GPIO35/32, divider 0.6)
   ├── GPIO27 → RELAY_CTRL (aktif-LOW, PC817 → Q20 → K1 E-WAVE)
   ├── GPIO14 ← E_STOP_SENSE (aktif-HIGH trip, Q21 inverting)
   ├── GPIO2 STATUS_LED, GPIO4 RS485_DIR, GPIO18/19 PZEM UART1
   └── J_EXP (GPIO13/23/33/36/39 spare), test point TP_* (sheet 11)

E-STOP CHAIN (09_EMERGENCY — SAFETY-CRITICAL §57):
+5V_RELAY → J_ESTOP.1 → loop NC eksternal → J_ESTOP.2 = RELAY_RET →
K1 COIL+ → COIL− → Q20 AO3400A → PGND (star BATTERY−)
D21 SS14 flyback, D22/D23 SMAJ5.0A TVS field, Q20 gate: R70/R71
Drive: +3V3→R72→PC817 LED; katoda→GPIO27 (SINK, aktif-LOW) + R78 pull-up
Sense: RELAY_RET→R75/R77→Q21 gate; drain→GPIO14 (R76 pull-up, C75 1ms)
E-STOP MEMUTUS SISI SUPPLAI COIL — bukan sisi drain — JANGAN DIUBAH.
```

## 2. Grounding (arsitektur §3.4)

- **BAT−**: terminal negatif baterai (topologi kelvin shunt FL-2 100A/75mV).
- **PGND**: power return (M1, Q20, relay, ACS IP) — star NET-TIE **NT0**
  (BAT−↔PGND) di sheet 03.
- **AGND**: analog (INA219, ACS712 VIOUT, ADC divider) — NET-TIE **NT1**
  (AGND↔PGND).
- **DGND**: digital (ESP32, CP2102, sensor I²C, e-stop sense) — NET-TIE
  **NT2** (DGND↔PGND) — satu titik, arus divider 34 µA.
- Pour: F.Cu + B.Cu zone per-domain (lihat daftar zone); ACS1/2_IP± pour
  tebal untuk jalur 100A.
- Mains PZEM (sheet 10): zona terisolasi + creepage ≥ 6.4 mm (rule
  MAINS_SELV_CREEPAGE di .kicad_dru) — L/N/250VAC hanya di J_PZEM_AC/F3/RV3.

## 3. Peta Pin ESP32 (kontrak — JANGAN diubah, §2 brief)

| GPIO | Fungsi | GPIO | Fungsi |
|------|--------|------|--------|
| 2 | STATUS_LED | 21 | I2C_SDA |
| 4 | RS485_DIR | 22 | I2C_SCL |
| 14 | E_STOP_SENSE (INPUT_PULLUP, HIGH=trip) | 25 | CAN_TX |
| 15 | (cadangan) | 26 | CAN_RX |
| 16 | RS485_TX | 27 | RELAY_CTRL (aktif-LOW) |
| 17 | RS485_RX | 32 | ACS2_ADC |
| 18 | PZEM_RX | 33 | (spare J_EXP) |
| 19 | PZEM_TX | 34 | VBAT_SENSE (in-only) |
| 13 | (spare J_EXP) | 35 | ACS1_ADC (in-only) |
| 36/39 | (spare J_EXP, in-only) | EN/IO0 | boot (Q1/Q2 auto) |

## 4. Kontrak I²C

- Bus 3V3, pull-up R40/R41 4.7k, 100 kHz (`I2C_FREQUENCY=100000`).
- Alamat: **INA219 0x40, SHT31 0x44, DS3231 0x68, PCF8574 (eksternal) 0x20**
  — tidak ada konflik.
- Ekspansi relai: level shifter BSS138 (Q22/Q23) ke sisi 5V; pull-up sisi
  5V R79/R84 4.7k ke +5V_RELAY_EXP. Saat F4 putus, sisi 5V float — bus 3V3
  sensor on-board tetap berfungsi (isolasi fault).
- PCF8574: open-drain, aktif-LOW, boot 0xFF = semua relai OFF.

## 5. Rantai E-Stop (ringkas — detail di HARDWARE_REVISION §2–3)

| Kondisi | K1 | E_STOP_SENSE |
|---------|----|--------------|
| Normal, GPIO27 LOW (ON) | energized | LOW (sehat) |
| GPIO27 HIGH / floating | OFF | LOW |
| E-stop DITEKAN | OFF (suplai coil terputus FISIK) | HIGH (trip) |
| Kabel loop putus | OFF | HIGH |
| F2/+5V_RELAY hilang | OFF | HIGH |
| Jalur sense putus | — | HIGH (INPUT_PULLUP) |

## 6. Layout (keputusan kunci)

- 140 × 100 mm; mount hole M3 di 4 sudut; edge connectors di tepi
  kiri/kanan; mains di kanan-bawah terisolasi.
- Zona: power kiri-atas → digital tengah → e-stop/relay bawah-kiri →
  komms kanan; shunt FL-2 atas dengan kelvin sense.
- Antenna ESP32: keepout (24–72, 93.5–100) tanpa copper.
- Netclass: Default 0.2/0.15; PWR 0.5/0.15 (+3V3/+5V/relay rails/BAT);
  MAINS_AC 0.5 + creepage 6.4 mm vs SELV.
- Routing: autorouter (Freerouting) untuk signal, review manual untuk
  kelvin/power; via 0.6/0.3 (PWR 0.8/0.4).
