#!/usr/bin/env node
/*
 * apply-deploy-config.js - Injektor konfigurasi deployment satu perintah.
 * =====================================================================
 * Melakukan langkah 4 & 5 checklist go-live sekaligus, tanpa edit manual:
 *   - js/config.js : API_BASE            = URL deployment GAS
 *                    VAPID_PUBLIC_KEY    = kunci publik (dari vapid-keys.json)
 *   - sw.js        : konstanta API_BASE  = URL yang sama
 *   - firmware .ino: GAS_URL             = URL yang sama
 *                    DEVICE_TOKEN        = token perangkat
 *                    WIFI_SSID/WIFI_PASS = (opsional via --ssid/--pass)
 *
 * KEAMANAN:
 *   - Pasangan kunci diverifikasi (privat -> publik) SEBELUM ditulis.
 *   - Semua penggantian dihitung dulu di memori; bila SATU pola tidak
 *     cocok tepat satu kali, TIDAK ADA file yang ditulis (atomik).
 *   - Kunci privat TIDAK PERNAH ditanam ke file mana pun di sini.
 *
 * Pemakaian:
 *   node tools/apply-deploy-config.js --url https://script.google.com/macros/s/<ID>/exec [opsi]
 *
 * Opsi:
 *   --url <URL>      (WAJIB) URL deployment Web App GAS
 *   --keys <file>    vapid-keys.json (default: deploy/vapid-keys.json)
 *   --token <t>      token perangkat (default: isi deploy/device-token.txt)
 *   --device-id <id> ID perangkat firmware (default: biarkan nilai .ino)
 *   --ssid / --pass  kredensial WiFi firmware (opsional)
 *   --pwa-dir <dir>  folder PWA (default: <ROOT>/pwa-push-alarm; bila
 *                    tidak ada, coba <ROOT> - layout repo PWA mandiri)
 *   --fw-dir <dir>   folder firmware (default: <ROOT>/firmware)
 *   --out <dir>      tulis salinan lengkap terpasang ke <dir>/ (template asli
 *                    tidak disentuh). Tanpa --out = patch di tempat.
 *
 * Kode keluar: 0 = sukses & terverifikasi, 1 = ditolak/dirolak balik.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..'); // download/ atau root checkout multi-repo

// Auto-deteksi layout: paket (pwa-push-alarm/) atau repo PWA mandiri
// (berkas di root). Override eksplisit lewat --pwa-dir.
const pwaDirArg = argValue(['--pwa-dir']);
const fwDirArg = argValue(['--fw-dir']);
let PWA_DIR = pwaDirArg ? path.resolve(pwaDirArg) : path.join(ROOT, 'pwa-push-alarm');
if (!pwaDirArg && !fs.existsSync(path.join(PWA_DIR, 'js', 'config.js')) &&
    fs.existsSync(path.join(ROOT, 'js', 'config.js'))) {
  PWA_DIR = ROOT;
}
const FW_DIR = fwDirArg ? path.resolve(fwDirArg)
  : (fs.existsSync(path.join(ROOT, 'firmware', 'MonitorIoT_Firmware.ino'))
    ? path.join(ROOT, 'firmware')
    : (fs.existsSync(path.join(ROOT, 'MonitorIoT_Firmware', 'MonitorIoT_Firmware.ino'))
      ? path.join(ROOT, 'MonitorIoT_Firmware')
      : path.join(ROOT, 'firmware')));
const FW_FILE = path.join(FW_DIR, 'MonitorIoT_Firmware.ino');

/* ---------------- argumen ---------------- */
function argValue(names) {
  for (const n of names) {
    const i = process.argv.indexOf(n);
    if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  }
  return null;
}

const url = argValue(['--url']);
const keysPath = argValue(['--keys']) ||
  (fs.existsSync(path.join(ROOT, 'deploy', 'vapid-keys.json'))
    ? path.join(ROOT, 'deploy', 'vapid-keys.json')
    : path.join(ROOT, 'vapid-keys.json'));
