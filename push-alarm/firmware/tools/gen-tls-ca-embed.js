#!/usr/bin/env node
/*
 * gen-tls-ca-embed.js - Hasilkan konstanta C (PEM) untuk firmware ESP32
 * dari sertifikat root Google (pki.goog/roots.pem).
 * Keluaran: potongan C siap tempel ke MonitorIoT_Firmware.ino.
 *
 * Pemakaian:
 *   node tools/gen-tls-ca-embed.js                     # roots.pem di samping
 *   node tools/gen-tls-ca-embed.js --roots <file.pem>  # sumber lain
 *   node tools/gen-tls-ca-embed.js --out <file.c>      # keluaran lain
 *
 * Perbarui roots.pem dengan:
 *   curl -fsSL https://pki.goog/roots.pem -o tools/roots.pem
 * lalu jalankan alat ini dan tempel hasilnya ke .ino (ganti blok
 * GTS_ROOT_R1 / GTS_ROOT_R4 lama). Verifikasi dengan
 * tools/verify-tls-ca-embed.js sebelum flash.
 */
'use strict';
const fs = require('fs');
const path = require('path');

function argValue(names) {
  for (let i = 2; i < process.argv.length; i++) {
    if (names.includes(process.argv[i]) && process.argv[i + 1]) {
      return process.argv[i + 1];
    }
  }
  return null;
}

const RAW = argValue(['--roots']) || path.join(__dirname, 'roots.pem');
const OUT = argValue(['--out']) || path.join(__dirname, 'tls-ca-embed.c');

const pem = fs.readFileSync(RAW, 'utf8');
const certs = pem.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g) || [];

// pki.goog/roots.pem urutan tetap: R1, R2, R3, R4 (dipastikan oleh
// tools/verify-tls-ca-embed.js memakai openssl subject).
const pick = { R1: 0, R4: 3 };
const names = { R1: 'GTS_ROOT_R1', R4: 'GTS_ROOT_R4' };

let out = [];
out.push('/* Dihasilkan otomatis oleh tools/gen-tls-ca-embed.js dari');
out.push(' * https://pki.goog/roots.pem (Google Trust Services). Jangan edit manual. */');
for (const key of Object.keys(pick)) {
  const cert = certs[pick[key]];
  if (!cert) { console.error('gagal: cert ' + key + ' tidak ditemukan'); process.exit(1); }
  const lines = cert.replace(/\r/g, '').split('\n').filter(l => l.length > 0);
  out.push('const char* ' + names[key] + ' =');
  for (let i = 0; i < lines.length; i++) {
    out.push('  "' + lines[i] + (i < lines.length - 1 ? '\\n' : '') + '"' + (i < lines.length - 1 ? '' : ';'));
  }
  out.push('');
}
fs.writeFileSync(OUT, out.join('\n') + '\n');
console.log('[OK] Ditulis: ' + OUT);
console.log('Sidik jari SHA-256 DER (untuk dicocokkan manual bila perlu):');
const crypto = require('crypto');
for (const key of Object.keys(pick)) {
  const b64 = certs[pick[key]].replace(/-----(BEGIN|END) CERTIFICATE-----/g, '')
    .replace(/\s+/g, '');
  const der = Buffer.from(b64, 'base64');
  console.log('  ' + names[key] + ': ' +
    crypto.createHash('sha256').update(der).digest('hex'));
}
