#!/usr/bin/env node
/*
 * prep-deploy-secrets.js - Persiapan artefak rahasia deployment (portabel).
 * =====================================================================
 * 1. Baca pasangan kunci VAPID dari vapid-keys.json (hasil
 *    tools/generate-vapid-keys.js --save) di folder rahasia.
 * 2. Buat token perangkat firmware 32-byte acak (base64url) ->
 *    device-token.txt (dipakai ULANG bila sudah ada, agar token tidak
 *    berubah antar-jalankan).
 * 3. Tulis script-properties.txt - daftar nilai Script Properties GAS
 *    siap salin-tempel.
 *
 * Folder rahasia (urutan prioritas):
 *   1. argumen   --secrets-dir <dir>
 *   2. folder deploy/ paket rilis  : <tools>/../deploy  (punya
 *      vapid-keys.json)  - layout paket MonitorIoT.
 *   3. folder lokal repo deploy     : <tools>/../secrets (di-gitignore).
 *
 * Semua berkas hasil ditulis dengan izin 0600.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

function argValue(names) {
  for (let i = 2; i < process.argv.length; i++) {
    if (names.includes(process.argv[i]) && process.argv[i + 1]) {
      return process.argv[i + 1];
    }
  }
  return null;
}

const TOOLS_DIR = __dirname;

function pickSecretsDir() {
  const arg = argValue(['--secrets-dir']);
  if (arg) return path.resolve(arg);
  const pkg = path.join(TOOLS_DIR, '..', 'deploy');
  if (fs.existsSync(path.join(pkg, 'vapid-keys.json'))) return pkg;
  return path.join(TOOLS_DIR, '..', 'secrets');
}

const DEPLOY_DIR = pickSecretsDir();
const KEYS_PATH = path.join(DEPLOY_DIR, 'vapid-keys.json');
const TOKEN_PATH = path.join(DEPLOY_DIR, 'device-token.txt');
const PROPS_PATH = path.join(DEPLOY_DIR, 'script-properties.txt');

fs.mkdirSync(DEPLOY_DIR, { recursive: true });

/* ---------- 1. Kunci VAPID ---------- */
let keys;
try {
  keys = JSON.parse(fs.readFileSync(KEYS_PATH, 'utf8'));
} catch (e) {
  console.error('GAGAL: tidak dapat membaca ' + KEYS_PATH);
  console.error('Jalankan dulu: node tools/generate-vapid-keys.js --save');
  console.error('lalu pindahkan vapid-keys.json ke ' + DEPLOY_DIR);
  process.exit(1);
}

