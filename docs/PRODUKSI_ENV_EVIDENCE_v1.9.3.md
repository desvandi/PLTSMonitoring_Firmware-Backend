# PRODUCTION ENV EVIDENCE — PWA Vercel (Jawaban atas P1 #5 Auditor · GATE A)

> **Status arsip repo (2026-09-06):** Salinan resmi untuk arsip rantai rilis dari
> dokumen referensi Gate A dalam paket sesi bench v1.9.3. Substansi identik dengan
> versi paket; referensi path lokal workspace engineer digeneralisasi dan nama
> akun Vercel diringkas. Lihat juga addendum §8 di
> `docs/FINAL_SOFTWARE_AUDIT_v1.9.3.md` serta struktur gate
> `docs/FINAL_ACCEPTANCE_GATES_v1.9.3.md`.

> **Tanggal probe:** 2026-09-06 09:11–09:13 UTC (probe ulang 10:05 UTC, hasil sama) · Dilakukan oleh: Engineer (Super Z)
> **Objek:** deployment produksi PWA yang live dari main `9e47b4f` (pasca PARITY-4)
> **Metode:** probe runtime publik (HTTP) + inspeksi bundle client ter-deploy —
> bukan klaim dari source/CI. Inilah "deployment evidence" yang diminta auditor:
> P1 #5 ("harus ada deployment evidence bahwa NEXT_PUBLIC_BACKEND_API_BASE_URL
> benar-benar menunjuk ke authoritative backend production").

> **KLASIFIKASI FINAL (Independent Auditor, 2026-09-06):** gap ini **bukan
> software-code defect** (kode ter-deploy benar dan fail-closed), **tetapi tetap
> RELEASE BLOCKER OPERASIONAL untuk OTA production** — karena PWA production
> environment adalah bagian dari topologi OTA Phase A/B. Status tetap BLOCKING
> sampai topologi OTA yang dipilih terbukti bekerja. Ini dokumen referensi
> **Gate A** dalam struktur gate final audit (A–F).

---

## 1. Hasil probe runtime (fakta mentah)

### 1a. Domain & build

| Item | Nilai |
|---|---|
| URL produksi | `plts-monitoring-pwa.vercel.app` → **307 redirect** → `jmse-plts-monitoring.vercel.app` (HTTP 200) |
| `/api/health` | 200, `nodeEnv: "production"`, `service: "plts-monitor-pwa"` |
| Bundle = main terbaru? | **YA** — chunk client memuat marker PARITY-4 (`voltageLowWarn` ×2, `alarmThresholds`), pin rilis `v1.9.3`, dan resolver OTA `api.github.com/repos/desvandi/PLTSMonitoring_Firmware-Backend/releases/tags` |

### 1b. Respons `/api/health` (endpoint publik by-design untuk Vercel Cron)

```json
{
  "checks": {
    "mqttBrokerConfigured": false,
    "mqttCredentialsConfigured": false,
    "gasUrlConfigured": false,
    "jwtSecretConfigured": false
  },
  "gasPing": {
    "attempted": false,
    "configured": false,
    "error": "NEXT_PUBLIC_GAS_INSIGHTS_URL tidak diset"
  }
}
```

### 1c. Inspeksi bundle client (19 chunk, 2,0 MB)

- **TIDAK ADA** URL backend/perangkat yang ter-bake (tidak ada `NEXT_PUBLIC_API_BASE_URL`,
  `NEXT_PUBLIC_BACKEND_API_BASE_URL`, MQTT, atau GAS URL nyata di bundle).
  Satu-satunya `script.google.com/macros/s/` hanyalah *placeholder form* Push Alarm
  (`placeholder="https://script.google.com/macros/s/…/exec"`), bukan nilai env.
- `/api/ota/check` dan `/api/reports` menjawab **405 terhadap GET** — benar: keduanya
  route POST + auth + CSRF (bukan bug). `/api/version` → 401 (auth wajib, sesuai desain).

## 2. Interpretasi (jujur, tanpa membungkus)

1. **Kode yang ter-deploy sudah benar dan fail-closed**: build terbaru (PARITY-4),
   tidak pernah menyajikan data fabrikasi (mock 503 jujur, demo force-off di produksi).
2. **Env transport produksi BELUM dikonfigurasi sama sekali.** Konsekuensi:
   - `backendApi` jatuh ke fallback same-origin → `/api/ota/check` (resolver GitHub)
     tetap hidup; `/api/reports` PWA-origin = 503 jujur (DAILY memang milik GAS).
   - Jalur Reports primer (PARITY-3) TIDAK lewat env: ReportsView memakai
     `device.gas_webapp_url` per-device dari sysConfig (binding /setup) →
     laporan tetap bisa hidup bila binding perangkat sudah dibuat.
   - Mode "zero-touch GAS cloud viewer" (F9, terdokumentasi di README/auth-provider)
     tetap berfungsi untuk MEMBACA — tapi **view OTA/config/calibration/relays/settings
     di-gate operator** (`OPERATOR_ONLY_VIEWS` di app-shell) dan login REST perangkat
     membutuhkan transport.
