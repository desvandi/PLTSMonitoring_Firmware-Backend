# REMEDIATION.md — Audit p.489–p.491 (Firmware) + Kontrak GAS p.482

> Baseline audit: commit `28bf0a9` (PR #43). Respons engineer eksekutor atas
> temuan firmware (RBAC p.489, transport/cookie p.490, persistence p.491)
> ditambah kontrak ACK GAS (p.482).

## Ringkasan status

| Point | Temuan | Severity | Status | Bukti verifikasi |
|---|---|---|---|---|
| p.489 | Firmware tanpa privilege/RBAC boundary (semua JWT = full akses) | 🟠 P2 | ✅ FIXED | Build prod + dev SUKSES; native harness GREEN |
| p.490 | HTTP :80 plaintext + JWT cookie tanpa `Secure` di production | 🔴 P1 | ✅ FIXED (COOKIE_SECURE + HSTS + lockdown fail-closed; arsitektur TLS gateway diwajibkan) | Build production SUKSES (flag aktif) |
| p.491 | Persistence failure alarm/config dilaporkan sukses | 🔴 P1 | ✅ FIXED (bool + rollback + FAIL) | Build + native harness GREEN |
| p.482 | GAS ackAlarm hanya berbasis alarmId | 🟠 P2 | ✅ FIXED (ackToken HMAC; GAS harus di-redeploy operator) | `verify-ack-token.js` 12/12 PASS |

## p.489 — Role claim + capability enforcement di boundary perangkat

**Masalah**: `checkAuth()` hanya memverifikasi JWT dan membuang username —
tidak ada pemeriksaan role/capability setelah autentikasi. JWT payload hanya
`sub/iat/exp`. Model role viewer/operator hanya ada di PWA; begitu command
mencapai firmware dengan kredensial valid, firmware tidak membedakan viewer
dan operator (factory reset, reboot, kalibrasi, konfigurasi, relay, OTA).

**Perbaikan**:

1. `Utils::jwtSign()` kini membawa klaim `"role"` (default `"operator"` —
   akun web tunggal adalah admin); `jwtVerify()` versi baru mengekstrak
   klaim. **Token tanpa klaim role (issued firmware lama, TTL ≤ 15 mnt)
   di-resolve ke `"viewer"` — least privilege, fail-closed, bukan fail-open.**
2. `AuthManager::checkAuthRole(server, requiredRole)` — verifikasi JWT +
   pencocokan role constant-time.
3. `Web/Common.h` menambah `requireRole("operator")` (401 bila tidak
   terautentikasi, 403 bila role tidak mencukupi) dan diterapkan pada SEMUA
   endpoint berbahaya:
   - `POST /api/reboot`, `/api/factory_reset/prepare`, `/api/factory_reset/confirm`
   - `POST /api/config`, `/api/calibration` (+ point voltage, acs712 zero)
   - relay: `POST /api/relays/{ch}/on|off|pulse|acknowledge|clear`, `/api/relays/all_off`
   - `POST /api/alarms/{code}/acknowledge`, `/api/alarms/acknowledge-all`
   - `POST /api/ota` (role gate SEBELUM `Update.begin()`, pola flag rejected
     sama dengan auth gate — viewer tidak pernah menulis satu byte ke
     partisi OTA)
   - `POST /api/config/device`, `/api/config/password`, `/api/config/import`
4. Login menerbitkan token `operator` eksplisit; jalur refresh mewarisi
   lewat `issueAccessToken()`.

Boundary privilege kini ada DI PERANGKAT — PWA UI tidak lagi menjadi
satu-satunya pembatas (defense-in-depth penuh: UI gate → render gate →
command layer PWA → auth+CSRF+role firmware).

## p.490 — Transport production & cookie

**Masalah**: production menjalankan HTTP plaintext :80; `COOKIE_SECURE`
tidak pernah didefinisikan, sehingga JWT/refresh cookie dikirim tanpa
`Secure` — credential terekspos pada pengamat jaringan lokal.

**Perbaikan (fail-closed, arsitektur tetap dihormati)**:

1. `[env:production]` platformio.ini kini mendefinisikan `-DCOOKIE_SECURE`:
   cookie `jwt` dan `refresh` menjadi `Secure; SameSite=Strict`. Akses
   langsung plain-HTTP ke perangkat TIDAK BISA lagi membawa sesi
   terautentikasi (browser menolak mengirim cookie Secure via HTTP) —
   fail-closed yang MEMAKSA jalur TLS.
2. `sendSecurityHeaders()` menambah `Strict-Transport-Security:
   max-age=31536000` pada PRODUCTION_BUILD — header lolos melalui gateway
   TLS ke browser dan mem-pin HTTPS untuk hostname yang diekspos.
3. **Arsitektur production yang diwajibkan (didokumentasikan)**:
   `PWA → HTTPS → TLS gateway (Cloudflare Tunnel / nginx TLS proxy) →
   ter-autentikasi → ESP32 :80`. Cookie Secure bekerja karena browser hanya
   melihat hop HTTPS. Menambah `Secure` saja memang tidak menyelesaikan
   transport — karena itulah kombinasi ini yang dipilih: cookie Secure +
   HSTS + penolakan sesi via HTTP = jalur plaintext tertutup untuk sesi,
   jalur production satu-satunya adalah TLS gateway. (Embedded TLS server
   self-signed akan mematahkan fetch() browser dari PWA HTTPS — didokumentasikan
   sebagai alternatif lokal-CA di catatan arsitektur.)

## p.491 — Persistence fail-closed untuk konfigurasi keamanan

**Masalah**: `saveAlarmConfig()`/`saveDeviceConfig()`/`saveEmergencyConfig()`
bertipe `void`, `begin()` dan hasil `put*()` tidak diperiksa, dan
`ConfigUpdater::applyUpdate()` tetap menjawab `res.ok=true; "config updated"`.
Operator bisa memegang keyakinan palsu bahwa threshold proteksi tersimpan,
padahal hanya RAM yang berubah — reboot mengembalikan batas lama
(skenario audit: `voltageLowCritical` kembali ke nilai lama).

**Perbaikan**:

1. `ConfigStore`: ketiga fungsi menjadi `bool` dengan pola
   `saveBatteryConfig()` (begin() dicek; setiap `putFloat/putUChar/putULong/
   putChar/putString` dibandingkan ukuran tulisannya; log sukses/gagal).
2. `ConfigUpdater::applyUpdate()`: snapshot pra-commit → commit RAM →
   persist; **alarm (safety policy) FAIL-CLOSED**: gagal NVS → 11 threshold
   di-rollback ke nilai lama → `res.ok=false` + pesan eksplisit. Device
   name/timezone: rollback + FAILED (dan save tak lagi berjalan saat tidak
   ada perubahan — hemat NVS). Battery mempertahankan jalur WARNING-degraded
   yang sudah dipuji auditor.
3. `EmergencySupervisor::applyCommand("CONFIG")`: snapshot 13 field → gagal
   NVS → rollback + `applyPins()` dikembalikan + event `CONFIG_REFUSED` +
   result `REFUSED` (bukan `APPLIED`).
4. `handleDevicePost` (rename): snapshot + rollback + HTTP 500 eksplisit.
5. `importAll()`: setiap kegagalan save menandai import FAILED (restore
   tidak lengkap dilaporkan jujur).

Prinsip yang diterapkan seragam: **persistence gagal → mutasi dianggap
GAGAL + RAM di-rollback** — operator tidak pernah diberi keyakinan palsu
tentang keadaan kebijakan keamanan.

## p.482 (kontrak GAS) — ackAlarm ber-kapabilitas

`push-alarm/gas/Code.gs`:

- Token `ackToken = HMAC-SHA256(PUSH_ACK_SECRET, alarmId|bucket-6jam)`
  ditempel pada SETIAP payload push (`sendAlarmToAll`) dan dihitung segar
  pada fallback `latestAlarm`.
- `ackAlarm` tanpa token valid → **DITOLAK** (fail-closed). Escape hatch
  legacy hanya dengan Script Property `PUSH_ACK_ALLOW_LEGACY='true'`.
- Secret auto-provision 64-hex saat pertama kali dibutuhkan (rotasi =
  ganti property).
- PWA (`src/sw.ts` + `pwa-push-alarm/sw.js` di repo PWA) mengirim token
  bersama ACK — deploy PWA dulu, GAS kemudian (kompatibel selama transisi).

Harness verifikasi lokal (VM + mock Apps Script): 12/12 PASS — tokenless
ditolak, token valid diterima + idempoten, token salah/alarm lain ditolak,
auto-provision bekerja, legacy opt-in/opt-out dihormati.

## Verifikasi build

```
pio run -e development   → SUCCESS (RAM 37.8%, Flash 93.7%)
pio run -e production    → SUCCESS (RAM ~35%, Flash 91.3%)  [+ COOKIE_SECURE aktif]
scripts/native/run-native-tests.sh → ALL NATIVE HARNESS GREEN
```

## Catatan operator (deployment)

1. **FLASH firmware production baru** ke perangkat (build dari repo ini).
   Setelah upgrade, sesi lama maksimal 15 menit berumur; token tanpa klaim
   role diperlakukan viewer → re-login operator memulihkan mutasi.
2. Pastikan perangkat production diakses melalui TLS gateway (Cloudflare
   Tunnel/nginx) — cookie Secure menutup jalur HTTP langsung.
3. Redeploy GAS PushService (`push-alarm/gas/Code.gs`) setelah PWA
   ter-deploy untuk mengaktifkan kontrak ackToken.
4. Opsional: set Script Property `PUSH_ACK_SECRET` manual untuk rotasi
   terkendali (tanpa diset, secret auto-provision sekali).

## Self-Audit 2026-09-16 (pra-audit final)

1. **`scripts/inject_production_flags.py`** — `KeyError: 'CPPDEFINES'` saat
   build production dijalankan TANPA secret env: crash traceback menggantikan
   laporan validasi yang jujur. Diperbaiki (dictionary live + `.get()`):
   tanpa secret → build DITOLAK dengan daftar flag yang hilang (fail-closed
   bersih); dengan secret → build SUCCESS seperti sebelumnya.
2. **`push-alarm/gas/Code.gs` — unsubscribe kini mewajibkan autentikasi
   perangkat (simetri K-7)**: sebelumnya siapa pun yang mengetahui URL
   endpoint push korban dapat menghapus langganan korban secara diam-diam
   (mematikan pengiriman alarm — mutasi tanpa autentikasi, keluarga temuan
   p.482). Browser korban tetap dapat berhenti berlangganan lokal; endpoint
   basi dipangkas GAS saat push berikutnya memantul 410.
3. **`push-alarm/tests/cross-audit-test.js` dipulihkan** — 164/164 PASS:
   kontrol negatif K-7 (subscribe/unsubscribe tanpa/salah token ditolak),
   regresi payload ber-kredensial, kredensial tersalur ke SW untuk
   pushsubscriptionchange, dan K6b baru (kontrak firmware modular:
   TELEMETRY bertanda tangan HMAC + prinsip pengirim tunggal di seluruh
   pohon sumber).

Kontrak GAS K-7 lengkapan operator: token yang dipakai PWA saat mendaftar
push adalah token perangkat AKTIF (sama dengan yang dipakai firmware untuk
`ingest`) — pastikan `FW_DEVICE_TOKEN`/`FW_DEVICE_TOKENS` memuatnya.

---

## Putaran 2026-09-16 (P0-2, P0-3, P1 push hardening, p.493 backend)

### P0-2 — Unifikasi kontrak push ke canonical GAS
- `code.gs/PushService.gs` BARU: modul Web Push pada backend CANONICAL —
  SATU trust boundary untuk telemetry + push. Aksi baru di dispatcher:
  `PUSH_SUBSCRIBE` / `PUSH_UNSUBSCRIBE` / `PUSH_ACK` (push-scoped token,
  p.493), `PUSH_ALARM_INGEST` (auth canonical: HMAC ATAU token — kontrak
  sensor identik legacy), `PUSH_TEST` (ADMIN token — tanpa jalur publik),
  `PUSH_STATUS` (observability).
- `code.gs/Code.gs`: hook `pushEvaluateEmergency_` pada recordTelemetry_
  (DI LUAR lock telemetry; kegagalan push tidak pernah menggagalkan
  telemetry) — tepi TRIP/SAFE emergency menjadi AlarmEvent critical.
- `push-alarm/gas/Code.gs`: ditandai LEGACY (jalur migrasi) + hardening
  penuh (lihat bawah). Target arsitektur: ESP32 → canonical GAS → alarm
  state → PushService → PWA.

### P0-3 — Outbox AlarmEvent durable (kedua backend)
- Model: AlarmEvent { eventId, deviceId, alarmCode, generation, raisedAt,
  clearedAt, state, delivery{status, attempts, lastAttemptAt, ...} }.
- Urutan DURABLE-FIRST: event disimpan PENDING DULU → push dikirim →
  status delivery diperbarui. Kegagalan → PENDING → retry backoff
  eksponensial (maks 6 percobaan, umur 24 jam → EXPIRED jujur).
- Menutup dua failure mode auditor: (a) push terkirim tapi state gagal
  disimpan → duplikat; (b) state tersimpan tapi push gagal → notifikasi
  hilang diam-diam.

### P1 — Hardening push service
- **LockService** pada SEMUA mutasi state (subscribe/unsubscribe/ingest/
  ack/outbox) — disiplin yang sama dengan canonical telemetry.
- **deviceId binding** pada record langganan (audit fleet: endpoint ↔
  device ↔ lastSeen); delivery ter-scoped per device (record lama tanpa
  binding tetap menerima selama migrasi).
- **Ownership unsubscribe**: hanya record milik device (atau record lama
  tanpa binding) yang bisa dihapus — device A tidak bisa menghapus
  langganan device B.
- **testPush**: GET ditutup default (`TEST_PUSH_ALLOW_PUBLIC='true'`
  untuk jendela migrasi); POST wajib device.id + token. "Rate limiting
  adalah mitigasi DoS, bukan authorization."
- **p.493 backend**: PUSH_TOKENS (Script Property) — kapabilitas langganan
  SAJA, DITOLAK untuk ingest. Kredensial browser ≠ kredensial firmware.
- **apiBase pada payload** (getScriptUrl_): SW ACK tidak lagi bergantung
  konstanta yang tertanam di file.

### P0-4 — Clock authority (kontrak + prosedur)
- `docs/CLOCK_AUTHORITY_E2E.md` BARU: kontrak otoritas waktu lintas-layer
  (event_time = otoritas ordering; timeQuality menyertai sampel; sequence
  independen waktu; DAILY = timezone deployment) + 6 skenario acceptance
  E2E (T1-T6) pada perangkat nyata dengan bukti ke
  docs/hardware-acceptance/.

### Verifikasi
- Audit silang PWA-GAS-FW: **181/181 PASS** (dari 164; kontrak baru K9 =
  14 asersi: PUSH_TOKENS, deviceId binding, ownership, outbox
  PENDING/SENT/retry, migrasi localStorage, apiBase payload).
- Syntax check canonical Code.gs + PushService.gs + legacy Code.gs: OK.

### Self-audit pasca-remediasi — smoke UI suite di-upgrade ke arsitektur provisioning runtime (2026-09-16)
- Temuan self-audit: `tests/smoke-test-pwa.js` (suite 2 dari 3 di
  `tests/run-all.sh`) masih mengasumsikan arsitektur lama
  (config.js statis terisi) sehingga 6 asersi render gagal palsu
  setelah P0-1: app yang belum diprovision justru MENAMPILKAN layar
  setup (perilaku benar), bukan dashboard.
- Perbaikan: rewrite 3 fase —
  (A) kejujuran tanpa provisioning (layar setup tampil, dashboard
  disembunyikan, form lengkap);
  (B) provisioning via UI nyata (fill + submit + reload);
  (C) render dashboard dari mock GAS + asersi p.493 end-to-end
  (localStorage TIDAK berisi deviceId/deviceToken; sessionStorage YA).
- Bug harness ditemukan & diperbaiki: mock GAS dipasang di
  `browserContext.route()` (bukan `page.route()`) — setelah reload
  halaman dikendalikan service worker, dan fetch GAS di-inisiasi dari
  dalam SW; `page.route()` tidak mencegatnya sehingga SW menjawab 503
  internal. `context.route()` menangkap keduanya.
- Jumlah asersi: 17 → 25 (suite PWA); total harness 241
  (35 kripto + 25 smoke + 181 cross-audit K1-K9). Header `run-all.sh`
  dan `tools/prepush-audit.js` disinkronkan.
- Hasil: `tests/run-all.sh` = **3/3 SUITE LULUS** terhadap PWA main
  (c22fe17) + firmware main.
