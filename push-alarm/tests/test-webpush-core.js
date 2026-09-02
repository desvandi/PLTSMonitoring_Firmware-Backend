#!/usr/bin/env node
/*
 * test-webpush-core.js - Validasi menyeluruh WebPushCore.
 *
 * 1) Vektor uji resmi:
 *    - SHA-256 (FIPS 180): "" dan "abc"
 *    - HMAC-SHA256 (RFC 4231 TC1)
 *    - HKDF (RFC 5869 TC1)
 *    - NIST GCM: key/IV nol, plaintext kosong
 *    - ECDSA P-256 + RFC 6979 (vektor "sample")
 * 2) Uji acak terhadap kripto Node:
 *    - ECDH P-256 (20x)
 *    - publicFromPrivate vs derive Node (10x)
 *    - AES-128-GCM encrypt lalu decrypt Node (20x, panjang acak)
 *    - ECDSA verify via WebCrypto (10x)
 * 3) End-to-end Web Push:
 *    - encryptPayload -> dekripsi manual dengan kunci privat browser
 *    - vapidHeaders -> verifikasi JWT ES256 via WebCrypto
 */
'use strict';

const path = require('path');
const fs = require('fs');
const ROOT = path.join(__dirname, '..');
// Sadar-layout: paket rilis (ROOT/download) atau clone multi-repo (ROOT,
// berisi gas/ langsung). Override: env MONITORIOT_DL.
const DL = process.env.MONITORIOT_DL ||
  (fs.existsSync(path.join(ROOT, 'download', 'gas', 'webpush-core.js'))
    ? path.join(ROOT, 'download') : ROOT);

// Loader sadar-nama: webpush-core.js (paket rilis, bisa di-require) atau
// WebPushCore.gs (repo GAS, konvensi clasp; konten identik, dieval dengan
// shim module agar tetap dapat diekspor).
function loadWebPushCore(gasDir) {
  const jsPath = path.join(gasDir, 'webpush-core.js');
  const gsPath = path.join(gasDir, 'WebPushCore.gs');
  if (fs.existsSync(jsPath)) return require(jsPath);
  if (fs.existsSync(gsPath)) {
    const src = fs.readFileSync(gsPath, 'utf8');
    const mod = { exports: {} };
    // eslint-disable-next-line no-new-func
    new Function('module', 'exports', src)(mod, mod.exports);
    return mod.exports;
  }
  throw new Error('File inti kripto GAS tidak ditemukan (webpush-core.js / WebPushCore.gs).');
}

const WPC = loadWebPushCore(path.join(DL, 'gas'));
const nodeCrypto = require('crypto');
const { webcrypto } = require('crypto');

let pass = 0, fail = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { pass++; console.log('  PASS ' + name); }
  else { fail++; failures.push(name + (detail ? ' :: ' + detail : '')); console.log('  FAIL ' + name + (detail ? ' :: ' + detail : '')); }
}

function hexToBytes(hex) {
  hex = hex.replace(/[^0-9a-f]/gi, '');
  const out = [];
  for (let i = 0; i < hex.length; i += 2) out.push(parseInt(hex.slice(i, i + 2), 16));
  return out;
}

