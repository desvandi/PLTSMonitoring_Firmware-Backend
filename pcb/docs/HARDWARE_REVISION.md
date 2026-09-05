# HARDWARE REVISION — S12 Relay Expansion Integration

> Dokumen rekonsiliasi revisi hardware PCB `plts-monitoring-controller`.
> Status: DRAFT untuk review — JANGAN direlease tanpa sign-off engineer.
> Scope: schematic 13 sheet + PCB 2-layer 140×100 mm + BOM S10.

## 1. Ringkasan Perubahan

| # | Scope | Perubahan | Status |
|---|-------|-----------|--------|
| R1 | 09_EMERGENCY | Drive relay AKTIF-LOW (kontrak firmware `EmergencyRelayDriver` GPIO27) | DONE — schematic + PCB |
| R2 | 09_EMERGENCY | Sense e-stop INVERTING (HIGH = trip, fail-safe INPUT_PULLUP GPIO14) | DONE — schematic + PCB |
| R3 | firmware `Core/Config.h` | Konstanta ACS712 0.185 → 0.0396 V/A (divider PCB 0.6 × 66 mV/A) | DONE (branch terpisah) |
| R4 | 12_RELAY_EXP (BARU) | Antarmuka modul relai ekspansi 8-ch PCF8574 + level shifter BSS138 | DONE — schematic + PCB |
| R5 | 01_POWER | C52 bulk +5V 22 µF → 47 µF (headroom dip koil relay saat LM2596 load-step) | DONE |

## 2. R1 — Drive Relay Aktif-LOW (09_EMERGENCY)

### 2.1 Masalah
Topologi lama: `RELAY_CTRL → R72 330R → OPT1 anoda; katoda → DGND`
(**aktif-HIGH**). Kontrak firmware (docs/RELAY_GPIO_HARDWARE_CONTRACT.md
reserved-GPIO + EmergencyRelayDriver): **GPIO27 = OUTPUT active-LOW** —
mismatch polaritas menyebabkan relay terbalik (ON saat idle-HIGH, OFF saat
command ON).

### 2.2 Solusi (PCB — keputusan owner R1)
```
+3V3 ── R72 330R ── OPT1 anoda
OPT1 katoda ── RELAY_CTRL ── GPIO27 (ESP32 SINK)
                      │
                      └── R78 10k ── +3V3   (pull-up fail-safe)
```
- `LOW = energized` (IO27 menyerap (3.3−1.2)/330 = 6,4 mA — aman untuk GPIO
  ESP32; IF maks PC817 50 mA).
- **R78 10k baru**: GPIO27 floating (boot/reset/crash/watchdog) → LED PC817
  MATI → Q20 OFF (R71 pulldown) → K1 OFF. Fail-safe ganda (R71 + R78).
- Output opto tidak berubah: C=+5V, E→R70 100R→gate Q20; E-stop tetap
  memutus SISI SUPPLAI coil (topologi §11.1 terkunci — tidak diubah).

### 2.3 Netlist kunci (verifikasi ERC/netlist)
`RELAY_CTRL = OPT1.2 + R78.1 + TP_RELAY_CTRL + U1.IO27`

## 3. R2 — Sense E-Stop Inverting (09_EMERGENCY)

### 3.1 Masalah
Sense lama: divider non-inverting `RELAY_RET → R75/R76 → GPIO14`
(**HIGH = sehat, LOW = trip**). Kontrak firmware: **GPIO14 INPUT_PULLUP** —
jalur putus → pull-up internal → HIGH = "sehat" = **fail-DANGEROUS**.

### 3.2 Solusi (PCB — keputusan owner R2)
```
RELAY_RET ── R75 47k ──┬── R77 100k ── DGND      (divider 3,4 V saat sehat)
                        └── Q21 BSS138 gate
Q21 drain ── E_STOP_SENSE ── GPIO14
Q21 drain ── R76 10k ── +3V3  (pull-up)
Q21 drain ── C75 100nF ── DGND (τ = 1 ms)
Q21 source ── DGND
```
- E-stop SEHAT (loop NC tertutup, rail ada): Vgs = 5×100/147 = 3,4 V → Q21
  ON → sense ≈ 0 V (**LOW = sehat**).
- E-stop TRIP / kabel putus / F2 putus / rail hilang: divider 0 V → Q21 OFF
  → R76 pull-up → sense = 3,3 V (**HIGH = trip**).
