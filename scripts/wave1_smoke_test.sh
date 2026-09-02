#!/usr/bin/env bash
# =============================================================================
# wave1_smoke_test.sh — Uji gelombang-1 + gelombang-3 terhadap deployment GAS ASLI
# -----------------------------------------------------------------------------
# Membuktikan pipeline ingest hidup di produksi (bukan di sandbox):
#   1. PING          → 200 PONG            (kontrak PWA /setup §2.4)
#   2. TELEMETRY     → 200 ACCEPTED        (jalur token + sequence — GAS-2-A)
#   3. LATEST        → 200, sequence sama  (read model hidup)
#   4. TELEMETRY HMAC→ 200 ACCEPTED        (envelope v2.1 — GAS-2-B/C; opsional)
#   5. OTA_PUBLISH tanpa admin_token → 401 (gerbang admin Gel-3 — GAS-2-K)
#   6. CALIBRATION_PUBLISH v=0 → 400       (rentang kalibrasi Gel-3 — GAS-2-K)
#   7. CALIBRATION_ACK dummy → 404         (binding device Gel-3 — GAS-2-I)
#
# Seksi 5-6 berfungsi ganda sebagai DETEKTOR VERSI: bila Code.gs di URL ini
# masih versi lama (belum memuat patch Gel-3), seksi tsb GAGAL — ikuti pesan
# peringatannya (deploy ulang Code.gs + bersihkan baris smoke bila terlanjur).
#
# Persyaratan: curl, openssl, python3.
#
# Pemakaian:
#   GAS_URL='https://script.google.com/macros/s/XXXX/exec' \
#   AUTH_TOKEN='plts_sec_ANDA' \
#   DEVICE_KEY='PLTS_MONITOR_01' \
#   ./scripts/wave1_smoke_test.sh
#
# Jalur HMAC (opsional — perlu secret per-device yang terdaftar di sheet
# Devices, kolom `secret`):
#   DEVICE_ID='PLTS_MONITOR_01' DEVICE_SECRET='secret_anda' di-export juga.
#
# CATATAN JUJUR:
#   - Sequence smoke test = epoch detik (unik per eksekusi; firmware asli
#     memakai counter NVS monotonik). Lompatan sequence tercatat sebagai gap
#     yang jujur di ledger — itu memang perilaku yang benar.
#   - Jika sheet Devices Anda sudah berisi perangkat terdaftar, DEVICE_KEY
#     harus salah satu yang terdaftar (gerbang fleet fail-closed).
#   - Uji publish OTA POSITIF tidak ada di skrip ini (menulis manifest asli
#     ke produksi). Jalur positif = panel OTA PWA dengan ADMIN_TOKEN asli.
#   - Skrip ini TIDAK menyimpan kredensial apa pun.
# =============================================================================
set -u

GAS_URL="${GAS_URL:-}"
AUTH_TOKEN="${AUTH_TOKEN:-}"
DEVICE_KEY="${DEVICE_KEY:-PLTS_MONITOR_01}"
DEVICE_ID="${DEVICE_ID:-}"
DEVICE_SECRET="${DEVICE_SECRET:-}"
TIMEOUT=30

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  PASS  $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $1 — $2"; }

for tool in curl openssl python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Butuh: $tool"; exit 2; }
done

if [ -z "$GAS_URL" ] || [ -z "$AUTH_TOKEN" ]; then
  echo "GAGAL: set GAS_URL dan AUTH_TOKEN (lihat header skrip)."
  echo "  contoh: GAS_URL='https://script.google.com/macros/s/.../exec' AUTH_TOKEN='plts_sec_...' $0"
  exit 2
fi

post() {  # post <json-body> → raw response string
  curl -s -m "$TIMEOUT" -X POST \
    -H 'Content-Type: text/plain' \
    --data "$1" "$GAS_URL"
}

field() { # field <json> <py-expr over d> → value
  python3 -c 'import json,sys;d=json.loads(sys.argv[1]);print(eval(sys.argv[2],{"d":d}))' "$1" "$2" 2>/dev/null
}

echo "=== WAVE-1 + WAVE-3 SMOKE TEST vs $GAS_URL ==="

# --- 1) PING ---------------------------------------------------------------
echo "[1] PING (kontrak PWA /setup):"
ping_resp=$(post "{\"action\":\"PING\",\"token\":\"$AUTH_TOKEN\"}")
ping_code=$(field "$ping_resp" 'd["code"]')
ping_msg=$(field "$ping_resp" 'd["message"]')
if [ "$ping_code" = "200" ] && [ "$ping_msg" = "PONG" ]; then
  ok "PING → SUCCESS/PONG"
else
  bad "PING" "code=$ping_code msg=$ping_msg — cek AUTH_TOKEN / deployment /impersonated akses"
  echo "$ping_resp" | head -c 400; echo
fi

