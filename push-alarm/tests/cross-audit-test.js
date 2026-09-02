#!/usr/bin/env node
/*
 * cross-audit-test.js - Harness audit silang PWA-GAS-FIRMWARE.
 * =====================================================================
 * Memverifikasi 7 kontrak Tabel 11 (matriks kesiapan audit silang)
 * secara mekanis dengan menjalankan KODE ASLI ketiga komponen:
 *
 *   - PWA   : js/push-manager.js + sw.js dimuat di vm dengan mock
 *             browser (navigator, pushManager, Notification, localStorage).
 *   - GAS   : gas/PushService.gs (atau Code.gs) + gas/webpush-core.js
 *             dimuat di vm dengan mock PropertiesService/UrlFetchApp/
 *             ContentService/Utilities (runtime Apps Script).
 *   - FIRMWARE : skema laporan direplikasi persis dari
 *             firmware/MonitorIoT_Firmware.ino (+ pemeriksaan statis
 *             bahwa .ino mengirim field kontrak & tidak punya jalur push).
 *
 * Push service mock menerima body terenkripsi dari GAS lalu
 * MENDEKRIPSI seperti browser nyata (RFC 8188/8291) memakai kunci
 * privat langganan - sehingga jalur GAS -> push service -> SW PWA
 * diverifikasi ujung-ke-ujung, bukan sekadar simulasi.
 *
 * Pemakaian:  node scripts/cross-audit-test.js
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

const ROOT = path.join(__dirname, '..');
// Sadar-layout: paket rilis (ROOT/download) atau clone multi-repo (ROOT,
// berisi pwa-push-alarm/, gas/, firmware/ langsung).
// Override eksplisit: env MONITORIOT_DL.
const DL = process.env.MONITORIOT_DL ||
  (fs.existsSync(path.join(ROOT, 'download', 'gas', 'PushService.gs'))
    ? path.join(ROOT, 'download') : ROOT);
const PWA = path.join(DL, 'pwa-push-alarm');
const GAS = path.join(DL, 'gas');
const FW_INO = fs.existsSync(path.join(DL, 'firmware', 'MonitorIoT_Firmware.ino'))
  ? path.join(DL, 'firmware', 'MonitorIoT_Firmware.ino')
  : path.join(DL, 'MonitorIoT_Firmware', 'MonitorIoT_Firmware.ino');

const GAS_URL = 'https://script.google.com/macros/s/AUDITSILANG123456/exec';
const APP_ORIGIN = 'https://app.local';
const DEVICE_TOKEN = 'token-uji-audit-silang-2026';
const DEVICE_ID = 'esp32-greenhouse-01';

/* ======================= Util & pencatat ======================= */

const tally = {};   // kontrak -> {pass, fail}
let totalPass = 0, totalFail = 0;

function check(contract, name, cond) {
  if (!tally[contract]) tally[contract] = { pass: 0, fail: 0 };
  if (cond) { tally[contract].pass++; totalPass++; console.log('  PASS [' + contract + '] ' + name); }
  else { tally[contract].fail++; totalFail++; console.log('  FAIL [' + contract + '] ' + name); }
}

