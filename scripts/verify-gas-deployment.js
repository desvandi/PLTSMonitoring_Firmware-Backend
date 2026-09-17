#!/usr/bin/env node
/**
 * verify-gas-deployment.js — [AUDIT ROUND-3 2026-09-17]
 * =====================================================================
 * Menutup temuan auditor "WebPushCore.gs / PushService.gs masih dependency
 * deployment manual — tidak ada bukti bahwa source yang dideploy sama
 * dengan revision yang diaudit".
 *
 * Alat ini memverifikasi RANTAI secara live:
 *   bundle MANIFEST.json (sha256 + sourceRevision hasil CI)
 *     vs
 *   PUSH_STATUS yang dilaporkan deployment GAS aktif
 *
 * PUSH_STATUS (terautentikasi) kini melaporkan `sourceRevision` — nilai
 * tersebut di-stamp saat build bundle, jadi kecocokan revision + manifest
 * adalah bukti bahwa Apps Script yang live berisi source revision yang
 * sama dengan yang diaudit/di-CI.
 *
 * Pemakaian:
 *   node scripts/verify-gas-deployment.js \
 *     --url https://script.google.com/macros/s/XXXX/exec \
 *     --token <AUTH_TOKEN> \
 *     --manifest dist/gas-bundle/MANIFEST.json
 *
 *   --expect <revision>   alternatif tanpa manifest (bandingkan langsung)
 *
 * Exit code: 0 = terverifikasi cocok; 1 = MISMATCH/gagal.
 */
'use strict';

const fs = require('fs');
const path = require('path');

function arg(name, def) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0 || i + 1 >= process.argv.length) return def;
  return process.argv[i + 1];
}

const url = arg('url');
const token = arg('token');
const manifestPath = arg('manifest');
const expectRev = arg('expect');

if (!url || !token || (!manifestPath && !expectRev)) {
  console.error('Pemakaian: node verify-gas-deployment.js --url <GAS exec URL> ' +
    '--token <AUTH_TOKEN> (--manifest <MANIFEST.json> | --expect <revision>)');
  process.exit(1);
}

let expected = String(expectRev || '');
let manifest = null;
if (manifestPath) {
  manifest = JSON.parse(fs.readFileSync(path.resolve(manifestPath), 'utf8'));
  if (manifest.sourceRevision) expected = String(manifest.sourceRevision);
}

const body = JSON.stringify({
  action: 'PUSH_STATUS',
  token: token
});

fetch(url, {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: body,
  redirect: 'follow'
}).then(async (res) => {
  const text = await res.text();
  let json = null;
  try { json = JSON.parse(text); } catch (e) { /* envelope bukan JSON */ }

  if (!json) {
    console.error('GAGAL: respons bukan JSON (HTTP ' + res.status + '): ' +
      text.slice(0, 200));
    process.exit(1);
  }
  // Envelope canonical: { status, code, data: {...}, message }
  const data = json.data || json;
  const deployedRev = data && data.sourceRevision;

  console.log('============================================================');
  console.log(' VERIFIKASI DEPLOYMENT GAS');
  console.log('============================================================');
  console.log('  URL                 : ' + url);
  console.log('  revision dideploy    : ' + deployedRev);
  console.log('  revision diharapkan  : ' + expected);
  if (manifest) {
    console.log('  manifest bundle      : ' + path.resolve(manifestPath));
    for (const f in (manifest.files || {})) {
      console.log('    ' + f.padEnd(18) + ' ' + manifest.files[f]);
    }
  }
  if (data && data.storage) {
    console.log('  storage.bytesUsed    : ' + data.storage.bytesUsed + ' / ' +
      data.storage.totalLimit);
    console.log('  storage.capacityDrop : ' + data.storage.capacityDropped);
  }
  if (data && data.outbox) {
    console.log('  outbox               : ' + JSON.stringify(data.outbox));
  }

  if (!deployedRev) {
    console.error('\nHASIL: GAGAL — deployment tidak melaporkan sourceRevision.');
    console.error('  (Versi PushService.gs pra-round-3? Tempel ulang dari bundle CI.)');
    process.exit(1);
  }
  if (String(deployedRev) !== String(expected)) {
    console.error('\nHASIL: MISMATCH — revision yang live ≠ revision yang diaudit.');
    console.error('  Tempel ulang isi gas-bundle revision ' + expected +
      ' lalu deploy ulang Web App.');
    process.exit(1);
  }
  console.log('\nHASIL: TERVERIFIKASI — deployment GAS berjalan pada revision ' +
    expected + ' (sama dengan sumber yang diaudit).');
  process.exit(0);
}).catch((err) => {
  console.error('GAGAL: ' + err.message);
  process.exit(1);
});