# --- 2) TELEMETRY (token + sequence — GAS-2-A) ------------------------------
echo "[2] TELEMETRY (token + sequence, jalur firmware-generic v1.5.0):"
SEQ=$(date +%s)
V=$(python3 -c 'import random;print(round(random.uniform(50.0,52.5),2))')
tel_body=$(python3 - "$AUTH_TOKEN" "$DEVICE_KEY" "$SEQ" "$V" <<'EOF'
import json, sys
token, key, seq, v = sys.argv[1:5]
print(json.dumps({
  "action": "TELEMETRY", "token": token, "device_key": key,
  "data": {"sequence": int(seq), "v_bat": float(v), "i_bat_dc": -2.5,
           "p_bat_dc": -125.0, "i_ac_load": 1.2, "ina219_ok": True,
           "free_heap": 178000, "rssi": -60, "fw_version": "wave1-smoke"}}))
EOF
)
tel_resp=$(post "$tel_body")
tel_code=$(field "$tel_resp" 'd["code"]')
tel_dec=$(field "$tel_resp" 'd["data"]["decision"]' 2>/dev/null)
if [ "$tel_code" = "200" ] && { [ "$tel_dec" = "ACCEPTED" ] || [ "$tel_dec" = "ACCEPTED_LATE" ]; }; then
  ok "TELEMETRY seq=$SEQ → $tel_dec (baris masuk sheet Telemetry)"
else
  bad "TELEMETRY" "code=$tel_code decision=$tel_dec"
  echo "$tel_resp" | head -c 400; echo
  case "$tel_resp" in
    *Unknown\ device_key*) echo "  → DEVICE_KEY '$DEVICE_KEY' belum terdaftar di sheet Devices (gerbang fleet)";;
    *sequence*) echo "  → cek field sequence terkirim";;
  esac
fi

# --- 3) LATEST --------------------------------------------------------------
echo "[3] LATEST (read model):"
lat_resp=$(post "{\"action\":\"LATEST\",\"token\":\"$AUTH_TOKEN\",\"device_key\":\"$DEVICE_KEY\"}")
lat_code=$(field "$lat_resp" 'd["code"]')
lat_seq=$(field "$lat_resp" 'd["data"]["sequence"]' 2>/dev/null)
if [ "$lat_code" = "200" ] && [ "$lat_seq" = "$SEQ" ]; then
  ok "LATEST → sequence $lat_seq (baris barusan terbaca kembali)"
else
  bad "LATEST" "code=$lat_code seq=$lat_seq (harusnya $SEQ)"
  echo "$lat_resp" | head -c 400; echo
fi

# --- 4) TELEMETRY via HMAC (opsional — kontrak v2.1 / GAS-2-B+C) -------------
if [ -n "$DEVICE_ID" ] && [ -n "$DEVICE_SECRET" ]; then
  echo "[4] TELEMETRY via HMAC (envelope v2.1, jalur GasAdvisor):"
  TS=$(date +%s)
  NONCE=$(openssl rand -hex 16)
  DATA_JSON=$(python3 - "$DEVICE_ID" "$TS" <<'EOF'
import json, sys
did, ts = sys.argv[1], sys.argv[2]
print(json.dumps({
  "protocolVersion": 2, "firmwareVersion": "1.6.2-smoke", "deviceId": did,
  "sequence": int(ts) + 1, "timestamp": None, "timeQuality": "UNKNOWN",
  "battery": {"voltage": {"value": 51.1, "quality": "VALID"},
              "current": {"value": -2.0, "quality": "VALID"},
              "soc": {"value": 80.0, "quality": "ESTIMATED",
                      "provenance": "SHUNT"}},
  "health": {"sensorHealth": {"ina219": "ONLINE"}, "freeHeap": 170000,
             "wifiRssi": -59},
  "overallQuality": "VALID"}))
EOF
)
  DIGEST=$(printf '%s' "$DATA_JSON" | openssl dgst -sha256 -hex | awk '{print $NF}')
  CANONICAL=$(printf 'HMAC-SHA256\nTELEMETRY\n%s\n%s\n%s\n%s' "$TS" "$NONCE" "$DEVICE_ID" "$DIGEST")
  SIG=$(printf '%s' "$CANONICAL" | openssl dgst -sha256 -hmac "$DEVICE_SECRET" \
        -hex | awk '{print $NF}')
  HMAC_BODY=$(python3 - "$DATA_JSON" "$TS" "$NONCE" "$DEVICE_ID" "$SIG" <<'EOF'
import json, sys
dj, ts, nonce, did, sig = sys.argv[1:6]
print(json.dumps({"action": "TELEMETRY",
  "auth": {"method": "HMAC-SHA256", "timestamp": int(ts), "nonce": nonce,
           "deviceId": did, "signature": sig},
  "data": dj}))
EOF
)
  hmac_resp=$(post "$HMAC_BODY")
  hmac_code=$(field "$hmac_resp" 'd["code"]')
  hmac_dec=$(field "$hmac_resp" 'd["data"]["decision"]' 2>/dev/null)
  if [ "$hmac_code" = "200" ] && { [ "$hmac_dec" = "ACCEPTED" ] || [ "$hmac_dec" = "ACCEPTED_LATE" ]; }; then
    ok "HMAC TELEMETRY → $hmac_dec (canonical string sinkron)"
  else
    bad "HMAC TELEMETRY" "code=$hmac_code decision=$hmac_dec"
    echo "$hmac_resp" | head -c 400; echo
    case "$hmac_resp" in
      *unknown\ device*) echo "  → DEVICE_ID belum terdaftar di sheet Devices";;
      *signature\ mismatch*) echo "  → DEVICE_SECRET beda dengan sheet Devices kolom secret";;
    esac
  fi