- Jalur sense putus → INPUT_PULLUP internal → HIGH = trip (fail-safe ✓).
- Q21 short D-S → sense LOW palsu → firmware mismatch-check menangkap.

### 3.3 Netlist kunci
`E_STOP_SENSE = Q21.3 + R76.1 + C75.1 + TP_ESTOP + U1.IO14`
`E_STOP_DIV (Net-(Q21-G)) = Q21.1 + R75.2 + R77.1`

## 4. R4 — Sheet 12_RELAY_EXP (BARU)

### 4.1 Arsitektur (docs/RELAY_INTEGRATION_ARCHITECTURE.md §2.2)
PCF8574 + 8 relai + driver berada di **modul eksternal**; PCB ini menyediakan:
- **Rail daya**: `+5V → F4 polyfuse 1.1A hold → FB2 ferrite 600R →
  +5V_RELAY_EXP` (beban 8×70 mA + PCF8574 ≈ 640 mA; margin fuse 1,7×).
- **J_RELAY_EXP** header 2×03: `1=+5V_RELAY_EXP 2=PGND 3=SDA_5V 4=PGND
  5=SCL_5V 6=INT_5V` (GND ganda mereduksi loop arus koil pada ribbon).
- **Level shifter I²C BSS138** (Q22/Q23): bus utama 3V3 (SHT31/DS3231/INA219,
  R40/R41 4k7) ↔ sisi 5V PCF8574 (VIH 0.7×VCC = 3,5 V > 3,3 V → shifting
  WAJIB). Pull-up sisi 5V: R79/R84 4k7 ke `+5V_RELAY_EXP` (rail mati → bus
  3V3 tetap hidup untuk sensor on-board — isolasi fault alami).
- **TVS D24/D25 SMAJ5.0A (DNP)** di SDA/SCL_5V — proteksi kabel eksternal,
  pasang bila lingkungan industrial bernoise.
- **INT divider** R82 10k/R83 15k (0,6): INT_5V 5 V → 3,0 V aman GPIO.
  v1 firmware TIDAK memakai INT (kontrak §6); net lokal `RELAY_EXP_INT`
  disiapkan untuk readback kontak aux (jumper wire ke GPIO13/23/33 saat
  fitur diaktifkan).

### 4.2 Keamanan (Safety Boundary §4)
- E-WAVE GPIO27 **terisolasi** dari sistem relai ekspansi (antarmuka hanya
  I²C; PCF8574 boot 0xFF = semua OFF; aktif-LOW sesuai modul standar).
- Tidak ada GPIO baru dipakai (SDA/SCL shared 21/22) — kontrak reserved-GPIO
  §4.3 terpenuhi.

### 4.3 Netlist kunci
`+5V_RELAY_EXP = FB2.2 + J_RELAY_EXP.1 + R79.1 + R84.1 (+PWR_FLAG)`
`I2C_SDA = Q22.2(S) + R40.2 + U1.33 + U3.6 + U6.1 + U7.15 + J_I2C_EXP.3`
`Net-(D24-A2) = Q22.3(D) + R79.2 + D24.2 + J_RELAY_EXP.3`

## 5. R5 — C52 22 µF → 47 µF (01_POWER)

Keputusan owner: daya relai dari rail +5V utama (opsi a). K1 HF115F coil
~125 mA + modul ekspansi (via F4) hingga 700 mA saat 8 koil simultan —
transien load-step LM2596HVS 3 A dijaga C52 47 µF 25V X5R (1210) + C50/C57
input. Verifikasi drop < 250 mV = item TEST_PLAN P9.

## 6. Deviasi & Catatan Placement (DN-S12)

| ID | Item | Catatan |
|----|------|---------|
| DN-S12-01 | J_RELAY_EXP di tepi kiri (6.5, 58) | Dekat J_I2C_EXP/J_EXP; kabel keluar tepi board; jauh dari antenna ESP32 |
| DN-S12-02 | Q22/Q23 di koridor M1-bottom | x 13–17, y 52–62; clearance M1 courtyard 0,35 mm (courtyard M1 direpair ke geometri true pads+fab) |
| DN-S12-03 | D24/D25 (DNP) di koridor y 63.65–65.95 | Antara M1 dan U1; sinyal I²C_5V pendek ke header |
| DN-S12-04 | F4/FB2 di (60, 63.3)/(54.5, 65.5) | Adjacent F2/M1-pad3 (+5V tap pendek); FB2 → rail kiri via koridor y~63 |
| DN-S12-05 | R82/R83 INT divider (11, 61.5)/(40, 64.8) | Relay_EXP_INT jalur sense high-Z 29 mm — tidak noise-critical (future use) |
| DN-S12-06 | Courtyard repair M1/U1/K1/J_I2C_EXP/J_EXP/R87 | Hand-drawn courtyards generasi lama (garis tidak tertutup/oversize/artefak teks) diganti rect true-geometry pads+fab+0.25 — DRC jadi deterministik |
| DN-S12-07 | U1 antenna keepout dihormati | F4/FB2 digeser keluar zona (24–72, 93.5–100) — keepout embedded U1 |