console.log('== 1. Vektor uji SHA-256 ==');
check('sha256("")', WPC.bytesToHex(WPC.sha256([])) ===
  'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
check('sha256("abc")', WPC.bytesToHex(WPC.sha256(WPC.utf8Encode('abc'))) ===
  'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
// blok > 55 byte (2 blok padding)
check('sha256(64 byte pattern)', WPC.bytesToHex(WPC.sha256(new Array(64).fill(0x61))) ===
  nodeCrypto.createHash('sha256').update(Buffer.alloc(64, 0x61)).digest('hex'));

console.log('== 2. Vektor uji HMAC-SHA256 (RFC 4231 TC1) ==');
const hmacExp = 'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7';
check('hmac TC1', WPC.bytesToHex(WPC.hmacSha256(new Array(20).fill(0x0b), WPC.utf8Encode('Hi There'))) === hmacExp);

console.log('== 3. Vektor uji HKDF (RFC 5869 TC1) ==');
{
  const ikm = new Array(22).fill(0x0b);
  const salt = hexToBytes('000102030405060708090a0b0c');
  const info = hexToBytes('f0f1f2f3f4f5f6f7f8f9');
  const okm = WPC.hkdf(salt, ikm, info, 42);
  check('hkdf TC1', WPC.bytesToHex(okm) ===
    '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865');
}

console.log('== 4. Vektor uji AES-GCM (NIST) ==');
{
  const key = new Array(16).fill(0);
  const iv = new Array(12).fill(0);
  const res = WPC.gcmEncrypt(key, iv, []);
  check('gcm empty tag', WPC.bytesToHex(res.tag) === '58e2fccefa7e3061367f1d57a4e7455a',
    WPC.bytesToHex(res.tag));
}

console.log('== 5. Vektor uji ECDSA + RFC 6979 (P-256, "sample") ==');
{
  const priv = hexToBytes('C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721');
  const msg = WPC.utf8Encode('sample');
  const sig = WPC.ecdsaSign(msg, priv);
  const rHex = WPC.bytesToHex(sig.r).replace(/^0+/, '').toLowerCase();
  const sHex = WPC.bytesToHex(sig.s).replace(/^0+/, '').toLowerCase();
  const expR = 'efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716';
  const expS = 'f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8';
  check('ecdsa rfc6979 r', rHex === expR, 'got ' + rHex);
  check('ecdsa rfc6979 s', sHex === expS, 'got ' + sHex);
}

console.log('== 6. ECDH acak vs Node crypto (20x) ==');
{
  let allOk = true;
  for (let i = 0; i < 20; i++) {
    const ecdh = nodeCrypto.createECDH('prime256v1');
    ecdh.generateKeys();
    const peerEcdh = nodeCrypto.createECDH('prime256v1');
    peerEcdh.generateKeys();
    const ourSecret = WPC.ecdhSharedSecret(
      ecdh.getPrivateKey(), peerEcdh.getPublicKey());
    const nodeSecret = ecdh.computeSecret(peerEcdh.getPublicKey());
    if (Buffer.from(ourSecret).toString('hex') !== nodeSecret.toString('hex')) {
      allOk = false;
      break;
    }
  }
  check('ecdh 20x acak', allOk);
}

console.log('== 7. publicKeyFromPrivate vs Node (10x) ==');
{
  let allOk = true;
  for (let i = 0; i < 10; i++) {
    const ecdh = nodeCrypto.createECDH('prime256v1');
    ecdh.generateKeys();
    const pub = WPC.publicKeyFromPrivate(ecdh.getPrivateKey());
    if (Buffer.from(pub).toString('hex') !== ecdh.getPublicKey().toString('hex')) {
      allOk = false;
      break;
    }
  }
  check('publicKeyFromPrivate 10x', allOk);
}

console.log('== 8. AES-128-GCM acak vs Node (20x) ==');
{
  let allOk = true;
  for (let i = 0; i < 20; i++) {
    const key = nodeCrypto.randomBytes(16);
    const iv = nodeCrypto.randomBytes(12);
    const pt = nodeCrypto.randomBytes(1 + Math.floor(Math.random() * 200));
    const res = WPC.gcmEncrypt(Array.from(key), Array.from(iv), Array.from(pt));
    const decipher = nodeCrypto.createDecipheriv('aes-128-gcm', key, iv);
    decipher.setAuthTag(Buffer.from(res.tag));
    const dec = Buffer.concat([decipher.update(Buffer.from(res.ciphertext)), decipher.final()]);
    if (dec.toString('hex') !== pt.toString('hex')) {
      allOk = false;
      console.log('    mismatch pada iterasi', i, 'len', pt.length);
      break;
    }
  }
  check('aes-gcm 20x acak', allOk);
}

console.log('== 9. ECDSA verify via WebCrypto (10x) ==');
async function testVerify() {
  let allOk = true;
  for (let i = 0; i < 10; i++) {
    const kp = await webcrypto.subtle.generateKey(
      { name: 'ECDSA', namedCurve: 'P-256' }, true, ['sign', 'verify']);
    const rawPriv = new Uint8Array(await webcrypto.subtle.exportKey('jwk', kp.privateKey));
    const jwkPriv = await webcrypto.subtle.exportKey('jwk', kp.privateKey);
    const jwkPub = await webcrypto.subtle.exportKey('jwk', kp.publicKey);
    const privBytes = Buffer.from(jwkPriv.d.replace(/-/g, '+').replace(/_/g, '/'), 'base64');
    const pubBytes = Buffer.concat([
      Buffer.from([0x04]),
      Buffer.from(jwkPub.x.replace(/-/g, '+').replace(/_/g, '/'), 'base64'),
      Buffer.from(jwkPub.y.replace(/-/g, '+').replace(/_/g, '/'), 'base64')
    ]);
    const msg = nodeCrypto.randomBytes(32 + i);
    const sig = WPC.ecdsaSign(Array.from(msg), Array.from(privBytes));
    const ok = await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' }, kp.publicKey,
      new Uint8Array(sig.raw), new Uint8Array(msg));
    if (!ok) { allOk = false; console.log('    gagal verifikasi iterasi', i); break; }
    // negative test: ubah 1 byte pesan -> harus gagal
    msg[0] ^= 0xff;
    const okNeg = await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' }, kp.publicKey,
      new Uint8Array(sig.raw), new Uint8Array(msg));
    if (okNeg) { allOk = false; console.log('    negative test gagal (verifikasi palsu!)'); break; }
  }
  check('ecdsa verify+negate 10x', allOk);
}

