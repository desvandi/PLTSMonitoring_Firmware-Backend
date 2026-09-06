# FINAL SOFTWARE AUDIT — v1.9.3 (Engineer-Side, Post PARITY-4)

> **Tanggal:** 2026-09-06 · **Peran:** Engineer — audit software-side, BUKAN Gate F.
> **Freeze line:** firmware SRC `e84798a7243bfb3e11b2398484ae66defdf9c292` (binary; repo main `1742985` hanya docs di atasnya) · PWA main `9e47b4f89f08db23a6eeafe56604255322f357af`.
>
> **Non-klaim eksplisit:** dokumen ini BUKAN bukti hardware acceptance, physical
> OTA, signed tag, ataupun GitHub Release. Semua item fisik tetap PENDING sesuai
> protocol masing-masing. Verdict final (Gate F) tetap wewenang Independent
> Auditor. Dokumen ini adalah input software-side untuk Gate F.

## 1. Metode

Re-run independen seluruh matrix test yang dijalankan CI, pada commit freeze
(lokasi clone bersih, sinkron dengan remote), ditambah: verifikasi rantai rilis
via GitHub API (terautentikasi), probe runtime publik kedua deployment Vercel,
dan pembandingan byte-level deployment push-alarm terhadap repo. Tidak ada
perubahan `firmware/`, `src/`, atau kode GAS dalam proses audit ini — freeze
dipertahankan.

## 2. Matrix hasil — software line @ freeze

| Suite | Re-run lokal 2026-09-06 | Bukti CI |
|---|---|---|
| Firmware `scripts/test_*.py` (25 file) | **25 / 25 PASS** | run #100 (push `e84798a`) SUCCESS · run #101 (PR docs `debeb92`) SUCCESS |
| Secret scan (`firmware/`, `scripts/`, `code.gs/`) | **bersih** (0 temuan) | — |
| GAS harness JS — contract 79 · parity-insights 33 · emergency 57 · data-honesty 38 · authorization 45 · hygiene 46 | **298 / 298 PASS** | harness lokal (GAS tidak punya CI — deploy manual, lihat §6) |
| PWA: ESLint · `tsc --noEmit` · vitest · `next build` | **PASS · PASS · 193/193 · SUCCESS** | CI `ci.yml` hijau; Vercel production build SUCCESS |
| PWA production-config gate (logika, env representatif CI) | **6/6 PASS** | job `Production identity & config gate` PASS di #100 |
| Reproducible build 2× · Ed25519 signing · manifest kanonik · release gate | n/a lokal (butuh PlatformIO) | run #100 SUCCESS — artifact SHA `24e8ae22…424e` |

Catatan: `validate-production-config.mjs --mode production` TANPA env transport
memang FAIL (fail-closed by design) — ini adalah echo lokal dari kondisi P1 #5
(Gate A), bukan defect; dengan env representatif CI gate lulus 6/6.

## 3. Rantai rilis (state per 2026-09-06 ~11:10 UTC)

- Git tag `v1.9.3` → **404** — belum dibuat (benar; milik release authority, urutan runbook).
- GitHub Release `v1.9.3` → **404** — belum publish (benar).
- Branch `release/v1.9.3` = `e84798a` + `df2a5e5` (docs re-cut);
  `git diff e84798a release/v1.9.3 -- firmware/` = **kosong** (sourceOnlyChanges terjaga).
- `docs/hardware-acceptance/v1.9.3.json` dan `docs/ota-physical-test/v1.9.3.json`
  → **belum ada di repo** (menunggu bench — benar; template bukan evidence).
- Artifact otoritatif: Actions run #100 (id 34014592009), `firmware.bin`
  SHA-256 `24e8ae225f53ad48c3cb2e9a0a0ebfc71cff4bbd6f7127a1d74e9425a7be424e`
  — byte-exact terhadap paket bench.

## 4. Probe runtime produksi (tanpa kredensial — HTTP publik)

### 4a. PWA utama — `jmse-plts-monitoring.vercel.app`

- HTTP 200 (Next.js, region hkg1, cache HIT); `sw.js` 200 (56.793 B),
  `manifest.webmanifest` 200, ikon 200, `/install` 200.
