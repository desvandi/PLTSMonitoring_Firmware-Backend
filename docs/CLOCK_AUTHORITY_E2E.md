# Kontrak Otoritas Waktu (Clock Authority) & Prosedur E2E

> Hasil remediasi audit 2026-09-16 — P0-4 / G6: "Clock/time authority belum
> dibuktikan E2E". Dokumen ini menetapkan kontrak lintas-layer sebagai
> KEBIJAKAN, dan prosedur acceptance yang harus dijalankan pada perangkat
> nyata sebelum status ini dianggap tertutup.

## 1. Kontrak otoritas waktu (kebijakan, mengikat semua layer)

### Domain waktu yang ada

| # | Domain | Pemilik | Peran |
|---|---|---|---|
| D1 | ESP32 RTC (DS3231) + estimasi epoch persist | Firmware TimeManager | Sumber `event_time` |
| D2 | NTP (sinkron berkala) | Firmware | Koreksi D1; `timeQuality` naik saat sinkron |
| D3 | `Date.now()` GAS (ingest_time) | Backend | Hanya waktu PENERIMAAN — TIDAK pernah menggantikan event_time |
| D4 | Timestamp Sheets | Storage | Kolom turunan; ordering MEMAKAI event_time |
| D5 | `Date.now()` browser PWA | Klien | Hanya render umur/"terakhir dilihat" — tidak pernah ordering data |

### Aturan mengikat

1. **`event_time` adalah otoritas ordering E2E.** LATEST/HISTORY/DAILY/
   alarm-duration diurutkan berdasarkan `event_time` (epoch detik dari
   perangkat), BUKAN ingest_time dan bukan waktu browser.
2. **`timeQuality` menyertai setiap sampel** (`VALID` | `DEGRADED` |
   `UNKNOWN`). Jam perangkat tak-terparse TIDAK boleh menulis "Invalid
   Date" ke sheet — backend mendegradasi jujur (fallback server time untuk
   ordering + flag `DEGRADED`).
3. **`sequence` (device_key + sequence) adalah identitas duplikat/gap**,
   independen dari waktu — dua sampel dengan urutan waktu terbalik tetap
   disimpan dengan flag `is_late`, tanpa disintetis ulang.
4. **DAILY bucket memakai timezone deployment** (Asia/Jakarta), dihitung
   dari `event_time`, bukan dari waktu penerimaan. Midnight boundary
   menimpa bucket HARI itu, bukan hari penerimaan.
5. **Clock skew tidak boleh "dipaksakan valid".** Laporan perangkat dengan
   event_time jauh di masa depan/masa lalu tetap masuk dengan kualitas
   sesuai; consumer PWA menampilkan indikator kualitas, bukan menyembunyikan.
6. **Restart/reboot tanpa NTP**: estimasi epoch persist + `monotonicMs`
   menjaga kontinuitas relatif; `timeQuality` tetap `UNKNOWN`/`DEGRADED`
   sampai sinkron NTP berikutnya — tidak pernah diklaim `VALID` tanpa bukti.

## 2. Prosedur acceptance E2E (harus pada perangkat nyata)

> Prasyarat: firmware produksi aktif, GAS canonical terdeploy, PWA
> production, akses Sheets. Setiap skenario dicatat bukti (screenshot row
> Sheets + tampilan PWA) ke `docs/hardware-acceptance/`.

### T1 — Baseline sinkron
1. Boot device dengan NTP aktif → tunggu sinkron.
2. Kirim TELEMETRY → verifikasi Sheets: `event_time` ≈ NTP ± 2 s,
   `time_quality = VALID`.
3. PWA LATEST menampilkan timestamp sesuai + tanpa ikon degraded.

### T2 — Skew +30 detik (manual override RTC)
1. Set RTC +30 s dari NTP, kirim TELEMETRY.
2. Sheets: `event_time` = waktu RTC (otoritas perangkat), kualitas tetap
   sesuai sumber (VALID bila NTP terakhir segar).
3. PWA: sampel tetap tampil pada urutan waktunya — TIDAK ada reordering
   oleh ingest_time.

### T3 — Skew ±5 menit & RTC unsynced (reboot tanpa NTP)
1. Putus NTP (blok UDP di router), reboot device.
2. TELEMETRY → Sheets: kualitas `UNKNOWN`/`DEGRADED`, event_time dari
   estimasi persist; TIDAK ada "Invalid Date".
3. NTP kembali → sampel berikutnya `VALID`; sampel lama TIDAK diubah
   retroaktif (jejak audit).

### T4 — Paket terlambat & duplikat
1. Hentikan koneksi 3 menit (spool aktif), pulihkan.
2. Verifikasi: replay urut `sequence`; duplikat (sequence sama) → `DUPLICATE`
   tanpa row baru; gap tercatat di SeqIndex; `is_late` untuk paket tua.
3. PWA history: urutan mengikuti `event_time`, tidak ada data sintetis
   pengisi gap.

### T5 — Boundary tengah malam (Asia/Jakarta)
1. Telemetry tepat sebelum & sesudah 00:00 WIB.
2. DAILY: energi masuk bucket hari masing-masing sesuai `event_time`;
   daily-energy tidak "pindah tanggal" karena keterlambatan penerimaan.

### T6 — Sumber waktu browser berbeda
1. Set jam laptop ±1 jam, buka PWA.
2. Ordering chart/alarm TIDAK berubah (memakai event_time); hanya label
   "x menit lalu" yang relatif — tidak menyesatkan ordering.

## 3. Bukti yang dihasilkan

Untuk setiap skenario: (a) row Sheets terkait (event_time, time_quality,
sequence, is_late), (b) tangkapan PWA (history/alarm/daily), (c) log
perangkat (timeQuality, uptimeSeconds, spoolDrops). Kumpulkan ke
`docs/hardware-acceptance/clock-e2e-v<ver>.md` — gate rilis memakai file
ini sebagai bukti penutupan G6.
