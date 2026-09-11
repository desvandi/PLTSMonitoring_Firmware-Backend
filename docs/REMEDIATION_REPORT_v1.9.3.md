# Laporan Remediasi Production Grade — PLTS Monitoring System

**Tanggal**: 2026-09-11
**Basis**: File Audit PLTS Monitoring Project (397 poin temuan, audit lintas-repo + runtime)
**Scope**: Komplit sampai P2 (sesuai keputusan pemilik proyek — breaking change diizinkan)
**Repos**: `PLTSMonitoring_Firmware-Backend` (PR #28, squash `6a166c28`), `PLTSMonitoring_PWA` (PR #11, squash `f81b7aee`)

---

## 1. Ringkasan Eksekutif

Seluruh **6 release blocker** audit telah ditutup di level source, ditambah hardening P1/P2 lintas subsistem. Semua perubahan diverifikasi terhadap kode aktual terlebih dahulu (13 temuan kunci dikonfirmasi langsung sebelum diperbaiki), lulus CI lengkap kedua repo (Python property tests + PlatformIO build development/staging/generic + Vitest 195/195 + typecheck + eslint + production build), dan sudah ter-merge ke `main` dengan deployment produksi Vercel yang baru aktif.

Skor audit awal → status setelah remediasi (level source):

| Subsistem | Skor audit | Status kini |
|---|---|---|
| Architecture | 9.0/10 | 🟢 dipertahankan |
| Relay transactional | 6.5/10 🔴 | 🟢 single executor + identitas transaksi utuh |
| I²C hardware integrity | 6.5/10 🔴 | 🟢 shadow-state fail-closed + mutex bus |
| MQTT delivery | 6.8/10 🔴 | 🟢 kontrak QoS-0 jujur + spool at-least-once |
| Storage | 7.2/10 🟡 | 🟢 blob CRC + validasi + marker reset |
| Authentication | 8.2/10 🟡 | 🟢 revocation + rotasi atomik + CRC nyata |
| OTA | 8.6/10 | 🟢 jobId CSPRNG + WDT invariant |
| PWA | 8.7/10 | 🟢 truth-semantics + envelope + CLOCK_SKEW |

> **Catatan penting**: "Production Grade" level source ≠ rilis final. Physical acceptance v1.9.3 (Gate F audit) tetap wajib dijalankan di hardware nyata — checklist lengkap ada di Bagian 6.

---

## 2. Release Blocker yang Ditutup

### BLOCKER A — Single Physical Relay Executor (audit p.34-59, 284-294)
- Queue ring-buffer custom (`volatile` head/tail, tanpa sinkronisasi) **diganti FreeRTOS queue native** yang membawa `QueuedRelayCommand` — identitas transaksi lengkap (transactionId/requestId/commandHash/envelope/safetyGeneration).
- **Tiga jalur eksekusi paralel dihapus**: (1) REST per-channel sudah via queue, (2) REST `all_off` kini via queue (tidak lagi `allOffWithResult()` langsung dari HTTP task), (3) MQTT relay kini via queue (tidak lagi `applyCommand()` langsung dari networkTask).
- **Safety generation** (audit p.366-369): `emergencyAllOff()` menaikkan epoch; command yang di-queue sebelum E-WAVE trip di-`BLOCKED_STALE_SAFETY` saat eksekusi — relay tidak bisa "hidup kembali" setelah emergency.
- `emergencyAllOff()` kini **menulis OFF ke SEMUA channel terlepas dari `reportedState`** (audit p.363-365: setelah fault I²C, physical state unknown — safety wajib mencoba menulis).
- Re-validasi saat eksekusi (audit p.375-377): freshness + safety generation diperiksa ulang saat dequeue, segera sebelum mutasi hardware.
- **Endpoint baru** `GET /api/relays/transactions/{transactionId}` — hasil akhir (EXECUTED/BLOCKED/REJECTED/FAILED/UNKNOWN + stateSequence) untuk rekonsiliasi PWA.

### BLOCKER B — Config Transactional Atomicity (audit p.274-279)
- Service baru `Services::ConfigUpdater` — mutasi dua fase: seluruh field divalidasi ke **candidate** (nol mutasi RAM), cross-field tier-order diuji pada candidate, baru commit. Distr<sup>1</sup> REST dan MQTT kini berbagi SATU hukum validasi.
- Request yang ditolak (400) **tidak lagi meninggalkan field terdahulu bermutasi di RAM**.

### BLOCKER C — Calibration REST Bypass (audit p.266-269)
- `calibration.point` dan `calibration.acs712_zero` REST kini melalui pipeline transaksi kanonik (envelope → freshness → journal → apply → ACK). Retry setelah timeout kini **dedupe** ke ACK pertama, bukan double-capture.

### BLOCKER D — Alarm ACK REST Bypass (audit p.270-271)
- `alarm.acknowledge` dan `acknowledgeAll` REST kini melalui pipeline yang sama; kontrak 404 untuk kode alarm tidak dikenal dipertahankan; setiap ACK tercatat dengan transactionId.

### BLOCKER E — Persistence Failure ≠ Success ACK (audit p.281-282, 319-323)
- `TransactionJournal::storeTransaction()` kini **rollback RAM** bila NVS commit gagal (false DUPLICATE untuk transaksi tidak-durabel hilang).
- Semua caller memeriksa return value: kegagalan journal → HTTP 503 / NACK `DURABILITY_FAILURE`, **tidak pernah** ACK sukses.
- `saveBatteryConfig()` mengembalikan `bool` dengan verifikasi tulis per-key.

### BLOCKER F — RelayExpanderDriver Shadow State (audit p.89-96)
- `setChannel()` menghitung state BERIKUTNYA tanpa menyentuh shadow; shadow **di-commit hanya setelah write sukses**. Kegagalan write → `_shadowUnknown=true` → mutasi normal ditolak sampai `recoverWithAllOff()` (write 0xFF + readback terverifikasi) berhasil.
- Bug "channel lain ikut ON karena bit sisa dari kegagalan sebelumnya" (audit p.90-91) **tertutup secara struktural**.
- `_readInput()` → `bool _readInput(uint8_t&)` — kegagalan read tidak lagi teralias ke 0xFF (yang nilainya valid = semua OFF); verifikasi readback di `begin()` tidak bisa lagi false-positive.
- **`Utils::I2CBusGuard`** — mutex rekursif FreeRTOS untuk bus Wire bersama (sensorTask/relayTask/networkTask). Klaim "thread-safe via I²C mutex" di header kini benar-benar didukung implementasi.

---

## 3. Hardening P1/P2 Lainnya

| Temuan Audit | Perbaikan |
|---|---|
| CORE-01: `isFieldAllowed()` scope type-only | Scope tuple `(type, action)` eksak — field yang diterima selalu ikut di-hash |
| CORE-02: envelope opsional | `validateCommandEnvelope()` wajib di SEMUA ingress: REST (relay/config/calibration/alarm/password/device), MQTT (config + OTA) |
| NaN/Inf/-0.0 canonical hash | Ditolak / dinormalisasi; delimiter `\| = \` di-escape (grammar tak ambigu) |
| Registry `(config, device)` hilang | **Bug laten ditemukan saat remediasi**: setiap update device-settings PWA gagal 400 — entri ditambahkan |
| AUTH-GATE-05: rotasi refresh race | `consumeRefreshToken()` — SATU operasi atomik find→validate→mark-used→rotate→persist di bawah mutex |
| AUTH-GATE-07: logout tanpa revocation | `revokeAllRefreshTokens()` dipanggil di logout — token curian mati saat logout, bukan 7 hari |
| AUTH-GATE-06: komentar "CRC32 guard" bohong | Blob rtokens kini bermagic+version+CRC32 nyata; legacy 224-byte diterima sekali lalu re-persist format baru |
| AUTH-GATE-04: PBKDF2 tanpa batas atas | Clamp [1000, 50000] saat boot |
| STORAGE: energy 6 key terpisah | Satu blob `ENERG` magic+version+CRC (+ legacy sync untuk rollback) + sanity gate non-finite/negatif |
| STORAGE-GATE-03: battery config tanpa validasi load | Range + cross-field validation penuh dengan fallback default yang di-log |
| STORAGE-GATE-06: factory reset bukan transaksi | Marker `ftr_p` IN_PROGRESS sebelum wipe + `handlePendingFactoryReset()` di boot menyelesaikan reset yang terputus |
| MQTT QoS lie (audit p.5, 55) | Semua komentar "QoS 1/PUBACK" diganti kontrak jujur QoS-0 best-effort + spool replay at-least-once + dedup GAS |
| OTA jobId collision (audit p.209-212) | `Utils::generateToken(32)` CSPRNG 128-bit (bukan unixTime+size) |
| WDT vs blocking TLS (audit p.26, 115) | Timeout HTTP 7s→4s; invariant terdokumentasi: worst case 8s < 10s TWDT |
| CORS origin mismatch (audit p.115) | `plts.example.com` → `https://jmse-plts-monitoring.vercel.app` (overridable `PIO_ALLOWED_CORS_ORIGINS`) |
| PWA CONFIRMED_ON stale (audit p.70) | Derivasi dari aksi yang sukses; QUEUED → PENDING; UNKNOWN first-class |
| PWA tanpa freshness (audit p.62-65) | `buildCommandEnvelope()` (TTL 60s, transactionId reuse saat retry) di SEMUA mutasi |
| CLOCK_SKEW (audit p.240-241) | `computeFreshness()` + status CLOCK_SKEW >5 menit di masa depan (mirror jendela ±300s HMAC GAS) + property tests |
| CI Node 20 vs Vercel 24 (audit p.24) | CI + engines dipin ke Node 24 |
| Metadata "Monitoring-only" (audit p.16) | Deskripsi produk jujur (relay, OTA, alarm, reports) |

---

## 4. Status Deployment (diverifikasi langsung)

| Layanan | Status | Bukti |
|---|---|---|
| PWA produksi | **LIVE** | `jmse-plts-monitoring.vercel.app` → 200, deployment `f81b7aee` (PR #11), build sukses dari main |
| Push-alarm produksi | **PULIHKAN** | Semua route 200 kembali (lihat insiden di Bagian 5) |
| Firmware CI | **HIJAU** | Python tests + PlatformIO (development/staging/generic) semua PASS sebelum merge |

---

## 5. Insiden & Perbaikan Deployment push-alarm

**Kronologi**: Project Vercel `plts-monitor-push-alarm` ternyata ter-link ke repo **Firmware-Backend** (root repo, tanpa `rootDirectory`) — koneksi ini dibuat 2026-09-11 pukul 09:08 UTC, sebelum sesi remediasi. Merge PR firmware #28 memicu auto-deploy produksi dari root repo firmware yang tidak berisi web app → deployment kosong menimpa deployment manual lama (`dpl_DzA2PBr8`, yang juga kosong menurut API — sumber aslinya direktori lokal `deploy-vercel/` yang tidak pernah masuk repo, persis temuan provenance P1 audit p.15) → domain 404 total.

**Perbaikan yang dijalankan**:
1. `rootDirectory` project diset ke `pwa-push-alarm` (sumber asli di repo PWA).
2. Deployment produksi dibuat langsung via API v13 dari file sumber `PLTSMonitoring_PWA/pwa-push-alarm/` (14 file, prefix rootDirectory) → `dpl_CW2MEX2egxB1gB4GaS5AooMykc4U` READY.
3. Verifikasi: `/`, `/manifest.json`, `/sw.js` semuanya 200; HTML Dashboard Alarm MonitorIoT tersaji dengan benar.

**Properti pengaman kini**: push firmware main berikutnya akan menghasilkan deployment ERROR (folder `pwa-push-alarm/` tidak ada di repo firmware) dan **tidak menimpa** produksi — fail-closed.

**Tindakan operator yang tersisa**: di Vercel dashboard → project `plts-monitor-push-alarm` → Settings → Git: **ganti connected repository ke `PLTSMonitoring_PWA`** (root directory `pwa-push-alarm` sudah benar). Setelah itu setiap merge PWA main akan auto-deploy push-alarm dengan provenance Git penuh (menutup P1 provenance audit secara permanen).

---

## 6. Physical Acceptance Checklist (WAJIB sebelum rilis resmi)

> Level source sudah Production Grade; klaim "zero error" hanya sah setelah checklist ini LULUS di hardware nyata (audit p.228, Gate F).

### T1 — I²C Fault Recovery Test (audit p.111, 20 siklus)
1. Semua relay OFF → 2. Cabut PCF8574 → 3. Kirim CH0 ON (REST, envelope lengkap) → 4. Verifikasi ACK bukan EXECUTED dan fault=true → 5. Verifikasi `reportedState` CH0 tidak berubah → 6. Pasang kembali PCF8574 → 7. Kirim CH1 OFF → 8. **Verifikasi CH0 tetap OFF secara fisik** (bit sisa tidak ada) → 9. CH0 OFF → 10. CH1 ON → 11. Kedua channel eksak.
**Kriteria**: 20 siklus = 0 unintended transition, 0 stale shadow-state, 0 false-success.

### T2 — Queue Concurrency Test (audit p.112)
10 request simultan multi-channel via REST (dengan envelope + transactionId unik). Verifikasi: FIFO utuh, identitas transaksi sampai hasil akhir, tidak ada eksekusi ganda/hilang, tidak ada korupsi queue. Cek `GET /api/relays/transactions/{id}` untuk tiap transaksi.

### T3 — Power-Loss Matrix (audit p.113)
Matikan daya pada titik: sebelum journal / setelah journal / setelah queue / saat I²C write / setelah write / sebelum ACK. Ekspektasi ditentukan per titik: hasil tak-pasti = UNKNOWN (bukan FAILED, bukan EXECUTED); boot selalu ALL OFF; lockout persist.

### T4 — Emergency Semantics (audit p.363-369)
1. CH0-CH7 ON (via queue) → 2. Isi queue dengan beberapa ON pending → 3. Picu E-WAVE → 4. **Verifikasi semua channel dicoba OFF** (termasuk yang reportedState=false) → 5. Verifikasi command pending diblok `BLOCKED_STALE_SAFETY` → 6. Clear emergency → 7. **Verifikasi relay tidak otomatis ON kembali** — operator harus mengirim command baru.

### T5 — Envelope & Replay
1. Command tanpa `expiresAt` → ditolak 400 → 2. Command kadaluarsa → ditolak → 3. Retry dengan `transactionId` sama → ACK pertama di-replay (DUPLICATE), tidak dieksekusi dua kali → 4. `transactionId` sama + payload beda → 409 CONFLICT.

### T6 — Auth
1. Login → refresh → replay token refresh lama → 401 (satu kali pakai) → 2. Login di browser A + B → logout dari A → **verifikasi B juga logout** (revocation menyeluruh) → 3. Restart device → sesi persist.

### T7 — Factory Reset Mid-Power-Loss
Mulai factory reset → potong daya di tengah → boot ulang → **verifikasi device menyelesaikan reset penuh** (bukan setengah-reset) via log `[RESET] Factory reset completed after interruption`.

### T8 — 24-Jam Soak
Telemetry berjalan, tidak ada reset WDT, dedup GAS bekerja (cek duplicate/late/gap di Sheets), alarm lifecycle benar, energy counter koheren setelah reboot.

---

## 7. Tindakan Operator Tersisa (di luar kode)

| # | Item | Prioritas |
|---|---|---|
| 1 | **Rotasi credential** GitHub PAT + Vercel token yang pernah dibagikan di chat (audit p.32, P0 operasional) | SEGERA |
| 2 | Vercel dashboard: isi env produksi PWA (MQTT broker/credentials, GAS URL, JWT secret — Gate A audit p.13/116; health check kini menunjukkan keempatnya belum terisi) | P0 |
| 3 | Vercel dashboard: relink project `plts-monitor-push-alarm` ke repo `PLTSMonitoring_PWA` (lihat Bagian 5) | P1 |
| 4 | Jalankan checklist T1-T8 di hardware → buat `docs/hardware-acceptance/v1.9.3.json` (Gate F) | P0 sebelum rilis |
| 5 | Flash ulang firmware produksi (artifact dari CI build `production` env dengan secrets) — firmware baru BARU source-level tanpa physical acceptance | P0 |
| 6 | Keputusan arsitektur: Secure Boot / Flash Encryption / eFuse anti-rollback (audit p.222-224) — minimal dokumentasikan sebagai batasan yang disengaja | P2 |
| 7 | Inisiatif v3 (bila fleet tumbuh): pisah `requestId` vs `transactionId` per-attempt, nonce-based CSP, manifest OTA tertanda-tangan | P3 |

---

## 8. Verifikasi Kualitas yang Dijalankan

- **13 temuan audit dikonfirmasi langsung ke kode** sebelum diperbaiki (tidak ada perbaikan buta).
- Firmware: `pio run -e development` + `-e staging` SUCCESS lokal; CI produksi (dengan secrets repo) PASS.
- Firmware: seluruh 20+ skrip test Python PASS (termasuk 2 test contract yang diperbarui mengikuti arsitektur ConfigUpdater bersama — kontrak semantik tetap, pola grep disesuaikan).
- PWA: Vitest **195/195** (termasuk 3 property test CLOCK_SKEW baru), `tsc --noEmit` bersih, `next build` sukses, ESLint 0 error.
- Vercel: deployment produksi PWA dari commit merge diverifikasi 200; push-alarm dipulihkan + diverifikasi.