- `/api/health`: `nodeEnv=production`; **env transport KOSONG**
  (`mqttBroker/mqttCredentials/gasUrl/jwtSecret` semua `false`,
  `NEXT_PUBLIC_GAS_INSIGHTS_URL` tidak diset) → **P1 #5 / Gate A tetap
  BLOCKING**. Klasifikasi final (Independent Auditor): *bukan software-code
  defect, tetapi release blocker operasional untuk OTA production*.
- Bundle client = main terbaru (marker PARITY-4 + pin `v1.9.3` + resolver OTA
  GitHub ada; tidak ada URL backend/perangkat ter-bake). Kode fail-closed benar.

### 4b. `plts-monitor-push-alarm.vercel.app` — TEMUAN

Deployment hidup (HTTP 200) dengan dua masalah:

1. **`sw.js` STALE** — hash ter-deploy ≠ repo main. Perbaikan keamanan
   **audit-2 K-5** (validasi same-origin URL notifikasi push — anti-phishing
   via payload `data.url`) **belum ter-deploy**; fix sudah ada di repo
   (`9e47b4f`). Murni masalah deployment, bukan kode. Kemungkinan besar
   project ini tidak ter-link ke repo (deploy manual via CLI di masa lalu)
   sehingga push ke main tidak memicu redeploy.
2. **`js/config.js` ter-deploy = placeholder** — `API_BASE` dan
   `VAPID_PUBLIC_KEY` belum diisi nilai riil → fitur push alarm belum aktif
   di produksi (konsisten status fitur: belum diaktifkan).

`manifest.json` dan `css/style.css` identik dengan repo. **Aksi yang
dibutuhkan:** redeploy dari main + isi konfigurasi riil (URL GAS webapp +
VAPID public key) — lihat §5.

## 5. Kredensial Vercel — optimasi account-level TERTUNDA

Token yang diberikan untuk optimasi akun **ditolak Vercel: 403
`invalidToken`** (dikonsistenkan pada 3 endpoint × 2 metode autentikasi;
header terverifikasi terkirim dengan benar). Kemungkinan: token terpotong,
salah salin, atau sudah dihapus/dirotasi. Konsekuensi: semua optimasi
account-level **tertunda** sampai token valid tersedia:

1. Verifikasi & pembenahan env var project (scope Production vs Preview) — Gate A.
2. Set env transport dengan nilai riil (URL tunnel HTTPS / MQTT / GAS) + redeploy.
3. Redeploy push-alarm dari main (menutup temuan stale sw.js K-5).
4. Verifikasi settings project (production branch, root directory, protection off).

Prinsip yang dipegang: **tidak ada nilai env/config palsu yang diset hanya
supaya indikator hijau** (anti-synthetic-evidence — selaras larangan auditor).

## 6. Yang TIDAK diklaim dokumen ini

- Hardware acceptance 12 kriteria (INA219 readback, akurasi arus 1.5/50/90 A,
  PGA 100 A, 120–150 A, hysteresis, tanda arus, pembagi tegangan, V×I, pga_mode) — PENDING.
- Physical OTA 16 kriteria dua fase + rollback/recovery — PENDING.
- Signed tag `v1.9.3` / GitHub Release / provenance binding final — belum dibuat (urutan runbook).
- Deploy manual `code.gs/Code.gs` ke Apps Script + `GEMINI_API_KEY` di sheet
  Config — milik pemilik project, belum dibuktikan (tidak ada CI GAS).
- Verdict Gate F — wewenang Independent Auditor.

## 7. Kesimpulan engineer

Software line v1.9.3 (SRC `e84798a` + PWA `9e47b4f`) **hijau penuh** pada
seluruh matrix yang dapat diverifikasi tanpa perangkat fisik: 25/25 test
Python firmware, 298/298 test harness GAS, 193/193 vitest PWA, lint + typecheck
+ build + gate config PASS, secret scan bersih, rantai rilis konsisten dan
reproducible (CI #100, artifact byte-exact). Satu-satunya temuan sisi
deployment adalah `plts-monitor-push-alarm` (stale `sw.js` + config
placeholder) — perbaikan kodenya sudah berada di main, tidak menyentuh source
bebas-rilis (release line) firmware/PWA, dan tidak mempengaruhi rantai rilis
v1.9.3. Seluruh blocker yang tersisa sesuai struktur gate final audit A–F:
evidence fisik, kredensial, dan otoritas rilis.
