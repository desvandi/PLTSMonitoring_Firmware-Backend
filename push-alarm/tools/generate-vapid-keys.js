#!/usr/bin/env node
/*
 * generate-vapid-keys.js - Generator pasangan kunci VAPID (P-256).
 * ------------------------------------------------------------------
 * Kunci publik  : 65 byte tidak terkompresi (0x04||X||Y), base64url.
 *                 Ditanam di js/config.js (VAPID_PUBLIC_KEY) PWA.
 * Kunci privat  : 32 byte, base64url.
 *                 Disimpan di GAS -> Script Properties (WAJIB RAHASIA).
 *
 * Pemakaian:
 *   node generate-vapid-keys.js            -> cetak ke layar
 *   node generate-vapid-keys.js --save env -> tulis vapid-keys.json
 */
'use strict';

const crypto = require('crypto');

function b64url(buf) {
  return Buffer.from(buf).toString('base64')
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

const ecdh = crypto.createECDH('prime256v1');
ecdh.generateKeys();

// getPrivateKey() membuang byte nol di depan (encoding DER) sehingga
// bisa < 32 byte. Pad ke kiri agar panjang selalu 32 byte - webpush-core
// GAS juga toleran terhadap skalar pendek (temuan X-8 audit silang),
// tetapi keluaran baku 32 byte mencegah kejutan di alat lain.
function pad32(buf) {
  if (buf.length === 32) return buf;
  return Buffer.concat([Buffer.alloc(32 - buf.length), buf]);
}

const publicKey = b64url(ecdh.getPublicKey());          // 65 byte
const privateKey = b64url(pad32(ecdh.getPrivateKey())); // selalu 32 byte

console.log('================ VAPID KEY PAIR (P-256) ================');
console.log('');
console.log('Public Key  (base64url, untuk PWA - js/config.js):');
console.log('  ' + publicKey);
console.log('');
console.log('Private Key (base64url, untuk GAS - Script Properties):');
console.log('  ' + privateKey);
console.log('');
console.log('Penting:');
console.log('  1. Kunci privat TIDAK BOLEH dimasukkan ke kode PWA/git.');
console.log('  2. Simpan di GAS: Project Settings -> Script Properties');
console.log('     -> VAPID_PRIVATE_KEY = <private key di atas>');
console.log('     -> VAPID_PUBLIC_KEY  = <public key di atas>');
console.log('  3. Rotasi kunci: jalankan ulang alat ini, perbarui kedua sisi,');
console.log('     lalu PWA otomatis berlangganan ulang via event');
console.log('     pushsubscriptionchange.');
console.log('========================================================');

if (process.argv.includes('--save')) {
  const fs = require('fs');
  const path = require('path');
  const out = path.join(__dirname, '..', 'vapid-keys.json');
  fs.writeFileSync(out, JSON.stringify({
    publicKey: publicKey,
    privateKey: privateKey,
    createdAt: new Date().toISOString()
  }, null, 2));
  console.log('Tersimpan ke: ' + out);
  console.log('PERINGATAN: vapid-keys.json berisi kunci privat -');
  console.log('jangan di-commit ke repositori (tambahkan ke .gitignore).');
}
