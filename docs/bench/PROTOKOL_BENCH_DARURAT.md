# Protokol Bench Lapisan Darurat — Firmware Modular v1.7.0 (E-WAVE)

> **Status dokumen**: eksekusi operator PENDING. Logika sudah teruji mekanis
> (85 asersi: `scripts/test_emergency_modular_logic.py`) + syntax-check penuh,
> tetapi **sembilan skenario bench di bawah WAJIB dijalankan operator di
> perangkat fisik SEBELUM daya tinggi dihubungkan** — aktuator keselamatan
> yang tidak pernah diuji adalah bahaya terselubung.
>
> Dokumen ini mengadaptasi skenario acceptance WAVE-7 (S6–S11 di Panduan
> Deploy Edisi 4, Bab 5.3) ke firmware modular `firmware/`. Pemetaan ke nomor
> S diberikan pada setiap skenario.

## Prasyarat

1. **Perangkat**: ESP32 dengan firmware modular v1.7.0 (`pio run -e production`,
   `PLTS_ENABLE_EMERGENCY=1`), relai 5 V optocoupler **aktif-LOW** pada GPIO 27
   (default, dapat diubah via GAS CONFIG `relayPin`), tombol E-stop NC pada
   GPIO 14, LED status GPIO 2.
2. **Kontaktor/daya**: hubungkan relai pada kontaktor BEBAN, BUKAN pada sumber
   DC utama monitoring. Selama bench, beban = lampu pijar/beban resistif +
   amperemeter — jangan beban sensitif-glitch tanpa UPS (B2 dan B3 memutus
   daya sesaat sesuai desain).
3. **Akses**: ADMIN_TOKEN terisi (halaman `/setup` PWA), perangkat online
   (WiFi + NTP sinkron — kanal HMAC butuh jam valid), menu **Kontrol Darurat**
   (ikon perisai) PWA terbuka.
4. **Amankan dulu**: pastikan skenario B1 lulus **sebelum** kontaktor
   dihubungkan ke beban nyata. Seluruh skenario B2–B9 dijalankan dengan daya
   bench terbatas.
5. **Baca indikator**: `GET /api/status` (perangkat) → blok `emergency`
   {state, reason, estopOpen, relayEnergized, trips, crashChain}; LED: RUN =
   menyala solid, EMERGENCY = kedip 2 Hz.

## Sembilan Skenario