console.log('== 10. End-to-end: encryptPayload -> dekripsi sisi "browser" ==');
async function testPayload() {
  // Simulasi kunci langganan browser
  const ua = nodeCrypto.createECDH('prime256v1');
  ua.generateKeys();
  const uaPub = ua.getPublicKey();          // 65 byte 0x04||X||Y
  const authSecret = nodeCrypto.randomBytes(16);
  const salt = nodeCrypto.randomBytes(16);
  const asPriv = nodeCrypto.randomBytes(32);

  const subscription = {
    endpoint: 'https://fcm.googleapis.com/fcm/send/dToken123',
    keys: {
      p256dh: WPC.b64urlEncode(Array.from(uaPub)),
      auth: WPC.b64urlEncode(Array.from(authSecret))
    }
  };
  const payload = JSON.stringify({
    id: 'ALM-2026-0829-001', title: 'SUHU KRITIS',
    body: 'Suhu greenhouse 1 mencapai 41.2 C (ambang 38 C)',
    severity: 'critical', url: './index.html?from=push'
  });

  const enc = WPC.encryptPayload(subscription, payload, {
    salt: Array.from(salt), asPrivateKey: Array.from(asPriv)
  });

  // ---- Dekripsi sisi browser (reference implementation Node) ----
  const asPub = Buffer.from(enc.asPublicKey);
  const ecdhSecret = ua.computeSecret(asPub);
  const keyInfo = Buffer.concat([
    Buffer.from('WebPush: info\0'), uaPub, asPub]);
  const ikm = Buffer.from(nodeCrypto.hkdfSync('sha256', ecdhSecret,
    authSecret, keyInfo, 32));
  const cek = Buffer.from(nodeCrypto.hkdfSync('sha256', ikm,
    salt, Buffer.from('Content-Encoding: aes128gcm\0'), 16));
  const nonce = Buffer.from(nodeCrypto.hkdfSync('sha256', ikm,
    salt, Buffer.from('Content-Encoding: nonce\0'), 12));

  const body = Buffer.from(enc.body);
  const bodySalt = body.slice(0, 16);
  const rs = body.readUInt32BE(16);
  // Framing RFC 8188/8291: idlen(1) + keyid(idlen) setelah rs.
  const idlen = body[20];
  const asPubFromBody = body.slice(21, 21 + idlen);
  const ctAndTag = body.slice(21 + idlen);
  check('salt pada body sesuai', bodySalt.equals(salt));
  check('rs = 4096', rs === 4096);
  check('idlen = 65 (kunci efemeral di header keyid)', idlen === 65);
  check('keyid pada body = asPublicKey', asPubFromBody.equals(Buffer.from(enc.asPublicKey)));

  const decipher = nodeCrypto.createDecipheriv('aes-128-gcm', cek, nonce);
  decipher.setAuthTag(ctAndTag.slice(ctAndTag.length - 16));
  const pt = Buffer.concat([
    decipher.update(ctAndTag.slice(0, ctAndTag.length - 16)),
    decipher.final()
  ]);
  check('tag GCM valid (dekripsi sukses)', true);
  const lastByte = pt[pt.length - 1];
  check('delimiter rekord akhir 0x02', lastByte === 0x02);
  const plain = pt.slice(0, pt.length - 1).toString('utf8');
  check('payload utuh', plain === payload);
}