function b64url(buf) {
  return Buffer.from(buf).toString('base64')
    .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function b64urlToBuf(s) {
  return Buffer.from(String(s).replace(/-/g, '+').replace(/_/g, '/'), 'base64');
}

/* ======================= Kunci VAPID uji ======================= */

const vapidEcdh = crypto.createECDH('prime256v1');
vapidEcdh.generateKeys();
const VAPID_PUB = b64url(vapidEcdh.getPublicKey());   // 65 byte
const VAPID_PRIV = b64url(vapidEcdh.getPrivateKey()); // 32 byte

/* ======================= Mock runtime GAS ======================= */

const gasProps = new Map();
const PropertiesService = {
  getScriptProperties() {
    return {
      getProperty: (k) => (gasProps.has(k) ? gasProps.get(k) : null),
      setProperty: (k, v) => { gasProps.set(k, String(v)); },
      deleteProperty: (k) => { gasProps.delete(k); }
    };
  }
};

const endpointRegistry = {}; // endpoint -> {priv,pub,auth} (kunci "browser")

const pushService = {
  requests: [],          // {url, headers, bodyBytes, payloadIsBlob}
  nextStatus: 201,
  strictEndpoints: false,
  setNextStatus(s) { this.nextStatus = s; },
  fetch(url, options) {
    options = options || {};
    const p = options.payload;
    let bodyBytes = [];
    const payloadIsBlob = !!(p && p.__blob);
    if (payloadIsBlob) bodyBytes = p.bytes.slice();
    else if (Array.isArray(p)) bodyBytes = p.slice();
    this.requests.push({
      url,
      headers: options.headers || {},
      bodyBytes,
      payloadIsBlob,
      method: options.method || 'get'
    });
    let status = this.nextStatus;
    this.nextStatus = 201;
    // Endpoint yang tidak dikenal browser = langganan mati -> 410.
    if (this.strictEndpoints && !endpointRegistry[url] && status === 201) status = 410;
    return {
      getResponseCode: () => status,
      getContentText: () => (status < 300 ? '' : 'error')
    };
  }
};

const gasCtx = vm.createContext({
  PropertiesService,
  UrlFetchApp: { fetch: (u, o) => pushService.fetch(u, o) },
  ContentService: {
    MimeType: { JSON: 'JSON' },
    createTextOutput(t) { return { _text: t, setMimeType() { return this; } }; }
  },
  Utilities: {
    getUuid: () => crypto.randomUUID(),
    newBlob: (bytes, contentType) => ({
      __blob: true, bytes: Array.from(bytes || []), contentType
    })
  },
  Logger: { log() {} },
  SpreadsheetApp: {
    getActiveSpreadsheet() { throw new Error('Sheet tidak dipakai dalam audit silang'); }
  },
  console: { log() {} }
});
// Nama file inti kripto: webpush-core.js (paket rilis) atau WebPushCore.gs
// (repo GAS, konvensi clasp) - keduanya konten identik.
const GAS_CORE_FILE = fs.existsSync(path.join(GAS, 'webpush-core.js'))
  ? 'webpush-core.js'
  : (fs.existsSync(path.join(GAS, 'WebPushCore.gs')) ? 'WebPushCore.gs' : null);
if (!GAS_CORE_FILE) {
  throw new Error('File inti kripto GAS tidak ditemukan (webpush-core.js / WebPushCore.gs).');
}
// Nama file utama GAS: PushService.gs (paket rilis / repo monitoriot-gas)
// atau Code.gs (konvensi Apps Script di repo firmware-code.gs) - konten
// identik, hanya nama file yang berbeda.
const GAS_MAIN_FILE = fs.existsSync(path.join(GAS, 'PushService.gs'))
  ? 'PushService.gs'
  : (fs.existsSync(path.join(GAS, 'Code.gs')) ? 'Code.gs' : null);
if (!GAS_MAIN_FILE) {
  throw new Error('File utama GAS tidak ditemukan (PushService.gs / Code.gs).');
}
vm.runInContext(fs.readFileSync(path.join(GAS, GAS_CORE_FILE), 'utf8'),
  gasCtx, { filename: GAS_CORE_FILE });
vm.runInContext(fs.readFileSync(path.join(GAS, GAS_MAIN_FILE), 'utf8'),
  gasCtx, { filename: GAS_MAIN_FILE });
gasCtx.PUSH_CONFIG.USE_SHEET_STORAGE = false; // uji memakai penyimpanan Properties
gasProps.set('VAPID_PUBLIC_KEY', VAPID_PUB);
gasProps.set('VAPID_PRIVATE_KEY', VAPID_PRIV);
gasProps.set('FW_DEVICE_TOKEN', DEVICE_TOKEN);

function gasPost(obj) {
  const out = gasCtx.doPost({ postData: { contents: JSON.stringify(obj) } });
  return JSON.parse(out._text);
}
function gasGet(action) {
  const out = gasCtx.doGet({ parameter: { action: action } });
  return JSON.parse(out._text);
}
function storedSubs() { return JSON.parse(gasProps.get('PUSH_SUBSCRIPTIONS') || '[]'); }
function alarmLog() { return JSON.parse(gasProps.get('ALARM_LOG') || '[]'); }

/** Dekripsi body push persis seperti browser (RFC 8188/8291). */
function decryptRequest(req) {
  const keys = endpointRegistry[req.url];
  if (!keys) throw new Error('endpoint tidak dikenal: ' + req.url);
  const body = Buffer.from(req.bodyBytes);
  const salt = body.slice(0, 16);
  const idlen = body[20];
  const asPub = body.slice(21, 21 + idlen);
  const ctTag = body.slice(21 + idlen);
  const ecdh = crypto.createECDH('prime256v1');
  ecdh.setPrivateKey(keys.priv);
  const secret = ecdh.computeSecret(asPub);
  const keyInfo = Buffer.concat([Buffer.from('WebPush: info\0'), keys.pub, asPub]);
  const ikm = Buffer.from(crypto.hkdfSync('sha256', secret, keys.auth, keyInfo, 32));
  const cek = Buffer.from(crypto.hkdfSync('sha256', ikm, salt,
    Buffer.from('Content-Encoding: aes128gcm\0'), 16));
  const nonce = Buffer.from(crypto.hkdfSync('sha256', ikm, salt,
    Buffer.from('Content-Encoding: nonce\0'), 12));
  const d = crypto.createDecipheriv('aes-128-gcm', cek, nonce);
  d.setAuthTag(ctTag.slice(ctTag.length - 16));
  const pt = Buffer.concat([d.update(ctTag.slice(0, ctTag.length - 16)), d.final()]);
  return JSON.parse(pt.slice(0, pt.length - 1).toString('utf8'));
}

/* ======================= Mock browser ======================= */

let currentSub = null;
let lastAppKey = null;                 // applicationServerKey terakhir dipakai subscribe
const browserEvents = [];              // {t:'subscribe'|'local-unsubscribe', endpoint}
const gasRequests = [];                // semua permintaan PWA/SW -> GAS
const swNotifications = [];            // hasil showNotification
const openWindows = [];
let seq = 0;
const timeline = [];                   // urutan peristiwa lintas komponen

function makeSubscription(appKeyBytes) {
  const ecdh = crypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const keys = {
    priv: ecdh.getPrivateKey(),
    pub: ecdh.getPublicKey(),
    auth: crypto.randomBytes(16)
  };
  const endpoint = 'https://push.test.local/fcm/send/' +
    crypto.randomBytes(16).toString('hex');
  endpointRegistry[endpoint] = keys;
  const sub = {
    endpoint,
    options: { applicationServerKey: appKeyBytes ? Array.from(appKeyBytes) : null },
    toJSON() {
      return { endpoint, keys: { p256dh: b64url(keys.pub), auth: b64url(keys.auth) } };
    },
    async unsubscribe() {
      delete endpointRegistry[endpoint];
      if (currentSub === sub) currentSub = null;
      browserEvents.push({ t: 'local-unsubscribe', endpoint });
      timeline.push({ seq: seq++, t: 'local-unsubscribe' });
      return true;
    }
  };
  currentSub = sub;
  browserEvents.push({ t: 'subscribe', endpoint });
  timeline.push({ seq: seq++, t: 'browser-subscribe' });
  return sub;
}

const mockReg = {
  pushManager: {
    async getSubscription() { return currentSub; },
    async subscribe(opts) {
      lastAppKey = opts && opts.applicationServerKey
        ? Buffer.from(opts.applicationServerKey) : null;
      return makeSubscription(lastAppKey);
    }
  }
};

/** Jembatan fetch: permintaan PWA/SW ke GAS diteruskan ke runtime GAS. */
async function bridgeFetch(url, opts) {
  if (typeof url !== 'string' || url.indexOf(GAS_URL) !== 0) {
    throw new Error('fetch di luar GAS tidak diizinkan dalam uji: ' + url);
  }
  let action = 'snapshot';
  const qi = url.indexOf('?');
  if (qi >= 0) {
    action = new URLSearchParams(url.slice(qi + 1)).get('action') || 'snapshot';
  }
  if (opts && String(opts.method).toUpperCase() === 'POST') {
    const bodyStr = typeof opts.body === 'string' ? opts.body : '';
    let kind = 'server-post';
    try { kind = 'server-' + (JSON.parse(bodyStr).action || 'post'); } catch (e) { /* biarkan */ }
    timeline.push({ seq: seq++, t: kind });
    const out = gasCtx.doPost({ postData: { contents: bodyStr } });
    let response = null;
    try { response = JSON.parse(out._text); } catch (e) { /* abaikan */ }
    gasRequests.push({ method: 'POST', body: bodyStr, response });
    return { ok: true, status: 200, async json() { return JSON.parse(out._text); }, async text() { return out._text; } };
  }
  timeline.push({ seq: seq++, t: 'server-get-' + action });
  gasRequests.push({ method: 'GET', action, response: null });
  const out = gasCtx.doGet({ parameter: { action: action } });
  return { ok: true, status: 200, async json() { return JSON.parse(out._text); }, async text() { return out._text; } };
}

/* ======================= Muat push-manager.js (PWA) ======================= */

const SWR = function () {};
SWR.prototype.showNotification = function () {};

const pageCtx = vm.createContext({
  navigator: {
    language: 'id-ID',
    userAgent: 'CrossAuditHarness/1.0 (Node)',
    onLine: true,
    serviceWorker: {
      getRegistration: async () => mockReg,
      ready: Promise.resolve(mockReg)
    }
  },
  window: { PushManager: function () {}, Notification: function () {} },
  Notification: { permission: 'granted', requestPermission: async () => 'granted' },
  ServiceWorkerRegistration: SWR,
  localStorage: (() => {
    const m = new Map();
    return {
      getItem: (k) => (m.has(k) ? m.get(k) : null),
      setItem: (k, v) => m.set(k, String(v)),
      removeItem: (k) => m.delete(k)
    };
  })(),
  fetch: bridgeFetch,
  AbortController,
  setTimeout,
  clearTimeout,
  // [FIX 2026-09-01] atob KETAT meniru peramban nyata: karakter di luar
  // A-Za-z0-9+/ (termasuk '-'/'_' base64url) MELEMPAR InvalidCharacterError.
  // Mock lama (Buffer.from base64) toleran-korupsi: menerima base64url diam-diam
  // sehingga menutupi bug decoder push-manager (kunci VAPID gagal subscribe di
  // peramban sungguhan). Lihat uji K7 "decoder menolak mentah base64url".
  atob: (s) => {
    if (/[^A-Za-z0-9+\/=]/.test(String(s))) {
      throw new Error('InvalidCharacterError: The string to be decoded contains invalid characters');
    }
    return Buffer.from(s, 'base64').toString('binary');
  },
  btoa: (s) => Buffer.from(s, 'binary').toString('base64'),
  URL,
  URLSearchParams,
  module: { exports: {} }
});
vm.runInContext(fs.readFileSync(path.join(PWA, 'js', 'push-manager.js'), 'utf8'),
  pageCtx, { filename: 'push-manager.js' });
const AlarmPushManager = pageCtx.module.exports;

/* ======================= Muat sw.js (PWA) ======================= */

const swHandlers = {};
const swSelf = {
  registration: {
    pushManager: mockReg.pushManager,
    async showNotification(title, options) {
      swNotifications.push({ title, options });
    }
  },
  clients: {
    async matchAll() { return []; },
    async openWindow(url) { openWindows.push(url); return {}; }
  },
  location: { origin: APP_ORIGIN },
  skipWaiting() {},
  addEventListener(name, fn) { swHandlers[name] = fn; }
};

const swSrcRaw = fs.readFileSync(path.join(PWA, 'sw.js'), 'utf8');
// Substitusi API_BASE persis seperti langkah deployment (README langkah 5).
const swSrc = swSrcRaw.replace(/const API_BASE =\s*'[^']*';/,
  "const API_BASE = '" + GAS_URL + "';");

