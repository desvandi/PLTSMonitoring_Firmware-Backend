#!/usr/bin/env bash
# make-gas-bundle.sh — [AUDIT ROUND-3 2026-09-17] GAS source bundle yang
# ter-STAMP revision, untuk menutup temuan "deployed GAS source binding".
# =====================================================================
# Rantai bukti yang dibangun:
#   Git commit (GITHUB_SHA)
#     → bundle PushService.gs + Code.gs + WebPushCore.gs
#     → PUSH_SOURCE_REVISION di-stamp = revision build
#     → MANIFEST.json (sha256 per file + sha256 bundle)
#     → artefak CI (immutable, bisa diunduh auditor)
#     → operator menempel isi bundle ke Apps Script
#     → scripts/verify-gas-deployment.js membandingkan revision yang
#       dilaporkan PUSH_STATUS (live) dengan manifest bundle.
#
# Dengan begitu "source-level PASS" dan "deployed-system PASS" terikat pada
# revision yang SAMA dan dapat diverifikasi ulang kapan pun.
#
# Pemakaian:
#   bash scripts/make-gas-bundle.sh [revision] [outdir]
#   revision default: git rev-parse HEAD (pendek)
#   outdir  default: dist/gas-bundle
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

REV="${1:-$(git rev-parse --short HEAD)}"
OUT="${2:-dist/gas-bundle}"
STAMP_PLACEHOLDER="dev-unstamped"

FILES=(
  "code.gs/PushService.gs"
  "code.gs/Code.gs"
  "push-alarm/gas/WebPushCore.gs"
)

for f in "${FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "GAGAL: file sumber GAS tidak ditemukan: $f" >&2
    exit 1
  fi
done

if grep -q "$STAMP_PLACEHOLDER" code.gs/PushService.gs; then
  echo "Stamp bundle dengan revision: $REV"
else
  echo "PERINGATAN: placeholder stamp tidak ditemukan di PushService.gs —"
  echo "bundle akan memakai revision yang sudah tertulis di sumber."
fi

rm -rf "$OUT"
mkdir -p "$OUT"

# Salin + stamp PushService.gs (hanya file yang membawa konstanta revision).
sed "s/$STAMP_PLACEHOLDER/$REV/" code.gs/PushService.gs > "$OUT/PushService.gs"
cp code.gs/Code.gs "$OUT/Code.gs"
cp push-alarm/gas/WebPushCore.gs "$OUT/WebPushCore.gs"

# Validasi sintaksis semua file bundle (node mem-parse grammar Apps Script =
# subset ES yang kompatibel; kegagalan parse = bundle TIDAK boleh rilis).
for f in PushService.gs Code.gs WebPushCore.gs; do
  node -e "
    const vm = require('vm');
    const src = require('fs').readFileSync('$OUT/$f', 'utf8');
    try { new vm.Script(src, { filename: '$f' }); }
    catch (e) { console.error('PARSE ERR $f:', e.message); process.exit(1); }
  "
  echo "  sintaks OK: $f"
done

# Manifest: sha256 per file + total bundle + metadata build.
MANIFEST="$OUT/MANIFEST.json"
{
  echo '{'
  echo "  \"sourceRevision\": \"$REV\","
  echo "  \"builtAt\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo "  \"files\": {"
  FIRST=1
  for f in PushService.gs Code.gs WebPushCore.gs; do
    H=$(sha256sum "$OUT/$f" | cut -d' ' -f1)
    if [ $FIRST -eq 1 ]; then FIRST=0; else echo '    ,'; fi
    echo "    \"$f\": \"$H\""
  done
  echo '  }'
  echo '}'
} > "$MANIFEST"

BUNDLE_SHA=$(cat "$OUT/PushService.gs" "$OUT/Code.gs" "$OUT/WebPushCore.gs" "$MANIFEST" \
  | sha256sum | cut -d' ' -f1)

cat <<SUMMARY

============================================================
 BUNDLE GAS SIAP — revision: $REV
============================================================
  PushService.gs   : $(sha256sum "$OUT/PushService.gs" | cut -d' ' -f1)
  Code.gs          : $(sha256sum "$OUT/Code.gs" | cut -d' ' -f1)
  WebPushCore.gs   : $(sha256sum "$OUT/WebPushCore.gs" | cut -d' ' -f1)
  bundleSha256     : $BUNDLE_SHA
  lokasi           : $OUT

  Langkah deploy + verifikasi:
    1. Buka proyek Apps Script canonical.
    2. Tempel isi ketiga file di atas (TIMPA, jangan tambah file kembar).
    3. Deploy → Web App baru (Execute as: Me, Access: Anyone).
    4. node scripts/verify-gas-deployment.js \\
         --url <URL-deployment> --token <AUTH_TOKEN> --manifest $OUT/MANIFEST.json
============================================================
SUMMARY
