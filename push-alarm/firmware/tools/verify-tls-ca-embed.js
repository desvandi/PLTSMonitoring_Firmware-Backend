#!/usr/bin/env node
/*
 * verify-tls-ca-embed.js - Buktikan PEM yang tertanam di firmware
 * byte-identik dengan root CA resmi pki.goog dan valid menurut openssl.
 *
 * Pemakaian:
 *   node tools/verify-tls-ca-embed.js            # path default repo
 *   node tools/verify-tls-ca-embed.js --ino F --roots F
 *
 * Kode keluar: 0 = kedua root valid & identik, 1 = ada masalah.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const os = require('os');

function argValue(names) {
  for (let i = 2; i < process.argv.length; i++) {
    if (names.includes(process.argv[i]) && process.argv[i + 1]) {
      return process.argv[i + 1];
    }
  }
  return null;
}

const INO = argValue(['--ino']) ||
  path.join(__dirname, '..', 'MonitorIoT_Firmware', 'MonitorIoT_Firmware.ino');
const RAW = argValue(['--roots']) || path.join(__dirname, 'roots.pem');

const ino = fs.readFileSync(INO, 'utf8');
const raw = fs.readFileSync(RAW, 'utf8');
const certs = raw.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g);

function extractPem(name) {
  const m = new RegExp('const char\\* ' + name + ' =([\\s\\S]*?);', '').exec(ino);
  if (!m) throw new Error('konstanta ' + name + ' tidak ditemukan');
  const literals = m[1].match(/"([^"]*)"/g).map(s => s.slice(1, -1));
  // Unescape \n literal dua-karakter menjadi newline PEM sungguhan.
  return literals.join('').replace(/\\n/g, '\n').replace(/\n+$/, '\n');
}

let fail = 0;
for (const [name, idx] of [['GTS_ROOT_R1', 0], ['GTS_ROOT_R4', 3]]) {
  const pem = extractPem(name);
  const tmp = path.join(os.tmpdir(), name + '-' + process.pid + '.pem');
  fs.writeFileSync(tmp, pem);
  const identik = certs[idx].trim() === pem.trim();
  console.log((identik ? '[OK] ' : '[GAGAL] ') + name + ' byte-identik dengan pki.goog (' + pem.length + ' char)');
  if (!identik) fail++;
  try {
    const out = execFileSync('openssl', ['x509', '-in', tmp,
      '-noout', '-subject'], { encoding: 'utf8' }).trim();
    console.log((/GTS Root/.test(out) ? '[OK] ' : '[GAGAL] ') + name + ' openssl valid: ' + out);
    if (!/GTS Root/.test(out)) fail++;
  } catch (e) {
    console.log('[GAGAL] ' + name + ' openssl TIDAK dapat mengurai: ' + e.message.split('\n')[0]);
    fail++;
  } finally {
    try { fs.unlinkSync(tmp); } catch (e) { /* abaikan */ }
  }
}
if (fail) {
  console.log('VERIFIKASI GAGAL - jangan flash sebelum diperbaiki.');
  process.exit(1);
}
console.log('VERIFIKASI LULUS - rantai TLS firmware valid.');