console.log('== 11. VAPID JWT: verifikasi via WebCrypto ==');
async function testVapid() {
  const ecdh = nodeCrypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const privB64 = WPC.b64urlEncode(Array.from(ecdh.getPrivateKey()));
  const pubB64 = WPC.b64urlEncode(Array.from(ecdh.getPublicKey()));

  const hdrs = WPC.vapidHeaders(
    'https://fcm.googleapis.com/fcm/send/dToken123',
    privB64, pubB64, 'mailto:admin@contoh.id', 43200);

  check('aud diambil dari origin endpoint', hdrs.aud === 'https://fcm.googleapis.com');
  const m = /^vapid t=(.+), k=(.+)$/.exec(hdrs.Authorization);
  check('format header Authorization', !!m);
  if (m) {
    const jwt = m[1];
    const pubClaim = m[2];
    check('k = pubkey', pubClaim === pubB64);
    const parts = jwt.split('.');
    check('jwt 3 bagian', parts.length === 3);
    // verifikasi signature
    const keyRaw = await webcrypto.subtle.importKey(
      'raw', new Uint8Array(ecdh.getPublicKey()),
      { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
    const signingInput = new TextEncoder().encode(parts[0] + '.' + parts[1]);
    const sigBytes = WPC.b64urlDecode(parts[2]);
    const ok = await webcrypto.subtle.verify(
      { name: 'ECDSA', hash: 'SHA-256' }, keyRaw,
      new Uint8Array(sigBytes), signingInput);
    check('jwt signature valid', ok);
    const header = JSON.parse(Buffer.from(parts[0], 'base64url').toString());
    const payload = JSON.parse(Buffer.from(parts[1], 'base64url').toString());
    check('header {typ,alg ES256}', header.typ === 'JWT' && header.alg === 'ES256');
    check('payload aud', payload.aud === 'https://fcm.googleapis.com');
    check('payload sub mailto', payload.sub === 'mailto:admin@contoh.id');
    check('payload exp masuk akal', payload.exp > Date.now() / 1000 &&
      payload.exp < Date.now() / 1000 + 43260);
  }
}

console.log('== 12. Skalar privat pendek < 32 byte (temuan X-8) ==');
async function testShortScalar() {
  // Simulasikan kunci privat yang byte nol depannya dibuang ekspor DER:
  // skalar 32 byte dengan byte pertama 0x00 -> versi "pendek" 31 byte.
  const priv32 = Buffer.concat([
    Buffer.from([0x00]),
    nodeCrypto.randomBytes(31)
  ]);
  const short = priv32.slice(1); // 31 byte
  check('persiapan: skalar pendek 31 byte', short.length === 31);

  // 1) publicKeyFromPrivate menerima skalar pendek.
  const pubFromShort = WPC.publicKeyFromPrivate(Array.from(short));
  const pubFromFull = WPC.publicKeyFromPrivate(Array.from(priv32));
  check('publicKeyFromPrivate(31 byte) == (32 byte)',
    Buffer.from(pubFromShort).equals(Buffer.from(pubFromFull)));

  const dh = nodeCrypto.createECDH('prime256v1');
  dh.setPrivateKey(priv32);
  check('cocok dengan turunan Node crypto',
    Buffer.from(pubFromShort).equals(dh.getPublicKey()));

  // 2) ecdsaSign menerima skalar pendek; tanda tangan tetap valid.
  const msg = nodeCrypto.randomBytes(48);
  const sig = WPC.ecdsaSign(Array.from(msg), Array.from(short));
  const keyRaw = await webcrypto.subtle.importKey(
    'raw', new Uint8Array(dh.getPublicKey()),
    { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
  const okSig = await webcrypto.subtle.verify(
    { name: 'ECDSA', hash: 'SHA-256' }, keyRaw,
    new Uint8Array(sig.raw), new Uint8Array(msg));
  check('ecdsaSign(31 byte) menghasilkan tanda tangan valid', okSig);

  // 3) ECDH dengan skalar pendek sama dengan Node.
  const peer = nodeCrypto.createECDH('prime256v1');
  peer.generateKeys();
  const secretShort = WPC.ecdhSharedSecret(
    Array.from(short), Array.from(peer.getPublicKey()));
  check('ecdhSharedSecret(31 byte) == Node',
    Buffer.from(secretShort).equals(dh.computeSecret(peer.getPublicKey())));

  // 4) encryptPayload dengan asPrivateKey pendek tetap terdekripsi.
  const ua = nodeCrypto.createECDH('prime256v1');
  ua.generateKeys();
  const authSecret = nodeCrypto.randomBytes(16);
  const salt = nodeCrypto.randomBytes(16);
  const subscription = {
    endpoint: 'https://fcm.googleapis.com/fcm/send/shortScalar',
    keys: {
      p256dh: WPC.b64urlEncode(Array.from(ua.getPublicKey())),
      auth: WPC.b64urlEncode(Array.from(authSecret))
    }
  };
  const enc = WPC.encryptPayload(subscription, '{"uji":"skalar-pendek"}', {
    salt: Array.from(salt), asPrivateKey: Array.from(short)
  });
  const body = Buffer.from(enc.body);
  const idlen = body[20];
  const asPub = body.slice(21, 21 + idlen);
  const ikm = Buffer.from(nodeCrypto.hkdfSync('sha256',
    ua.computeSecret(asPub), authSecret,
    Buffer.concat([Buffer.from('WebPush: info\0'), ua.getPublicKey(), asPub]), 32));
  const cek = Buffer.from(nodeCrypto.hkdfSync('sha256', ikm, salt,
    Buffer.from('Content-Encoding: aes128gcm\0'), 16));
  const nonce = Buffer.from(nodeCrypto.hkdfSync('sha256', ikm, salt,
    Buffer.from('Content-Encoding: nonce\0'), 12));
  const ctTag = body.slice(21 + idlen);
  const dec = nodeCrypto.createDecipheriv('aes-128-gcm', cek, nonce);
  dec.setAuthTag(ctTag.slice(ctTag.length - 16));
  const pt = Buffer.concat([dec.update(ctTag.slice(0, ctTag.length - 16)), dec.final()]);
  check('encryptPayload(asPriv 31 byte) terdekripsi utuh',
    pt.slice(0, pt.length - 1).toString('utf8') === '{"uji":"skalar-pendek"}');

  // 5) Skalar kosong/terlalu panjang tetap ditolak.
  let tolak = false;
  try { WPC.ecdsaSign(Array.from(msg), []); } catch (e) { tolak = true; }
  check('skalar kosong ditolak', tolak);
}

(async () => {
  try {
    await testVerify();
    await testPayload();
    await testVapid();
    await testShortScalar();
  } catch (e) {
    fail++;
    failures.push('async suite: ' + e.message);
    console.log('  FAIL async suite :: ' + e.stack);
  }
  console.log('\n====================================');
  console.log('TOTAL: ' + pass + ' pass, ' + fail + ' fail');
  if (failures.length) {
    console.log('Failures:');
    failures.forEach((f) => console.log('  - ' + f));
    process.exit(1);
  }
  console.log('SEMUA UJI LULUS');
})();
