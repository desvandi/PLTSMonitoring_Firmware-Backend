#!/usr/bin/env bash
# push-all-repos.sh - Dorong repositori MonitorIoT ke remote masing-masing.
# =====================================================================
# Alur: (1) jalankan prepush-audit.js pada SEMUA repo di konfigurasi -
#       push diblokir total bila ada temuan rahasia/higienitas;
#       (2) pasang remote "origin" dan push branch aktif per repo.
#
# Konfigurasi: file repos.conf (satu baris per repo):
#     <path-repo>|<url-remote>
#   - path relatif terhadap lokasi repos.conf (atau absolut).
#   - url dikosongkan -> repo dilewati dengan pesan (belum diisi).
#   Contoh:
#     ../monitoriot-pwa|git@github.com:anda/monitoriot-pwa.git
#     ../monitoriot-gas|https://github.com/anda/monitoriot-gas.git
#
# Pemakaian:
#   ./push-all-repos.sh               # audit lalu push semua
#   ./push-all-repos.sh --dry-run     # tampilkan perintah tanpa eksekusi
#   ./push-all-repos.sh --conf FILE   # konfigurasi lain
set -u

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
CONF="${CONF:-}"
DRY_RUN=0

for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --conf) shift_placeholder=1 ;;
    *) CONF="$arg" ;;
  esac
done
# Dukung bentuk --conf FILE dan FILE langsung.
i=0
for arg in "$@"; do
  i=$((i + 1))
  if [ "$arg" = "--conf" ]; then
    next=$((i + 1))
    # shellcheck disable=SC2124
    CONF="${@:next:1}"
  fi
done

[ -z "$CONF" ] && CONF="$SCRIPTDIR/../repos.conf"
CONF="$(cd "$(dirname "$CONF")" && pwd)/$(basename "$CONF")"

if [ ! -f "$CONF" ]; then
  echo "GAGAL: konfigurasi tidak ditemukan: $CONF"
  echo "Salin repos.conf.example menjadi repos.conf lalu isi URL remote."
  exit 1
fi
echo "=============================================================="
echo " PUSH REPOSITORI MONITORIOT"
echo "=============================================================="
echo "Konfigurasi   : $CONF"
echo "Mode          : $([ $DRY_RUN -eq 1 ] && echo 'dry-run (tanpa eksekusi)' || echo 'nyata')"

# ---- kumpulkan entri ----
declare -a REPO_PATHS=() REPO_URLS=()
while IFS= read -r line || [ -n "$line" ]; do
  line="${line%%#*}"
  line="$(echo "$line" | tr -d '\r' | xargs 2>/dev/null || true)"
  [ -z "$line" ] && continue
  repo_path="${line%%|*}"
  repo_url=""
  case "$line" in
    *"|"*) repo_url="${line#*|}" ;;
  esac
  # resolusi relatif ke lokasi repos.conf
  case "$repo_path" in
    /*) abs_path="$repo_path" ;;
    *)  abs_path="$(dirname "$CONF")/$repo_path" ;;
  esac
  abs_path="$(cd "$abs_path" 2>/dev/null && pwd)" || {
    echo "[LEWAT] path tidak ditemukan: $repo_path"
    continue
  }
  if [ ! -d "$abs_path/.git" ]; then
    echo "[LEWAT] bukan repositori git: $abs_path"
    continue
  fi
  REPO_PATHS+=("$abs_path")
  REPO_URLS+=("$repo_url")
done < "$CONF"

if [ "${#REPO_PATHS[@]}" -eq 0 ]; then
  echo "GAGAL: tidak ada repositori valid di $CONF"
  exit 1
fi
echo "Repo terdaftar: ${#REPO_PATHS[@]}"

# ---- gerbang 1: prepush-audit ----
echo ""
echo "== Gerbang pre-push (rahasia + higienitas) =="
AUDIT_TOOL="$SCRIPTDIR/prepush-audit.js"
if [ ! -f "$AUDIT_TOOL" ]; then
  echo "GAGAL: alat audit tidak ditemukan: $AUDIT_TOOL"
  exit 1
fi
if [ $DRY_RUN -eq 1 ]; then
  echo "[dry-run] akan menjalankan: node $AUDIT_TOOL ${REPO_PATHS[*]}"
else
  if ! node "$AUDIT_TOOL" "${REPO_PATHS[@]}"; then
    echo ""
    echo "PUSH DIBLOKIR: prepush-audit menemukan temuan."
    echo "Perbaiki dulu (lihat rincian di atas), lalu jalankan ulang."
    exit 1
  fi
fi

# ---- gerbang 2: push per repo ----
echo ""
echo "== Push per repositori =="
HASIL_GAGAL=0
TERPUSH=0
for idx in "${!REPO_PATHS[@]}"; do
  repo="${REPO_PATHS[$idx]}"
  url="${REPO_URLS[$idx]}"
  name="$(basename "$repo")"
  echo ""
  echo "-- $name --"
  if [ -z "$url" ]; then
    echo "   [LEWAT] URL remote belum diisi di repos.conf (baris: $name)"
    continue
  fi
  branch="$(git -C "$repo" rev-parse --abbrev-ref HEAD)"
  if git -C "$repo" remote | grep -qx origin; then
    cmd_set="git remote set-url origin $url"
  else
    cmd_set="git remote add origin $url"
  fi
  cmd_push="git push -u origin $branch"
  if [ $DRY_RUN -eq 1 ]; then
    echo "   [dry-run] cd $repo && git $cmd_set && $cmd_push"
    continue
  fi
  if git -C "$repo" remote | grep -qx origin; then
    git -C "$repo" remote set-url origin "$url" || { HASIL_GAGAL=1; continue; }
  else
    git -C "$repo" remote add origin "$url" || { HASIL_GAGAL=1; continue; }
  fi
  if git -C "$repo" push -u origin "$branch"; then
    echo "   [OK] $name terdorong ke $url"
    TERPUSH=$((TERPUSH + 1))
  else
    echo "   [GAGAL] push $name gagal (cek akses/SSH/kredensial)"
    HASIL_GAGAL=1
  fi
done

echo ""
echo "=============================================================="
if [ $DRY_RUN -eq 1 ]; then
  echo " DRY-RUN selesai - tidak ada yang didorong."
  echo "=============================================================="
  exit 0
fi
if [ $HASIL_GAGAL -ne 0 ]; then
  echo " SELESAI DENGAN GALAT: ada push yang gagal ($TERPUSH sukses)."
  echo "=============================================================="
  exit 1
fi
echo " SELESAI: $TERPUSH/${#REPO_PATHS[@]} repositori terdorong."
echo "=============================================================="
exit 0
