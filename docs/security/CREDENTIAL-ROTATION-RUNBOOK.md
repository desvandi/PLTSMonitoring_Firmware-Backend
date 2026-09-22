# Runbook Rotasi Kredensial — Temuan #19

> Inventaris machine-readable: `docs/security/credential-inventory.json`
> Penegakan anti-fabrikasi: `scripts/check_surface_policy.py` (job CI
> `surface-policy-gate`) — `lastRotated` non-null WAJIB disertai
> `evidenceRef` yang menunjuk file bukti nyata.
> Asal temuan: audit baseline 14-field 2026-09-18, temuan #19 —
> "rotasi kredensial".

## 0. Prinsip yang mengikat

1. **Bukti, bukan klaim.** Setiap rotasi menghasilkan file bukti di
   `docs/security/evidence/C-XX-<tanggal>.json` yang berisi minimal:
   `rotatedAt`, `rotatedBy` (peran manusia), `verification` (hasil
   perintah verifikasi + exit code), dan `scope` (device/fleet/layer).
   Tanpa file itu, inventaris tetap `lastRotated: null` — klaim rotasi
   tanpa bukti = fabrikasi, dan CI menolaknya.
2. **Satu lapis per jendela.** Jangan merotasi MQTT + GAS + VAPID pada
   hari yang sama. Urutan rekomendasi tiap kredensial ada di §2; jika
   dua lapis harus berdekatan, beri jeda minimal 24 jam dan verifikasi
   lapis pertama hijau dulu.
3. **Staging sebelum produksi.** Kredensial build-time (C-03, C-04)
   diverifikasi dulu pada env `staging` (platformio / GitHub
   Environment) sebelum menyentuh `production`.
4. **Rollback dirancang sebelum dipakai.** Setiap prosedur di bawah
   punya bagian "Rollback" — kredensial lama JANGAN dihapus dari
   penyimpanan aman sampai verifikasi lapis baru hijau DAN jendela
   rollback (7 hari) lewat.
5. **Bukan-salah-nama.** `config/pwa-pin.txt` berisi SHA commit yang
   di-pin (penanda versi PWA), BUKAN kredensial — jangan pernah
   dimasukkan ke rotasi. (Rename ke `pwa-commit-pin.txt` opsional;
   tidak ada konsumen kode saat ini.)

## 1. Peta kredensial → lapisan → tooling yang sudah ada

| ID | Kredensial | Penyimpanan saat ini | Tooling repo yang dipakai |
|---|---|---|---|
| C-01 | User/pass operator web device | NVS per-device (AuthManager) | `POST /api/ln` (butuh pass lama) |
| C-02 | Secret JWT per-device | NVS per-device (Core::jwtSecret) | ProvisionHandlers (AP setup) |
| C-03 | Kredensial MQTT broker | GitHub Environment `production` → di-inject saat build | `inject_production_flags.py`, `.env.example` |
| C-04 | Keypair Ed25519 penanda OTA | PEM 0600 lokal operator (`firmware_signing_private.pem`, ter-gitignore) | `sign_firmware.py --gen-keys/--sign/--verify` |
| C-05 | Token fleet + secret per-device GAS | Sheet DEVICES (device_key, secret) + `config.token` di firmware-generic | `make-gas-bundle.sh`, `prep-deploy-secrets.js` |
| C-06 | Keypair VAPID push | Secret deployment GAS push-alarm | `generate-vapid-keys.js` |
| C-07 | Secret env PWA (JWT server, dll.) | Vercel env (saat ini TIDAK terprovision — mode browser-configured) | `live-smoke-test.mjs` utk verifikasi |
| C-08 | Token akses repo (GitHub PAT) | Milik operator/agent | — (prosedur manual GitHub) |
| C-09 | Kunci Gemini API (insights) | Env build/ops + config GAS | `.env.example` (GEMINI_API_KEY) |

## 2. Prosedur per kredensial

### C-01 — Kredensial operator web device (per device)

**Rotate:** (1) Login ke device via PWA (kredensial lama). (2) `POST
/api/ln` dengan password lama + password baru (ExtraHandlers.cpp
menolak tanpa password lama — jangan lewat endpoint lain). (3) Logout,
login ulang dengan kredensial baru. **Verify:** `python3
scripts/device_healthcheck.py --device <ip> --phase login` (fase-2
login, exit 0 hanya bila kredensial baru benar-benar diterima). **Blast
radius:** operator device itu saja. **Rollback:** ulang prosedur dengan
password lama (yang masih diketahui operator). **Cadence:** 180 hari,
atau segera saat ada indikasi kebocoran.

