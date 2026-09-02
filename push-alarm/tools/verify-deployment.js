#!/usr/bin/env node
/*
 * verify-deployment.js - Gerbang verifikasi kontrak K7 (kunci VAPID)
 *                        dan konsistensi URL GAS saat deployment.
 * =====================================================================
 * Menjawab pertanyaan penting sesaat sebelum produksi:
 *   1. Apakah API_BASE di js/config.js SAMA dengan API_BASE di sw.js?
 *   2. Apakah URL sudah format deployment GAS yang benar (bukan placeholder)?
 *   3. Apakah VAPID_PUBLIC_KEY valid (base64url, 65 byte, prefiks 0x04)?
 *   4. Bila kunci privat disertakan: apakah pasangan privat->publik
 *      COCOK dengan kunci publik yang ditanam di PWA?
 *      (ini membuktikan GAS dan PWA memakai pasangan kunci yang sama -
 *       ketidakcocokan inilah penyebab push ditolak 400/403 oleh
 *       push service, padahal langganan berhasil dibuat.)
 *
 * Sumber kunci (salah satu):
 *   --keys    vapid-keys.json  (hasil generate-vapid-keys.js --save)
 *   --private <base64url> [--public <base64url>]
 *   env: VAPID_PRIVATE_KEY / VAPID_PUBLIC_KEY
 *
 * Pemakaian:
 *   node tools/verify-deployment.js                       # cek konfigurasi PWA saja
 *   node tools/verify-deployment.js --keys vapid-keys.json
 *   node tools/verify-deployment.js --private <priv> --public <pub>
 *
 * Auto-deteksi layout (bila --config/--sw tidak diberikan):
 *   - paket monorepo : <ROOT>/pwa-push-alarm/js/config.js
 *   - repo PWA berdiri sendiri: <ROOT>/js/config.js
 *   (ROOT = induk folder tools/)
 *
 * Kode keluar: 0 = siap deploy, 1 = ada masalah (blokir deployment).
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..');

/* ---------- argumen CLI sederhana ---------- */
function argValue(names) {
  for (const n of names) {
    const i = process.argv.indexOf(n);
    if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  }
  return null;
}

/* Auto-deteksi path PWA: dukung layout paket (pwa-push-alarm/) dan
 * layout repo PWA berdiri sendiri (file di root repo). */
function defaultPwaFile(rel) {
  const pkg = path.join(ROOT, 'pwa-push-alarm', rel);
  if (fs.existsSync(pkg)) return pkg;
  const repo = path.join(ROOT, rel);
  if (fs.existsSync(repo)) return repo;
  return pkg; // kembalikan default agar pesan galat tetap jelas
}

const configPath = argValue(['--config']) || defaultPwaFile(path.join('js', 'config.js'));
const swPath = argValue(['--sw']) || defaultPwaFile('sw.js');
const keysPath = argValue(['--keys']);
const pubArg = argValue(['--public']) || process.env.VAPID_PUBLIC_KEY || null;
const privArg = argValue(['--private']) || process.env.VAPID_PRIVATE_KEY || null;
const urlArg = argValue(['--url']);

let failures = 0;
function ok(msg) { console.log('  [OK]    ' + msg); }
function bad(msg) { failures++; console.log('  [GAGAL] ' + msg); }
function info(msg) { console.log('  [..]    ' + msg); }

function b64urlToBuf(s) {
  return Buffer.from(String(s).replace(/-/g, '+').replace(/_/g, '/'), 'base64');
}

/* ---------- 1. Baca konfigurasi PWA ---------- */
console.log('== Verifikasi kesiapan deployment (kontrak K7 Tabel 11) ==\n');

let cfgSrc = '';
try { cfgSrc = fs.readFileSync(configPath, 'utf8'); }
catch (e) { bad('Tidak dapat membaca ' + configPath); process.exit(1); }
let swSrc = '';
try { swSrc = fs.readFileSync(swPath, 'utf8'); }
catch (e) { bad('Tidak dapat membaca ' + swPath); process.exit(1); }

const cfgApi = /API_BASE:\s*'([^']+)'/.exec(cfgSrc);
const swApi = /const API_BASE =\s*'([^']+)'/.exec(swApiSafe(swSrc));
function swApiSafe(src) { return src; } // placeholder agar terbaca alur

const cfgUrl = cfgApi ? cfgApi[1] : null;
const swUrl = (swApi ? swApi[1] : null) || (/(?:const API_BASE =)\s*'([^']+)'/.exec(swSrc) || [])[1];
const cfgKey = /VAPID_PUBLIC_KEY:\s*'([^']+)'/.exec(cfgSrc);
const cfgPub = cfgKey ? cfgKey[1] : null;