const tokenArg = argValue(['--token']);
const tokenFile = path.join(ROOT, 'deploy', 'device-token.txt');
const deviceId = argValue(['--device-id']);
const ssid = argValue(['--ssid']);
const wifiPass = argValue(['--pass']);
const outDir = argValue(['--out']);

let failures = 0;
function ok(msg) { console.log('  [OK]    ' + msg); }
function bad(msg) { failures++; console.log('  [GAGAL] ' + msg); }
function info(msg) { console.log('  [..]    ' + msg); }

function b64urlToBuf(s) {
  return Buffer.from(String(s).replace(/-/g, '+').replace(/_/g, '/'), 'base64');
}
function bufToB64url(b) {
  return Buffer.from(b).toString('base64')
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

console.log('== Terapkan konfigurasi deployment MonitorIoT ==\n');

/* ---------------- 1. Validasi URL ---------------- */
if (!url) {
  bad('Argumen --url wajib: URL deployment Web App GAS.');
  process.exit(1);
}
if (!/^https:\/\/script\.google\.com\/macros\/s\/[A-Za-z0-9_-]+\/exec$/.test(url)) {
  bad('URL bukan format Web App GAS yang valid: ' + url);
  process.exit(1);
}
if (/GANTI|PLACEHOLDER|DEMO/i.test(url)) {
  bad('URL tampak seperti placeholder/demo, bukan deployment nyata: ' + url);
  process.exit(1);
}
ok('Format URL deployment GAS valid: ' + url);

/* ---------------- 2. Validasi pasangan kunci ---------------- */
let keys;
try {
  keys = JSON.parse(fs.readFileSync(keysPath, 'utf8'));
} catch (e) {
  bad('Tidak dapat membaca ' + keysPath + ' - jalankan dulu ' +
    'tools/generate-vapid-keys.js --save');
  process.exit(1);
}
let privBuf = b64urlToBuf(keys.privateKey);
if (privBuf.length === 0 || privBuf.length > 32) {
  bad('Kunci privat tidak valid (' + privBuf.length + ' byte).');
  process.exit(1);
}
if (privBuf.length < 32) {
  info('Skalar privat pendek ' + privBuf.length + ' byte - dipad ke 32 byte (X-8).');
  privBuf = Buffer.concat([Buffer.alloc(32 - privBuf.length), privBuf]);
}
let pubB64;
try {
  const dh = crypto.createECDH('prime256v1');
  dh.setPrivateKey(privBuf);
  pubB64 = bufToB64url(dh.getPublicKey());
} catch (e) {
  bad('Kunci privat bukan skalar P-256 valid: ' + e.message);
  process.exit(1);
}
if (pubB64 !== keys.publicKey) {
  bad('Pasangan kunci pada ' + path.basename(keysPath) +
    ' TIDAK cocok (publicKey != turunan privateKey).');
  process.exit(1);
}
const pubBuf = b64urlToBuf(pubB64);
if (pubBuf.length !== 65 || pubBuf[0] !== 0x04) {
  bad('Kunci publik turunan tidak berformat 65 byte / 0x04.');
  process.exit(1);
}
ok('Pasangan kunci VAPID terverifikasi (privat -> publik cocok).');

/* ---------------- 3. Token perangkat ---------------- */
let token = tokenArg;
if (!token && fs.existsSync(tokenFile)) {
  token = fs.readFileSync(tokenFile, 'utf8').trim();
}
if (!token || token.length < 16) {
  bad('Token perangkat tidak tersedia. Berikan --token <token> atau buat ' +
    'download/deploy/device-token.txt (jalankan scripts/prep-deploy-secrets.js).');
  process.exit(1);
}
ok('Token perangkat siap (' + token.length + ' karakter).');

/* ---------------- 4. Baca & patch di memori ---------------- */
function readSrc(p) { return fs.readFileSync(p, 'utf8'); }

let cfgSrc, swSrc, fwSrc;
try {
  cfgSrc = readSrc(path.join(PWA_DIR, 'js', 'config.js'));
  swSrc = readSrc(path.join(PWA_DIR, 'sw.js'));
  fwSrc = readSrc(FW_FILE);
} catch (e) {
  bad('Tidak dapat membaca berkas sumber: ' + e.message);
  process.exit(1);
}

// replaceExact: pola harus cocok TEPAT satu kali, selain itu gagal total.
const patchLog = [];
function replaceExact(src, regex, replacement, label) {
  const m = src.match(regex);
  if (!m || m.length !== 1) {
    bad('Pola tidak ditemukan / lebih dari satu: ' + label);
    return null;
  }
  patchLog.push(label);
  return src.replace(regex, replacement);
}

let cfgOut = cfgSrc;
let swOut = swSrc;
let fwOut = fwSrc;

// --- js/config.js ---
cfgOut = replaceExact(cfgOut, /API_BASE:\s*'[^']*'/,
  "API_BASE: '" + url + "'", 'config.js API_BASE');