const cachesMock = {
  async open() {
    return { put() {}, add() {}, addAll() {}, match: async () => null, delete() {} };
  },
  async keys() { return []; },
  async match() { return null; },
  async delete() { return true; }
};
const swCtx = vm.createContext({
  self: swSelf,
  caches: cachesMock,
  fetch: bridgeFetch,
  URL,
  URLSearchParams,
  Response: function (body) { this.body = body; },
  setTimeout,
  clearTimeout,
  AbortController,
  console: { log() {} }
});
vm.runInContext(swSrc, swCtx, { filename: 'sw.js' });
vm.runInContext('this.__swApi = { API_BASE: API_BASE };', swCtx, { filename: 'sw-api.js' });
const swApi = swCtx.__swApi;

async function dispatchPush(payloadOrNull) {
  const waits = [];
  swHandlers.push({
    data: payloadOrNull
      ? { json: () => payloadOrNull, text: () => JSON.stringify(payloadOrNull) }
      : null,
    waitUntil: (p) => { waits.push(p); }
  });
  await Promise.all(waits.map((w) => Promise.resolve(w)));
}
async function dispatchClick(action, data) {
  const waits = [];
  swHandlers.notificationclick({
    action: action,
    notification: { close() {}, data: data || {} },
    waitUntil: (p) => { waits.push(p); }
  });
  await Promise.all(waits.map((w) => Promise.resolve(w)));
}

/* ======================= Simulator firmware ======================= */
/* Skema identik dengan MonitorIoT_Firmware.ino (lihat pemeriksaan
 * statis K6 di bawah yang memastikan .ino mengirim field ini). */

let fwTs = 1756000000000;
const fwT = () => (fwTs += 60000);

function fwSensor(name, value, unit, alarm, severity, status) {
  return {
    name,
    value: (value === null || value === undefined) ? null : Math.round(value * 10) / 10,
    unit,
    alarm,
    severity,
    status: status || '-'
  };
}
function fwReport(sensors, reportedAt, extra) {
  return Object.assign({
    action: 'ingest',
    token: DEVICE_TOKEN,
    device: { id: DEVICE_ID, fw: '1.0.0', uptimeMs: 42000 },
    sensors: sensors,
    reportedAt: reportedAt
  }, extra || {});
}

const S_TEMP = (v, alarm) => fwSensor('Suhu Greenhouse 1', v, 'C', alarm,
  alarm ? 'critical' : 'info',
  alarm ? ('suhu ' + v.toFixed(1) + ' C melebihi ambang 40.0 C')
    : ('suhu ' + v.toFixed(1) + ' C (ambang maks 40.0 C)'));
const S_HUM = (v, alarm) => fwSensor('Kelembapan Udara', v, '%', alarm,
  alarm ? 'warning' : 'info', alarm ? 'kelembapan rendah' : '-');
const S_SOIL = (v, alarm) => fwSensor('Kelembapan Tanah', v, '%', alarm,
  alarm ? 'warning' : 'info',
  alarm ? ('tanah ' + v.toFixed(0) + '% di bawah ambang 30% (perlu siram)') : '-');

/* ======================= Skenario utama ======================= */

