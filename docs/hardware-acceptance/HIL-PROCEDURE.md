# HIL-PROCEDURE — Prosedur Kualifikasi Hardware-in-the-Loop (Gate 9)

Status: **PROSEDUR SIAP DIEKSEKUSI operator/test engineer.** Semua bukti
harus dihasilkan dari perangkat ESP32 FISIK nyata dengan firmware hasil
build produksi (SHA tercatat di `release.json`). Nilai rekaan/bayangan =
pelanggaran disiplin anti-false-closure. Verifier
`scripts/verify_gate_qualification.py` MENOLAK rilis tanpa bukti lengkap.

File bukti: `docs/hardware-acceptance/v{versi}-gate.json`
(salin dari `gate-qualification.template.json`, isi dari pengukuran nyata).

Peralatan minimum: ESP32 produksi (PLTS-ESP32-xxx), relai 8-kanal PCF8574 +
beban representatif per kanal, broker MQTT uji (akun nyata), stasiun kerja
dengan akses MQTT + REST + serial monitor, power supply yang dapat
diputus/di-brownout, ammeter/voltmeter referensi.

Urutan eksekusi direkomendasikan: A → B → … → L (A..K = matriks inti,
L = soak). Setiap bagian memetakan ke kunci `checks` di file bukti.

## A. boot / relayAllOffBoot (`checks.boot`, observed.relayAllOffBoot)

1. Flash firmware produksi (catat SHA256 biner; harus sama dengan
   `release.json.firmwareSha256`).
2. Boot dingin 3×, catat `bootTimeSec`, pastikan log `RELAY: Controller
   initialized` + semua kanal OFF (verifikasi fisik: beban mati, atau
   baca register PCF8574 0xFF).
3. **PASS** jika semua boot: 8 kanal OFF + lockout NVS dipertahankan.

## B. sensors / alarms (`checks.sensors`, `checks.alarms`)

Kalibrasi/verifikasi sensor sesuai protokol INA219 v1.9.2+
(`verify_ina219_hardware_acceptance.py` — bukti terpisah di
`v{versi}.json`). Bagian ini memastikan alarm registry menyala pada
 kondisi buatan (putus sensor SHT31/INA219 → alarm aktif + honest state).

## C. Power abuse (`checks.configPersistence` + observed.power/brownout/WDT)

1. **Power cycle** saat relay CH3 ON: putus VCC 10× berturut — setelah
   reboot semua kanal OFF (boot policy), konfigurasi NVS bertahan
   (maxOnTime/lease/policy dibaca kembali).
2. **WDT reset**: suspend relayTask via debugger/pause watchdog feed —
   perangkat reset, boot bersih, hitung `watchdogResetCount`.
3. **Brownout**: turunkan VCC di bawah ambang — reset reason = brownout,
   hitung `brownoutResetCount`.

## D. I²C failure (`observed.i2cFailureRecovery`)

Lepas jumper SDA/SCL PCF8574 saat sistem berjalan:
- tulis relay → harus GAGAL jujur (`fault=true`, alarm Critical,
  reportedState TIDAK diubah);
- sambung kembali → shadow recovery `ALL OFF` terverifikasi, mutasi
  normal kembali diizinkan.

## E. E-WAVE cascade / E-stop (`checks.emergencyRelay` + observed.ewave*)

1. Nyalakan ≥3 kanal; picu E-WAVE (kondisi sensor darurat).
2. Seluruh kanal OFF ≤1 tick (200 ms) + **safety latch** aktif.
3. Perintah ON saat latch → **BLOCKED (SAFETY_LATCHED)**.
4. Hanya jalur ARM operator eksplisit yang melepas latch.
5. E-stop fisik: tekan → OFF; lepas → TIDAK re-arm (harus ARM manual).
6. `checks.emergencyAtomicity` (PH8-01): selama burst perintah ON
   berkecepatan penuh (loop REST 10 Hz), picu `emergencyAllOff` —
   hitung pelanggaran (kanal menyala SETELAH emergency) → **harus 0**;
   `postTripOnRefused=true`, `armRequiredToReenable=true`.
7. `checks.watchdogStarvationBank` (PH8-02): matikan MQTT+WiFi+GAS lalu
   biarkan supervisor starvation terpicu → relay BANK ikut dipaksa OFF
   (forceSafetyAllOff), bukan hanya GPIO E-WAVE.

## F. Clock invalid (`checks.clockInvalidReject`, PH8-03)

1. Pastikan clock belum NTP-sync (boot tanpa WiFi).
2. Kirim perintah RELAY ON / OTA start dengan `expiresAt` terisi →
   **REJECTED CLOCK_INVALID**.
3. Kirim perintah OFF (safe-direction) → tetap DITERIMA.
4. Setelah NTP sync: perintah ON berjalan normal; perintah kedaluwarsa →
   REJECTED EXPIRED.

## G. Safety-config lockdown (`checks.safetyConfigLockdown`, PH8-04)

