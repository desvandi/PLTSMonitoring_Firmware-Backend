# Panduan Validasi Bench — RS485 Pylontech, Peta Register Modbus, PZEM, Hardening Modbus TCP, Pengembalian OTA

> **Status**: prosedur operator PENDING. Dokumen ini menutup lima keterbatasan
> yang TIDAK bisa ditutup dari kursi programmer (butuh perangkat fisik), dengan
> alat yang sekarang sudah ada di firmware: **konsol capture RS485 pasif**,
> **cek silang peta register**, **driver PZEM-004T (flag OFF sampai lulus)**,
> **catatan hardening jaringan Modbus TCP**, dan **protokol pengembalian
> fisik OTA v1.7.1**. Prinsipnya tetap: *jangan
> menebak protokol vendor — tangkap frame aslinya, verifikasi ke manual.*

---

## 1. Capture frame vendor Pylontech RS485 (menutup keterbatasan #2)

Slot klien `PylontechRs485` sengaja dibiarkan RESERVED sampai frame vendor
asli terdokumentasi. Konsol capture (v1.7.0) adalah alatnya — **pasif total**:
DE transceiver dipakukan LOW (mode terima permanen), firmware TIDAK PERNAH
mengirim satu byte pun ke bus vendor.

### Langkah

1. **Kabel**: A/B RS485 baterai → terminal A/B MAX3485 (GPIO 16 TX / 17 RX
   via transceiver). Pastikan GND referensi terhubung.
2. **Aktifkan mode console** (benc-only, polling BMS dimatikan):
   `bmsProto = "rs485_console"` (NVS `plts_batt`), lalu reboot. Log boot
   harus memunculkan `RS485_CONSOLE_ACTIVE — passive vendor-frame capture
   started`.
3. **Tangkap**: biarkan baterai mengirim frame periodiknya (mode console
   Pylontech mengirim data otomatis). Ambil via
   `GET /api/rs485/frames` (login perangkat diperlukan) — salin keluaran
   JSON-nya. Minimal **30 frame berturut-turut** + catat periode antar-frame
   dengan jam tangan.
4. **Serahkan hasil** (issue GitHub atau vendor Pylontech): hex mentah +
   kondisi baterai saat capture (SOC, arus, jumlah modul). Parser
   `PylontechRs485` baru hanya akan ditulis DARI frame asli ini — bukan dari
   dugaan.

### Yang TIDAK boleh dilakukan

- Jangan mengaktifkan `rs485_console` bersamaan dengan `modbus_rtu` — mode
  ini eksklusif by design (manager membangun nol klien polling).
- Jangan men-interpretasi hex secara manual lalu meng-hardcode hasil dugaan
  ke firmware — melewati seluruh tujuan capture.

---

## 2. Verifikasi peta register Modbus ke manual baterai (menutup keterbatasan #1)

Peta default (`Comm/ModbusMap.h`, jendela FC03 register 100–112) adalah
**CONTOH** sampai diverifikasi terhadap manual baterai Anda. Prosedur cek
silang (baca-saja, tanpa risiko):

### Langkah

1. **Baca manual** baterai/rak Anda: cari tabel "holding register" / "FC 03"
   untuk tegangan pack, arus, SOC, SOH, tegangan sel min/maks, suhu, CCL/DCL,
   siklus, flag fault, jumlah sel. Catat: alamat awal, urutan, satuan/skala
   (×0.01 V? ×0.1 A?), tanda arus (discharge positif?), lebar register.
2. **Bandingkan terhadap `ModbusMap.h`**: setiap konstanta `OFF_*` + `SCALE_*`
   + `MODBUS_RACK_CURRENT_SIGN`. Satu per satu — jangan "kira-kira mirip".
3. **Uji satu jendela baca** dengan alat Modbus umum (qmodbus/mbpoll di
   laptop, ATAU konsol `rs485_console` + anotasi manual): kirim query FC03
   yang sama dengan yang dikirim firmware (`slave, 0x03, startReg=100,
   count=13`) dan cocokkan byte respons dengan telemetri `/api/status`
   (blok `bms`). Nilai yang masuk gerbang plausibilitas tapi SALAH SATUAN
   (mis. 4800 = "48.00 V" vs "480.0 V") adalah tanda skala keliru — perbaiki
   `SCALE_*`, jangan "kalikan di tempat lain".
4. **Rekaman bukti**: simpan tabel pemetaan final (register ↔ field ↔ skala
   ↔ sumber halaman manual) di dokumen situs. Tanpa tabel ini, deployment
   TIDAK boleh dinyatakan produksi.

### Tanda arus (raw sign)

Kebanyakan rak BMS melaporkan arus **discharge-positif**; kanonikal firmware
adalah **charge-positif** sehingga dikalikan `MODBUS_RACK_CURRENT_SIGN=-1`.
Alarm `BMS_CURRENT_MISMATCH` (cek silang shunt INA219) akan menyala dalam
satu siklus poll bila konstanta ini salah — itu penjaga bawaan, bukan alasan
melewati verifikasi manual.

---