| ID | Skenario | Langkah | Hasil yang diharapkan | Lulus? |
|----|----------|---------|----------------------|--------|
| B1 | **Boot isolasi** | Cabut daya ESP32 10 detik; sambungkan kembali; amati LED + `/api/status` selama satu menit penuh (WiFi boleh belum nyala) | Relai isolasi sejak reset (GPIO Hi-Z → opto mati); dalam < 2 s firmware menegaskan ISOLATED; LED kedip 2 Hz; `emergency.state=EMERGENCY`, `reason=BOOT`, `relayEnergized=false` sebelum WiFi tersambung; multimeter kontaktor: jalur TERBUKA | ☐ |
| B2 | **Trip otomatis ambang** (=S8) | Dari editor ambang (GAS CONFIG), naikkan `vbatLowV` di atas tegangan aktual (mis. 54 V); tunggu ≥ 3 detik (debounce 3 @ 10 Hz); kembalikan ambang | Trip dengan alasan `VBAT_LOW` pada panel; `emergency.reason=VBAT_LOW`; LED 2 Hz; kejadian `TRIP` muncul di EmergencyEvents GAS; setelah histeresis terlampaui + masa pulih 60 s, ARM diterima | ☐ |
| B3 | **E-stop fisik** (=S9) | Tekan tombol E-stop NC; amati panel; putar untuk melepas; tekan ARM | Sistem TERISOLASI **seketika** (jalur hardware memutus catu relai — bahkan jika ESP32 mati); indikator E-stop menyala di panel (`estopOpen=true`); melepas tombol **TIDAK** menyalakan ulang — hanya ARM yang bisa | ☐ |
| B4 | **ARM ditolak** (=S7/S11) | Dengan pemicu masih aktif (mis. tegangan masih di bawah ambang, atau cabut kabel INA219 saat policy=1), tekan ARM; perbaiki kondisi; tekan ARM lagi setelah masa pulih | ARM pertama DITOLAK dengan **alasan bahasa Indonesia** di toast (mis. "sensor INA219 tidak terdeteksi — proteksi arus DC nonaktif (sensorFailPolicy=1)"); baris queue `REJECTED` + pesan alasan di GAS; setelah kondisi aman + 60 s, ARM kedua diterima — sistem RUN, baris `APPLIED` | ☐ |
| B5 | **ARM diterima** (=S7 selesai) | Dari kondisi sehat, tekan ARM | Sistem RUN (LED solid, `relayEnergized=true`, kontaktor tertutup); kejadian `ARMED` di EmergencyEvents; ACK `APPLIED — relay energized` | ☐ |
| B6 | **EMERGENCY STOP jarak jauh** (=S6) | Dari panel Kontrol Darurat, tekan EMERGENCY STOP lalu ketik kata STOP sebagai konfirmasi | TERISOLASI dalam ≤ 15 detik (poll `EMERGENCY_PENDING`); `emergency.state=EMERGENCY`, `reason=OPERATOR`; kejadian `DISARMED` di EmergencyEvents; alert Telegram terkirim (bila dikonfigurasi); tekan ulang DISARM → tetap APPLIED tanpa efek ganda (idempoten) | ☐ |
| B7 | **Perintah basi (TTL)** (=S10) | Matikan WiFi perangkat; kirim EMERGENCY STOP; tunggu 11 menit; nyalakan WiFi | Perintah EXPIRED di queue (TTL 10 menit) — **tidak diterapkan**; sistem tetap RUN; tidak ada perubahan `emergency.state`; kejadian tidak muncul | ☐ |
| B8 | **Crash-loop hold** | Reboot paksa perangkat 3× berturut-turut lebih cepat dari 5 menit (cabut-colok daya), lalu coba ARM | ARM DITOLAK dengan alasan "crash-loop hold active — power-cycle stable first"; `crashChain=3` di `/api/status`; setelah dibiarkan menyala ≥ 5 menit lalu reboot sehat, chain reset dan ARM diterima | ☐ |
| B9 | **CONFIG persisten** | Kirim GAS CONFIG (mis. `vbatLowHystV: 2.0`) → reboot perangkat → cek `/api/status` dan log | ACK `APPLIED — emergency config updated`; kejadian `CONFIG_APPLIED`; setelah reboot nilai baru bertahan (NVS `plts_emg`); `trips` (hitungan seumur-hidup) TIDAK hilang saat reboot | ☐ |

## Aturan lolos/gagal

Skenario **B3 dan B4 MENGUJI FILOSOFI, bukan hanya fungsi**: sistem yang
menolak menyala saat belum aman, dan sistem yang tetap mati setelah E-stop
dilepas, adalah BUKTI arah gagal-aman bekerja. Bila **satu saja** skenario
gagal: **hentikan komisioning**, putus kontaktor, periksa wiring relai
(polaritas aktif-LOW!), posisi/NC E-stop, dan konfigurasi pin — lalu ulangi
dari B1. Jangan "coba ARM saja dan lanjut".

## Yang tidak diuji di bench ini (jujur)

- **Kanal arus genset (iGen)**: slot RESERVED — papan modular belum memasang
  ACS712 kedua. Trigger `I_AC_GEN_OVER` dorman dan TIDAK ikut kebijakan
  sensor-loss (lihat catatan port di `EmergencySupervisor.h`).
- **PZEM-004T**: jalur upgrade daya AC terukur — lihat
  `PANDUAN_VALIDASI.md` (validasi bench terpisah, flag masih OFF).
- Daya AC tetap estimasi (ACS712 × 220 V × PF 0.9) sampai PZEM tervalidasi.

## Kaitan dengan uji mekanis (bukti logika)

Setiap skenario dipetakan ke grup asersi `test_emergency_modular_logic.py`:

| Skenario | Grup mekanis | Invarian |
|----------|--------------|----------|
| B1 | E1/E2/E8c | isolasi fail-safe sebelum LittleFS/WiFi |
| B2 | A2–A7 | debounce + latching + histeresis |
| B3 | C1–C3 | E-stop latch; lepas ≠ re-energize |
| B4 | B1–B9, S1–S3 | gerbang ARM fail-closed |
| B5 | B6/B4 | ARM operator-only |
| B6 | D1–D2 | DISARM aman + idempoten |
| B7 | R-grup (kontrak GAS TTL) | perintah basi tidak diterapkan |
| B8 | E10/B8 | rantai crash NVS |
| B9 | R7a–R7d + E14c | rentang & default lintas GAS-firmware |
