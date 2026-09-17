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
  // [K10 round-3] status per-endpoint persisten — untuk skenario partial
  // delivery (satu subscriber 503, lainnya 201) tanpa memengaruhi suite lama.
  endpointStatus: {},
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
    if (this.endpointStatus[url] !== undefined) status = this.endpointStatus[url];
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
  // [P1 concurrency 2026-09-16] LockService mock — tryLock selalu sukses
  // (eksekusi serial diuji lewat urutan panggilan, bukan kontensi nyata).
  LockService: {
    getScriptLock() {
      return { tryLock: () => true, releaseLock() {} };
    }
  },
  // [P0-1] ScriptApp.getService().getUrl() — sumber apiBase payload ACK.
  ScriptApp: {
    getService() { return { getUrl: () => GAS_URL }; }
  },
  ContentService: {
    MimeType: { JSON: 'JSON' },
    createTextOutput(t) { return { _text: t, setMimeType() { return this; } }; }
  },
  Utilities: {
    getUuid: () => crypto.randomUUID(),
    // [audit p.482] HMAC untuk ACK capability token (node crypto ≈ GAS).
    computeHmacSha256Signature: (message, secret) =>
      Array.from(crypto.createHmac('sha256', String(secret)).update(String(message)).digest()),
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

/* ======================= [K10 round-3] Runtime GAS CANONICAL =======================
 * Auditor round-3 (2026-09-17): 181 asersi lama hanya menguji backend push
 * LEGACY (push-alarm/gas/Code.gs) — sedangkan seluruh perbaikan P1 round-3
 * (multi-device isolation, per-sub delivery cursor, lifecycle write-back,
 * lock discipline, kapasitas Script Properties) hidup di backend CANONICAL
 * (code.gs/Code.gs + code.gs/PushService.gs). Konteks ini menjalankan
 * dispatcher canonical PENUH dengan mock yang MENEGAKKAN batas nyata GAS:
 *   - PropertiesService: 9 KB/value dan 500 KB total — setProperty LEMPAR
 *     bila dilanggar (test kapasitas jadi mekanis, bukan klaim).
 *   - LockService terinstrumentasi: menghitung (a) fetch jaringan yang
 *     terjadi selama lock dipegang (harus 0 — P1-D), (b) tryLock bersarang
 *     (harus 0 — P1-D), (c) mode kontensi forceBusy (ingest harus 503
 *     fail-closed tanpa setengah-mutasi).
 *   - CacheService + SpreadsheetApp (Config/Devices sheet) untuk jalur
 *     autentikasi canonical (token legacy + fleet gate). */

const CANON_DIR = process.env.MONITORIOT_CANON_DIR ||
  (fs.existsSync(path.join(ROOT, '..', 'code.gs', 'PushService.gs'))
    ? path.join(ROOT, '..', 'code.gs') : null);

const canonProps = new Map();
const canonPropsStats = { setCalls: 0, getCalls: 0, overValueThrows: 0, totalThrows: 0 };
const CANON_VALUE_LIMIT = 9 * 1024;     // batas nyata GAS: 9 KB per value
const CANON_TOTAL_LIMIT = 500 * 1024;   // batas nyata GAS: 500 KB total
const CanonPropertiesService = {
  getScriptProperties() {
    return {
      getProperty(k) {
        canonPropsStats.getCalls++;
        return canonProps.has(k) ? canonProps.get(k) : null;
      },
      setProperty(k, v) {
        canonPropsStats.setCalls++;
        const val = String(v);
        if (val.length > CANON_VALUE_LIMIT) {
          canonPropsStats.overValueThrows++;
          throw new Error('PropertiesService (mock penegak): value > 9KB — ' + k);
        }
        let total = val.length;
        canonProps.forEach((vv, kk) => { if (kk !== k) total += String(vv).length; });
        if (total > CANON_TOTAL_LIMIT) {
          canonPropsStats.totalThrows++;
          throw new Error('PropertiesService (mock penegak): total store > 500KB');
        }
        canonProps.set(k, val);
      },
      deleteProperty(k) { canonProps.delete(k); },
      getProperties() {
        const o = {};
        canonProps.forEach((v, k) => { o[k] = v; });
        return o;
      }
    };
  }
};

const canonLock = {
  held: 0,
  fetchesUnderLock: 0,
  nestedTryLock: 0,
  forceBusy: false,
  getScriptLock() {
    const self = this;
    return {
      tryLock() {
        if (self.forceBusy) return false;
        if (self.held > 0) self.nestedTryLock++;
        self.held++;
        return true;
      },
      releaseLock() { self.held--; }
    };
  }
};

function canonMakeSheet(name, rows) {
  const data = rows.map((r) => r.slice());
  return {
    getName: () => name,
    getDataRange() { return { getValues: () => data.map((r) => r.slice()) }; },
    getRange(row, col, numRows, numCols) {
      return {
        getValues() {
          const out = [];
          for (let r = row - 1; r < row - 1 + numRows && r < data.length; r++) {
            const base = data[r] || [];
            const line = [];
            for (let c = col - 1; c < col - 1 + numCols; c++) {
              line.push(base[c] !== undefined ? base[c] : '');
            }
            out.push(line);
          }
          return out;
        },
        setValues(vals) {
          for (let r = 0; r < (vals || []).length; r++) {
            const tr = row - 1 + r;
            while (data.length <= tr) data.push([]);
            for (let c = 0; c < vals[r].length; c++) {
              data[tr][col - 1 + c] = vals[r][c];
            }
          }
        }
      };
    },
    getLastRow: () => data.length,
    getLastColumn: () => data.reduce((m, r) => Math.max(m, r.length), 0),
    appendRow(row) { data.push(row.slice()); },
    setFrozenRows() {},
    setColumnWidth() {},
    clear() { data.length = 0; }
  };
}

const CANON_AUTH_TOKEN = 'canon-auth-token-round3-2026-09';
const CANON_ADMIN_TOKEN = 'canon-admin-token-round3-2026-09';
const DEV_A = 'esp32-greenhouse-01';
const DEV_B = 'esp32-greenhouse-02';

const canonSheets = {
  'Config': canonMakeSheet('Config', [
    ['PARAMETER', 'VALUE'],
    ['AUTH_TOKEN', CANON_AUTH_TOKEN],
    ['ADMIN_TOKEN', CANON_ADMIN_TOKEN],
    ['DEVICE_KEY', DEV_A],
    ['TIMEZONE', 'Asia/Jakarta']
  ]),
  'Devices': canonMakeSheet('Devices', [
    ['device_key', 'secret', 'label', 'last_nonce', 'last_ts', 'firmware_type'],
    [DEV_A, 'secret-dev-a', 'Greenhouse 1', '', '', 'modular'],
    [DEV_B, 'secret-dev-b', 'Greenhouse 2', '', '', 'modular']
  ])
};

const canonCache = new Map();
let canonCtx = null;
function setupCanonCtx() {
  if (!CANON_DIR) return false;
  canonCtx = vm.createContext({
    PropertiesService: CanonPropertiesService,
    UrlFetchApp: {
      fetch(u, o) {
        // [P1-D] bukti mekanis: network I/O SELAMA lock dipegang = pelanggaran
        if (canonLock.held > 0) canonLock.fetchesUnderLock++;
        return pushService.fetch(u, o);
      }
    },
    LockService: canonLock,
    ScriptApp: { getService() { return { getUrl: () => GAS_URL }; } },
    ContentService: {
      MimeType: { JSON: 'JSON' },
      createTextOutput(t) { return { _text: t, setMimeType() { return this; } }; }
    },
    Utilities: {
      getUuid: () => crypto.randomUUID(),
      computeHmacSha256Signature: (message, secret) =>
        Array.from(crypto.createHmac('sha256', String(secret)).update(String(message)).digest()),
      computeDigest: (algo, data) =>
        Array.from(crypto.createHash('sha256').update(String(data)).digest()),
      formatDate: (d) => new Date(d).toISOString(),
      newBlob: (bytes, contentType) => ({
        __blob: true, bytes: Array.from(bytes || []), contentType
      }),
      Charset: { UTF_8: 'UTF_8' },
      DigestAlgorithm: { SHA_256: 'SHA_256' }
    },
    CacheService: {
      getScriptCache() {
        return {
          get: (k) => (canonCache.has(k) ? canonCache.get(k) : null),
          put: (k, v) => canonCache.set(k, String(v)),
          remove: (k) => canonCache.delete(k)
        };
      }
    },
    SpreadsheetApp: {
      getActiveSpreadsheet() {
        return {
          getSheetByName(n) { return canonSheets[n] || null; },
          insertSheet(n) { canonSheets[n] = canonMakeSheet(n, []); return canonSheets[n]; }
        };
      },
      getUi() { return { alert() {} }; }
    },
    Logger: { log() {} },
    console: { log() {}, warn() {}, error() {} }
  });
  vm.runInContext(fs.readFileSync(path.join(GAS, GAS_CORE_FILE), 'utf8'),
    canonCtx, { filename: 'canonical-WebPushCore.gs' });
  vm.runInContext(fs.readFileSync(path.join(CANON_DIR, 'Code.gs'), 'utf8'),
    canonCtx, { filename: 'canonical-Code.gs' });
  vm.runInContext(fs.readFileSync(path.join(CANON_DIR, 'PushService.gs'), 'utf8'),
    canonCtx, { filename: 'canonical-PushService.gs' });
  canonProps.set('VAPID_PUBLIC_KEY', VAPID_PUB);
  canonProps.set('VAPID_PRIVATE_KEY', VAPID_PRIV);
  canonProps.set('PUSH_TOKENS', JSON.stringify([
    { deviceId: DEV_A, token: 'push-token-dev-a-round3' },
    { deviceId: DEV_B, token: 'push-token-dev-b-round3' }
  ]));
  return true;
}

function canonPost(obj) {
  const out = canonCtx.doPost({ postData: { contents: JSON.stringify(obj) } });
  return JSON.parse(out._text);
}
function canonIngest(deviceId, sensors, reportedAt) {
  return canonPost({ action: 'PUSH_ALARM_INGEST', token: CANON_AUTH_TOKEN,
    device_key: deviceId, data: { sensors, reportedAt } });
}
/** Langganan browser baru untuk device tertentu (lewat dispatcher canonical). */
function canonMakeBrowserSub(deviceId, token, label) {
  const ecdh = crypto.createECDH('prime256v1');
  ecdh.generateKeys();
  const auth = crypto.randomBytes(16);
  const endpoint = 'https://push.test.local/canon/' + label + '/' +
    crypto.randomBytes(8).toString('hex');
  endpointRegistry[endpoint] = { priv: ecdh.getPrivateKey(), pub: ecdh.getPublicKey(), auth };
  const res = canonPost({
    action: 'PUSH_SUBSCRIBE',
    device: { id: deviceId },
    token: token,
    endpoint: endpoint,
    keys: { p256dh: b64url(ecdh.getPublicKey()), auth: b64url(auth) },
    context: { lang: 'id-ID', tz: 'Asia/Jakarta', ua: 'K10-harness' }
  });
  return { res, endpoint };
}

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
  // [p.493] sessionStorage — kredensial push kini hidup selama sesi saja.
  sessionStorage: (() => {
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

// [AUDIT p.493 2026-09-16] Kontrak GAS K-7: subscribe/unsubscribe WAJIB
// membawa device.id + token. Seed kredensial ke sessionStorage mock PWA
// (jalur operator baru). Jalur LEGACY localStorage diuji terpisah pada
// bagian K9 (migrasi sekali-jalan).
pageCtx.sessionStorage.setItem('push.deviceId', DEVICE_ID);
pageCtx.sessionStorage.setItem('push.deviceToken', DEVICE_TOKEN);

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
// [P0-1 2026-09-16] sw.js tidak lagi menanam konstanta API_BASE. Konfigurasi
// runtime dikirim via postMessage dari halaman — persis jalur app.js baru.
const swSrc = swSrcRaw;

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
// [P0-1] Ekpose helper runtime sebagai ganti konstanta API_BASE yang hilang.
vm.runInContext('this.__swApi = { getApiBase: (typeof getApiBase_ === "function") ? getApiBase_ : null };', swCtx, { filename: 'sw-api.js' });
const swApi = swCtx.__swApi;

// [AUDIT p.493 / P0-1 2026-09-16] Kirim konfigurasi runtime + kredensial
// perangkat ke konteks SW — persis yang dilakukan js/app.js pada deployment
// nyata setelah register(). resubscribe() dan ACK membutuhkannya.
swHandlers.message({
  data: {
    type: 'PLTS_PUSH_ALARM_RUNTIME_CONFIG',
    config: { apiBase: GAS_URL }
  }
});
swHandlers.message({
  data: {
    type: 'PLTS_PUSH_ALARM_DEVICE_CREDENTIALS',
    credentials: { deviceId: DEVICE_ID, token: DEVICE_TOKEN }
  }
});

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
  // [SELF-AUDIT 2026-09-16] Kontrol negatif K-7 dulu: subscribe TANPA
  // kredensial harus DITOLAK GAS (fail-closed) — membuktikan gerbangnya
  // aktif sebelum membuktikan jalur positifnya.
  {
    const savedId = pageCtx.sessionStorage.getItem('push.deviceId');
    const savedTok = pageCtx.sessionStorage.getItem('push.deviceToken');
    pageCtx.sessionStorage.removeItem('push.deviceId');
    pageCtx.sessionStorage.removeItem('push.deviceToken');
    const anon = new AlarmPushManager(GAS_URL, VAPID_PUB);
    const rAnon = await anon.enable();
    check('K1', 'subscribe TANPA device.id+token DITOLAK GAS (K-7)',
      rAnon.ok === false && /perangkat/i.test(String(rAnon.message)));
    pageCtx.sessionStorage.setItem('push.deviceId', savedId);
    pageCtx.sessionStorage.setItem('push.deviceToken', savedTok);
  }
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
  // [SELF-AUDIT 2026-09-16] Regresi kontrak K-7: payload subscribe WAJIB
  // membawa autentikasi perangkat (device.id + token) — tanpa itu GAS
  // menolak (lihat kontrol negatif di atas). Post TERAKHIR dipakai karena
  // kontrol negatif di atas juga mengirim POST subscribe (tanpa kredensial).
  {
    const subReq = [...gasRequests].reverse().find((r) => r.method === 'POST' &&
      r.body.indexOf('"subscribe"') >= 0);
    const subBody = subReq ? JSON.parse(subReq.body) : {};
    check('K1', 'payload subscribe membawa device.id + token (K-7)',
      subBody.device && subBody.device.id === DEVICE_ID &&
      subBody.token === DEVICE_TOKEN);
  }

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
    action: 'subscribe',
    device: { id: DEVICE_ID },
    token: DEVICE_TOKEN,
    endpoint: stored1.endpoint,
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

  const unkRes = gasPost({
    action: 'unsubscribe',
    device: { id: DEVICE_ID },
    token: DEVICE_TOKEN,
    endpoint: 'https://push.test.local/tak-ada'
  });
  check('K2', 'unsubscribe endpoint tak dikenal -> removed 0, tetap ok',
    unkRes.ok === true && unkRes.removed === 0);
  // [SELF-AUDIT 2026-09-16] Kontrak K-7 simetris pada unsubscribe: tanpa
  // kredensial / token salah DITOLAK fail-closed — menutup jalur DoS
  // silent-unsubscribe bagi pihak yang mengetahui URL endpoint korban.
  const unkNoAuth = gasPost({
    action: 'unsubscribe',
    endpoint: 'https://push.test.local/tak-ada'
  });
  check('K2', 'unsubscribe TANPA autentikasi DITOLAK (K-7 simetris)',
    unkNoAuth.ok === false);
  const unkBadTok = gasPost({
    action: 'unsubscribe',
    device: { id: DEVICE_ID },
    token: 'token-salah',
    endpoint: 'https://push.test.local/tak-ada'
  });
  check('K2', 'unsubscribe dengan token SALAH DITOLAK',
    unkBadTok.ok === false);

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
  const ackTokenOf = (id) => gasCtx.makeAckToken_(id);
  await dispatchClick('ack', notifData);
  const ackReq = [...gasRequests].reverse()
    .find((r) => r.method === 'POST' && r.body.indexOf('"ackAlarm"') >= 0);
  check('K4', 'ACK terkirim dari aksi notifikasi ke GAS', !!ackReq);
  const ackBody = ackReq ? JSON.parse(ackReq.body) : {};
  check('K4', 'alarmId pada ACK identik dengan payload terkirim',
    ackBody.alarmId === 'ALM-DIRECT-001');
  // [audit p.482] Capability token ikut menempel pada notifikasi dan dikirim
  // bersama ACK — inilah otorisasinya, bukan sekadar pengetahuan URL+alarmId.
  check('K4', 'ACK membawa capability token (p.482)',
    typeof ackBody.ackToken === 'string' && ackBody.ackToken.length === 64);
  check('K4', 'payload push membawa ackToken (p.482)',
    typeof (notifData && notifData.ackToken) === 'string');
  check('K4', 'GAS menerima ACK (ok) termasuk jalur sendAlarmToAll langsung (temuan X-6)',
    ackReq && ackReq.response && ackReq.response.ok === true);
  check('K4', 'respons memuat alarmId yang sama',
    ackReq && ackReq.response && ackReq.response.alarmId === 'ALM-DIRECT-001');
  const entry = alarmLog().find((e) => e.id === 'ALM-DIRECT-001');
  check('K4', 'ALARM_LOG mencatat acknowledgedAt', entry && !!entry.acknowledgedAt);
  const ackAgain = gasPost({ action: 'ackAlarm', alarmId: 'ALM-DIRECT-001',
    ackToken: ackTokenOf('ALM-DIRECT-001') });
  check('K4', 'ACK idempoten (kali kedua tetap ok)', ackAgain.ok === true);
  check('K4', 'ACK tak dikenal ditolak eksplisit',
    gasPost({ action: 'ackAlarm', alarmId: 'TIDAK-ADA',
      ackToken: ackTokenOf('TIDAK-ADA') }).ok === false);
  // [audit p.482] NEGATIVE: tokenless / wrong-token ACK rejected (fail-closed).
  check('K4', 'p.482: ACK TANPA token ditolak (fail-closed)',
    gasPost({ action: 'ackAlarm', alarmId: 'ALM-DIRECT-001' }).ok === false);
  check('K4', 'p.482: ACK dengan token SALAH ditolak',
    gasPost({ action: 'ackAlarm', alarmId: 'ALM-DIRECT-001',
      ackToken: 'deadbeef'.repeat(8) }).ok === false);
  check('K4', 'p.482: token alarm LAIN tidak berlaku untuk alarm ini',
    gasPost({ action: 'ackAlarm', alarmId: 'ALM-DIRECT-001',
      ackToken: ackTokenOf('ALM-LAIN-999') }).ok === false);

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

  /* ---------------- K6b: Firmware MODULAR (kontrak GAS HEAD) ---------------- */
  // [SELF-AUDIT 2026-09-16] K6 di atas memeriksa snapshot monolitik lawas;
  // firmware PRODUKSI sekarang modular (firmware_v1.ino + modul) dengan
  // kontrak GAS yang lebih kuat: envelope TELEMETRY bertanda tangan
  // HMAC-SHA256 (AI/GasAdvisor.cpp), bukan ingest+token polos. K6b memastikan
  // kontrak HEAD itu tetap dipatuhi DAN prinsip pengirim tunggal berlaku di
  // seluruh pohon sumber modular.
  console.log('\n--- K6b Firmware modular (TELEMETRY bertanda tangan, GAS HEAD) ---');
  const FW_MOD = path.join(DL, 'firmware');
  if (fs.existsSync(path.join(FW_MOD, 'AI', 'GasAdvisor.cpp'))) {
    const gasAdvisorSrc = fs.readFileSync(path.join(FW_MOD, 'AI', 'GasAdvisor.cpp'), 'utf8');
    const gasAdvisorCode = gasAdvisorSrc.replace(/\/\*[\s\S]*?\*\//g, '')
      .replace(/^\s*\/\/.*$/gm, '');
    check('K6b', 'FW modular mengirim action=TELEMETRY (kontrak HEAD)',
      gasAdvisorCode.indexOf('envelope["action"] = "TELEMETRY"') >= 0);
    check('K6b', 'FW modular menandatangani request GAS (HMAC-SHA256)',
      /_signRequest/.test(gasAdvisorCode) && /HMAC-SHA256|sha256Hmac|sha256/i.test(gasAdvisorCode));
    // Prinsip pengirim tunggal pada seluruh pohon modular: tidak ada jalur
    // push/ACK/subscribe di firmware produksi.
    let modFiles = [];
    (function collect(dir) {
      for (const name of fs.readdirSync(dir)) {
        const p = path.join(dir, name);
        const st = fs.statSync(p);
        if (st.isDirectory() && !name.startsWith('.')) collect(p);
        else if (/\.(cpp|h|ino)$/.test(name)) modFiles.push(p);
      }
    })(FW_MOD);
    check('K6b', 'pohon sumber modular dapat dibaca (' + modFiles.length + ' file)',
      modFiles.length > 20);
    const violators = modFiles.filter((p) => {
      const src = fs.readFileSync(p, 'utf8')
        .replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '');
      return /testPush|sendAlarmToAll|ackAlarm|"subscribe"/.test(src);
    });
    check('K6b', 'prinsip pengirim tunggal: pohon modular tanpa jalur push/ACK',
      violators.length === 0);
  } else {
    check('K6b', 'firmware modular tidak ada dalam checkout — LEWATI (opsional)', true);
  }

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
  check('K6', 'id kejadian = ALM-<device>-<sensor-slug>-<waktu> (P1-A)',
    tempEventId.indexOf('ALM-' + DEVICE_ID + '-suhu-greenhouse-1-') === 0);
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
    res.triggered.length === 1 && res.triggered[0].indexOf('ALM-' + DEVICE_ID + '-kelembapan-tanah-') === 0);
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
    res.triggered.length === 1 && res.triggered[0].indexOf('ALM-' + DEVICE_ID + '-suhu-greenhouse-1-') === 0);
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
    nf.options.data.alarmId && nf.options.data.alarmId.indexOf('ALM-' + DEVICE_ID + '-suhu-greenhouse-1-') === 0);
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
  // [P0-1 2026-09-16] Template config kini KOSONG (bukan placeholder).
  // Nilai produksi datang dari build-config.js (env) atau setup runtime.
  const cfgUrlMatch = /API_BASE:\s*['"]([^'"]*)['"]/.exec(cfgSrc);
  const cfgUrl = cfgUrlMatch ? cfgUrlMatch[1] : '';
  check('K7', 'template config.js KOSONG & bebas placeholder (nilai via build/setup)',
    cfgUrl === '' && cfgSrc.indexOf('GANTI_DENGAN') === -1);
  check('K7', 'sw.js TIDAK lagi menanam konstanta API_BASE (runtime config)',
    /const API_BASE\s*=/.test(swSrcRaw) === false);
  const cfgMod = require(path.join(PWA, 'js', 'config.js'));
  check('K7', 'config.js menyediakan slot VAPID_PUBLIC_KEY (build-time)',
    typeof cfgMod.APP_CONFIG.VAPID_PUBLIC_KEY === 'string');
  check('K7', 'config.js mengekspor status APP_PROVISIONED (kejujuran deployment)',
    cfgMod.APP_PROVISIONED === false);
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
  check('K7', 'getApiBase_() SW menurunkan URL dari runtime config halaman',
    typeof swApi.getApiBase === 'function' &&
    (await swApi.getApiBase()) === GAS_URL);
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
  console.log('\n--- K8 Hardening produksi (testPush terautentikasi, subjek, TLS) ---');

  // (a0) [P1 hardening 2026-09-16] testPush GET kini DITUTUP default —
  // "rate limiting adalah mitigasi DoS, bukan authorization".
  const tpMark = pushService.requests.length;
  const tp0 = gasGet('testPush');
  check('K8', 'testPush GET publik DITOLAK default (auth wajib)',
    tp0.ok === false && /dinonaktifkan/i.test(tp0.message || ''));
  check('K8', 'penolakan GET tidak memicu kirim push',
    pushService.requests.length === tpMark);

  // (a1) testPush POST tanpa kredensial -> DITOLAK
  const tpAnon = gasPost({ action: 'testPush' });
  check('K8', 'testPush POST tanpa device.id+token DITOLAK',
    tpAnon.ok === false && /autentikasi/i.test(tpAnon.message || ''));

  // (a2) testPush POST dengan kredensial -> diterima (rate limit mulai)
  const tp1 = gasPost({ action: 'testPush',
    device: { id: DEVICE_ID }, token: DEVICE_TOKEN });
  check('K8', 'testPush POST terautentikasi diterima (mengirim push uji)',
    tp1.ok === true && pushService.requests.length === tpMark + 1);
  const tp2 = gasPost({ action: 'testPush',
    device: { id: DEVICE_ID }, token: DEVICE_TOKEN });
  check('K8', 'testPush kedua dalam interval ditolak (rate-limit)',
    tp2.ok === false && /ditolak/i.test(tp2.message || ''));
  check('K8', 'testPush yang ditolak TIDAK memicu kirim push',
    pushService.requests.length === tpMark + 1);

  // (b) override interval tidak valid -> fallback konfigurasi (tetap ditolak)
  gasProps.set('TEST_PUSH_MIN_INTERVAL_MS', 'bukan-angka');
  const tp3 = gasPost({ action: 'testPush',
    device: { id: DEVICE_ID }, token: DEVICE_TOKEN });
  check('K8', 'interval tidak valid -> fallback konfigurasi (ditolak)',
    tp3.ok === false);

  // (c) override 0 = tanpa batas
  gasProps.set('TEST_PUSH_MIN_INTERVAL_MS', '0');
  const tp4 = gasPost({ action: 'testPush',
    device: { id: DEVICE_ID }, token: DEVICE_TOKEN });
  check('K8', 'override interval 0 mengizinkan testPush', tp4.ok === true);

  // (d) hapus property -> kembali ke konfigurasi default
  gasProps.delete('TEST_PUSH_MIN_INTERVAL_MS');
  const tp5 = gasPost({ action: 'testPush',
    device: { id: DEVICE_ID }, token: DEVICE_TOKEN });
  check('K8', 'tanpa property: rate-limit default aktif lagi', tp5.ok === false);

  // (d2) jendela migrasi eksplisit: TEST_PUSH_ALLOW_PUBLIC=true membuka GET.
  // Reset gerbang rate-limit agar menguji LOGIKA allow-public, bukan jam.
  gasProps.delete('TEST_PUSH_LAST_AT');
  gasProps.set('TEST_PUSH_ALLOW_PUBLIC', 'true');
  const tp6 = gasGet('testPush');
  check('K8', 'TEST_PUSH_ALLOW_PUBLIC=true membuka GET (jendela migrasi)',
    tp6.ok === true);
  gasProps.delete('TEST_PUSH_ALLOW_PUBLIC');
  const tp7 = gasGet('testPush');
  check('K8', 'setelah property dihapus, GET kembali tertutup', tp7.ok === false);

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

  /* ---------------- K9: p.493 / P0-3 — push hardening baru ---------------- */
  console.log('\n--- K9 Push hardening 2026-09-16 (p.493, P0-3, ownership) ---');

  // (a) [p.493] PUSH_TOKENS: token langganan bisa subscribe,
  //     TIDAK bisa ingest (pemisahan kapabilitas).
  const PUSH_TOKEN = 'token-push-khusus-langganan-2026';
  gasProps.set('PUSH_TOKENS',
    JSON.stringify([{ deviceId: DEVICE_ID, token: PUSH_TOKEN }]));
  {
    const savedTok = pageCtx.sessionStorage.getItem('push.deviceToken');
    pageCtx.sessionStorage.setItem('push.deviceToken', PUSH_TOKEN);
    const pmPushTok = new AlarmPushManager(GAS_URL, VAPID_PUB);
    const rPushTok = await pmPushTok.enable();
    check('K9', 'PUSH_TOKENS diterima untuk subscribe (kapabilitas langganan)',
      rPushTok.ok === true);
    check('K9', 'record langganan menyimpan deviceId (binding fleet)',
      storedSubs().some(function (s) { return s.deviceId === DEVICE_ID; }));
    pageCtx.sessionStorage.setItem('push.deviceToken', savedTok);
  }
  check('K9', 'PUSH_TOKENS DITOLAK untuk ingest (bukan kredensial telemetry)',
    gasPost({ action: 'ingest', token: PUSH_TOKEN,
      device: { id: DEVICE_ID },
      sensors: [{ name: 'Suhu Greenhouse 1', value: 41, unit: 'C' }] }).ok === false);
  check('K9', 'push token salah tetap ditolak untuk subscribe',
    gasPost({ action: 'subscribe', device: { id: DEVICE_ID },
      token: 'salah', endpoint: 'https://push.test.local/x',
      keys: { p256dh: 'a', auth: 'b' } }).ok === false);

  // (b) [P1 ownership] unsubscribe tidak boleh menghapus langganan device lain
  const subsBefore = storedSubs().length;
  gasProps.set('PUSH_TOKENS',
    JSON.stringify([{ deviceId: DEVICE_ID, token: PUSH_TOKEN },
      { deviceId: 'esp32-lain', token: 'token-device-lain' }]));
  const ownedSub = storedSubs().find(function (s) { return s.deviceId === DEVICE_ID; });
  const crossUnsub = gasPost({ action: 'unsubscribe',
    device: { id: 'esp32-lain' }, token: 'token-device-lain',
    endpoint: ownedSub ? ownedSub.endpoint : 'https://push.test.local/tidak-ada' });
  check('K9', 'unsubscribe device lain TIDAK menghapus langganan device ini',
    crossUnsub.ok === true && storedSubs().length === subsBefore);

  // (c) [P0-3] Outbox durable: edge alarm menghasilkan AlarmEvent dengan
  //     status delivery, dan kegagalan pengiriman tetap PENDING untuk retry.
  gasProps.delete('PUSH_TOKENS');
  const evMark = JSON.parse(gasProps.get('ALARM_EVENTS') || '[]').length;
  const rRaise = gasPost(fwReport([
    { name: 'Suhu Greenhouse 1', value: 42.0, unit: 'C', alarm: true, severity: 'critical' },
    S_HUM(70, false), S_SOIL(50, false)
  ], fwT()));
  let events = JSON.parse(gasProps.get('ALARM_EVENTS') || '[]');
  check('K9', 'tepi naik membuat AlarmEvent baru di outbox',
    rRaise.ok === true && events.length === evMark + 1);
  const lastEv = events[events.length - 1];
  check('K9', 'AlarmEvent membawa identitas durable (eventId/deviceId/generation)',
    typeof lastEv.eventId === 'string' && lastEv.eventId.indexOf('ALM-') === 0 &&
    lastEv.deviceId === DEVICE_ID && lastEv.generation >= 1);
  check('K9', 'pengiriman sukses tercatat SENT di outbox',
    lastEv.delivery && lastEv.delivery.status === 'SENT' &&
    lastEv.delivery.attempts === 1);

  // (c2) Kegagalan total -> PENDING; setelah backoff jatuh tempo -> retry.
  // (Laporan juga memulihkan suhu dari (c), jadi event RSV ikut tercipta —
  // pilih event RAISED kelembapan-udara secara eksplisit, bukan events[last].)
  pushService.setNextStatus(500);
  gasPost(fwReport([
    { name: 'Kelembapan Udara', value: 20, unit: '%', alarm: true, severity: 'warning' },
    S_TEMP(30, false), S_SOIL(50, false)
  ], fwT()));
  events = JSON.parse(gasProps.get('ALARM_EVENTS') || '[]');
  let failEv = null;
  for (const ev of events) {
    if (ev.state === 'RAISED' && ev.alarmCode === 'kelembapan-udara') failEv = ev;
  }
  check('K9', 'kegagalan pengiriman -> event tetap PENDING (tidak hilang)',
    !!failEv && failEv.delivery.status === 'PENDING' && failEv.delivery.attempts === 1);
  // Paksa backoff jatuh tempo (lastAttemptAt digeser 10 menit ke belakang),
  // lalu ingest netral -> retry event PENDING terjadi.
  events = JSON.parse(gasProps.get('ALARM_EVENTS') || '[]');
  for (const ev of events) {
    if (ev.delivery && ev.delivery.status === 'PENDING' && ev.delivery.lastAttemptAt) {
      ev.delivery.lastAttemptAt = new Date(Date.now() - 10 * 60 * 1000).toISOString();
    }
  }
  gasProps.set('ALARM_EVENTS', JSON.stringify(events));
  const retryBefore = events.filter(function (e) { return e.delivery.status === 'PENDING'; }).length;
  gasPost(fwReport([S_TEMP(30, false), S_HUM(70, false), S_SOIL(50, false)], fwT()));
  events = JSON.parse(gasProps.get('ALARM_EVENTS') || '[]');
  const retryAfter = events.filter(function (e) { return e.delivery.status === 'PENDING'; }).length;
  check('K9', 'event PENDING di-retry setelah backoff jatuh tempo',
    retryAfter < retryBefore);

  // (d) [p.493] Migrasi sekali-jalan: kredensial lama di localStorage
  //     dipindah ke sessionStorage lalu localStorage dibersihkan.
  pageCtx.sessionStorage.removeItem('push.deviceId');
  pageCtx.sessionStorage.removeItem('push.deviceToken');
  pageCtx.localStorage.setItem('push.deviceId', DEVICE_ID);
  pageCtx.localStorage.setItem('push.deviceToken', DEVICE_TOKEN);
  {
    const pmLegacy = new AlarmPushManager(GAS_URL, VAPID_PUB);
    const creds = pmLegacy._deviceCredentials();
    check('K9', 'migrasi: kredensial terbaca dari jalur baru (sessionStorage)',
      creds && creds.deviceId === DEVICE_ID && creds.token === DEVICE_TOKEN);
    check('K9', 'migrasi: localStorage dibersihkan dari kredensial',
      pageCtx.localStorage.getItem('push.deviceId') === null &&
      pageCtx.localStorage.getItem('push.deviceToken') === null);
    check('K9', 'migrasi: nilai tersimpan di sessionStorage',
      pageCtx.sessionStorage.getItem('push.deviceId') === DEVICE_ID);
  }

  // (e) [P0-1] Payload push membawa apiBase (ACK SW tidak lagi bergantung
  //     konstanta sw.js).
  const lastReq = pushService.requests[pushService.requests.length - 1];
  const lastPayload = decryptRequest(lastReq);
  check('K9', 'payload push membawa apiBase untuk ACK SW',
    lastPayload.apiBase === GAS_URL);

  /* ---------------- K10: Canonical push contract (round-3 P1) ---------------- */
  console.log('\n--- K10: Push contract CANONICAL (round-3 P1) ---');
  if (!setupCanonCtx()) {
    check('K10', 'sumber canonical (code.gs/) tersedia untuk diuji', false);
  } else {
    const S = (name, value, unit, alarm, severity, status) =>
      ({ name, value, unit, alarm: !!alarm, severity: severity || 'info', status: status || '' });
    const canonSubIdOf = (ep) =>
      'S' + crypto.createHash('sha256').update(ep).digest('hex').slice(0, 16);
    const canonFind = (id) => canonCtx.pushFindEvent_(id);
    const canonSubs = () => canonCtx.pushSubsAll_();

    // (a) dispatcher + auth gate canonical
    let r10 = canonPost({ action: 'PUSH_ALARM_INGEST', data: { sensors: [S('Suhu', 41, 'C', true)] } });
    check('K10', 'ingest canonical TANPA token ditolak 401', r10.code === 401);
    r10 = canonPost({ action: 'PUSH_ALARM_INGEST', token: CANON_AUTH_TOKEN,
      device_key: 'device-tak-terdaftar', data: { sensors: [S('Suhu', 41, 'C', true)] } });
    check('K10', 'ingest device tak terdaftar ditolak 400 (fleet gate)', r10.code === 400);
    r10 = canonPost({ action: 'PUSH_SUBSCRIBE', device: { id: DEV_A }, endpoint: 'https://x/', keys: { p256dh: 'a', auth: 'b' } });
    check('K10', 'subscribe canonical tanpa push-token ditolak 401 (p.493)', r10.code === 401);

    // (b) [P1-A] isolasi multi-device + [P1-C] lifecycle
    const subA1 = canonMakeBrowserSub(DEV_A, 'push-token-dev-a-round3', 'a1');
    check('K10', 'subscribe canonical DEV_A diterima (200)', subA1.res.code === 200);
    const subB1 = canonMakeBrowserSub(DEV_B, 'push-token-dev-b-round3', 'b1');
    const subB2 = canonMakeBrowserSub(DEV_B, 'push-token-dev-b-broken', 'b2');
    check('K10', 'subscribe DEV_B dengan token SALAH ditolak 401', subB2.res.code === 401);
    const subB2b = canonMakeBrowserSub(DEV_B, 'push-token-dev-b-round3', 'b2');
    const subB3 = canonMakeBrowserSub(DEV_B, 'push-token-dev-b-round3', 'b3');
    check('K10', '3 langganan aktif untuk DEV_B',
      canonSubs().filter((s) => s.deviceId === DEV_B).length === 3);

    const t1 = Date.now();
    let ing = canonIngest(DEV_A, [S('Suhu Greenhouse', 41.2, 'C', true, 'critical', '41.2 C melebihi ambang')], t1);
    check('K10', 'tepi naik DEV_A → 1 event', ing.code === 200 && ing.data.triggered.length === 1);
    const idA = ing.data.triggered[0];
    check('K10', 'event id device-scoped: ALM-<dev>-<sensor>-…',
      idA.indexOf('ALM-' + DEV_A + '-suhu-greenhouse-') === 0);

    ing = canonIngest(DEV_B, [S('Suhu Greenhouse', 42.7, 'C', true, 'critical', '42.7 C melebihi ambang')], t1 + 1000);
    check('K10', '[P1-A] DEV_B alarm sensor SENAMA → event BARU dibuat (bukan tertelan state DEV_A)',
      ing.code === 200 && ing.data.triggered.length === 1);
    const idB = ing.data.triggered[0];
    check('K10', '[P1-A] eventId DEV_A ≠ eventId DEV_B', idA !== idB);
    check('K10', '[P1-A] generation keduanya = 1 (independen)',
      canonFind(idA).generation === 1 && canonFind(idB).generation === 1);
    check('K10', 'push DEV_A hanya ke langganan DEV_A (A1)',
      pushService.requests.filter((q) => q.url === subA1.endpoint).length === 1 &&
      pushService.requests.filter((q) => q.url === subB1.endpoint).length === 1);

    ing = canonIngest(DEV_A, [S('Suhu Greenhouse', 38.0, 'C', false, 'info', 'pulih')], t1 + 2000);
    check('K10', 'clear DEV_A → resolved = [event DEV_A]',
      ing.data.resolved.length === 1 && ing.data.resolved[0] === idA);
    const evA = canonFind(idA);
    const rsvA = canonCtx.pushEvents_().filter((e) => e.resolvesEventId === idA)[0];
    check('K10', '[P1-C] event RSV dibuat untuk event asal', !!rsvA);
    check('K10', '[P1-C] event RAISED asal DEV_A ditulis CLEARED + clearedAt + resolvedBy',
      evA.state === 'CLEARED' && evA.clearedAt !== null && evA.resolvedBy === rsvA.eventId);
    check('K10', '[P1-C] event RSV membawa resolvesEventId',
      rsvA.resolvesEventId === idA);
    check('K10', '[P1-A] event DEV_B TETAP RAISED setelah clear DEV_A',
      canonFind(idB).state === 'RAISED');

    // (c) [P1-B] partial delivery — retry hanya ke yang gagal
    pushService.endpointStatus[subB2b.endpoint] = 503;
    ing = canonIngest(DEV_B, [S('Tegangan Baterai', 3.9, 'V', true, 'critical', 'undervoltage')], t1 + 3000);
    const idC = ing.data.triggered[0];
    const evC0 = canonFind(idC);
    check('K10', 'partial: B1 terkirim, B2 gagal 503, B3 terkirim',
      pushService.requests.filter((q) => q.url === subB1.endpoint).length === 2 &&
      pushService.requests.filter((q) => q.url === subB2b.endpoint).length === 2 &&
      pushService.requests.filter((q) => q.url === subB3.endpoint).length === 2);
    check('K10', 'partial: status event PENDING + 2 marker SENT (B1,B3)',
      evC0.delivery.status === 'PENDING' &&
      Object.keys(evC0.delivery.perSub).length === 2 &&
      evC0.delivery.perSub[canonSubIdOf(subB1.endpoint)] === 'SENT' &&
      evC0.delivery.perSub[canonSubIdOf(subB3.endpoint)] === 'SENT');
    delete pushService.endpointStatus[subB2b.endpoint];   // push service pulih
    canonCtx.PUSH_CFG.OUTBOX_RETRY_BASE_MS = 0;           // backoff 0 → langsung due
    const rr = canonCtx.pushRetryPending_();
    check('K10', 'retry pass menemukan 1 event due', rr.ok === true && rr.retried === 1);
    check('K10', '[P1-B] retry hanya menambah pengiriman ke B2 (B1/B3 TIDAK diduplikasi)',
      pushService.requests.filter((q) => q.url === subB1.endpoint).length === 2 &&
      pushService.requests.filter((q) => q.url === subB2b.endpoint).length === 3 &&
      pushService.requests.filter((q) => q.url === subB3.endpoint).length === 2);
    const evC1 = canonFind(idC);
    check('K10', '[P1-B] setelah retry semua subscriber terkirim → status SENT',
      evC1.delivery.status === 'SENT' &&
      evC1.delivery.perSub[canonSubIdOf(subB2b.endpoint)] === 'SENT');
    canonCtx.PUSH_CFG.OUTBOX_RETRY_BASE_MS = 60000;       // pulihkan backoff

    // (d) [P1-D] kontensi lock → fail-closed 503, tanpa setengah-mutasi
    const eventsBefore = canonCtx.pushEvents_().length;
    canonLock.forceBusy = true;
    ing = canonIngest(DEV_B, [S('Arus Beban', 19.5, 'A', true, 'warning', 'overload')], t1 + 4000);
    check('K10', '[P1-D] lock diperebutkan → ingest 503 fail-closed', ing.code === 503);
    check('K10', '[P1-D] tidak ada setengah-mutasi saat busy (jumlah event tetap)',
      canonCtx.pushEvents_().length === eventsBefore);
    canonLock.forceBusy = false;
    ing = canonIngest(DEV_B, [S('Arus Beban', 19.5, 'A', true, 'warning', 'overload')], t1 + 4000);
    check('K10', 'ingest ulang setelah kontensi sukses (1 event)',
      ing.code === 200 && ing.data.triggered.length === 1);

    // (e) interleaving — merge per-event tidak menimpa mutasi lain
    pushService.endpointStatus[subB1.endpoint] = 503;
    pushService.endpointStatus[subB2b.endpoint] = 503;
    pushService.endpointStatus[subB3.endpoint] = 503;
    ing = canonIngest(DEV_B, [S('Kelembapan Tanah', 12, '%', true, 'warning', 'kering')], t1 + 5000);
    const idE1 = ing.data.triggered[0];
    check('K10', 'event E1 (semua endpoint 503) → PENDING',
      canonFind(idE1).delivery.status === 'PENDING');
    delete pushService.endpointStatus[subB1.endpoint];
    delete pushService.endpointStatus[subB2b.endpoint];
    delete pushService.endpointStatus[subB3.endpoint];
    ing = canonIngest(DEV_A, [S('Kelembapan Tanah', 12, '%', true, 'warning', 'kering')], t1 + 6000);
    const idE2 = ing.data.triggered[0];   // alarm DEV_A lain — diproses selagi E1 pending
    check('K10', 'event DEV_A lain (E2) tetap diproses normal saat E1 pending',
      ing.code === 200 && idE2 !== undefined);
    // merge terlambat E1 (interleaved) — TIDAK BOLEH menimpa E2
    canonCtx.pushRecordDelivery_(idE1,
      { sent: 1, failed: 0, removed: 0, perSub: { [canonSubIdOf(subB1.endpoint)]: 'SENT' } });
    const e1after = canonFind(idE1);
    const e2after = canonFind(idE2);
    check('K10', 'interleaving: merge E1 tercatat (marker B1)',
      e1after && e1after.delivery.perSub[canonSubIdOf(subB1.endpoint)] === 'SENT');
    check('K10', 'interleaving: E2 tidak tertimpa merge E1 (masih ada, status konsisten)',
      e2after !== null && e2after.eventId === idE2);
    check('K10', 'interleaving: indeks PENDING tidak memuat event terminal',
      canonCtx.pushPendingIdx_().indexOf(idE2) < 0);

    // (f) [QUOTA] jalur tenang murah (gerbang indeks PENDING)
    canonPropsStats.getCalls = 0; canonPropsStats.setCalls = 0;
    ing = canonIngest(DEV_B, [S('Arus Beban', 19.6, 'A', true, 'warning', 'overload bertahan')], t1 + 7000);
    check('K10', 'ingest level (tanpa tepi) diproses 200',
      ing.code === 200 && ing.data.triggered.length === 0);
    check('K10', '[QUOTA] jalur tenang ≤ 40 property-read', canonPropsStats.getCalls <= 40);
    check('K10', '[QUOTA] jalur tenang ≤ 3 property-write', canonPropsStats.setCalls <= 3);


    // (h) ACK canonical (p.482)
    const ackTok = canonCtx.pushMakeAckToken_(idA);
    r10 = canonPost({ action: 'PUSH_ACK', alarmId: idA, ackToken: ackTok });
    check('K10', 'ACK dengan capability token valid → 200', r10.code === 200 && !!r10.data.acknowledgedAt);
    const ackAt1 = r10.data.acknowledgedAt;
    r10 = canonPost({ action: 'PUSH_ACK', alarmId: idA, ackToken: ackTok });
    check('K10', 'ACK idempoten (acknowledgedAt tidak berubah)',
      r10.code === 200 && r10.data.acknowledgedAt === ackAt1);
    r10 = canonPost({ action: 'PUSH_ACK', alarmId: idA, ackToken: 'bogus-token' });
    check('K10', 'ACK token salah → 401', r10.code === 401);
    const ghostId = 'ALM-ghost-9999';
    r10 = canonPost({ action: 'PUSH_ACK', alarmId: ghostId, ackToken: canonCtx.pushMakeAckToken_(ghostId) });
    check('K10', 'ACK event tak dikenal → 404 (retensi outbox = jendela ACK)', r10.code === 404);

    // (i) PUSH_TEST terautentikasi admin
    r10 = canonPost({ action: 'PUSH_TEST', token: CANON_AUTH_TOKEN, device_key: DEV_A });
    check('K10', 'PUSH_TEST tanpa admin_token → 401', r10.code === 401);
    r10 = canonPost({ action: 'PUSH_TEST', token: CANON_AUTH_TOKEN, device_key: DEV_A,
      data: { admin_token: CANON_ADMIN_TOKEN } });
    check('K10', 'PUSH_TEST dengan admin_token → 200 + eventId TEST-',
      r10.code === 200 && r10.data.eventId.indexOf('TEST-') === 0);

    // (j) [P1-C] lifecycle emergency
    canonCtx.pushEvaluateEmergency_({ emgState: 'TRIPPED', emgReason: 'uji-harness' }, DEV_A);
    const emgEvents = canonCtx.pushEvents_().filter((e) => e.alarmCode === 'emergency');
    const emgRaised = emgEvents.find((e) => e.state === 'RAISED');
    check('K10', 'emergency TRIP → event EMG RAISED dibuat', !!emgRaised);
    canonCtx.pushEvaluateEmergency_({ emgState: 'SAFE' }, DEV_A);
    const emgRaisedAfter = canonFind(emgRaised.eventId);
    const emgRsv = canonCtx.pushEvents_().filter((e) => e.alarmCode === 'emergency')
      .find((e) => e.resolvesEventId === emgRaised.eventId);
    check('K10', '[P1-C] emergency SAFE → event RAISED asal CLEARED + RSV resolvesEventId',
      emgRaisedAfter.state === 'CLEARED' && !!emgRsv &&
      emgRaisedAfter.resolvedBy === emgRsv.eventId);

    // (k) endpoint mati → pruned
    const subDead = canonMakeBrowserSub(DEV_A, 'push-token-dev-a-round3', 'dead');
    pushService.endpointStatus[subDead.endpoint] = 410;
    const subsBeforeDead = canonSubs().length;
    ing = canonIngest(DEV_A, [S('Suhu Ruang', 55, 'C', true, 'critical', 'panas')], t1 + 9000);
    check('K10', 'endpoint 410 → langganan mati dipruned',
      canonSubs().length === subsBeforeDead - 1);
    delete pushService.endpointStatus[subDead.endpoint];

    // (l) PUSH_STATUS — observability round-3
    r10 = canonPost({ action: 'PUSH_STATUS', token: CANON_AUTH_TOKEN, device_key: DEV_A });
    check('K10', 'PUSH_STATUS melaporkan storage.bytesUsed + limit + capacityDropped',
      r10.code === 200 && r10.data.storage.bytesUsed > 0 &&
      r10.data.storage.totalLimit === canonCtx.PUSH_CFG.STORAGE_TOTAL_LIMIT);
    check('K10', 'PUSH_STATUS melaporkan sourceRevision (binding sumber)',
      typeof r10.data.sourceRevision === 'string' && r10.data.sourceRevision.length > 0);
    check('K10', 'PUSH_STATUS menyatakan semantika at-least-once per subscriber',
      String(r10.data.deliverySemantics).indexOf('at-least-once') >= 0);

    // (g) [CAP] worst-case: 205 event maksimum + 50 langganan → muat
    const capBody = 'x'.repeat(400);
    canonCtx.pushWithLock_(function () {
      for (let i = 0; i < 205; i++) {
        canonCtx.pushAppendEvent_({
          eventId: 'CAP-' + i,
          deviceId: DEV_B,
          alarmCode: 'cap-' + i,
          generation: 1,
          severity: 'warning',
          raisedAt: new Date(t1 + 8000 + i).toISOString(),
          clearedAt: null,
          state: 'SENT-TERMINAL',
          acknowledgedAt: null,
          payload: { id: 'CAP-' + i, title: 'CAP', body: capBody, severity: 'warning',
            tag: 'cap', url: './index.html', timestamp: t1 + i, requireInteraction: false },
          delivery: { status: 'SENT', attempts: 1, lastAttemptAt: new Date().toISOString(),
            sent: 1, failed: 0, removed: 0, perSub: {} }
        });
      }
    }, 20000);
    const capEvents = canonCtx.pushEvents_();
    check('K10', '[CAP] retensi outbox terjaga ≤ 200 setelah 205 append', capEvents.length <= 200);
    check('K10', '[CAP] event tertua (CAP-0) terevisi, termuda (CAP-204) ada',
      capEvents.some((e) => e.eventId === 'CAP-204') && !capEvents.some((e) => e.eventId === 'CAP-0'));
    const fiftySubs = [];
    for (let i = 0; i < 50; i++) {
      fiftySubs.push({
        endpoint: 'https://push.test.local/capsub/' + i + '/' + crypto.randomBytes(6).toString('hex'),
        subId: 'SC' + i,
        keys: { p256dh: b64url(Buffer.alloc(65, 4)), auth: b64url(crypto.randomBytes(16)) },
        deviceId: DEV_A,
        context: { lang: 'id-ID' },
        addedAt: new Date().toISOString(), lastSeenAt: new Date().toISOString()
      });
    }
    canonCtx.pushWithLock_(function () { canonCtx.pushSaveSubsAll_(fiftySubs); }, 20000);
    check('K10', '[CAP] 50 langganan tersimpan',
      canonCtx.pushSubsAll_().length === 50);
    let maxVal = 0, totalBytes = 0;
    canonProps.forEach((v, k) => { maxVal = Math.max(maxVal, String(v).length); totalBytes += String(v).length + k.length; });
    check('K10', '[CAP] SETIAP property value < 9 KB (batas GAS nyata)', maxVal < CANON_VALUE_LIMIT);
    check('K10', '[CAP] total property store < 500 KB (batas GAS nyata)', totalBytes < CANON_TOTAL_LIMIT);
    check('K10', '[CAP] mock penegak TIDAK pernah melempar (0 pelanggaran 9KB / 0 pelanggaran 500KB)',
      canonPropsStats.overValueThrows === 0 && canonPropsStats.totalThrows === 0);

    // (m) [P1-D] disiplin lock — bukti mekanis di akhir seluruh skenario
    check('K10', '[P1-D] 0 network fetch di dalam Script Lock (seluruh skenario)',
      canonLock.fetchesUnderLock === 0);
    check('K10', '[P1-D] 0 tryLock bersarang (seluruh skenario)', canonLock.nestedTryLock === 0);
    check('K10', '[P1-D] lock ter-release seimbang (held kembali 0)', canonLock.held === 0);
  }

  /* ---------------- Ringkasan ---------------- */
  console.log('\n==============================================================');
  console.log(' RINGKASAN PER KONTRAK (Tabel 11)');
  console.log('==============================================================');
  const order = ['K1', 'K2', 'K3', 'K4', 'K5', 'K6', 'K7', 'K8', 'K9', 'K10'];
  const names = {
    K1: 'Langganan (PWA-GAS)',
    K2: 'Penghapusan (PWA-GAS)',
    K3: 'Payload alarm (GAS-PWA)',
    K4: 'ACK (PWA-GAS)',
    K5: 'Deep-link (PWA internal)',
    K6: 'Ambang alarm (FW-GAS)',
    K7: 'Kunci VAPID (GAS-PWA)',
    K8: 'Hardening produksi',
    K9: 'Push hardening p.493/P0-3',
    K10: 'Canonical push round-3 P1'
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