cfgOut = replaceExact(cfgOut, /VAPID_PUBLIC_KEY:\s*'[^']*'/,
  "VAPID_PUBLIC_KEY: '" + pubB64 + "'", 'config.js VAPID_PUBLIC_KEY');

// --- sw.js (konstanta bisa terlipat dua baris: \s* mencakup newline) ---
swOut = replaceExact(swOut, /const API_BASE =\s*'[^']*'/,
  "const API_BASE = '" + url + "'", 'sw.js API_BASE');

// --- firmware .ino ---
fwOut = replaceExact(fwOut, /const char\* GAS_URL =\s*"[^"]*"/,
  'const char* GAS_URL = "' + url + '"', 'firmware GAS_URL');
fwOut = replaceExact(fwOut, /const char\* DEVICE_TOKEN =\s*"[^"]*"/,
  'const char* DEVICE_TOKEN = "' + token + '"', 'firmware DEVICE_TOKEN');
if (deviceId) {
  fwOut = replaceExact(fwOut, /const char\* DEVICE_ID =\s*"[^"]*"/,
    'const char* DEVICE_ID = "' + deviceId + '"', 'firmware DEVICE_ID');
}
if (ssid) {
  fwOut = replaceExact(fwOut, /const char\* WIFI_SSID =\s*"[^"]*"/,
    'const char* WIFI_SSID = "' + ssid + '"', 'firmware WIFI_SSID');
}
if (wifiPass) {
  fwOut = replaceExact(fwOut, /const char\* WIFI_PASS =\s*"[^"]*"/,
    'const char* WIFI_PASS = "' + wifiPass + '"', 'firmware WIFI_PASS');
}

if (failures > 0) {
  console.log('\nAda pola yang gagal - TIDAK ADA file ditulis.');
  process.exit(1);
}

/* ---------------- 5. Tulis hasil ---------------- */
let cfgDst, swDst, fwDst;
if (outDir) {
  const pwaOut = path.join(outDir, 'pwa-push-alarm');
  fs.mkdirSync(pwaOut, { recursive: true });
  fs.cpSync(PWA_DIR, pwaOut, { recursive: true });
  // Arduino IDE mensyaratkan sketch berada di folder bernama sama
  // dengan nama file .ino - build keluaran mengikuti aturan itu.
  fs.mkdirSync(path.join(outDir, 'MonitorIoT_Firmware'), { recursive: true });
  cfgDst = path.join(pwaOut, 'js', 'config.js');
  swDst = path.join(pwaOut, 'sw.js');
  fwDst = path.join(outDir, 'MonitorIoT_Firmware', 'MonitorIoT_Firmware.ino');
  info('Mode salinan: build lengkap ditulis ke ' + outDir +
    ' (template asli tidak disentuh).');
} else {
  cfgDst = path.join(PWA_DIR, 'js', 'config.js');
  swDst = path.join(PWA_DIR, 'sw.js');
  fwDst = FW_FILE;
  info('Mode di-tempat: file paket asli akan diperbarui.');
}
fs.writeFileSync(cfgDst, cfgOut);
fs.writeFileSync(swDst, swOut);
fs.writeFileSync(fwDst, fwOut);
ok(patchLog.length + ' nilai ditanam: ' + patchLog.join(', '));