async function main() {
  console.log('==============================================================');
  console.log(' AUDIT SILANG PWA - GAS - FIRMWARE (matriks kontrak Tabel 11)');
  console.log('==============================================================');
  const pushMgr = new AlarmPushManager(GAS_URL, VAPID_PUB);

  /* ---------------- K1: Langganan (PWA-GAS) ---------------- */
  console.log('\n--- K1 Langganan (PWA -> GAS) ---');
  const r1 = await pushMgr.enable();
  check('K1', 'PWA enable() sukses', r1.ok === true && r1.state === 'enabled');
  check('K1', 'tepat satu langganan tersimpan di GAS', storedSubs().length === 1);
  const stored1 = storedSubs()[0];
  check('K1', 'endpoint tersimpan identik byte-per-byte',
    stored1.endpoint === currentSub.endpoint);
  check('K1', 'p256dh tersimpan utuh (tanpa normalisasi berlebih)',
    stored1.keys.p256dh === currentSub.toJSON().keys.p256dh);
  check('K1', 'auth tersimpan utuh', stored1.keys.auth === currentSub.toJSON().keys.auth);
  check('K1', 'p256dh valid 65 byte (0x04||X||Y)', b64urlToBuf(stored1.keys.p256dh).length === 65);
  check('K1', 'auth valid 16 byte', b64urlToBuf(stored1.keys.auth).length === 16);
  check('K1', 'context klien tercatat (lang/tz/ua)',
    !!(stored1.context && stored1.context.lang && stored1.context.ua));

  const subCountBefore = browserEvents.filter((e) => e.t === 'subscribe').length;
  const r2 = await pushMgr.enable();
  check('K1', 'enable() ulang memakai langganan lama (tanpa subscribe baru)',
    r2.ok === true &&
    browserEvents.filter((e) => e.t === 'subscribe').length === subCountBefore &&
    storedSubs().length === 1);

  // Rotasi kunci pada endpoint sama -> upsert, bukan duplikat.
  const ecdh2 = crypto.createECDH('prime256v1');
  ecdh2.generateKeys();
  const p2 = b64url(ecdh2.getPublicKey());
  const a2 = b64url(crypto.randomBytes(16));
  const upResp = gasPost({
    action: 'subscribe', endpoint: stored1.endpoint,
    keys: { p256dh: p2, auth: a2 }, context: { test: 'rotasi-kunci' }
  });
  check('K1', 'upsert endpoint sama -> jumlah tetap 1',
    upResp.ok === true && storedSubs().length === 1);
  check('K1', 'kunci langganan diperbarui saat rotasi', storedSubs()[0].keys.p256dh === p2);

  // pushsubscriptionchange: buat ulang langganan + kirim ke GAS.
  const oldEndpoint = stored1.endpoint;
  const waits = [];
  swHandlers.pushsubscriptionchange({ waitUntil: (p) => waits.push(p) });
  await Promise.all(waits.map((w) => Promise.resolve(w)));
  check('K1', 'pushsubscriptionchange menghasilkan langganan baru',
    currentSub && currentSub.endpoint !== oldEndpoint);
  check('K1', 'endpoint lama + baru sama-sama terdaftar (perilaku nyata)',
    storedSubs().length === 2 &&
    storedSubs().some((s) => s.endpoint === oldEndpoint) &&
    storedSubs().some((s) => s.endpoint === currentSub.endpoint));
  const newRec = storedSubs().filter((s) => s.endpoint === currentSub.endpoint)[0];
  check('K1', 'context.reason=resubscribe tercatat',
    !!(newRec && newRec.context && newRec.context.reason === 'pushsubscriptionchange'));

  // Pembersihan endpoint mati via 410 saat kirim.
  pushService.strictEndpoints = true;
  const rotRes = gasCtx.sendAlarmToAll({ id: 'ALM-ROT-1', title: 'UJI ROTASI', severity: 'info' });
  check('K1', 'endpoint kedaluwarsa dibersihkan saat kirim (removed 1, sent 1)',
    rotRes.removed === 1 && rotRes.sent === 1);
  check('K1', 'sisa satu langganan aktif', storedSubs().length === 1);
  pushService.strictEndpoints = false;

  // Langganan tidak valid ditolak.
  check('K1', 'subscribe tanpa keys ditolak',
    gasPost({ action: 'subscribe', endpoint: 'https://push.test.local/x' }).ok === false);
  check('K1', 'subscribe endpoint http:// ditolak (temuan X-5)',
    gasPost({
      action: 'subscribe', endpoint: 'http://push.test.local/x',
      keys: { p256dh: p2, auth: a2 }
    }).ok === false);
  check('K1', 'registri tidak berubah oleh penolakan', storedSubs().length === 1);

  /* ---------------- K2: Penghapusan (PWA-GAS) ---------------- */
  console.log('\n--- K2 Penghapusan (PWA -> GAS) ---');
  const rd = await pushMgr.disable();
  check('K2', 'PWA disable() sukses', rd.ok === true);
  const serverSeqs = timeline.filter((e) => e.t === 'server-unsubscribe').map((e) => e.seq);
  const localSeqs = timeline.filter((e) => e.t === 'local-unsubscribe').map((e) => e.seq);
  check('K2', 'GAS diberi tahu SEBELUM unsubscribe lokal',
    serverSeqs.length > 0 && localSeqs.length > 0 &&
    Math.max(...serverSeqs) < Math.max(...localSeqs));
  check('K2', 'endpoint terhapus dari registri GAS', storedSubs().length === 0);

  const unkRes = gasPost({ action: 'unsubscribe', endpoint: 'https://push.test.local/tak-ada' });
  check('K2', 'unsubscribe endpoint tak dikenal -> removed 0, tetap ok',
    unkRes.ok === true && unkRes.removed === 0);

  // 410 saat kirim -> pembersihan otomatis.
  const re1 = await pushMgr.enable();
  check('K2', 'langganan aktif kembali untuk uji 410', re1.ok === true && storedSubs().length === 1);
  pushService.setNextStatus(410);
  const r410 = gasCtx.sendAlarmToAll({ id: 'ALM-410-1', title: 'UJI 410', severity: 'info' });
  check('K2', 'kirim ke endpoint 410 -> removed 1, sent 0',
    r410.removed === 1 && r410.sent === 0);
  check('K2', 'langganan mati terhapus otomatis', storedSubs().length === 0);

  // 404 juga membersihkan.
  await pushMgr.enable();
  pushService.setNextStatus(404);
  const r404 = gasCtx.sendAlarmToAll({ id: 'ALM-404-1', title: 'UJI 404', severity: 'info' });
  check('K2', 'kirim ke endpoint 404 -> ikut membersihkan', r404.removed === 1);

  const re2 = await pushMgr.enable();
  check('K2', 'langganan final aktif untuk rangkaian berikut',
    re2.ok === true && storedSubs().length === 1);

  /* ---------------- K3: Payload alarm (GAS -> PWA) ---------------- */
  console.log('\n--- K3 Payload alarm (GAS -> PWA) ---');
  const pushMark = pushService.requests.length;
  const dr = gasCtx.sendAlarmToAll({
    id: 'ALM-DIRECT-001', title: 'UJI PAYLOAD LANGSUNG',
    body: 'B'.repeat(500), severity: 'critical'
  });
  check('K3', 'terkirim ke 1 langganan', dr.sent === 1);
  const req1 = pushService.requests[pushService.requests.length - 1];
  check('K3', 'header Authorization vapid t=...,k=...',
    /^vapid t=.+,\s*k=.+/.test(req1.headers['Authorization'] || ''));
  check('K3', 'header TTL 86400 (24 jam)', req1.headers['TTL'] === '86400');
  check('K3', 'Urgency high untuk critical', req1.headers['Urgency'] === 'high');
  check('K3', 'Content-Encoding aes128gcm', req1.headers['Content-Encoding'] === 'aes128gcm');
  check('K3', 'payload terkirim sebagai Blob biner (temuan X-2)', req1.payloadIsBlob === true);
  check('K3', 'ukuran body terenkripsi <= 4096 byte', req1.bodyBytes.length <= 4096);

  const payload = decryptRequest(req1);
  check('K3', 'payload terdekripsi utuh sisi browser (id sama)', payload.id === 'ALM-DIRECT-001');
  check('K3', 'body panjang dibatasi 400 karakter', payload.body.length <= 400);
  check('K3', 'severity valid', payload.severity === 'critical');
  check('K3', 'url default deep-link from=push', payload.url === './index.html?from=push');
  check('K3', 'tag fallback = alarm-<id>', payload.tag === 'alarm-ALM-DIRECT-001');
  check('K3', 'timestamp numerik', typeof payload.timestamp === 'number');
  check('K3', 'requireInteraction true untuk critical', payload.requireInteraction === true);

  // Notifikasi: payload GAS dikonsumsi sw.js tanpa kehilangan field.
  await dispatchPush(payload);
  const n1 = swNotifications[swNotifications.length - 1];
  check('K3', 'judul notifikasi = payload.title', n1 && n1.title === payload.title);
  check('K3', 'tag notifikasi = payload.tag', n1 && n1.options.tag === payload.tag);
  check('K3', 'renotify true (critical)', n1 && n1.options.renotify === true);
  check('K3', 'requireInteraction true (critical)', n1 && n1.options.requireInteraction === true);
  check('K3', 'pola getar critical [300,150,300,150,300]',
    n1 && JSON.stringify(n1.options.vibrate) === JSON.stringify([300, 150, 300, 150, 300]));
  check('K3', 'data.alarmId = payload.id', n1 && n1.options.data.alarmId === payload.id);
  check('K3', 'data.url = payload.url', n1 && n1.options.data.url === payload.url);
  check('K3', 'data.ackUrl = API_BASE service worker', n1 && n1.options.data.ackUrl === GAS_URL);
  check('K3', 'dua aksi (Lihat Detail / Tandai Ditangani)',
    n1 && n1.options.actions && n1.options.actions.length === 2);
  const notifData = n1 ? n1.options.data : null;

  // Varian warning.
  gasCtx.sendAlarmToAll({ id: 'ALM-DIRECT-002', title: 'UJI WARNING', body: 'w', severity: 'warning' });
  const req2 = pushService.requests[pushService.requests.length - 1];
  check('K3', 'Urgency normal untuk warning', req2.headers['Urgency'] === 'normal');
  const payload2 = decryptRequest(req2);
  check('K3', 'warning: requireInteraction false', payload2.requireInteraction === false);
  await dispatchPush(payload2);
  const n2 = swNotifications[swNotifications.length - 1];
  check('K3', 'warning: renotify false', n2.options.renotify === false);
  check('K3', 'warning: getar [200]', JSON.stringify(n2.options.vibrate) === '[200]');

  /* ---------------- K4: ACK (PWA -> GAS) ---------------- */
  console.log('\n--- K4 ACK alarm (PWA -> GAS) ---');
  await dispatchClick('ack', notifData);
  const ackReq = [...gasRequests].reverse()
    .find((r) => r.method === 'POST' && r.body.indexOf('"ackAlarm"') >= 0);
  check('K4', 'ACK terkirim dari aksi notifikasi ke GAS', !!ackReq);
  const ackBody = ackReq ? JSON.parse(ackReq.body) : {};
  check('K4', 'alarmId pada ACK identik dengan payload terkirim',
    ackBody.alarmId === 'ALM-DIRECT-001');
  check('K4', 'GAS menerima ACK (ok) termasuk jalur sendAlarmToAll langsung (temuan X-6)',
    ackReq && ackReq.response && ackReq.response.ok === true);
  check('K4', 'respons memuat alarmId yang sama',
    ackReq && ackReq.response && ackReq.response.alarmId === 'ALM-DIRECT-001');
  const entry = alarmLog().find((e) => e.id === 'ALM-DIRECT-001');
  check('K4', 'ALARM_LOG mencatat acknowledgedAt', entry && !!entry.acknowledgedAt);
  const ackAgain = gasPost({ action: 'ackAlarm', alarmId: 'ALM-DIRECT-001' });
  check('K4', 'ACK idempoten (kali kedua tetap ok)', ackAgain.ok === true);
  check('K4', 'ACK id tak dikenal ditolak eksplisit',
    gasPost({ action: 'ackAlarm', alarmId: 'TIDAK-ADA' }).ok === false);

  const gasReqBefore = gasRequests.length;
  await dispatchClick('ack', { alarmId: null, url: './index.html?from=push', ackUrl: GAS_URL });
  const extraAcks = gasRequests.slice(gasReqBefore)
    .filter((r) => r.method === 'POST' && r.body.indexOf('"ackAlarm"') >= 0);
  check('K4', 'notifikasi tanpa alarmId tidak mengirim ACK', extraAcks.length === 0);

  /* ---------------- K5: Deep-link (internal PWA) ---------------- */
  console.log('\n--- K5 Deep-link (internal PWA) ---');
  const openMark = openWindows.length;
  await dispatchClick('view', notifData);
  check('K5', 'openWindow dipanggil saat notifikasi diklik', openWindows.length === openMark + 1);
  const openUrl = new URL(openWindows[openWindows.length - 1]);
  check('K5', 'URL tujuan dibuka pada origin PWA', openUrl.origin === APP_ORIGIN);
  check('K5', 'parameter from=push terjaga', openUrl.searchParams.get('from') === 'push');
  check('K5', 'url payload GAS memuat from=push', payload.url.indexOf('from=push') >= 0);

  await dispatchPush(Object.assign({}, payload,
    { id: 'ALM-DL-1', url: './index.html?from=push&section=alarms' }));
  const nDl = swNotifications[swNotifications.length - 1];
  await dispatchClick('view', nDl.options.data);
  const openUrl2 = new URL(openWindows[openWindows.length - 1]);
  check('K5', 'parameter section=alarms dipertahankan hingga URL akhir',
    openUrl2.searchParams.get('section') === 'alarms');

  const manifest = JSON.parse(fs.readFileSync(path.join(PWA, 'manifest.json'), 'utf8'));
  check('K5', 'shortcut manifest menunjuk ?section=alarms',
    manifest.shortcuts && manifest.shortcuts[0].url === './index.html?section=alarms');
  const appSrc = fs.readFileSync(path.join(PWA, 'js', 'app.js'), 'utf8');
  check('K5', 'app.js merespons section=alarms dan from=push (handleDeepLink)',
    appSrc.indexOf("params.get('section') === 'alarms'") >= 0 &&
    appSrc.indexOf("params.get('from') === 'push'") >= 0);
  const idxSrc = fs.readFileSync(path.join(PWA, 'index.html'), 'utf8');
  check('K5', 'index.html menyediakan anchor id="section-alarms"',
    idxSrc.indexOf('id="section-alarms"') >= 0);
  check('K5', 'CSP mengizinkan connect-src ke script.google.com',
    idxSrc.indexOf('connect-src') >= 0 && idxSrc.indexOf('https://script.google.com') >= 0);

  /* ---------------- K6: Ambang alarm (FW-GAS) ---------------- */
  console.log('\n--- K6 Ambang alarm (FIRMWARE -> GAS) ---');
  const fwSrc = fs.readFileSync(FW_INO, 'utf8');
  const fwCode = fwSrc.replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/^\s*\/\/.*$/gm, ''); // buang komentar utk pemeriksaan jalur
  check('K6', 'FW mengirim action=ingest', fwCode.indexOf('doc["action"] = "ingest"') >= 0);
  check('K6', 'FW menyertakan token perangkat', fwCode.indexOf('doc["token"]') >= 0);
  check('K6', 'FW mengirim flag alarm level per sensor', fwCode.indexOf('s["alarm"]') >= 0);
  check('K6', 'FW mengirim severity per sensor', fwCode.indexOf('s["severity"]') >= 0);
  check('K6', 'FW mengirim reportedAt', fwCode.indexOf('doc["reportedAt"]') >= 0);
  check('K6', 'prinsip pengirim tunggal: FW tanpa jalur push/ACK',
    !/testPush|sendAlarmToAll|ackAlarm|"subscribe"/.test(fwCode));

  // (a) token salah
  const pBefore = pushService.requests.length;
  let res = gasPost(fwReport([S_TEMP(29.4, false), S_HUM(71, false), S_SOIL(55, false)],
    fwT(), { token: 'TOKEN-SALAH' }));
  check('K6', 'token salah ditolak fail-closed', res.ok === false);
  check('K6', 'token salah tidak memicu push', pushService.requests.length === pBefore);

  // (b) laporan normal pertama
  res = gasPost(fwReport([S_TEMP(29.4, false), S_HUM(71, false), S_SOIL(55, false)], fwT()));
  check('K6', 'laporan normal diterima (3 sensor)', res.ok === true && res.received === 3);
  check('K6', 'kondisi normal -> nol kejadian, nol push',
    res.triggered.length === 0 && pushService.requests.length === pBefore);
  const snap0 = gasGet('snapshot');
  check('K6', 'snapshot tersaji untuk dashboard PWA',
    snap0.ok === true && snap0.sensors.length === 3 && Array.isArray(snap0.alarms));
  check('K6', 'skema sensor cocok dengan renderer app.js',
    snap0.sensors.every((s) =>
      'name' in s && 'value' in s && 'unit' in s && 'alarm' in s && 'status' in s));

  // (c) tepi naik suhu -> SATU push
  const tc = fwT();
  res = gasPost(fwReport([S_TEMP(41.2, true), S_HUM(70, false), S_SOIL(54, false)], tc));
  check('K6', 'tepi naik -> satu kejadian dipicu', res.triggered.length === 1);
  check('K6', 'tepi naik -> satu push terkirim (sent 1)',
    pushService.requests.length === pBefore + 1 && res.pushes.sent === 1);
  const tempEventId = res.triggered[0];
  const almPayload = decryptRequest(pushService.requests[pushService.requests.length - 1]);
  check('K6', 'id kejadian = ALM-<sensor-slug>-<waktu>',
    tempEventId.indexOf('ALM-suhu-greenhouse-1-') === 0);
  check('K6', 'judul KRITIS + nama sensor', almPayload.title === 'KRITIS: Suhu Greenhouse 1');
  check('K6', 'severity critical dari firmware', almPayload.severity === 'critical');
  check('K6', 'status firmware menjadi body alarm',
    almPayload.body.indexOf('41.2 C melebihi ambang') >= 0);
  check('K6', 'timestamp = reportedAt firmware', almPayload.timestamp === tc);
  check('K6', 'snapshot menampilkan alarm aktif', gasGet('snapshot').alarms.length === 1);
  const la1 = gasGet('latestAlarm');
  check('K6', 'latestAlarm mengembalikan alarm aktif', la1.ok === true && la1.alarm &&
    la1.alarm.id === tempEventId);
  check('K6', 'latestAlarm ber-skema payload penuh (fallback push)',
    la1.alarm && ['id', 'title', 'body', 'severity', 'tag', 'url', 'timestamp', 'requireInteraction']
      .every((k) => k in la1.alarm));

  // (d) alarm bertahan -> TIDAK ada push baru (inti kontrak)
  res = gasPost(fwReport([S_TEMP(41.5, true), S_HUM(70, false), S_SOIL(54, false)], fwT()));
  check('K6', 'alarm bertahan: tidak ada kejadian baru', res.triggered.length === 0);
  check('K6', 'alarm bertahan: TIDAK ada push tambahan (anti dobel-kirim)',
    pushService.requests.length === pBefore + 1);

  // (e) sensor kedua ikut alarm -> hanya push untuk sensor baru
  res = gasPost(fwReport([S_TEMP(41.6, true), S_HUM(70, false), S_SOIL(22, true)], fwT()));
  check('K6', 'sensor kedua naik -> hanya 1 kejadian baru',
    res.triggered.length === 1 && res.triggered[0].indexOf('ALM-kelembapan-tanah-') === 0);
  check('K6', 'push hanya untuk kejadian baru', pushService.requests.length === pBefore + 2);
  check('K6', 'snapshot memuat 2 alarm aktif', gasGet('snapshot').alarms.length === 2);

  // (f) suhu pulih -> kejadian ditutup + notifikasi PULIH
  res = gasPost(fwReport([S_TEMP(38.0, false), S_HUM(70, false), S_SOIL(23, true)], fwT()));
  check('K6', 'tepi turun menutup kejadian suhu',
    res.resolved.length === 1 && res.resolved[0] === tempEventId);
  check('K6', 'notifikasi pulih terkirim sekali', pushService.requests.length === pBefore + 3);
  const rsvPayload = decryptRequest(pushService.requests[pushService.requests.length - 1]);
  check('K6', 'notifikasi pulih severity info', rsvPayload.severity === 'info');
  check('K6', 'id notifikasi pulih berprefiks RSV-', rsvPayload.id.indexOf('RSV-') === 0);
  check('K6', 'tag pulih = tag alarm (menimpa notifikasi lama)',
    rsvPayload.tag === almPayload.tag);
  check('K6', 'alarm suhu keluar dari daftar aktif',
    gasGet('snapshot').alarms.length === 1 &&
    gasGet('snapshot').alarms[0].id !== tempEventId);

  // (g) sensor tidak dilaporkan tidak dianggap pulih
  res = gasPost(fwReport([S_TEMP(37.8, false), S_HUM(70, false)], fwT()));
  check('K6', 'sensor tanah absen -> alarm aktifnya dipertahankan',
    res.triggered.length === 0 && res.resolved.length === 0 &&
    gasGet('snapshot').alarms.length === 1);

  // (h) tanah pulih -> selesai
  res = gasPost(fwReport([S_TEMP(37.8, false), S_HUM(70, false), S_SOIL(45, false)], fwT()));
  check('K6', 'semua pulih -> daftar alarm aktif kosong',
    res.resolved.length === 1 && gasGet('snapshot').alarms.length === 0);
  const la2 = gasGet('latestAlarm');
  check('K6', 'latestAlarm null saat tak ada alarm aktif', la2.ok === true && la2.alarm === null);

  // (i) firmware lama tanpa flag alarm -> evaluasi ambang GAS
  res = gasPost(fwReport([
    { name: 'Suhu Greenhouse 1', value: 45.3, unit: 'C', status: '' },
    { name: 'Kelembapan Udara', value: 70, unit: '%' },
    { name: 'Kelembapan Tanah', value: 50, unit: '%' }
  ], fwT()));
  check('K6', 'firmware lama (tanpa flag): GAS mengevaluasi ambang',
    res.triggered.length === 1 && res.triggered[0].indexOf('ALM-suhu-greenhouse-1-') === 0);
  check('K6', 'evaluasi GAS: severity dari aturan (critical)',
    decryptRequest(pushService.requests[pushService.requests.length - 1]).severity === 'critical');
  res = gasPost(fwReport([
    { name: 'Suhu Greenhouse 1', value: 45.5, unit: 'C' },
    { name: 'Kelembapan Udara', value: 70, unit: '%' },
    { name: 'Kelembapan Tanah', value: 50, unit: '%' }
  ], fwT()));
  check('K6', 'kondisi bertahan (evaluasi GAS): tetap tanpa push baru',
    res.triggered.length === 0);
  res = gasPost(fwReport([
    { name: 'Suhu Greenhouse 1', value: 39.0, unit: 'C' },
    { name: 'Kelembapan Udara', value: 70, unit: '%' },
    { name: 'Kelembapan Tanah', value: 50, unit: '%' }
  ], fwT()));
  check('K6', 'pulih (evaluasi GAS): kejadian ditutup', res.resolved.length === 1);

  // (j) sensor gagal dibaca (value null)
  res = gasPost(fwReport([
    { name: 'Suhu Greenhouse 1', value: null, unit: 'C', status: 'sensor suhu gagal dibaca' },
    S_HUM(70, false), S_SOIL(50, false)
  ], fwT()));
  check('K6', 'nilai null diterima tanpa galat & tanpa alarm palsu',
    res.ok === true && res.triggered.length === 0);
  const snapNull = gasGet('snapshot');
  check('K6', 'snapshot memuat value null untuk PWA',
    snapNull.sensors[0].value === null);
  check('K6', 'app.js merender null sebagai "--" (temuan X-3)',
    appSrc.indexOf('raw === null') >= 0);

  // (k) token banyak perangkat
  gasProps.set('FW_DEVICE_TOKENS',
    JSON.stringify([{ deviceId: DEVICE_ID, token: DEVICE_TOKEN }]));
  check('K6', 'mode banyak perangkat: token salah tetap ditolak',
    gasPost(fwReport([S_TEMP(30, false)], fwT(), { token: 'SALAH' })).ok === false);
  check('K6', 'mode banyak perangkat: pasangan id+token cocok diterima',
    gasPost(fwReport([S_TEMP(30, false)], fwT())).ok === true);
  check('K6', 'mode banyak perangkat: id perangkat tak cocok ditolak',
    gasPost(fwReport([S_TEMP(30, false)], fwT(),
      { device: { id: 'esp32-lain', fw: '1.0.0', uptimeMs: 1 } })).ok === false);
  gasProps.delete('FW_DEVICE_TOKENS');

  // (l) laporan tanpa sensor
  check('K6', 'laporan tanpa sensor ditolak',
    gasPost({ action: 'ingest', token: DEVICE_TOKEN, device: { id: DEVICE_ID }, sensors: [] }).ok === false);

  // (m) push tanpa payload -> fallback latestAlarm (GAS <-> sw.js)
  const tL = fwT();
  gasPost(fwReport([S_TEMP(42.0, true), S_HUM(70, false), S_SOIL(50, false)], tL));
  const notifMark = swNotifications.length;
  await dispatchPush(null);
  const nf = swNotifications[swNotifications.length - 1];
  check('K3', 'push tanpa payload -> sw.js mengambil latestAlarm dari GAS',
    swNotifications.length === notifMark + 1 && nf.title === 'KRITIS: Suhu Greenhouse 1');
  check('K3', 'alarm fallback membawa data.alarmId untuk ACK',
    nf.options.data.alarmId && nf.options.data.alarmId.indexOf('ALM-suhu-greenhouse-1-') === 0);
  // pulihkan
  gasPost(fwReport([S_TEMP(36, false), S_HUM(70, false), S_SOIL(50, false)], fwT()));

  /* ---------------- K7: Kunci VAPID (GAS-PWA) ---------------- */
  console.log('\n--- K7 Kunci VAPID (GAS <-> PWA) ---');
  const pubBytes = b64urlToBuf(VAPID_PUB);
  check('K7', 'kunci publik GAS 65 byte tak terkompresi',
    pubBytes.length === 65 && pubBytes[0] === 0x04);
  check('K7', 'applicationServerKey PWA byte-identik dengan kunci publik GAS',
    lastAppKey && lastAppKey.equals(pubBytes));

  const reqAuth = pushService.requests.find((r) => r.headers['Authorization']);
  const mAuth = /^vapid t=(.+), k=(.+)$/.exec(reqAuth.headers['Authorization']);
  check('K7', 'header vapid terformat benar', !!mAuth);
  check('K7', 'klaim k= identik dengan kunci publik di Script Properties',
    mAuth && mAuth[2] === VAPID_PUB && mAuth[2] === gasProps.get('VAPID_PUBLIC_KEY'));

  const jwtParts = mAuth[1].split('.');
  const wc = crypto.webcrypto;
  const vKey = await wc.subtle.importKey('raw', new Uint8Array(pubBytes),
    { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
  const sigOk = await wc.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, vKey,
    new Uint8Array(b64urlToBuf(jwtParts[2])),
    new TextEncoder().encode(jwtParts[0] + '.' + jwtParts[1]));
  check('K7', 'tanda tangan JWT VAPID valid (dibuat kunci privat GAS)', sigOk);
  const claims = JSON.parse(b64urlToBuf(jwtParts[1]).toString('utf8'));
  check('K7', 'aud = origin push service', claims.aud === 'https://push.test.local');
  check('K7', 'sub = SUBJECT konfigurasi GAS', claims.sub === gasCtx.PUSH_CONFIG.SUBJECT);
  check('K7', 'exp dalam jendela 12 jam',
    claims.exp > Date.now() / 1000 && claims.exp <= Date.now() / 1000 + 12 * 3600 + 60);

  const cfgSrc = fs.readFileSync(path.join(PWA, 'js', 'config.js'), 'utf8');
  const cfgUrl = /API_BASE:\s*'([^']+)'/.exec(cfgSrc)[1];
  const swUrlConst = /const API_BASE =\s*'([^']+)'/.exec(swSrcRaw)[1];
  check('K7', 'API_BASE config.js === API_BASE sw.js', cfgUrl === swUrlConst);
  check('K7', 'placeholder deployment belum dikonfigurasi (pengingat)',
    cfgUrl.indexOf('GANTI_DENGAN_ID_DEPLOYMENT_ANDA') >= 0);
  const cfg = require(path.join(PWA, 'js', 'config.js'));
  check('K7', 'config.js menyediakan slot VAPID_PUBLIC_KEY',
    typeof cfg.VAPID_PUBLIC_KEY === 'string' && cfg.VAPID_PUBLIC_KEY.length > 40);
  const conv = AlarmPushManager.urlBase64ToUint8Array(VAPID_PUB);
  check('K7', 'konversi base64url -> Uint8Array 65 byte (mekanisme substitusi)',
    conv.length === 65 && conv[0] === 4);
  // (Regresi decoder 2026-09-01) Kunci yang PASTI memuat '-'/'_' harus didekode
  // byte-exact: atob ketat akan melempar bila push-manager lupa menormalkan.
  let urlSafeKey = null, urlSafeRaw = null;
  for (let tries = 0; tries < 200 && !urlSafeKey; tries++) {
    const dh = crypto.createECDH('prime256v1');
    urlSafeRaw = dh.generateKeys();
    const b64 = b64url(urlSafeRaw);
    if (b64.indexOf('-') >= 0 || b64.indexOf('_') >= 0) urlSafeKey = b64;
  }
  check('K7', 'kunci uji mengandung karakter base64url (-/_)',
    urlSafeKey !== null);
  if (urlSafeKey) {
    const conv2 = AlarmPushManager.urlBase64ToUint8Array(urlSafeKey);
    check('K7', 'decoder menolak mentah base64url & menormalkan dengan benar',
      conv2.length === 65 && Buffer.from(conv2).equals(urlSafeRaw));
  }
  check('K7', 'titik substitusi API_BASE sw.js berfungsi', swApi.API_BASE === GAS_URL);
  // Alat verifikasi dapat hidup di dua layout: paket rilis (DL/tools/)
  // atau repo PWA mandiri (DL/pwa-push-alarm/tools/).
  check('K7', 'alat verifikasi deployment tersedia',
    fs.existsSync(path.join(DL, 'tools', 'verify-deployment.js')) ||
    fs.existsSync(path.join(DL, 'pwa-push-alarm', 'tools', 'verify-deployment.js')));

  // (X-8) Kunci privat skalar pendek (31 byte, ekspor DER membuang nol
  // depan) tetap berfungsi di seluruh jalur GAS -> push service.
  const shortPriv = crypto.randomBytes(31); // skalar < 2^248
  const dhS = crypto.createECDH('prime256v1');
  dhS.setPrivateKey(Buffer.concat([Buffer.alloc(1), shortPriv]));
  const shortPub = b64url(dhS.getPublicKey());
  gasProps.set('VAPID_PUBLIC_KEY', shortPub);
  gasProps.set('VAPID_PRIVATE_KEY', b64url(shortPriv)); // 31 byte!
  const mk8 = pushService.requests.length;
  gasCtx.sendAlarmToAll({ id: 'ALM-X8-1', title: 'UJI SKALAR PENDEK', severity: 'info' });
  const reqX8 = pushService.requests[mk8];
  let x8ok = false;
  try {
    const m8 = /^vapid t=(.+), k=(.+)$/.exec(reqX8.headers['Authorization']);
    const p8 = m8[1].split('.');
    const k8 = await wc.subtle.importKey('raw', new Uint8Array(b64urlToBuf(shortPub)),
      { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
    x8ok = await wc.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, k8,
      new Uint8Array(b64urlToBuf(p8[2])),
      new TextEncoder().encode(p8[0] + '.' + p8[1]));
    x8ok = x8ok && m8[2] === shortPub;
  } catch (e) { x8ok = false; }
  check('K7', 'kunci privat 31 byte (temuan X-8) tetap menghasilkan JWT valid', x8ok);
  gasProps.set('VAPID_PUBLIC_KEY', VAPID_PUB);   // pulihkan kunci utama
  gasProps.set('VAPID_PRIVATE_KEY', VAPID_PRIV);

  /* ---------------- K8: Hardening produksi ---------------- */
  console.log('\n--- K8 Hardening produksi (anti-spam, subjek, TLS) ---');

  // (a) rate-limit testPush publik
  const tpMark = pushService.requests.length;
  const tp1 = gasGet('testPush');
  check('K8', 'testPush pertama diterima (mengirim push uji)',
    tp1.ok === true && pushService.requests.length === tpMark + 1);
  const tp2 = gasGet('testPush');
  check('K8', 'testPush kedua dalam interval ditolak (rate-limit)',
    tp2.ok === false && /ditolak/i.test(tp2.message || ''));
  check('K8', 'testPush yang ditolak TIDAK memicu kirim push',
    pushService.requests.length === tpMark + 1);

  // (b) override interval tidak valid -> fallback konfigurasi (tetap ditolak)
  gasProps.set('TEST_PUSH_MIN_INTERVAL_MS', 'bukan-angka');
  const tp3 = gasGet('testPush');
  check('K8', 'interval tidak valid -> fallback konfigurasi (ditolak)',
    tp3.ok === false);

  // (c) override 0 = tanpa batas
  gasProps.set('TEST_PUSH_MIN_INTERVAL_MS', '0');
  const tp4 = gasGet('testPush');
  check('K8', 'override interval 0 mengizinkan testPush', tp4.ok === true);

  // (d) hapus property -> kembali ke konfigurasi default
  gasProps.delete('TEST_PUSH_MIN_INTERVAL_MS');
  const tp5 = gasGet('testPush');
  check('K8', 'tanpa property: rate-limit default aktif lagi', tp5.ok === false);

  // (e) VAPID_SUBJECT override menimpa PUSH_CONFIG.SUBJECT
  const subjOverride = 'mailto:ops-monitoring@farm.example.id';
  gasProps.set('VAPID_SUBJECT', subjOverride);
  gasCtx.sendAlarmToAll({ id: 'ALM-SUBJ-1', title: 'UJI SUBJEK', severity: 'info' });
  const reqSubj = pushService.requests[pushService.requests.length - 1];
  const mSubj = /^vapid t=(.+), k=(.+)$/.exec(reqSubj.headers['Authorization']);
  const claimsSubj = JSON.parse(b64urlToBuf(mSubj[1].split('.')[1]).toString('utf8'));
  check('K8', 'VAPID_SUBJECT Script Property menimpa subjek JWT',
    claimsSubj.sub === subjOverride);
  gasProps.delete('VAPID_SUBJECT');
  gasCtx.sendAlarmToAll({ id: 'ALM-SUBJ-2', title: 'UJI SUBJEK', severity: 'info' });
  const reqSubj2 = pushService.requests[pushService.requests.length - 1];
  const mSubj2 = /^vapid t=(.+), k=(.+)$/.exec(reqSubj2.headers['Authorization']);
  const claimsSubj2 = JSON.parse(b64urlToBuf(mSubj2[1].split('.')[1]).toString('utf8'));
  check('K8', 'tanpa VAPID_SUBJECT -> fallback PUSH_CONFIG.SUBJECT',
    claimsSubj2.sub === gasCtx.PUSH_CONFIG.SUBJECT);

  // (f) firmware: verifikasi TLS root CA aktif default
  const fwSrcFull = fs.readFileSync(FW_INO, 'utf8');
  check('K8', 'FW memverifikasi rantai TLS: setCACert GTS_ROOT_R1 + R4',
    fwCode.indexOf('setCACert(GTS_ROOT_R1)') >= 0 &&
    fwCode.indexOf('setCACert(GTS_ROOT_R4)') >= 0);
  check('K8', 'setInsecure hanya di balik TLS_SKIP_CERT_VERIFY (opt-in)',
    fwCode.indexOf('#ifdef TLS_SKIP_CERT_VERIFY') >= 0 &&
    fwCode.indexOf('#ifdef TLS_SKIP_CERT_VERIFY') < fwCode.indexOf('setInsecure'));
  check('K8', 'versi firmware 1.1.0 tunggal-sumber (FW_VERSION)',
    /#define FW_VERSION "1\.1\.0"/.test(fwSrcFull) &&
    fwCode.indexOf('dev["fw"]') >= 0 &&
    fwSrcFull.indexOf('dev["fw"]       = FW_VERSION;') >= 0);

  // (g) PEM tertanam byte-identik dengan root resmi pki.goog
  // (sidik jari SHA-256 DER sertifikat, publik, dipinkan - sama seperti
  //  keluaran: openssl x509 -fingerprint -sha256)
  const pinned = {
    GTS_ROOT_R1: 'd947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf',
    GTS_ROOT_R4: '349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d'
  };
  for (const name of Object.keys(pinned)) {
    const mm = new RegExp('const char\\* ' + name + ' =([\\s\\S]*?);', '').exec(fwSrcFull);
    let fpOk = false;
    if (mm) {
      const literals = mm[1].match(/"([^"]*)"/g).map(function (s) { return s.slice(1, -1); });
      const pem = literals.join('').replace(/\\n/g, '\n');
      const b64 = pem.replace(/-----(BEGIN|END) CERTIFICATE-----/g, '')
        .replace(/\s+/g, '');
      const der = Buffer.from(b64, 'base64');
      const fp = crypto.createHash('sha256').update(der).digest('hex');
      fpOk = der.length > 0 && fp === pinned[name];
    }
    check('K8', 'PEM ' + name + ' byte-identik root pki.goog (sidik jari DER)', fpOk);
  }

  /* ---------------- Ringkasan ---------------- */
  console.log('\n==============================================================');
  console.log(' RINGKASAN PER KONTRAK (Tabel 11)');
  console.log('==============================================================');
  const order = ['K1', 'K2', 'K3', 'K4', 'K5', 'K6', 'K7', 'K8'];
  const names = {
    K1: 'Langganan (PWA-GAS)',
    K2: 'Penghapusan (PWA-GAS)',
    K3: 'Payload alarm (GAS-PWA)',
    K4: 'ACK (PWA-GAS)',
    K5: 'Deep-link (PWA internal)',
    K6: 'Ambang alarm (FW-GAS)',
    K7: 'Kunci VAPID (GAS-PWA)',
    K8: 'Hardening produksi'
  };
  for (const k of order) {
    const t = tally[k] || { pass: 0, fail: 0 };
    console.log('  ' + k + ' ' + names[k].padEnd(26) +
      ': ' + String(t.pass).padStart(2) + ' lulus, ' + t.fail + ' gagal');
  }
  console.log('--------------------------------------------------------------');
  console.log('  TOTAL: ' + totalPass + ' lulus, ' + totalFail + ' gagal');
  console.log(totalFail === 0
    ? '  SEMUA KONTRAK TABEL 11 LULUS VERIFIKASI SILANG'
    : '  ADA KONTRAK YANG GAGAL - PERBAIKI SEBELUM DEPLOYMENT');
  console.log('==============================================================');
  process.exit(totalFail === 0 ? 0 : 1);
}

main().catch((err) => {
  console.error('KESALAHAN HARNESS:', err);
  process.exit(1);
});
