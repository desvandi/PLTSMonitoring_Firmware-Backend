#!/usr/bin/env bash
# run-all.sh - Jalankan 3 suite regresi MonitorIoT terhadap checkout repo.
# =====================================================================
# Layout default (2 repositori GitHub, di-clone bersebelahan):
#   <induk>/plts_monitor_PWA_only                -> pwa-push-alarm/
#   <induk>/plts_monitor_firmware-code.gs-etc    -> push-alarm/ (repo ini:
#       gas/ + firmware/ + tests/ + tools/)
#
# Skrip membuat <induk>/monitoriot-checkout/ berisi tautan simbolik dengan
# nama yang diharapkan harness, lalu menjalankan:
#   tests/test-webpush-core.js   (35 asersi kripto)
#   tests/smoke-test-pwa.js      (17 asersi PWA; butuh Playwright+Chromium)
#   tests/cross-audit-test.js    (151 asersi kontrak Tabel 11 K1-K8)
#
# Layout lama (4 repo monitoriot-*) tetap didukung lewat auto-deteksi.
# Override: MONITORIOT_CHECKOUT, MONITORIOT_PWA_DIR, MONITORIOT_GAS_DIR,
# MONITORIOT_FW_DIR.
set -u

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
ALARM_DIR="$(cd "$SCRIPTDIR/.." && pwd)"     # .../push-alarm
REPO_ROOT="$(cd "$ALARM_DIR/.." && pwd)"     # repo firmware-code.gs-etc
BASE="$(cd "$REPO_ROOT/.." && pwd)"          # folder induk (kedua clone)

# --- Sumber PWA: layout 2-repo, fallback layout 4-repo lama ---
PWA_DIR="${MONITORIOT_PWA_DIR:-}"
if [ -z "$PWA_DIR" ]; then
  if [ -d "$BASE/plts_monitor_PWA_only/pwa-push-alarm" ]; then
    PWA_DIR="$BASE/plts_monitor_PWA_only/pwa-push-alarm"
  elif [ -d "$BASE/monitoriot-pwa" ]; then
    PWA_DIR="$BASE/monitoriot-pwa"
  else
    PWA_DIR="$BASE/plts_monitor_PWA_only/pwa-push-alarm"
  fi
fi

GAS_DIR="${MONITORIOT_GAS_DIR:-$ALARM_DIR/gas}"

# --- Sumber firmware: sketch di firmware/MonitorIoT_Firmware/ ---
FW_DIR="${MONITORIOT_FW_DIR:-$ALARM_DIR/firmware/MonitorIoT_Firmware}"

CHECKOUT="${MONITORIOT_CHECKOUT:-$BASE/monitoriot-checkout}"

for d in "$PWA_DIR" "$GAS_DIR" "$FW_DIR"; do
  if [ ! -d "$d" ]; then
    echo "GAGAL: komponen tidak ditemukan: $d"
    echo "Clone plts_monitor_PWA_only bersebelahan dengan repo ini,"
    echo "atau set MONITORIOT_PWA_DIR / MONITORIOT_GAS_DIR / MONITORIOT_FW_DIR."
    exit 1
  fi
done

# Cek Playwright (hanya dibutuhkan suite smoke).
cd "$ALARM_DIR"
if ! node -e "require.resolve('playwright')" >/dev/null 2>&1; then
  echo "GAGAL: pustaka playwright tidak ditemukan di $ALARM_DIR."
  echo "Jalankan sekali:  npm i -D playwright && npx playwright install chromium"
  exit 1
fi

# Bangun checkout berisi tautan simbolik sesuai nama yang dibaca harness.
rm -rf "$CHECKOUT"
mkdir -p "$CHECKOUT"
ln -s "$PWA_DIR" "$CHECKOUT/pwa-push-alarm"
ln -s "$GAS_DIR" "$CHECKOUT/gas"
ln -s "$FW_DIR" "$CHECKOUT/MonitorIoT_Firmware"

cleanup() { rm -rf "$CHECKOUT"; }
trap cleanup EXIT

echo "=============================================================="
echo " SUITE REGRESI MONITORIOT (201 asersi)"
echo " PWA : $PWA_DIR"
echo " GAS : $GAS_DIR"
echo " FW  : $FW_DIR"
echo "=============================================================="
GAGAL=0

echo ""
echo "== [1/3] Kripto Web Push (test-webpush-core.js) =="
if MONITORIOT_DL="$CHECKOUT" node "$SCRIPTDIR/test-webpush-core.js"; then
  echo "-> [1/3] LULUS"
else
  echo "-> [1/3] GAGAL"; GAGAL=1
fi

echo ""
echo "== [2/3] Uji asap PWA (smoke-test-pwa.js) =="
if MONITORIOT_PWA="$CHECKOUT/pwa-push-alarm" node "$SCRIPTDIR/smoke-test-pwa.js"; then
  echo "-> [2/3] LULUS"
else
  echo "-> [2/3] GAGAL"; GAGAL=1
fi

echo ""
echo "== [3/3] Audit silang kontrak (cross-audit-test.js) =="
if MONITORIOT_DL="$CHECKOUT" node "$SCRIPTDIR/cross-audit-test.js"; then
  echo "-> [3/3] LULUS"
else
  echo "-> [3/3] GAGAL"; GAGAL=1
fi

echo ""
echo "=============================================================="
if [ $GAGAL -ne 0 ]; then
  echo " HASIL: ADA SUITE YANG GAGAL - jangan deploy sebelum hijau penuh."
  echo "=============================================================="
  exit 1
fi
echo " HASIL: 3/3 SUITE LULUS (35 + 17 + 151 asersi)."
echo "=============================================================="
exit 0