## 3. Validasi PZEM-004T (menutup keterbatasan #9 — jalur upgrade)

Driver sudah terimplementasi dan teruji mekanis
(`scripts/test_pzem_driver.py`, 25 asersi termasuk CRC terhadap frame kanonik
`01 03 00 00 00 0A C5 CD`), tetapi **flag `PLTS_ENABLE_PZEM_AC` tetap 0**
sampai SATU unit fisik lulus prosedur ini:

### Langkah

1. **Wiring**: PZEM-004T v3 (versi terisolasi). ESP32 TX (GPIO 19) → RX
   PZEM; ESP32 RX (GPIO 18) ← TX PZEM; catu 5 V PZEM dari sumber terpisah
   (bukan pin 5 V ESP32 bila beban > 500 mA); GND bersama. Pin 32 sengaja
   TIDAK dipakai (cadangan ACS712 #2).
2. **Build**: `pio run -e production -- -DPLTS_ENABLE_PZEM_AC=1` (atau edit
   platformio.ini sementara). Flash.
3. **Bandingkan 24 jam** (atau minimal satu siklus beban penuh): tegangan
   PZEM vs multimeter AC sesekali; daya PZEM vs V_multimeter × A_clampmeter
   (toleransi wajar ±2–5%); frekuensi harus 50.0 ± 0.2 Hz di jaringan PLN;
   PF masuk akal untuk jenis beban (resistif ≈ 1.0, motor induktif 0.7–0.9).
4. **Cek kejujuran**: cabut PZEM → `/api/status` harus menunjukkan
   `ac.meter.connected=false` + semua nilai `null` (bukan 0, bukan nilai
   basi). Sambungkan kembali → nilai kembali MEASURED.
5. **Lolos?** Barulah flag boleh dinyalakan permanen di platformio.ini +
   catat hasil kalibrasi di dokumen situs. Tidak lulus → laporkan pola
   kegagalannya (timeout? CRC? nilai liar?) — jangan paksa.

> Setelah tervalidasi: blok `ac.meter` (MEASURED) menggantikan estimasi
> `estimatedPower` DALAM PELAPORAN; estimasi ACS712 tetap dipertahankan
> sebagai jalur mundur berlabel jelas. Energi PZEM (Wh) adalah pencacah
> meter itu sendiri — **reset saat meter kehilangan daya** — dan sengaja
> TIDAK diintegrasikan ke pencacah energi DC kanonik.

> **[W14]** *Plumbing* PZEM kini terbukti kontrak-benar secara virtual
> (`scripts/test_bench_w14_pzem.js`, 44 check: driver → telemetri → GAS
> ingest → LATEST → PWA gasEnvelope, termasuk kejujuran di bawah 6 mode
> gagal). Prosedur fisik di atas tinggal memvalidasi **AKURASI** (banding
> 24 jam vs multimeter referensi) — bukan lagi konektivitas. Bug framing
> protokol W14-1 (FC 0x03 vs 0x04 + offset decode) sudah diperbaiki
> SEBELUM flag pernah dinyalakan.

---

## 4. Hardening jaringan Modbus TCP (menutup keterbatasan #3)

Modbus TCP memang tanpa autentikasi — itu sifat protokolnya. Firmware ini
hanya berperan **klien polling** (tidak membuka port listening apa pun),
namun BMS/host gateway TCP tetap permukaan serang di jaringan lokal.
Kontrol yang tersedia operator (berlaku semua deployment):

1. **Segmen jaringan khusus**: BMS/host Modbus dan ESP32 di VLAN/SSID
   terpisah dari jaringan tamu dan internet. JANGAN pernah mem-forward
   port 502 ke luar (port-forwarding router / public IP).
2. **Allowlist firewall** pada host BMS (bila mendukung): hanya IP ESP32
   yang boleh terhubung ke port 502.
3. **Host `bmsHost` wajib privat**: set `cfgBmsModbusTcpHost` hanya ke
   alamat LAN pribadi (192.168.x.x / 10.x.x.x / 172.16-31.x.x). Bila suatu
   saat isian berupa host publik, perlakukan sebagai kesalahan konfigurasi.
4. **Cegah downgrade**: jangan menonaktifkan TLS MQTT/HTTPS hanya karena
   "Modbus-nya memang polos" — layer transport lain tetap terenkripsi.
5. **Dokumentasikan** topologi akhir (diagram satu halaman: ESP32 — switch —
   BMS host, tanpa jalur ke internet) di dokumen situs.

---

## 5. Verifikasi fisik pengembalian OTA v1.7.1 (wave 14)

Semantik rollback sudah terverifikasi virtual (44 check,
`scripts/test_bench_w14_ota_rollback.js`: state-machine `ota_data`
bootloader di-mirror 1:1 dari sumber ESP-IDF v4.4.7 + `Code.gs` asli di
sandbox + kripto HMAC nyata). Prosedur ini tinggal mengonfirmasi baris
serial + baris OtaEvents yang sama pada silikon nyata.

> **Kebijakan bootloader (diverifikasi dari sdkconfig arduino-esp32 2.0.17 +
> sumber IDF): image baru mendapat TEPAT SATU boot tak-terkonfirmasi.**
> Reset apa pun sebelum jendela sehat 60 s terpenuhi — blip daya, crash,
> WDT, semuanya setara — membuat bootloader menandai image ABORTED dan
> otomatis boot image lama. Konsekuensi praktis: **jangan matikan/matikan
> daya perangkat selama ±60 detik pertama setelah update OTA** — update
> akan kembali diam-diam (sejak W14-2, kembali tersebut dilaporkan sebagai
> event `ROLLBACK` di sheet OtaEvents).

### 5a. Jalur AKTIVASI (happy path)

1. **Prasyarat**: perangkat generic terdaftar di sheet DEVICES dengan
   `firmware_type` = `generic`; firmware berjalan v1.7.0; serial monitor
   aktif (115200); `ADMIN_TOKEN` terisi di sheet Config; binari v1.7.1
   terbit (hasil build CI terbaru).
2. **Publish**: panel OTA PWA → manifest v1.7.1, target `generic`.
3. **Amati serial (urutan lengkap)**: `[OTA] new version 1.7.1 (running
   1.7.0)` → `[OTA] Success v1.7.1 (...). Rebooting...` → `[BOOT] PLTS
   Monitor firmware v1.7.1` → `[OTA] pending-verify boot try #1` →
   (±60 s) `[OTA] new image validated — rollback cancelled.` →
   `[OTA-STATUS] ACTIVATED v1.7.1 -> HTTP=200`.
4. **Amati PWA/GAS**: panel OTA (OTA_LOG) menampilkan `ACTIVATED v1.7.1`;
   telemetri terbaru melaporkan `firmwareVersion` 1.7.1.
5. Biarkan daya menyala minimal 90 detik sejak reboot pertama image baru.

### 5b. Jalur PENGEMBALIAN FISIK (rollback via power-cut)

1. Dengan manifest 1.7.1 masih aktif dan device kembali berjalan 1.7.0
   (mis. setelah uji 5b sebelumnya, atau ulangi publish), biarkan siklus
   OTA berjalan.
2. Setelah reboot ke image baru terlihat (`[OTA] pending-verify boot try
   #1`), **cabut daya pada ±20 detik** (sebelum jendela 60 s).
3. Sambungkan kembali daya: bootloader boot **v1.7.0**. Serial:
   `[BOOT] PLTS Monitor firmware v1.7.0` lalu `[OTA] update v1.7.1 was
   reverted by the bootloader — running v1.7.0`.
4. Setelah STA naik: `[OTA-STATUS] ROLLBACK v1.7.1 -> HTTP=200` → panel
   OTA menampilkan `ROLLBACK v1.7.1` **tepat satu kali** (single-shot; boot
   berikutnya tidak melaporkan ulang).
5. **Perilaku lanjutan yang BENAR**: karena manifest 1.7.1 masih aktif dan
   running 1.7.0 < 1.7.1, siklus OTA per jam berikutnya akan MENGUNDUH
   ULANG update — itu retry yang diinginkan, bukan bug. Untuk tetap di
   1.7.0, hapus/baris manifest di sheet Ota atau publish ulang versi yang
   sama dengan running.
6. **Tree modular**: rollback terdeteksi + dilog lokal di serial
   (`OTA ROLLBACK (bootloader revert): v1.7.1 ...`), tetapi BELUM masuk
   sheet OtaEvents — jembatan GAS OTA_STATUS modular masih terbuka
   (dokumen README §wave 14 W14-2; getter `getBootRollbackVersion()` sudah
   disiapkan untuk jembatan itu).

### Kriteria lulus

| Jalur | LULUS bila | GAGAL bila |
|-------|------------|-----------|
| 5a | ACTIVATED tercatat tepat 1×; telemetri `firmwareVersion`=1.7.1; tidak ada `try #2` | update berulang tanpa ACTIVATED; image kembali ke 1.7.0 tanpa ROLLBACK tercatat |
| 5b | revert ke 1.7.0; event ROLLBACK v1.7.1 tepat 1×; tanpa dobel-lapor di boot berikutnya | device bootloop; ROLLBACK ganda; event tidak pernah muncul padahal STA naik |

---

## 6. Template log eksekusi

Salin tabel ini ke dokumen situs dan isi saat bench dijalankan; baris kosong
= belum dieksekusi (JANGAN diisi tebakan).

| Tanggal | Prosedur | Operator | Perangkat/unit serial | Hasil (LULUS/GAGAL + bukti) |
|---------|----------|----------|----------------------|------------------------------|
| | 1. Capture RS485 (≥30 frame) | | | |
| | 2. Verifikasi peta register (tabel final dilampirkan) | | | |
| | 3. PZEM 24 jam (flag dinyalakan?) | | | |
| | 4. Hardening Modbus TCP (diagram topologi) | | | |
| | 5a. OTA v1.7.1 aktivasi (serial + OTA_LOG) | | | |
| | 5b. OTA v1.7.1 pengembalian fisik (power-cut 20 s) | | | |
| | 7. Protokol bench darurat B1–B9 (dokumen terpisah) | | | |