### C-02 — Secret JWT per-device

**Rotate:** jalankan ulang provisioning per-device (ProvisionHandlers,
mode AP WPA2 + CSPRNG; secret baru ditulis ke NVS; AP password
diperlihatkan SEKALI — catat). **Verify:** fase-1 `device_healthcheck.py`
(200 `/api/status` tanpa session lama) + login fase-2. **Blast radius:**
semua session PWA ke device itu gugur (by design). **Rollback:** tidak
ada — secret lama sudah tertimpa; pastikan operator siap login ulang
sebelum rotate. **Cadence:** 365 hari atau saat dicurigai.

### C-03 — Kredensial MQTT broker (fleet, build-time)

**Rotate:** (1) Buat kredensial BARU di broker (jangan hapus yang lama
dulu). (2) Perbarui GitHub Environment `production`:
`MQTT_USERNAME`, `MQTT_PASSWORD` (+ `MQTT_ROOT_CA` bila CA broker
ikut berganti). (3) Build firmware — `inject_production_flags.py`
memvalidasi flag hasil inject dan **gagal build** bila kosong/tidak
valid (fail-closed sudah teruji di pipeline). (4) Flash via OTA
bertanda-tangan, staging dulu. **Verify:** `device_healthcheck.py`
fase-1 (status MQTT per device) + `load_soak_runner.py` rencana kecil
(lihat HIL-RUNNER-QUICKSTART §3) pada env staging. **Blast radius:**
device yang belum re-flash tetap pakai kredensial lama — JANGAN
hapus kredensial lama di broker sampai 100% fleet ter-migrasi
(cek via healthcheck per device). **Rollback:** kembalikan nilai env
GitHub + rebuild; kredensial lama masih hidup di broker. **Cadence:**
90 hari.

### C-04 — Keypair Ed25519 penanda OTA

**Rotate (key ceremony):** (1) `python3 scripts/sign_firmware.py
--gen-keys` di mesin operator TERPUTUS jaringan (PEM 0600, tidak
pernah masuk git — sudah di-gitignore). (2) Simpan private key baru ke
penyimpanan aman (vault/backup fisik). (3) Perbarui
`OTA_ED25519_PUBLIC_KEY_HEX` di GitHub Environment production. (4)
Build + rilis firmware versi baru → device menerima public key baru
melalui build ter-baru; SEBELUM itu, rilis yang sudah berjalan tetap
memverifikasi tanda dengan public key lama. **Batas jujur:** firmware
saat ini membawa SATU public key — rotasi adalah cutover bertahap
(fleet migrasi via rilis berturut), bukan dual-signing. Karena itu:
kunci lama tetap valid untuk device lama sampai fleet penuh ter-update;
jangan hancurkan kunci lama terlalu dini. **Verify:** `sign_firmware.py
--verify` pada biner baru + `verify_ota_evidence.py` pada evidence
rilis; satu device staging menjalani OTA penuh + rollback
(HIL-PROCEDURE bagian K). **Rollback:** firmware ter-build dengan
public key lama tetap ter-release (jangan force-upgrade semua
sekaligus). **Cadence:** 365 hari (key ceremony berat) atau segera
saat private key dicurigai bocor.

### C-05 — Token fleet + secret per-device GAS

**Rotate token fleet:** (1) Buat token baru di Sheet config GAS.
(2) `scripts/make-gas-bundle.sh` + `prep-deploy-secrets.js` → deploy
ulang GAS (deployment ID bisa berganti — perbarui `gasUrl` fleet via
config OTA). (3) Revoke token lama di sheet SETELAH fleet selesai
bermigrasi. **Rotate secret per-device:** baris DEVICES (kolom
`secret`) + `last_nonce` di-reset — firmware-generic membawa token
baru via config. **Verify:** `push-alarm/tools/verify-deployment.js` /
`scripts/verify-gas-deployment.js`; telemetri device mengirim
`(device_key, sequence)` dan GAS menjawab SUCCESS (bukan 401) —
cek log ingest 24 jam. **Blast radius:** seluruh fleet berhenti
ter-authorize jika token lama direvok sebelum migrasi selesai. **Rollback:**
aktifkan kembali token lama di sheet (jika belum dihapus permanen). **Cadence:**
90 hari; segera (revokasi dulu, baru rotasi) saat dicurigai bocor.

