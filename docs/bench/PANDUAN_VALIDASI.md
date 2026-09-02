# Panduan Validasi Bench — RS485 Pylontech, Peta Register Modbus, PZEM, Hardening Modbus TCP

> **Status**: prosedur operator PENDING. Dokumen ini menutup empat keterbatasan
> yang TIDAK bisa ditutup dari kursi programmer (butuh perangkat fisik), dengan
> alat yang sekarang sudah ada di firmware: **konsol capture RS485 pasif**,
> **cek silang peta register**, **driver PZEM-004T (flag OFF sampai lulus)**,
> dan **catatan hardening jaringan Modbus TCP**. Prinsipnya tetap: *jangan
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

## 5. Template log eksekusi

Salin tabel ini ke dokumen situs dan isi saat bench dijalankan; baris kosong
= belum dieksekusi (JANGAN diisi tebakan).

| Tanggal | Prosedur | Operator | Perangkat/unit serial | Hasil (LULUS/GAGAL + bukti) |
|---------|----------|----------|----------------------|------------------------------|
| | 1. Capture RS485 (≥30 frame) | | | |
| | 2. Verifikasi peta register (tabel final dilampirkan) | | | |
| | 3. PZEM 24 jam (flag dinyalakan?) | | | |
| | 4. Hardening Modbus TCP (diagram topologi) | | | |
| | 5. Protokol bench darurat B1–B9 | | | |