3. **CI tidak bisa melihat ini**: job `production-config-gate` di CI memakai env
   *representative* (`wss://broker.plts-monitoring.internal`, `plts-tunnel.example.com`)
   untuk menguji LOGIKA gate — bukan env deployment nyata. Hijau CI ≠ env produksi
   terpasang. Inilah akar "release evidence gap" P1 #5 — dan probe di atas
   membuktikannya sebagai **gap nyata**, bukan sekadar ketidakbuktian.

## 3. Matriks env produksi (apa yang HARUS/SEBAIKNYA diset di Vercel)

> `NEXT_PUBLIC_*` di-**bake saat build** — menambah/mengubahnya WAJIB diikuti
> **Redeploy** agar masuk ke bundle client.

| Variable | Wajib? | Untuk apa | Catatan |
|---|---|---|---|
| `NEXT_PUBLIC_API_BASE_URL` | **Ya** (mode REST) | Transport browser→ESP32 (status, login operator, OTA upload) | Harus `https://` (tunnel) bila PWA di-akses dari Vercel HTTPS — `http://LAN` akan diblokir *mixed content*. `http://LAN` hanya sah untuk PWA yang dijalankan lokal di bench. Gate CI menolak localhost |
| `NEXT_PUBLIC_MQTT_BROKER_URL` (+ `USERNAME`/`PASSWORD`) | Alternatif transport | Mode MQTT viewer (realtime subscribe-only) | Wajib `wss://`, broker self-hosted; broker publik DITOLAK gate |
| `NEXT_PUBLIC_GAS_INSIGHTS_URL` | Disarankan | Probe `/api/health` (gasPing) — **mekanisme bukti P1 #5**; fallback server-side insights | Server-side saja (tidak boleh dibaca browser) |
| `NEXT_PUBLIC_BACKEND_API_BASE_URL` | Opsional | Base agregasi alternatif (reports/OTA history) | Default = `NEXT_PUBLIC_API_BASE_URL`; jalur Reports primer sudah GAS-first via sysConfig per-device |
| `JWT_SECRET` | Tidak (produksi REST) | Hanya untuk mock-auth LAN di dev/demo | ≥32 char; di produksi login operator terjadi di perangkat |
| `NEXT_PUBLIC_DEMO_MODE` | **Jangan set** | — | Force-off di build produksi; gate CI gagal bila `true` |
| `NEXT_PUBLIC_EXPECTED_FIRMWARE_TAG` | **Jangan set** | — | Invarian di-pin `release-policy.json` = `v1.9.3`; nilai beda = build GAGAL (by design) |
| `NEXT_PUBLIC_RELEASE_CHANNEL` | Biarkan kosong | — | Harus `production` bila diset |
| `NEXT_PUBLIC_PUSH_API_BASE` + `NEXT_PUBLIC_PUSH_VAPID_PUBLIC_KEY` | Opsional | Web push alarm | URL valid + kunci VAPID 65-byte base64url |

## 4. Prosedur penutupan Gate A / P1 #5 (milik Anda — butuh akses dashboard Vercel)

1. Vercel → project `jmse-plts-monitoring` → Settings → Environment Variables
   (scope **Production**) → set minimal: `NEXT_PUBLIC_API_BASE_URL`
   (URL tunnel HTTPS perangkat) atau var MQTT; disarankan juga
   `NEXT_PUBLIC_GAS_INSIGHTS_URL`.
2. **Redeploy** (Deployments → Redeploy) — `NEXT_PUBLIC_*` ter-bake saat build.
3. Ambil bukti (ini artefak evidence-nya):
   ```bash
   curl -s https://jmse-plts-monitoring.vercel.app/api/health | tee health-production-<tanggal>.json
   # terima: checks.mqttBrokerConfigured / gasUrlConfigured = true, gasPing.reachable = true
   ```