### C-06 — Keypair VAPID push

**Rotate:** (1) `node push-alarm/tools/generate-vapid-keys.js`. (2)
Perbarui secret VAPID di deployment GAS push-alarm. (3) Deploy ulang.
**Blast radius (besar, jujur):** SEMUA subscription push browser gugur —
endpoint lama ditandatangani subjek VAPID lama; pengguna harus
re-subscribe (PWA meminta permission ulang saat panel push dibuka).
Rencanakan komunikasi pengguna SEBELUM rotasi. **Verify:** subscribe 1
device uji + `verify-deployment.js`; monitor angka subscription aktif
pasca-rotasi. **Rollback:** kembalikan keypair lama di secret GAS —
subscription yang belum re-subscribe hidup kembali. **Cadence:** 365
hari (blast radius besar — jangan lebih sering tanpa alasan).

### C-07 — Secret env PWA (server-side)

**Status saat ini (2026-09-22): TIDAK terprovision.** Probe live
`/api/health` jujur melaporkan `jwtSecretConfigured=false`, mode
`browser-configured` — tidak ada secret server yang perlu dirotasi
SEBELUM provisioning dilakukan. Ketika provisioning dilakukan:
**Rotate:** perbarui env di Vercel → redeploy. **Verify:**
`node scripts/live-smoke-test.mjs --monitoring <url> …` (assert
`/api/health` + header keamanan). **Blast radius:** session JWT server
gugur semua (pengguna login ulang). **Rollback:** kembalikan env lama
+ redeploy. **Cadence:** 90 hari sejak provisioning.

### C-08 — Token akses repo (GitHub PAT)

**Rotate:** GitHub → Settings → Developer settings → Personal access
tokens: buat token baru (scope minimum `repo`), gunakan untuk push,
lalu revoke token lama. Agent menggunakan identitas commit
"PLTS Prod Engineering (agent)" — token milik operator, bukan milik
repo. **Verify:** `git push --dry-run` + `gh api rate_limit` (header
ter-autentikasi). **Blast radius:** akses push orang/agent yang
memakai token itu. **Rollback:** token baru dibuat ulang bila salah
scope — tidak ada state rusak. **Cadence:** 30–90 hari, selalu
expiry pendek.

### C-09 — Kunci Gemini API (AI insights)

**Rotate:** Google AI Studio → API key baru → perbarui env
`GEMINI_API_KEY` (+ `GAS_INSIGHTS_URL` bila deployment berganti) →
revoke kunci lama. **Verify:** `node scripts/test_gas_parity_insights.js`
/ `test_gas_contract.js` (parity kontrak insights). **Blast radius:**
fitur AI insights mati sampai kunci baru aktif — bukan jalur kontrol. **Rollback:**
kembalikan kunci lama di env. **Cadence:** 90 hari.

## 3. Format file bukti (evidence)

`docs/security/evidence/C-XX-<YYYY-MM-DD>.json`:

```json
{
  "credential": "C-03",
  "rotatedAt": "2026-10-01T02:00:00+07:00",
  "rotatedBy": "operator (nama/peran)",
  "scope": "fleet MQTT, 12 device",
  "verification": {
    "command": "python3 scripts/device_healthcheck.py --device 10.0.0.5 --phase login",
    "exitCode": 0,
    "verdict": "HEALTHY"
  },
  "rollbackWindowUntil": "2026-10-08"
}
```

Lalu perbarui `credential-inventory.json`: `lastRotated` +
`evidenceRef: "docs/security/evidence/C-03-2026-10-01.json"`.
`check_surface_policy.py` menolak `evidenceRef` yang menunjuk file
yang tidak ada — bukti hilang = rotasi dianggap tidak terjadi.

## 4. Urutan rekomendasi jendela rotasi pertama

1. C-08 (token repo) — termurah, tanpa downtime.
2. C-05 (token fleet GAS) — satu jendela migrasi.
3. C-03 (MQTT) — staging dulu, per batch device.
4. C-01/C-02 (per device, menyertai kunjungan fisik/HIL).
5. C-09 → C-07 (saat provisioning PWA) → C-06 (komunikasi pengguna
   dulu) → C-04 (key ceremony).
