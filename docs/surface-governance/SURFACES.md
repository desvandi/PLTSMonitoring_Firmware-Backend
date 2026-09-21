# Tata Kelola Permukaan (Surface Governance) — Temuan #18

> Registry machine-readable: `docs/surface-governance/SURFACE-STATUS.json`
> Penegakan CI: `scripts/check_surface_policy.py` + job `surface-policy-gate`
> Asal temuan: audit baseline 14-field 2026-09-18, temuan #18 —
> "permukaan paralel legacy".

## 1. Masalah yang diselesaikan dokumen ini

Repo ini bukan berisi SATU sistem, melainkan **jalur deployment paralel**
yang tumbuh dari tiga generasi arsitektur (MonitorIoT greenhouse →
kanal GAS PLTS → REST/MQTT direct). Setiap jalur punya firmware dan/atau
backend GAS sendiri. Perbaikan keamanan yang mendarat di satu jalur
**tidak otomatis menyebar** ke jalur lain — inilah risiko divergensi
temuan #18: sistem terlihat diperbaiki, sementara salinan paralelnya
tetap membawa kelemahan lama, tanpa ada yang gagal build.

Contoh divergensi nyata yang sudah terukur pada 2026-09-22:

- `firmware/Web/AuthHandlers.cpp` (jalur REST) mengimplementasikan
  login + CSRF + cookie session + peran operator.
- `firmware-generic/` (jalur GAS) TIDAK punya auth web sama sekali —
  WebServer-nya hanya hidup di mode AP setup ([FW-A3] WPA2+CSPRNG),
  `handleClient()` tidak pernah dipanggil saat STA — jadi bukan celah
  terbuka, tetapi juga bukan "sudah sama amannya": dua model keamanan
  berbeda yang harus diverifikasi terpisah selamanya.
- `code.gs/Code.gs` membawa kontrak identitas `(device_key, sequence)`
  + HMAC v2.1; `push-alarm/gas/Code.gs` (1.352 baris, duplikat berbeda)
  tidak membawa penanda itu.

## 2. Matriks kemampuan per permukaan

| Kemampuan | firmware/ | firmware-generic/ | code.gs/ | push-alarm/gas/ | MonitorIoT |
|---|---|---|---|---|---|
| Status | **active** | **active** | **active** | **frozen** | **deprecated** |
| Auth web (login/CSRF/session) | ya | AP-setup saja (WPA2) | n/a | n/a | n/a |
| Identitas (device_key, sequence) + ledger gap | (REST: cookie+CSRF) | ya [P1-00x] | ya [P1-00x] | **tidak** | **tidak** |
| Auth kanal GAS (HMAC v2.1) | n/a | token+device_key | ya | **tidak** | token saja |
| OTA bertanda-tangan + rollback | ya (MQTT/GAS) | ya (+laporan ROLLBACK jujur) | ya (sisi server) | **tidak** | **tidak** |
| Pinning TLS | MQTT_ROOT_CA | GAS_ROOT_CA (GTS R4) | — (sisi GAS) | — | ya (GTS R1+R4) |
| Watchdog | ya (esp_task_wdt) | ya | n/a | n/a | **tidak** |
| Emergency lokal tanpa jaringan | ya (EmergencySupervisor) | ya (emgTick local-first) | n/a | n/a | tidak |
| Terhubung pipeline rilis CI | ya (tag v*) | ya (rilis biner) | bundle manual | **tidak** | **tidak** |

(n/a = tidak relevan untuk jenis permukaan tersebut; **tidak** = risiko
divergensi nyata yang menjadikan permukaan itu dibekukan/dipensiunkan.)

## 3. Kebijakan penegakan (apa yang CI tolak)

1. **Permukaan frozen/deprecated berubah** — setiap file di bawah
   `push-alarm/gas/` dan `push-alarm/firmware/MonitorIoT_Firmware/`
   dip-pin SHA-256. Ubah satu byte, tambah file, hapus file → job
   `surface-policy-gate` GAGAL. Satu-satunya jalan mengubah adalah
   memperbarui pin di `SURFACE-STATUS.json` pada PR yang sama — pin di
   diff adalah tanda tangan eksplisit reviewer, terlihat dan bisa
   di-negotiate, bukan modifikasi senyap.
2. **Marker hardening hilang dari permukaan aktif** — refactor yang
   menghapus `P0-003` (fail-closed device secret), `getOrCreateApPassword`
   ([FW-A3]), `setCACert(GAS_ROOT_CA)`, `HMAC-SHA256`/`DEVICES_HEADER`/
   `SeqIndex` di code.gs, atau `esp_task_wdt` → GAGAL. Refactor tetap
   boleh, marker harus dipulihkan/didaftarkan ulang dengan sengaja.
3. **Fabrikasi riwayat rotasi** — `docs/security/credential-inventory.json`
   menolak `lastRotated` non-null tanpa `evidenceRef` yang menunjuk file
   bukti yang benar-benar ada (temuan #19, lihat runbook rotasi).

Batas scope yang diakui jujur: checker ini analisis statis penanda —
ia TIDAK membuktikan semantik dua kontrak identik, TIDAK membandingkan
logika baris-per-baris antar permukaan, dan TIDAK memeriksa repo PWA
(permukaan lintas-repo dicatat sebagai `external-ref`).

## 4. Keputusan terbuka (butuh pemilik repo, bukan agent)

| ID | Keputusan | Opsi | Konsekuensi menunda |
|---|---|---|---|
| D-1 | Nasib `push-alarm/gas/` | pensiunkan → migrasi fleet ke `code.gs/`; ATAU promosikan jadi kanonik → naikkan kontrak P1-00x + HMAC v2.1 | Dua backend GAS diverifikasi berbeda tetap hidup; audit keamanan harus dilakukan dua kali |
| D-2 | Nasib `push-alarm/firmware/MonitorIoT_Firmware/` | hapus setelah konfirmasi tidak ada fleet greenhouse aktif mem-flash dari sini | Permukaan deprecated tetap di repo; risiko orang baru mem-flash jalur tanpa hardening |

Kedua keputusan sengaja TIDAK diambil sepihak oleh agent — penghapusan
jalur deployment adalah keputusan pemilik sistem. Sampai diputuskan,
pin menjaga status quo secara auditable.

## 5. Checklist perbaikan keamanan lintas permukaan

Ketika sebuah perbaikan keamanan menyentuh kontrak bersama, gunakan
checklist ini di PR (salin ke deskripsi PR):

- [ ] `firmware/` — terdampak? sudah diterapkan?
- [ ] `firmware-generic/` — terdampak? sudah diterapkan?
- [ ] `code.gs/` — terdampak? sudah diterapkan? (bundle GAS + redeploy)
- [ ] `push-alarm/gas/` (frozen) — jika terdampak: tulis alasannya di PR
      dan perbarui pin, ATAU selesaikan dulu keputusan D-1.
- [ ] `MonitorIoT/` (deprecated) — jika terdampak: tulis alasannya di PR
      dan perbarui pin, ATAU tinggalkan sengaja (dokumentasikan).
- [ ] Repo PWA — `pwa-push-alarm/` / `public/firmware/manifest.json`
      terdampak? (penegakan di repo PWA sendiri)
