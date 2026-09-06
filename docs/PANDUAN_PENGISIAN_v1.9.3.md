# PANDUAN PENGISIAN EVIDENCE v1.9.3 — SESI BENCH

> **Status arsip repo (2026-09-06):** Salinan resmi untuk arsip rantai rilis dari
> panduan paket sesi bench v1.9.3 yang diserahkan kepada pemilik project. File
> pendamping paket (form cetak `PLTS-v1.9.3-Buku-Lapangan-P0.pdf`, 2 template
> pre-fill JSON, salinan `production-artifact/` dari Actions run #100) dengan
> sengaja TIDAK disimpan di repo ini: template pre-fill hanya boleh masuk repo
> SETELAH terisi dari bench (anti bukti sintetis — lihat §7), dan binary rilis
> resmi disajikan sebagai aset GitHub Release pada Gate D. Sumber template
> resmi di repo: `docs/hardware-acceptance/v1.9.3.template.json` dan
> `docs/ota-physical-test/v1.9.3.template.json`. Struktur gate final audit:
> `docs/FINAL_ACCEPTANCE_GATES_v1.9.3.md`.

> Pendamping (paket bench): `PLTS-v1.9.3-Buku-Lapangan-P0.pdf` (form cetak) + 2 template pre-fill JSON.
> Panduan ini menjelaskan **kapan**, **di mana**, dan **bagaimana** setiap field diisi,
> beserta perintah verifier dan commit ter-pin yang WAJIB diikuti persis.
>
> ⚠️ **Baca dulu `docs/PRODUKSI_ENV_EVIDENCE_v1.9.3.md`** (hasil probe runtime 2026-09-06):
> env transport produksi Vercel belum dikonfigurasi. OTA view bersifat
> operator-only — Phase A/B membutuhkan topologi browser→perangkat (tunnel HTTPS
> + redeploy Vercel, atau PWA lokal di bench). Tanpa itu Phase A macet di login
> operator.

---

## 0. Keadaan awal (sudah dipersiapkan — RE-CUT 2026-09-06)

> **RE-CUT 2026-09-06**: SRC di-pin ulang dari `c2a2c1ca` (pra-parity) ke **`e84798a`**
> (main pasca PARITY-3/4) agar firmware yang dirilis konsisten dengan PWA produksi
> (Vercel, deploy dari main) — tanpa ini, kartu alarm/BMS di PWA akan ditolak
> canonicalizer firmware lama dan temuan CLOSED terbuka ulang saat runtime.
> Branch `release/v1.9.3` sudah dibuat ulang di origin (`e84798a` + commit docs re-cut
> `df2a5e5` — nol perubahan source firmware). Merge PR #24 ke main sudah selesai.

| Item | Nilai | Status |
|---|---|---|
| Branch rilis | `release/v1.9.3` = `e84798a` + docs re-cut `df2a5e5` (binary = `e84798a`) | siap |
| **SRC** (source commit yang diuji) | `e84798a7243bfb3e11b2398484ae66defdf9c292` | — |
| **SRC_TS** (committer timestamp) | `1788673260` | — |
| **firmwareSha256** | `24e8ae225f53ad48c3cb2e9a0a0ebfc71cff4bbd6f7127a1d74e9425a7be424e` | terverifikasi vs artifact |
| Artifact CI produksi | `production-artifact/` (Actions run #100, id 34014592009, push main `e84798a`, `gitDirty=false`) | siap flash |
| PWA produksi (Vercel) | live dari main `9e47b4f` (PARITY-3 + PARITY-4 — kartu alarm editable, requestId, mock fail-closed) | siap Phase A |
| Tag `v1.9.3` | **belum ada** (benar — dibuat di langkah 5) | milik Anda |

**Urutan eksekusi** (sama seperti buku lapangan):

```
0 Pra-sesi (flash + wiring) → 1 P0-01 bench 12 kriteria → 2 P0-02a Phase A (PWA menolak)
→ 3 commit evidence (tanggal ter-pin) → 4 tag -s + push → CI chain → Release publish
→ 5 P0-02b Phase B 16 kriteria + rollback → 6 verifier ota → provenance final
```

## 1. File yang Anda terima

| File | Peran |
|---|---|
| `hardware-acceptance-v1.9.3.prefill.json` | Salin ke repo sebagai `docs/hardware-acceptance/v1.9.3.json`, lalu isi saat bench |
| `ota-physical-test-v1.9.3.prefill.json` | Salin ke repo sebagai `docs/ota-physical-test/v1.9.3.json`, isi bertahap (Phase A dulu, Phase B setelah publish) |
| `PLTS-v1.9.3-Buku-Lapangan-P0.pdf` | Form cetak — tulis nilai pengukuran di kertas, lalu pindahkan ke JSON |
| `production-artifact/` | Binary CI byte-exact: `firmware.bin`, `release.json`, `SHA256SUMS`, dll. |

**Prefill yang SUDAH terisi (jangan diubah kecuali instruksi):**
`gitCommit` = SRC, `firmwareSha256` = SHA artifact, `hardwareIdentity.boardRevision`
= `bench-prototype`, `relayBoardRevision` = `none`, `canonicalReleaseVersion` = `1.9.3`,
`canonicalReleaseFirmwareSha256` = SHA ekspektasi (verifikasi ulang setelah publish),
`canonicalReleaseUrl`.

Kedua template **sudah diuji terhadap verifier asli repo** — semua nilai prefill
diterima (boardRevision release-eligible, SHA match, gitCommit match). Yang gagal saat
ini hanyalah field PENDING yang memang harus diisi dari bench.

## 2. Mapping field — Hardware Acceptance (`v1.9.3.json`)

| Kriteria buku lapangan | Field JSON `observed.*` | Toleransi |
|---|---|---|
| K1 readback boot | `configReadbackHex` = "0x0FFF", `configReadbackPgaBits` = 1 | persis `0x0FFF` |
| K2 arus rendah 1.5A | `lowCurrentPwa`, `lowCurrentRef` | Δ ≤ 0.5 A |
| K3 arus menengah 50A | `midCurrentPwa`, `midCurrentRef` | Δ ≤ 1.0 A |
| K4 pra-transisi 90A | `preTransitionPwa`, `preTransitionRef` | Δ ≤ 1.5 A |
| K5 PGA UP 100A→105A | `transitionUpObserved` = true, `transitionUpPwa`, `transitionUpRef` | log serial terlihat |
| K6 puncak 120A/150A | `peakCurrent120Pwa/Ref`, `peakCurrent150Pwa/Ref` | ≤2.0 A / ≤3.0 A, TIDAK saturasi ~106A |
| K7 PGA DOWN 150A→85A | `transitionDownObserved` = true, `transitionDownPwa/Ref` | log serial terlihat |
| K8 histeresis 95A 30 dtk | `hysteresisChatterCount` = 0 | chatter ≤ 0 (protokol: >2 = gagal) |
| K9 tanda arus | `dischargeSignCorrect`, `chargeSignCorrect` | discharge = negatif |
| K10 pembagi tegangan | `voltagePwa`, `voltageRef`, `voltageDelta`, `adcFineTuneApplied` | Δ ≤ 0.5 V |
| K11 daya V×I | `powerPwa`, `powerExpected`, `powerDeltaPct` | ±5% |
| K12 telemetry pga_mode | `telemetryPgaModeMatchesHardware` = true | PWA + Google Sheet = serial |

- Setiap kriteria → set `checks.<nama>` = `"PASS"` / `"FAIL"` (nama kunci ada di template).
- Setelah 12 kriteria: `verdict` = `"PASS"` (semua lulus) / `"BLOCKED"`.
- Signoff: `testEngineer`, `reviewer`, `releaseManager` (nama), `testDate` (YYYY-MM-DD).
- **Wajib**: `hardwareSerial` = `hardwareIdentity.deviceSerial` = serial asli modul ESP32
  (tertera di shield logam / boot log). Ganti kedua marker `ISI_DI_BENCH_SERIAL_ESP32`
  dengan nilai yang sama persis.

## 3. Mapping field — OTA Physical Test (`v1.9.3.json`)

**Phase A (sebelum publish) — isi dulu:**

| Observasi | Field |
|---|---|
| PWA menolak fetch (v1.9.3 belum dipublish) | `observed.pwaFailClosedPrePublish` = true |
| Badge peringatan latest ≠ authorized tampil | `observed.pwaLatestMismatchWarningShown` = true |

**Phase B (setelah publish):**

| Kriteria | Field utama |
|---|---|
| C1 versi pra-OTA + uptime | `preOtaVersion`, `preOtaUptimeSec`, `deviceStartingVersion` |
| C2/C3 resolusi authorized by tag | `pwaCanonicalVersion` = "1.9.3", `pwaCanonicalSha256` |
| C4 SHA cocok di klien | `pwaCanonicalShaMatchedClient` = true |
| C8 durasi/ukuran upload | `uploadDurationSec`, `uploadBytes` |
| C10/C11 versi + identitas pasca-OTA | `postOtaVersion` = "1.9.3", `postOtaBuildProfile` = "PRODUCTION" |
| C12 relay aman | `relayStateAfterReboot` (mis. "all OFF, ISOLATED") |
| C13 sensor/alarm | `sensorReadingsNormal` = true, `alarmsActiveAfterOta` |
| C14 status terminal ACTIVATED | `otaHistoryEntryStatus` = "success", `deviceLifecycleEvents` (array) |
| C15 tiga sumber konsisten | (checks saja) |
| C16 rollback | `rollbackTestResult` (narasi hasil Opsi A/B) |

Setelah semua: `verdict` = `"PASS"` / `"BLOCKED"`, signoff seperti di atas.
**Kunci check `deviceRunning180` adalah label legacy schema v1.8.0** — nilainya tetap
PASS ketika versi terpasang 1.9.3 (dicatat di `postOtaVersion`). Jangan mengganti nama kunci.

`canonicalReleaseGitCommit` = SHA **commit evidence yang Anda tag** (bukan SRC).
Setelah tag dibuat: `git rev-parse v1.9.3^{}` → salin ke field ini.

## 4. Perintah verifier (jalankan SEBELUM commit)

```bash
cd PLTSMonitoring_Firmware-Backend

# Setelah mengisi hardware acceptance:
python3 scripts/verify_ina219_hardware_acceptance.py \
  --version 1.9.3 \
  --source-commit e84798a7243bfb3e11b2398484ae66defdf9c292 \
  --release-json <path>/production-artifact/release.json \
  --hw-dir docs/hardware-acceptance
# exit 0 = PASS → lanjut ke commit ter-pin

# Setelah publish + mengisi Phase B:
python3 scripts/verify_ota_evidence.py \
  --version 1.9.3 \
  --canonical-release-json <unduhan>/modular-release.json \
  --ota-dir docs/ota-physical-test
# exit 0 = PASS → fleet OTA authorized
```

## 5. Commit evidence dengan tanggal ter-pin (WAJIB — byte-exact REL-03)

Binary menanam `SOURCE_DATE_EPOCH` dari **committer timestamp** HEAD. Agar binary
yang diuji = binary yang dirilis (byte-identical), commit evidence HARUS membawa
timestamp yang sama dengan SRC:

```bash
git fetch origin
git checkout release/v1.9.3       # HEAD = e84798a + df2a5e5 (docs re-cut) — evidence di atas ini
git add docs/hardware-acceptance/v1.9.3.json
GIT_AUTHOR_DATE="@1788673260 +0000" GIT_COMMITTER_DATE="@1788673260 +0000" \
  git commit -m "docs(v1.9.3): hardware acceptance evidence — PASS (bench-prototype, 12/12 criteria)"
```

> Kesalahan umum: commit dengan tanggal "sekarang" → `provenance-binding` melaporkan
> `shaMatches: false` → jaminan byte-exact hilang (mode kegagalan v1.8.0).

## 6. Tag bertanda tangan + publish

```bash
git config user.name  "PLTS Release Manager"
git config user.email "desvandi101@gmail.com"    # WAJIB UID ini (kunci GPG 5E0BB8EF44199645)

EVIDENCE_COMMIT=$(git rev-parse HEAD)            # commit evidence hasil langkah 5
git tag -a v1.9.3 -s -m "Release v1.9.3 — Reproducible build + INA219 canonical chain (REL-03/REL-04 closed)

Signed-off-by: PLTS Release Manager <desvandi101@gmail.com>" "$EVIDENCE_COMMIT"

git push origin v1.9.3
```

Push tag memicu rantai CI lengkap: build-production (sign) → reproducible-build 2x →
release-gate → verify-tag-signature → verify-hardware-acceptance → release-publish.

**Verifikasi publish:**

```bash
curl -s -o /dev/null -w "%{http_code}\n" \
  https://api.github.com/repos/desvandi/PLTSMonitoring_Firmware-Backend/releases/tags/v1.9.3
# harus 200 (sebelumnya 404)
```

Wajib ada di aset rilis: `modular-firmware.bin`, `.sig`, `modular-release.json`,
`modular-manifest-canonical.json`, `hardware-acceptance.json`, `provenance-binding.json`.
`releases/latest` harus menunjuk v1.9.3 (peringatan mismatch di PWA hilang).

**Setelah publish**: verifikasi `modular-release.json`.firmwareSha256 == nilai prefill
(`24e8ae22…`). Jika sama → byte-exact terbukti. Jika beda → JANGAN lanjut Phase B;
periksa apakah commit evidence dibuat dengan tanggal ter-pin (langkah 5).

## 7. Jebakan yang paling sering (dari re-audit)

1. **Bukti sintetis = BLOCK total.** Semua `observed` harus dari alat ukur nyata.
   Kriteria 2 (1.5A), 6 (150A tanpa saturasi), 10 (tegangan) TIDAK opsional.
2. **Tiga identitas harus terikat**: firmware (SRC+SHA), hardware (boardRevision
   `bench-prototype` + serial ESP32 nyata, keduanya identik di `hardwareSerial` dan
   `deviceSerial`), release (tag `v1.9.3` bertanda tangan).
3. **S10/S12 BLOCKED** sebagai basis acceptance — hanya `bench-prototype`.
4. **Kunci check jangan di-rename** — verifier membandingkan nama persis
   (`deviceRunning180` termasuk label legacy).
5. **Phase A sebelum publish, Phase B setelah publish.** Jika Phase A menawarkan
   v1.8.0 untuk di-push → BERHENTI, kebijakan fail-closed rusak.
6. **Hardening baru**: saat menekan Push di PWA, aplikasi me-resolve ulang release
   secara fresh; pesan "Authorized release identity changed since it was fetched"
   adalah guard yang diharapkan (re-fetch lalu review), bukan bug.
7. **Setelah semua selesai**: revoke token GitHub yang dipakai sesi ini (pernah
   tampil plain-text), buat token baru scope minimalis.

## 8. Setelah Phase B — provenance final

```bash
python3 scripts/generate_provenance_binding.py   # mengikat tiga identitas ke hardwareTestedBinary + hardwareAcceptance
```

Lampirkan hasilnya ke rilis (aset `provenance-binding.json` diperbarui) dan
arsipkan buku lapangan terisi + foto pengukuran sebagai lampiran audit.