## 7. Analisis Daya & Termal (rail +5V)

| Beban | Arus | Catatan |
|-------|------|---------|
| ESP32 WROOM (WiFi TX peak) | ~240 mA | U1 |
| Sensor + transceiver (SHT31, DS3231, INA219, MAX3485, SN65HVD230) | ~60 mA | |
| AP2112K quiescent + load 3V3 | ~5 mA | |
| CP2102 + USB | ~30 mA | |
| K1 coil (E-WAVE) | ~125 mA | Fuse F2 0.5A hold |
| Modul relai ekspansi (8 koil + PCF8574) | ~640 mA peak | Fuse F4 1.1A hold |
| **Total peak** | **~1.1 A** | LM2596HVS 3 A → margin 2.7× |

Disipasi: LM2596 η~88% @ 1.1 A/5 V → P_loss ≈ 0.75 W (heatsink modul).
F4 I²t: polyfuse 1.1A hold / ~2.2A trip.

## 8. Item Verifikasi BOM (gate e SAFETY REVIEW)

- K1 HF115F-1/5V-H: rating kontak ≥ 5 A/30 VDC + INRUSH aktuator aktual
  (asumsi #6 audit §11.1) — naik kelas bila inrush > 5 A.
- Pemetaan pin fisik HF115F-1 vs simbol (1=COIL+ 2=COIL− 3=COM 4=NO).
- F4 polyfuse 1812 1.1A hold: verifikasi derating suhu (1.1A @ 25C → ~0.88A
  @ 60C ambient — masih > 0.7A beban).
- BSS138 Vgs(th) max 2.0 V < 3.4 V drive divider ✓ (margin 1.4 V).
- PCF8574 address 0x20 (A0=A1=A2=GND modul) — konflik alamat dicek: 0x40
  (INA219), 0x44 (SHT31), 0x68 (DS3231) — unik ✓.

## 9. Hasil Autorouting & Rekap DRC (S12, sesi lanjutan)

**Metode**: Freerouting 2.1.0 (REST API v1, SSE snapshot) + bridge-router A*
deterministik (escape-cell validation, via-grid, string-pull smoothing) +
ripup-reroute selektif (≤3 track korban per jalur, heal-cycle no-rip).
Track merging kolinear file-level (4394 → ~1850 segmen).

**Status DRC final**: 0 error; 157 warning (147 silk kosmetik, 5 starved-thermal
pad yang konektivitasnya sudah dilakukan track routing, 3 dangling stub, 2
copper sliver); severity silk/starved di-set warning (manufacturing notice).

**Unconnected 13 pair** (dari 266 awal — 95% terselesaikan):
- L_AC (F3↔J_PZEM_AC) + N_AC (J_PZEM_AC↔RV3): koridor creepage 6.4 mm
  terhalang tetangga pad/routing — butuh push-and-shove interaktif GUI
  (known-issue). L_AC_FUSED + sebagian N_AC ter-route ✓.
- 11 pair fragmen/stub sisa iterasi (DGND×2, CAN_H/RX, EN, E_STOP_SENSE
  fragmen — net utama E_STOP_SENSE, RELAY_CTRL, GPIO33, RS485_TX, I2C,
  CAN, PZEM_RX, M_PZEM-TX sudah terhubung ke pin U1/U8/U9 — verifikasi
  pad-adjacency pcbnew).

**Fab outputs (S10)**: pcb/fab/gerbers/ (9 layer + Excellon PTH/NPTH
terpisah, format decimal, origin absolute) + pcb/fab/PickAndPlace.csv
(kedua sisi, mm) — siap kirim ke fab house 2-layer 1.6 mm FR-4 2 oz.

**Catatan routing ground**: PGND full-pour B.Cu + tie-down via NT0/NT1/NT2
(DGND pad NT2 + via PGND 66,71.5 — arsitektur star-tie dipertahankan);
pour fragmentasi ditambal bridge A*; island-removal zone default.