4. Verifikasi fungsional tambahan pasca-rilis v1.9.3 (P1 #6):
   - `curl -s -o /dev/null -w "%{http_code}" https://api.github.com/repos/desvandi/PLTSMonitoring_Firmware-Backend/releases/tags/v1.9.3` → **200**
   - Login operator di PWA → OTA view → "Fetch Authorized Release" → tampil
     `v1.9.3`, SHA `24e8ae22…`, tanpa peringatan mismatch.
5. Arsipkan JSON health + screenshot OTA view sebagai lampiran buku lapangan
   (bagian verifikasi akhir).

## 5. Catatan khusus Phase A/B sesi bench (topologi browser→perangkat)

OTA view **operator-only** → browser yang dipakai Phase A/B harus bisa login REST
ke perangkat. Dua topologi yang sah:

- **A (produksi penuh, disarankan):** ESP32 bench di belakang tunnel HTTPS
  (mis. Cloudflare Tunnel) → set `NEXT_PUBLIC_API_BASE_URL` ke URL tunnel →
  redeploy → jalankan Phase A/B dari deployment Vercel produksi itu sendiri
  (sekaligus jadi bukti P1 #5/P1 #6).
- **B (bench lokal):** jalankan PWA lokal di PC bench
  (`npm ci && npm run build && npm start`) dengan `NEXT_PUBLIC_API_BASE_URL=http://<ip-lan-esp32>`
  — sah untuk uji fisik, tetapi evidence "PWA produksi" untuk P1 #5 tetap
  butuh topologi A (atau probe health no. 4).

## 6. Status optimasi akun Vercel — EKSEKUSI 2026-09-06 ~11:25 UTC

> Token kedua (diberikan user setelah token pertama invalid) **VALID** — akun
> Vercel milik pemilik project (tier hobby). Sesuai arahan user, token dipakai HANYA
> untuk keperluan project ini (plts-monitoring-pwa + plts-monitor-push-alarm;
> remote-relay & jambisolarpanel tidak disentuh).

### 6a. `jmse-plts-monitoring.vercel.app` (project plts-monitoring-pwa)

- **Git-link benar**: `desvandi/PLTSMonitoring_PWA`, branch main, node 24.x;
  deployment READY terakhir dari `9e47b4f` (= freeze PWA). Domain lama
  `plts-monitoring-pwa.vercel.app` 307 → domain utama. Tidak ada yang salah.
- **Env var**: 9 var sudah TERSEDIA sejak 2026-09-02 (production+preview):
  `NEXT_PUBLIC_API_BASE_URL`, `NEXT_PUBLIC_GAS_INSIGHTS_URL`,
  `NEXT_PUBLIC_MQTT_BROKER_URL/USERNAME/PASSWORD`, `JWT_SECRET`, `DEMO_MODE`*
  , `NEXT_PUBLIC_DEMO_MODE`, `SERWIST_DEV`. Nilai tidak dapat dibaca token
  (no-decrypt scope), namun bukti tidak-langsung (env dibuat SEBELUM deploy
  09-06, bundle tanpa URL ter-bake, health semua false) ⇒ **nilainya kosong**.
  *`DEMO_MODE` tanpa prefix tidak dipakai kode (kode membaca
  `NEXT_PUBLIC_DEMO_MODE`) — var menganggur, aman dibiarkan/dihapus.
- **SW produksi diverifikasi non-dev**: `__WB_DISABLE_DEV_LOGS` aktif +
  mesin precache serwist ada. Sehat.
- **Gate A tetap terbuka** — menunggu NILAI RIIL dari Anda (URL tunnel HTTPS
  perangkat / MQTT / URL GAS insights). Setelah nilai masuk → set var →
  redeploy → probe `/api/health` (prosedur §4). Tidak ada nilai sintetis
  yang diset demi indikator hijau.

### 6b. `plts-monitor-push-alarm.vercel.app` — DIPERBAIKI

- **Temuan lama (kini closed)**: sw.js ter-deploy usang — tanpa fix audit-2
  K-5. **Fix**: deployment baru `dpl_DzA2PBr8uoUg5bv28hBozJJkvqgc`
  (production, READY) dari isi `pwa-push-alarm/` repo main `9e47b4f`
  (API v13 file-upload, 13 file, identik repo):
  - `sw.js` live sha256 `be7fdd30fed6d05d…` = **repo main** (match) —
    **K-5 same-origin notification URL fix kini LIVE**;
  - header `vercel.json` aktif (sw.js: no-cache + Service-Worker-Allowed: /);
  - semua file inti 200 (index/manifest/js/css/icons).
- **Kejadian dicatat (transparansi)**: percobaan pertama deploy via
  `gitSource` (dpl_6g7DKFkt) salah sasaran — akan build repo ROOT (karena
  project tak ter-link & rootDirectory null) → langsung DIBATALKAN + DIHAPUS
  sebelum READY; domain tidak pernah menyajikannya; sw.js lama tetap
  terverifikasi sepanjang proses.
- **Config tetap placeholder (jujur)**: `js/config.js` — `API_BASE` (URL
  webapp GAS) + `VAPID_PUBLIC_KEY` menunggu nilai Anda → tanpa itu fitur
  push alarm memang belum aktif (konsisten status fitur).
- **Penghubungan git ditolak API** (`PATCH /v9/projects` tidak menerima
  `link`) → langkah dashboard 1-menit MILIK ANDA untuk auto-deploy masa
  depan: project `plts-monitor-push-alarm` → Settings → Git → **Connect
  Git Repository** `desvandi/PLTSMonitoring_PWA` → Root Directory
  `pwa-push-alarm` → Production Branch `main`. (Setelah terhubung, push ke
  main yang menyentuh `pwa-push-alarm/` otomatis ter-deploy.)
- Script deploy tersimpan di workspace engineer: `deploy_push_alarm.py`
  (bisa dipakai ulang; token dibaca dari file 600).