else
  echo "[4] HMAC dilewati (set DEVICE_ID + DEVICE_SECRET untuk menguji jalur v2.1)"
fi

# --- 5) WAVE-3: gerbang admin OTA (GAS-2-K) ----------------------------------
# Aman-mutasi pada Code.gs Gel-3 (ditolak 401 SEBELUM menulis apa pun).
# Pada deployment BASI (pra-Gel-3): manifest inert (URL tak tersedia +
# sha256 nol) ditulis — perangkat akan menolaknya; hapus barisnya di sheet Ota.
echo "[5] OTA_PUBLISH tanpa admin_token (gerbang admin Gel-3):"
ota_resp=$(post "{\"action\":\"OTA_PUBLISH\",\"token\":\"$AUTH_TOKEN\",\"manifest\":{\"version\":\"0.0.0-smoke-probe\",\"url\":\"https://invalid.smoke.test/nowhere.bin\",\"sha256\":\"$(printf '0%.0s' $(seq 1 64))\",\"hmac\":\"$(printf '0%.0s' $(seq 1 64))\"}}")
ota_code=$(field "$ota_resp" 'd["code"]')
ota_msg=$(field "$ota_resp" 'd["message"]')
if [ "$ota_code" = "401" ] && echo "$ota_msg" | grep -qi 'admin_token\|ADMIN_TOKEN'; then
  ok "OTA_PUBLISH tanpa admin_token → 401 (${ota_msg})"
else
  bad "Gerbang admin OTA" "code=$ota_code msg=$ota_msg"
  echo "  → Code.gs di URL ini kemungkinan BELUM memuat patch Gel-3 (masih menerima"
  echo "    publish tanpa admin_token). Deploy ulang Code.gs, lalu hapus baris"
  echo "    '0.0.0-smoke-probe' di sheet Ota bila terlanjur tertulis."
fi

# --- 6) WAVE-3: rentang kalibrasi (GAS-2-K) -----------------------------------
# v_calib=0 harus ditolak 400 SEBELUM baris Calibration ditulis.
echo "[6] CALIBRATION_PUBLISH v_calib=0 (rentang kalibrasi Gel-3):"
cal_resp=$(post "{\"action\":\"CALIBRATION_PUBLISH\",\"token\":\"$AUTH_TOKEN\",\"device_key\":\"$DEVICE_KEY\",\"v_calib\":0,\"i_calib_dc\":1.0,\"i_calib_ac\":1.0}")
cal_code=$(field "$cal_resp" 'd["code"]')
cal_msg=$(field "$cal_resp" 'd["message"]')
if [ "$cal_code" = "400" ] && echo "$cal_msg" | grep -qi 'sane range'; then
  ok "v_calib=0 → 400 di luar rentang wajar (tidak ada baris ditulis)"
else
  bad "Rentang kalibrasi" "code=$cal_code msg=$cal_msg"
  echo "  → Code.gs kemungkinan BELUM patch Gel-3. BILA BARIS v_calib=0 TERLANJUR"
  echo "    MASUK sheet Calibration: HAPUS SEGERA sebelum perangkat mem-poll"
  echo "    (kalibrasi 0 = semua tegangan terbaca 0)."
fi

# --- 7) WAVE-3: pengikatan CALIBRATION_ACK (GAS-2-I) ---------------------------
# command_id dummy + device_key eksplisit → 404 jujur, tidak mengubah apa pun.
echo "[7] CALIBRATION_ACK command_id dummy (binding device Gel-3):"
ack_resp=$(post "{\"action\":\"CALIBRATION_ACK\",\"token\":\"$AUTH_TOKEN\",\"device_key\":\"$DEVICE_KEY\",\"command_id\":\"smoke-dummy-00000000\"}")
ack_code=$(field "$ack_resp" 'd["code"]')
if [ "$ack_code" = "404" ]; then
  ok "ACK command_id dummy → 404 not found (jujur, tanpa mutasi)"
else
  bad "ACK dummy" "code=$ack_code (harusnya 404)"
  echo "$ack_resp" | head -c 300; echo
fi

echo "=== HASIL: $PASS PASS, $FAIL FAIL ==="
[ "$FAIL" -eq 0 ] || exit 1
exit 0