/* ---------------- 6. Verifikasi pasca-tulis ---------------- */
const cfgCheck = /API_BASE:\s*'([^']+)'/.exec(fs.readFileSync(cfgDst, 'utf8'));
const keyCheck = /VAPID_PUBLIC_KEY:\s*'([^']+)'/.exec(fs.readFileSync(cfgDst, 'utf8'));
const swCheck = /const API_BASE =\s*'([^']+)'/.exec(fs.readFileSync(swDst, 'utf8'));
const fwUrlCheck = /const char\* GAS_URL =\s*"([^"]+)"/.exec(fs.readFileSync(fwDst, 'utf8'));
const fwTokCheck = /const char\* DEVICE_TOKEN =\s*"([^"]+)"/.exec(fs.readFileSync(fwDst, 'utf8'));

if (cfgCheck && cfgCheck[1] === url) ok('config.js API_BASE  == URL deployment');
else bad('config.js API_BASE tidak sesuai pasca-tulis.');

if (keyCheck && keyCheck[1] === pubB64) ok('config.js VAPID_PUBLIC_KEY == kunci publik');
else bad('config.js VAPID_PUBLIC_KEY tidak sesuai pasca-tulis.');

if (swCheck && swCheck[1] === url) ok('sw.js API_BASE == URL deployment (sama dengan config.js)');
else bad('sw.js API_BASE tidak sesuai pasca-tulis.');

if (fwUrlCheck && fwUrlCheck[1] === url) ok('firmware GAS_URL == URL deployment');
else bad('firmware GAS_URL tidak sesuai pasca-tulis.');

if (fwTokCheck && fwTokCheck[1] === token) ok('firmware DEVICE_TOKEN == token');
else bad('firmware DEVICE_TOKEN tidak sesuai pasca-tulis.');

// Placeholder tersisa? (WiFi diperbolehkan - diisi via --ssid/--pass atau IDE)
const fwFinal = fs.readFileSync(fwDst, 'utf8');
if (/GANTI_NAMA_WIFI|GANTI_PASSWORD_WIFI/.test(fwFinal)) {
  info('WiFi firmware masih placeholder - isi lewat --ssid/--pass atau di IDE.');
}
const fwOtherPlaceholders = fwFinal
  .replace(/GANTI_NAMA_WIFI/g, '').replace(/GANTI_PASSWORD_WIFI/g, '');
if (/GANTI/.test(fwOtherPlaceholders)) {
  bad('Firmware masih memuat placeholder GANTI_* lain.');
}

/* ---------------- 7. Hasil ---------------- */
console.log('');
if (failures === 0) {
  console.log('HASIL: konfigurasi diterapkan & terverifikasi.');
  console.log('');
  console.log('Langkah berikutnya:');
  console.log('  1. Pastikan Script Properties GAS sudah diisi ' +
    '(deploy/script-properties.txt).');
  console.log('  2. Jalankan gerbang independen:');
  console.log('     node tools/verify-deployment.js' +
    (outDir
      ? ' --config ' + cfgDst + ' --sw ' + swDst + ' --keys ' + keysPath
      : ' --keys ' + keysPath));
  console.log('  3. Unggah folder PWA ke hosting HTTPS, flash firmware.');
  process.exit(0);
} else {
  console.log('HASIL: ' + failures + ' masalah - periksa keluaran di atas.');
  process.exit(1);
}