Dari CONFIG remote (build produksi): coba ubah `sensorFailPolicy`,
`estopEnabled`, `relayPin`, `estopPin` → semua **REFUSED** + event
CONFIG_REFUSED tercatat. Fisik tidak berubah.

## H. Comm-loss fail-safe (`checks.commLossFailSafe`, PH8-05) — PER KANAL

Provisioning: 7 kanal policy=OFF lease=900s; 1 kanal (mis. CH7)
policy=HOLD_SAFE hanya bila hazard classification terdokumentasi
(build flag `-DPLTS_ALLOW_HOLD_SAFE`); tanpa dokumentasi → 8 kanal OFF.

Matriks per gangguan (WiFi OFF / MQTT broker dihentikan / GAS
unreachable / proxy PWA+backend dimatikan):
1. Saat authority hilang: log `command authority LOST`.
2. Sebelum lease: kanal ON bertahan.
3. Setelah lease (atau percepat dengan lease kecil saat provisioning,
   mis. 60s): kanal policy=OFF **FORCE OFF** (source=COMM_LOSS,
   alarm Critical); kanal HOLD tetap ON + **tepat satu** WARNING.
4. Selama outage, kirim perintah REST lokal → lease kanal itu
   diperpanjang (operator presence).
5. Pulihkan authority: log `RESTORED`; **TIDAK ada kanal menyala
   otomatis**; `commLossForced` tetap jujur sampai perintah baru.
6. `checks.staleCommandBlocked`: kirim perintah ber-`expiresAt` pendek,
   putus MQTT sebelum dieksekusi, sambung kembali → perintah itu
   **BLOCKED** (tidak di-replay).

## I. Pin change refused (`checks.pinChangeRefused`, PH8-08)

Build produksi saat relay ON: kirim CONFIG `relayPin`/`estopPin` baru →
REFUSED; state fisik tidak berpindah ke GPIO baru.

## J. MQTT topic binding (`checks.mqttTopicBinding`, GATE-3)

1. Publikasikan perintah relay ke topik perangkat LAIN
   (`plts/<otherId>/config`) dengan kredensial yang salah alamat →
   TIDAK dieksekusi; hitam `foreignTopicCount` bertambah; security log
   tercatat.
2. Publikasikan ke topik sendiri (QoS 1) → dieksekusi + ACK.

## K. OTA durability + QoS (`checks.otaDurableReservation`, `checks.qosTimeoutBudget`)

1. Mulai OTA via MQTT; putus koneksi di tengah unduhan → record
   RESERVED **persist** (reboot → masih ada), status jujur, jalur
   pemulihan (lanjut/batal) berfungsi. Urutan: RESERVED → barunduh.
2. Matikan broker: anggaran timeout teramati (tidak menggantung),
   reconnect **full jitter** (catat interval antar percobaan — tidak
   seragam), laporan QoS jujur (QoS 0/1 sesuai kontrak, tidak
   mengklaim delivery yang tidak dibuktikan).

## L. Soak 24–72 jam (`checks.soak24h`, observed.soak*)

Jalankan dengan beban campuran: telemetry 5s + reconnect storm terjadwal
(broker di-restart tiap 6 jam) + OTA check berkala + request web + alarm
churn buatan. **PASS** jika:
- tidak ada reset tak terduga (`resetCount` = hanya yang disengaja),
- `minFreeHeap` stabil (tidak ada penurunan monoton antar jam),
- tidak ada stack watermark melewati ambang kritis,
- tidak ada kebocoran transaksi/kredensial di log.

## Sertifikasi akhir (Gate 10)

Setelah A–L PASS untuk build produksi final:
1. Tanda tangan tiga peran (testEngineer, reviewer, releaseManager).
2. Verifikasi ulang `gitCommit` == source commit rilis dan
   `firmwareSha256` == artefak rilis.
3. Release gate CI menjalankan `verify_gate_qualification.py` (wajib
   untuk versi > 1.9.3) — tanpa bukti, rilis DIBLOKIR.
4. Pemantauan produksi pasca-rilis: 7 hari pertama = periode
   perketat (alarm COMM_LOSS/E-WAVE harus nihil kecuali gangguan nyata);
   insiden apa pun → post-mortem + bukti tambahan sebelum status
   "production soak" dinyatakan.

## Batasan jujur prosedur ini

- **PH8-07 (physical output verification)**: TIDAK dapat ditutup dengan
  prosedur software — butuh redesain hardware (auxiliary contact /
  bukti arus-tegangan) sebelum `stateConfidence` boleh meninggalkan
  `SOFTWARE_ONLY`. Tetap tercatat sebagai risiko terbuka.
- **F8 (broker ACL live)**: jalankan `scripts/broker-acl-test.mjs`
  (repo PWA) terhadap akun broker produksi — bukti terpisah dari file
  ini (lihat PWA docs/broker-acl/ACL-POLICY.md).
- Soak 24–72 jam mensyaratkan jendela waktu operator; jangan dipersingkat.
