# HIL-RUNNER-QUICKSTART — Bench Operator Guide (GATE-9 / GATE-10)

Panduan cepat menjalankan tooling HIL & soak di **bench nyata**.
Prosedur lengkap tetap `HIL-PROCEDURE.md`; dokumen ini menjelaskan
pemakaian tool + urutan sesi yang aman. Terakhir diperbarui: 2026-09-22
(cabang `gate9/hil-tooling-soak`).

## 0. Prinsip yang dipegang tool

- **Verdict tidak pernah di-set PASS oleh tool.** Tool hanya mencatat hasil
  TERUKUR (`PASS`/`FAIL`) dan meninggalkan sisanya `PENDING`. PASS final
  mensyaratkan 22/22 check terukur + signoff 3 peran (testEngineer,
  reviewer, releaseManager) — diverifikasi `verify_gate_qualification.py`.
- **READ-ONLY default.** Semua mutasi (relay ON, config POST, emergency,
  OTA, reboot) terpisah di balik flag `--i-confirm-*`. Tanpa flag → bagian
  SKIPPED, exit tetap jujur.
- **Kredensial hanya via environment** — tidak pernah di argv, file, atau
  log. Jangan menaruh password di command line.
- **Kegagalan precondition = exit 2**, bukan PASS kosong (anti-false-closure).

## 1. Setup sekali (bench)

```bash
export DEVICE_URL=http://192.168.x.x        # URL perangkat bench
export DEVICE_USER=<operator-username>
export DEVICE_PASS=<operator-password>
export HIL_MQTT_HOST=<broker-host>          # untuk bagian J
export HIL_MQTT_PORT=8883
export HIL_MQTT_USERNAME=<viewer/device-user>
export HIL_MQTT_PASSWORD=<password>
# opsional: export HIL_MQTT_CA=/path/ca.pem
pip3 install "paho-mqtt==2.1.0"
```

Sumber identitas build (untuk mengikat bukti ke binary):

```bash
python3 scripts/hil_runner.py --release-json ci-artifacts/modular/release.json \
  --version 1.9.4 --test-engineer "Nama Test Engineer" \
  --hardware-serial <serial-bench> --broker "$HIL_MQTT_HOST"
```

`gitCommit`/`firmwareSha256` bukti otomatis diambil dari `release.json`.

## 2. Urutan sesi yang disarankan

Rencana lengkap: `python3 scripts/hil_runner.py --plan`

| Sesi | Bagian | Perintah inti | Catatan |
|---|---|---|---|
| 1 (read-only) | A, J | `--section A --section J` | Identitas + boot + topic binding; tanpa flag mutasi |
| 2 (actuation) | C, E, F, G, I | `--section C --section E --section F --section G --section I --i-confirm-actuation` | Beban representatif; E-stop fisik dipicu operator |
| 3 (soak) | L | `--section L --soak-min 1440` (24 jam) | `soak24h` hanya diisi pada soak >= 24 jam — di bawah itu tetap PENDING (jujur) |
| 4 (OTA) | K | sesi lanjutan `--i-confirm-ota` | Ikuti HIL-PROCEDURE.md bagian K |
| Fisik murni | B, D, H, M | — (manual) | Sensor fisik, jumper I2C, outage, retensi journal |

Bagian yang TIDAK diotomasi runner: B, D, H, K (manual), M — hasilnya
dicatat operator pada finalisasi (lihat HIL-PROCEDURE.md).

Contoh sesi 1:

```bash
python3 scripts/hil_runner.py --device "$DEVICE_URL" \
  --section A --section J --yes=false \
  --emit-evidence docs/hardware-acceptance/v1.9.4-gate.DRAFT.json
```

Setiap sesi berikutnya menulis ulang DRAFT (bagian yang belum dieksekusi
tetap PENDING — runner tidak menghapus kemajuan sesi sebelumnya secara
diam-diam; verifikasi ulang isi file setelah tiap sesi).

## 3. Load/soak broker (GATE-10 / F4)

```bash
python3 scripts/load_soak_runner.py --plan          # lihat rencana dulu
SOAK_MQTT_HOST=<prod-broker> SOAK_MQTT_USERNAME=<u> SOAK_MQTT_PASSWORD=<p> \
python3 scripts/load_soak_runner.py --devices 10 --duration 3600 \
  --i-confirm-publisher --out soak-run.json --raw-out soak-run-raw.json
```

- Publikasi HANYA ke `plts/SOAK-<runId>-NNN/status` — topik kontrol
  perangkat nyata tidak mungkin tersentuh.
- Budget default: latencyP95 2000 ms, dropRatio 0.001, disconnects 0,
  duplicates 0. Atur via `--latency-p95-budget-ms` dst.
- `--raw-out` menyimpan sampel latensi mentah untuk agregasi multi-chunk
  atau re-budgeting tanpa re-run.
- Catatan jujur: diukur dari satu stasiun; bukan distribusi geografis.

## 4. Finalisasi bukti → rilis

1. Pastikan `v1.9.4-gate.DRAFT.json` 22/22 terukur (tanpa PENDING).
2. Isi signoff 3 peran + salin ke `v1.9.4-gate.json` (hilangkan DRAFT).
3. Validasi lokal: `python3 scripts/verify_gate_qualification.py --version 1.9.4 --release-json <release.json>`.
4. Rilis tetap diblok CI (`verify-hardware-acceptance`, P0-2) sampai
   bukti lengkap + tag-signed — tidak ada jalan pintas.

Pengingat: nilai yang tidak diukur TIDAK BOLEH diisi manual dengan "PASS".
Jika sebuah check tidak bisa diukur hari itu, biarkan PENDING dan
jadwalkan sesi ulang — inilah disiplin anti-false-closure proyek ini.