/* ---------- 2. Konsistensi URL ---------- */
if (!cfgUrl) bad('API_BASE tidak ditemukan di config.js');
if (!swUrl) bad('API_BASE tidak ditemukan di sw.js');
if (cfgUrl && swUrl) {
  if (cfgUrl === swUrl) ok('API_BASE config.js === API_BASE sw.js');
  else bad('API_BASE BERBEDA! config.js=' + cfgUrl + ' | sw.js=' + swUrl +
    ' -> push tanpa payload & ACK akan mengarah ke server yang salah.');
}
const urlToCheck = urlArg || cfgUrl;
if (urlToCheck) {
  if (/^https:\/\/script\.google\.com\/macros\/s\/[A-Za-z0-9_-]+\/exec$/.test(urlToCheck)) {
    ok('Format URL deployment GAS valid');
  } else {
    bad('URL bukan format Web App GAS (https://script.google.com/macros/s/<ID>/exec): ' + urlToCheck);
  }
  if (/GANTI_DENGAN|AKfycbxGANTI/.test(urlToCheck)) {
    bad('API_BASE masih placeholder - ganti dengan URL deployment Anda.');
  } else {
    ok('API_BASE bukan placeholder');
  }
}

/* ---------- 3. Validitas kunci publik PWA ---------- */
if (!cfgPub) {
  bad('VAPID_PUBLIC_KEY tidak ditemukan di config.js');
} else {
  if (/GANTI_DENGAN/.test(cfgPub)) {
    bad('VAPID_PUBLIC_KEY masih placeholder - ganti dengan kunci dari generate-vapid-keys.js');
  } else {
    const pubBytes = b64urlToBuf(cfgPub);
    if (pubBytes.length === 65 && pubBytes[0] === 0x04) {
      ok('VAPID_PUBLIC_KEY valid: base64url, 65 byte, kurva P-256 tak terkompresi');
    } else {
      bad('VAPID_PUBLIC_KEY tidak valid (harus 65 byte berprefiks 0x04); diperoleh ' +
        pubBytes.length + ' byte.');
    }
  }
}

/* ---------- 4. Pasangan kunci (bila kunci privat tersedia) ---------- */
let privB64 = privArg;
let pubClaim = pubArg;
if (keysPath) {
  try {
    const k = JSON.parse(fs.readFileSync(keysPath, 'utf8'));
    privB64 = privB64 || k.privateKey || null;
    pubClaim = pubClaim || k.publicKey || null;
    info('Membaca pasangan kunci dari ' + keysPath);
  } catch (e) {
    bad('Tidak dapat membaca/mengurai ' + keysPath + ': ' + e.message);
  }
}

if (privB64) {
  let privBuf = b64urlToBuf(privB64);
  if (privBuf.length === 0 || privBuf.length > 32) {
    bad('Kunci privat tidak valid (0 atau > 32 byte); diperoleh ' + privBuf.length + ' byte.');
  } else {
    // Skalar < 32 byte sah: byte nol depan dibuang ekspor DER
    // (temuan X-8 audit silang) -> pad kiri sebelum dipakai.
    if (privBuf.length < 32) {
      info('Kunci privat ' + privBuf.length + ' byte (skalar pendek) - dipad ke 32 byte.');
      privBuf = Buffer.concat([Buffer.alloc(32 - privBuf.length), privBuf]);
    }
    try {
      const dh = crypto.createECDH('prime256v1');
      dh.setPrivateKey(privBuf);
      const derivedPub = dh.getPublicKey();
      const derivedB64 = Buffer.from(derivedPub).toString('base64')
        .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
      if (cfgPub && derivedB64 === cfgPub) {
        ok('Pasangan kunci COCOK: kunci publik PWA diturunkan dengan benar dari kunci privat GAS');
      } else {
        bad('PASANGAN KUNCI TIDAK COCOK! Pemverifikasi VAPID akan menolak push.\n' +
          '         publik di PWA      : ' + cfgPub + '\n' +
          '         turunan dari privat: ' + derivedB64);
      }
      if (pubClaim && pubClaim !== derivedB64) {
        bad('publicKey pada sumber kunci tidak cocok dengan turunan kunci privat.');
      } else if (pubClaim) {
        ok('publicKey pada sumber kunci konsisten dengan kunci privat');
      }
    } catch (e) {
      bad('Kunci privat tidak dapat dipakai (bukan skalar P-256 yang valid): ' + e.message);
    }
  }
} else {
  info('Kunci privat tidak disertakan - lewati uji pasangan kunci ' +
    '(jalankan ulang dengan --keys vapid-keys.json untuk uji penuh).');
}

/* ---------- 5. Pengingat manual ---------- */
console.log('');
info('Pastikan juga (manual): Script Properties GAS berisi VAPID_PUBLIC_KEY & ' +
  'VAPID_PRIVATE_KEY yang sama, dan FW_DEVICE_TOKEN sudah dibuat.');
console.log('\n================================================');
if (failures === 0) {
  console.log(' HASIL: SIAP DEPLOY (kontrak K7 terpenuhi' +
    (privB64 ? ' termasuk uji pasangan kunci' : '') + ')');
  process.exit(0);
} else {
  console.log(' HASIL: ' + failures + ' masalah - PERBAIKI sebelum deployment');
  process.exit(1);
}
