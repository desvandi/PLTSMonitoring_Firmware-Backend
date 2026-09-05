# TEST_PLAN — PLTS Monitoring Controller PCB

> Prosedur validasi hardware post-fab (gate P1-P10 + S12).
> Prasyarat: board direvisi sesuai HARDWARE_REVISION.md (R1-R5).
> Peralatan: PSU bench 48-54V (limit 0.5A), DMM, oscilloscope, USB serial,
  logic analyzer, modul relai 8-ch PCF8574 (aktif-LOW), loop e-stop NC.

## P0. Inspeksi & Kesiapan

- [ ] Visual: short solder, polaritas, retak; ukur board 140×100; multiplikasi
  footprint vs PickAndPlace.csv.
- [ ] Ukur rail TANPA supply: +5V↔GND > 1 kΩ; +3V3↔GND > 10 kΩ;
  +5V_RELAY_EXP↔PGND > 100 Ω (modul TIDAK terpasang).
- [ ] Verifikasi DNP: D24/D25 (TVS opsi), RV3 (MOV DNP), BT1 opsional.

## P1. Bring-up Power (tanpa ESP32 terpasang? — U1 soldered; tahap bertahap)

1. Supply 48V via J_BAT, limit 0.5A:
   - [ ] F1 utuh; D1 drop ~0.5V; PROTECTED_VBAT = 48V ± 0.3 (TP_BAT+).
   - [ ] M1 out = +5.0V ± 0.15 (TP_5V); ripple < 50 mVpp @ 0.3A.
   - [ ] U11 out = +3.3V ± 0.1 (TP_3V3); ripple < 30 mVpp.
   - [ ] +5V_RELAY (TP_5V_RELAY) = +5V − drop FB50; F2 utuh.
   - [ ] +5V_RELAY_EXP (J_RELAY_EXP.1) = +5V − drop F4/FB2 (modul lepas).
2. Termal: M1 heatsink < 70°C @ 0.5A load dummy.

## P2. ESP32 & Firmware

- [ ] USB enumerasi (CP2102N); flash firmware v1.8.x via esptool.
- [ ] Boot log 115200; no boot loop; STATUS_LED heartbeat.
- [ ] WiFi connect + NTP + RTC DS3231 set (baca 0x68 sukses).
- [ ] Sensor: SHT31 (0x44) T/RH masuk akal; INA219 (0x40) shunt 0mV idle.
- [ ] ADC: VBAT_SENSE = VBAT × 0.1042 (divider R20a/b/R21) ± 2%.
- [ ] ACS712: VIOUT = 2.5V (0A) ± 30mV; konstanta **39.6 mV/A**
  (divider 0.6 — verifikasi firmware Config.h R3!).

## P3. Komunikasi

- [ ] RS485 Modbus RTU ke PZEM-004T (UART2 9600 8N1, DE timing GPIO4).
- [ ] CAN TWAI 500k ke Pylontech BMS (GPIO25/26, SN65HVD230, 120R term?).
- [ ] UART0 console + auto-download (Q1/Q2 EN/IO0) via esptool reset.

## P4. Relay Expansion (S12 — PCF8574 module eksternal)

1. Hubungkan modul 8-ch ke J_RELAY_EXP (2x03: 5V/GND/SDA/SCL/INT).
   - [ ] I²C scan menemukan 0x20 (+ sensor on-board tetap: 0x40/0x44/0x68).
   - [ ] Tulis 0xFF → semua relai OFF; tulis 0xFE → relai ch1 ON.
   - [ ] Latency toggle 8 kanal < 10 ms; crosstalk: glitch SDA/SCL saat
     switching koil (snubber modul harus ada — kontrak §12).
   - [ ] Bus 3V3 sensor tetap hidup saat F4 dilepas (isolasi fault).
   - [ ] INT (opsional): ukur divider R82/R83 = 0.6; GPIO baca 3.0V HIGH.
2. Arus rail: 8 koil ON = 560-700 mA; F4 tidak trip; drop
   +5V_RELAY_EXP < 250 mV (C52 47uF menahan dip — R5 verifikasi).
3. Burn-in 30 menit 8 kanal ON: F4 hangat (< 85°C), FB2 dingin.

## P5. E-Stop Chain (SAFETY-CRITICAL — tabel 9 kondisi §57)

Tabel kebenaran (K1 = kontak NO aktuator; sense diukur di TP_ESTOP):

| # | Kondisi | K1 | E_STOP_SENSE (TP_ESTOP) | GPIO27 |
|---|---------|----|-------------------------|--------|
| 1 | Normal ON (firmware drive LOW) | energized | ~0V | 0V |
| 2 | Firmware idle (HIGH) | OFF | ~0V | 3.3V |
| 3 | ESP32 reset/boot (floating) | OFF | ~0V | ~3.3V (R78) |
| 4 | ESP32 dicabut | OFF | ~0V | 3.3V (R78) |
| 5 | E-stop DITEKAN | OFF | 3.3V | bebas |
| 6 | Kabel loop diputus | OFF | 3.3V | bebas |
| 7 | F2 dilepas | OFF | 3.3V | bebas |
| 8 | Coil K1 dilepas | OFF (NO terbuka) | ~0V (mismatch alarm) | LOW |
| 9 | Jalur sense diputus (TP unplug) | — | 3.3V (INPUT_PULLUP) | — |

- [ ] Semua 9 kondisi diverifikasi fisik; alarm firmware mismatch
  (GPIO27 LOW + sense HIGH berkelanjutan) terpicu pada kondisi 5-7.
- [ ] Q21 inverting: ukur drain 0V (sehat) / 3.3V (trip); τ C75 ~1 ms.
- [ ] R78: ukur RELAY_CTRL = 3.3V saat U1 dilepas (fail-safe).
- [ ] K1 kontak: dry contact JE_RELAY continuity sesuai state.
- [ ] Response time e-stop: < 10 ms (scope di TP_RELAY_RET vs K1 NO).

## P6. Kalibrasi Sensor

- [ ] INA219: shunt 75mV/100A; kalibrasi 2 titik (0A + known load ≥ 10A);
  error < 1%.
- [ ] ACS712 ×2: 0A offset 2.5V; kalibrasi 39.6 mV/A (verifikasi nilai
  divider R30/R32 = 10k/15k di PCB!).
- [ ] VBAT divider: 2 titik (45V + 54V); linearitas ± 1%.

## P7. EMC & Field

- [ ] Radiated kasar: WiFi TX tidak degradasi saat K1 switching (jarak
  modul antenna).
- [ ] TVS field D22/D23: pulsa 500V (pico-generator?) bila tersedia.
- [ ] D24/D25 TVS I²C (DNP): pasang bila glitch terlihat di P4.

## P8. Environmental

- [ ] Suhu ruang operasi 0-50°C: rails stabil; F4 tidak nuisance-trip.
- [ ] Humidity 85%: tidak ada leakage alarm INA219.

## P9. Durabilitas

- [ ] 500× siklus e-stop ON/OFF; 1000× relay ch toggle.
- [ ] Power cycle 50×: boot selalu fail-safe (K1 OFF, semua relai OFF).

## P10. Acceptance & Release

- [ ] Semua P0-P9 PASS → tandai hardware-acceptance JSON (format repo
  docs/hardware-acceptance).
- [ ] BOM final vs as-built; foto board; serial number.
- [ ] Sign-off engineer + tanggal.