function b64urlToBuf(s) {
  return Buffer.from(String(s).replace(/-/g, '+').replace(/_/g, '/'), 'base64');
}
function bufToB64url(b) {
  return Buffer.from(b).toString('base64')
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

// Validasi pasangan: turunkan publik dari privat (dengan pad skalar pendek X-8).
let privBuf = b64urlToBuf(keys.privateKey);
if (privBuf.length < 32) {
  privBuf = Buffer.concat([Buffer.alloc(32 - privBuf.length), privBuf]);
}
if (privBuf.length !== 32) {
  console.error('GAGAL: kunci privat bukan skalar P-256 yang valid.');
  process.exit(1);
}
const dh = crypto.createECDH('prime256v1');
dh.setPrivateKey(privBuf);
const derivedPub = bufToB64url(dh.getPublicKey());
if (derivedPub !== keys.publicKey) {
  console.error('GAGAL: publicKey tidak cocok dengan turunan privateKey!');
  process.exit(1);
}
const pubBuf = b64urlToBuf(keys.publicKey);
if (pubBuf.length !== 65 || pubBuf[0] !== 0x04) {
  console.error('GAGAL: publicKey bukan 65 byte berprefiks 0x04.');
  process.exit(1);
}
console.log('[OK] Pasangan kunci VAPID valid (privat -> publik cocok, 65 byte).');

// Hardening: pastikan vapid-keys.json tidak terbuka grup/lainnya.
try {
  const mode = fs.statSync(KEYS_PATH).mode & 0o777;
  if ((mode & 0o077) !== 0) {
    fs.chmodSync(KEYS_PATH, 0o600);
    console.log('[OK] Izin vapid-keys.json diketatkan ke 600 (sebelumnya ' +
      mode.toString(8) + ').');
  }
} catch (e) { /* abaikan bila tidak dapat distat */ }

/* ---------- 2. Token perangkat (idempoten) ---------- */
let token;
if (fs.existsSync(TOKEN_PATH)) {
  token = fs.readFileSync(TOKEN_PATH, 'utf8').trim();
  if (token.length >= 16) {
    console.log('[OK] Token perangkat dipakai ulang dari device-token.txt ' +
      '(tidak diregenerasi).');
  } else {
    console.error('GAGAL: device-token.txt ada tetapi isinya terlalu pendek.');
    process.exit(1);
  }
} else {
  token = bufToB64url(crypto.randomBytes(32));
  fs.writeFileSync(TOKEN_PATH, token + '\n', { mode: 0o600 });
  fs.chmodSync(TOKEN_PATH, 0o600);
  console.log('[OK] Token perangkat baru dibuat (32 byte acak, base64url).');
}
if (token.length < 16) {
  console.error('GAGAL: token terlalu pendek (min 16 karakter).');
  process.exit(1);
}

/* ---------- 3. Script Properties siap-tempel ---------- */
const deviceId = 'esp32-greenhouse-01';
const props = [
  '# =================================================================',
  '# MONITORIOT - NILAI SCRIPT PROPERTIES GAS (siap salin-tempel)',
  '# =================================================================',
  '# Cara pakai (sekali saja):',
  '#   GAS editor -> Project Settings (ikon roda gigi) -> Script Properties',
  '#   -> Add script property -> salin NAMA dan NILAI persis di bawah.',
  '#',
  '# PERINGATAN KEAMANAN:',
  '#   - VAPID_PRIVATE_KEY dan FW_DEVICE_TOKEN adalah RAHASIA.',
  '#   - Jangan di-commit ke git, jangan dibagikan, jangan ditanam di PWA.',
  '#   - File ini dibuat bersama vapid-keys.json pada ' +
    (keys.createdAt || '(tanggal tidak dicatat)') + '.',
  '# =================================================================',
  '',
  'VAPID_PUBLIC_KEY=' + keys.publicKey,
  'VAPID_PRIVATE_KEY=' + keys.privateKey,
  'FW_DEVICE_TOKEN=' + token,
  '',
  '# -----------------------------------------------------------------',
  '# (Opsional) banyak perangkat: hapus FW_DEVICE_TOKEN di atas dan pakai',
  '# daftar berikut (tambahkan satu baris JSON per perangkat):',
  '# FW_DEVICE_TOKENS=[{"deviceId":"' + deviceId + '","token":"' + token + '"}]',
  ''
].join('\n');

fs.writeFileSync(PROPS_PATH, props, { mode: 0o600 });
fs.chmodSync(PROPS_PATH, 0o600);

console.log('[OK] Ditulis: ' + PROPS_PATH + ' (siap tempel).');
console.log('');
console.log('Folder rahasia: ' + DEPLOY_DIR);
console.log('  - vapid-keys.json        : pasangan kunci VAPID (privat!)');
console.log('  - device-token.txt       : token perangkat firmware');
console.log('  - script-properties.txt  : nilai Script Properties GAS');
console.log('');
console.log('PERINGATAN: seluruh folder di atas adalah RAHASIA.');
console.log('Pastikan namanya terdaftar di .gitignore sebelum git add.');
console.log('');
console.log('Device ID bawaan firmware: ' + deviceId +
  ' (ubah di .ino bila perlu).');
