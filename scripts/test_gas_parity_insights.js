#!/usr/bin/env node
/**
 * test_gas_parity_insights.js — [PARITY-3 AUDIT 2026-09-06] GAS cross-layer
 * parity regression tests (Level 2 evidence, real Code.gs in a Node sandbox).
 * =============================================================================
 * Covers the gaps this audit closed in code.gs/Code.gs:
 *
 *   I1   INSIGHTS action exists (doPost) — fail-closed 503 without
 *        GEMINI_API_KEY (NEVER a mock insight)
 *   I2   INSIGHTS without telemetry rows → honest 404 NO_TELEMETRY
 *   I3   INSIGHTS happy path via mocked Gemini → validated InsightsEnvelope
 *        (advisoryOnly forced, source 'gemini', capped)
 *   I4   cache: same input rows → cached:true; new rows → regenerate
 *   I5   Gemini invalid JSON / bad category → honest 502, no partial accept
 *   I6   Gemini HTTP error → honest 502 with reason
 *   I7   doGet INSIGHTS over GET + HMAC query envelope (data = '') — the
 *        exact envelope GasAdvisor::fetchInsights() signs
 *   I8   doGet INSIGHTS wrong signature / replayed nonce → 401
 *   I9   doPost INSIGHTS over the HMAC body envelope (data '')
 *
 *   O1   OTA_STATUS accepts the full modular lifecycle vocabulary
 *   O2   OTA_STATUS still refuses unknown events (fail-closed word list)
 *
 *   D1   DAILY report carries honest peakChargeA/peakDischargeA
 *   D2   DAILY cap is 90 (PWA monthly range)
 *
 *   P1   PING handshake reports Devices!firmware_type
 *
 *   E1   EMERGENCY_COMMAND without body device_key (HMAC-authenticated) no
 *        longer 400s on String(undefined) — the guard's original intent
 *   E2   Telegram cooldown is PER-TOPIC: emergency alerts use
 *        EMERGENCY_ALERT_COOLDOWN_MIN (2), low-battery keeps 30
 *   E3   TELEMETRY piggyback marks the emergency row DELIVERED
 *
 * Usage: node scripts/test_gas_parity_insights.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');
const { patchHarnessSetup_ } = require('./harness-setup-fix.js');

// ---------------------------------------------------------------------------
// Minimal GAS service mocks (same conventions as test_wave1_integration.js,
// with a ROUTED UrlFetchApp: Gemini vs Telegram)
// ---------------------------------------------------------------------------

class FakeSheet {
  constructor(name, header) {
    this.name = name;
    this.rows = header ? [header.slice()] : [];
  }
  getLastRow() { return this.rows.length; }
  getLastColumn() { return this.rows.reduce((m, r) => Math.max(m, r.length), 0); }
  appendRow(row) { this.rows.push(row.slice()); return this; }
  getDataRange() {
    const self = this;
    return { getValues: () => self.rows.map((r) => r.slice()) };
  }
  getRange(row, col, numRows = 1, numCols = 1) {
    const self = this;
    const chainable = () => ({
      setFontWeight: () => chainable(),
      setValues: () => chainable(),
      setValue: () => chainable(),
    });
    return {
      getValues: () => {
        const out = [];
        for (let i = 0; i < numRows; i++) {
          const r = self.rows[row - 1 + i] || [];
          out.push(r.slice(col - 1, col - 1 + numCols));
        }
        return out;
      },
      setValues: (vals) => {
        for (let i = 0; i < numRows; i++) {
          while (self.rows.length < row + i) self.rows.push([]);
          const r = self.rows[row - 1 + i];
          for (let j = 0; j < numCols; j++) r[col - 1 + j] = vals[i][j];
        }
        return chainable();
      },
      setValue: (v) => {
        while (self.rows.length < row) self.rows.push([]);
        const r = self.rows[row - 1];
        r[col - 1] = v;
        return chainable();
      },
      setFontWeight: chainable().setFontWeight,
    };
  }
  clear() { this.rows = []; return this; }
  setFrozenRows() { return this; }
  setColumnWidth() { return this; }
}

class FakeSpreadsheet {
  constructor() { this.sheets = {}; }
  getSheetByName(name) { return this.sheets[name] || null; }
  insertSheet(name) { this.sheets[name] = new FakeSheet(name, null); return this.sheets[name]; }
}

function toSignedBytes(buf) {
  return Array.from(buf).map((b) => (b > 127 ? b - 256 : b));
}

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cacheStore = new Map();
  const telegram = { calls: [] };
  const gemini = {
    code: 200,
    body: JSON.stringify({ candidates: [{ content: { parts: [{ text: '[]' }] } }] }),
    calls: [],
  };

  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, parseInt, parseFloat, RegExp, Error,
    SpreadsheetApp: {
      getActiveSpreadsheet: () => ss,
      getUi: () => ({ alert: () => {} }),
    },
    LockService: {
      getScriptLock: () => ({
        tryLock: () => { if (locks.held) return false; locks.held = true; locks.acquired++; return true; },
        releaseLock: () => { locks.held = false; },
      }),
    },
    CacheService: {
      getScriptCache: () => ({
        get: (k) => {
          const e = cacheStore.get(k);
          if (!e) return null;
          if (e.exp < Date.now()) { cacheStore.delete(k); return null; }
          return e.v;
        },
        put: (k, v, ttlSec) => cacheStore.set(k, { v, exp: Date.now() + (ttlSec || 600) * 1000 }),
        remove: (k) => cacheStore.delete(k),
      }),
    },
    Utilities: {
      getUuid: () => crypto.randomUUID(),
      DigestAlgorithm: { SHA_256: 'SHA_256' },
      Charset: { UTF_8: 'UTF_8' },
      computeDigest: (alg, value, charset) =>
        toSignedBytes(crypto.createHash('sha256')
          .update(Buffer.from(String(value), 'utf8')).digest()),
      computeHmacSha256Signature: (value, key, charset) =>
        toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
          .update(Buffer.from(String(value), 'utf8')).digest()),
      // formatDate — used by dayKeyInTz_ for DAILY bucketing ('yyyy-MM-dd'
      // only). Minimal tz support: Asia/Jakarta (+7) and UTC fallback.
      formatDate: (date, tz, fmt) => {
        const d = date instanceof Date ? date : new Date(date);
        const offsetH = String(tz) === 'Asia/Jakarta' ? 7 : 0;
        const shifted = new Date(d.getTime() + offsetH * 3600000);
        const y = shifted.getUTCFullYear();
        const m = String(shifted.getUTCMonth() + 1).padStart(2, '0');
        const day = String(shifted.getUTCDate()).padStart(2, '0');
        return y + '-' + m + '-' + day;
      },
    },
    ContentService: {
      createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
      MimeType: { JSON: 'JSON' },
    },
    UrlFetchApp: {
      fetch: (url, opts) => {
        if (String(url).indexOf('generativelanguage.googleapis.com') !== -1) {
          gemini.calls.push({ url: String(url), opts });
          return {
            getResponseCode: () => gemini.code,
            getContentText: () => gemini.body,
          };
        }
        telegram.calls.push(String(url));
        return { getResponseCode: () => 200, getContentText: () => '{}' };
      },
    },
  };
  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks, cacheStore, telegram, gemini };
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

let passed = 0, failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  PASS  ${name}`); }
  else {
    failed++; failures.push(name + (detail ? ` — ${detail}` : ''));
    console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`);
  }
}

function doPost(env, body) {
  const out = env.sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
  return JSON.parse(out.text);
}

function doGet(env, params) {
  const out = env.sandbox.doGet({ parameter: params });
  return JSON.parse(out.text);
}

function sha256hex(s) {
  return crypto.createHash('sha256').update(Buffer.from(s, 'utf8')).digest('hex');
}

function hmacHex(secret, message) {
  return crypto.createHmac('sha256', Buffer.from(secret, 'utf8'))
    .update(Buffer.from(message, 'utf8')).digest('hex');
}

/** Canonical HMAC string — byte-identical to Code.gs verifyHmac_. */
function canonical(action, ts, nonce, deviceId, dataJson) {
  return 'HMAC-SHA256\n' + action + '\n' + ts + '\n' + nonce + '\n' +
         deviceId + '\n' + sha256hex(dataJson);
}

function setConfig(env, key, value) {
  const cfg = env.ss.sheets['Config'];
  const rows = cfg.getDataRange().getValues();
  for (let i = 0; i < rows.length; i++) {
    if (String(rows[i][0]) === key) { cfg.getRange(i + 1, 2).setValue(value); return; }
  }
  cfg.appendRow([key, value]);
}

// Canonical nested telemetry POST (modular firmware shape, minimal fields).
function postTelemetry(env, deviceKey, seq, fields) {
  const f = fields || {};
  return doPost(env, {
    action: 'TELEMETRY',
    token: TOKEN,
    device_key: deviceKey,
    data: {
      protocolVersion: '2',
      deviceId: deviceKey,
      sequence: seq,
      timestamp: new Date(Date.now() - (60 - (seq % 60)) * 60000).toISOString(),
      battery: {
        voltage: { value: f.vBat !== undefined ? f.vBat : 51.2, quality: 'VALID' },
        current: { value: f.iBat !== undefined ? f.iBat : -3.2, quality: 'VALID' },
        power: { value: f.pBat !== undefined ? f.pBat : -160, quality: 'VALID' },
        soc: { value: f.soc !== undefined ? f.soc : 78, quality: 'VALID' },
        chargeWh: 1200, dischargeWh: 3400,
      },
      environment: { temperature: { value: 29.1 }, humidity: { value: 61 } },
      health: { freeHeap: 180000, wifiRssi: -58, sensorHealth: { ina219: 'ONLINE' } },
    },
  });
}

const TOKEN = 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';
const ADMIN = 'plts_admin_p3_9a3f7c2d5e8b1a6f';
const DEV = 'PLTS_PARITY_01';

/** Wrap a model text into the Gemini generateContent response shape. */
function geminiBody(text) {
  return JSON.stringify({ candidates: [{ content: { parts: [{ text: String(text) }] } }] });
}

const GEMINI_OK_BODY = geminiBody(JSON.stringify({
  insights: [
    { category: 'battery_analysis', severity: 'info',
      title: 'Steady discharge', body: 'Current held around -3.2 A for the sampled window.' },
    { category: 'energy_analysis', severity: 'warning',
      title: 'Net discharge trend', body: 'Cumulative dischargeWh exceeded chargeWh.' },
  ],
}));

// ---------------------------------------------------------------------------
console.log('\n=== [PARITY-3] GAS CROSS-LAYER PARITY TESTS ===\n');

const env = createGasContext();
patchHarnessSetup_(env, TOKEN);
setConfig(env, 'ADMIN_TOKEN', ADMIN);
setConfig(env, 'TELEGRAM_BOT_TOKEN', 'bot-token-fixture');
setConfig(env, 'TELEGRAM_CHAT_ID', 'chat-fixture');
env.sandbox.invalidatePltsCache();
env.ss.sheets['Devices'].appendRow([DEV, 'secret_p3_0123456789abcdef', 'Bench', '', '', 'modular']);

// == INSIGHTS =================================================================
console.log('[INSIGHTS]');
{
  // I1 — fail-closed without GEMINI_API_KEY
  const r1 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I1 fail-closed 503 without GEMINI_API_KEY',
    r1.code === 503 && r1.data && r1.data.success === false &&
    r1.data.error === 'GEMINI_API_KEY_NOT_CONFIGURED', JSON.stringify(r1));
  check('I1b no mock ever served (no Gemini call)',
    env.gemini.calls.length === 0, String(env.gemini.calls.length));

  setConfig(env, 'GEMINI_API_KEY', 'gemini-key-fixture');
  env.sandbox.invalidatePltsCache();

  // I2 — no telemetry yet
  const r2 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I2 honest 404 NO_TELEMETRY before any rows',
    r2.code === 404 && r2.data && r2.data.error === 'NO_TELEMETRY', JSON.stringify(r2));

  // Seed telemetry rows
  for (let s = 1; s <= 3; s++) postTelemetry(env, DEV, s, {});

  // I3 — happy path
  env.gemini.body = GEMINI_OK_BODY;
  const r3 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I3 success envelope (PWA InsightsEnvelope shape)',
    r3.code === 200 && r3.data.success === true && Array.isArray(r3.data.insights) &&
    r3.data.insights.length === 2 && r3.data.cached === false &&
    typeof r3.data.generatedAt === 'string', JSON.stringify(r3).slice(0, 200));
  check('I3b advisoryOnly + source forced server-side',
    r3.data.insights.every((i) => i.advisoryOnly === true && i.source === 'gemini'),
    JSON.stringify(r3.data.insights));
  check('I3c prompt carries honest telemetry (sent to Gemini)',
    env.gemini.calls.length === 1 && /48V LiFePO4/.test(String(env.gemini.calls[0].opts.payload)),
    'payload missing telemetry prompt');

  // I4 — cache hit on the same input set
  env.gemini.body = JSON.stringify({ candidates: [{ content: { parts: [{ text: 'should not be called' }] } }] });
  const r4 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I4 cache hit → cached:true, no second Gemini call',
    r4.code === 200 && r4.data.cached === true && r4.data.insights.length === 2 &&
    env.gemini.calls.length === 1, JSON.stringify(env.gemini.calls.length));

  // I4b — new telemetry invalidates the cache (fingerprint change)
  postTelemetry(env, DEV, 4, {});
  env.gemini.body = GEMINI_OK_BODY;
  const r4b = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I4b fresh rows regenerate (cached:false, second Gemini call)',
    r4b.code === 200 && r4b.data.cached === false && env.gemini.calls.length === 2,
    'calls=' + env.gemini.calls.length);

  // I5 — invalid model output (bad category) → whole response rejected
  env.gemini.body = geminiBody(JSON.stringify({ insights: [
    { category: 'not_a_category', severity: 'info', title: 'x', body: 'y' },
  ] }));
  postTelemetry(env, DEV, 5, {});
  const r5 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I5 invalid category → honest 502 GEMINI_RESPONSE_INVALID (no partial accept)',
    r5.code === 502 && r5.data && r5.data.error === 'GEMINI_RESPONSE_INVALID',
    JSON.stringify(r5).slice(0, 160));

  // I5b — unparseable JSON
  env.gemini.body = geminiBody('not json at all');
  postTelemetry(env, DEV, 6, {});
  const r5b = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I5b unparseable Gemini text → honest 502',
    r5b.code === 502 && r5b.data && r5b.data.error === 'GEMINI_RESPONSE_INVALID', '');

  // I6 — Gemini HTTP error
  env.gemini.code = 429;
  env.gemini.body = JSON.stringify({ error: { message: 'quota exceeded' } });
  postTelemetry(env, DEV, 7, {});
  const r6 = doPost(env, { action: 'INSIGHTS', token: TOKEN, device_key: DEV });
  check('I6 Gemini HTTP error → honest 502 GEMINI_ERROR with reason',
    r6.code === 502 && r6.data && r6.data.error === 'GEMINI_ERROR' &&
    /HTTP 429/.test(String(r6.data.message)), JSON.stringify(r6.data).slice(0, 160));
  env.gemini.code = 200;
}

// == INSIGHTS over GET (firmware GasAdvisor path) =============================
console.log('[INSIGHTS GET — GasAdvisor envelope]');
{
  let nonceSeq = 0;
  const nowSec = () => Math.floor(Date.now() / 1000);
  const dev = DEV, sec = 'secret_p3_0123456789abcdef';

  const mkParams = (sig) => {
    const ts = nowSec();
    const nonce = 'p3nonce' + String(++nonceSeq).padStart(24, '0');
    const p = {
      action: 'INSIGHTS',
      auth_method: 'HMAC-SHA256',
      auth_timestamp: String(ts),
      auth_nonce: nonce,
      auth_device_id: dev,
      auth_signature: sig || hmacHex(sec, canonical('INSIGHTS', ts, nonce, dev, '')),
    };
    return p;
  };

  postTelemetry(env, DEV, 20, {});
  env.gemini.body = GEMINI_OK_BODY;

  // I7 — valid signature over GET
  const g1 = doGet(env, mkParams());
  check('I7 doGet INSIGHTS accepts the GasAdvisor query HMAC (data="")',
    g1.code === 200 && g1.data && g1.data.success === true &&
    Array.isArray(g1.data.insights), JSON.stringify(g1).slice(0, 160));

  // I8 — wrong signature
  const g2 = doGet(env, mkParams('deadbeef'.repeat(8)));
  check('I8 doGet INSIGHTS wrong signature → 401',
    g2.code === 401 && /signature mismatch|Unauthorized/.test(g2.message), JSON.stringify(g2));

  // I8b — wrong action confusion (TELEMETRY signature replayed as INSIGHTS)
  const ts = nowSec();
  const nonce = 'p3nonce' + String(++nonceSeq).padStart(24, '0');
  const telemSig = hmacHex(sec, canonical('TELEMETRY', ts, nonce, dev, ''));
  const g3 = doGet(env, {
    action: 'INSIGHTS', auth_method: 'HMAC-SHA256', auth_timestamp: String(ts),
    auth_nonce: nonce, auth_device_id: dev, auth_signature: telemSig,
  });
  check('I8b action-confusion replay (TELEMETRY sig on INSIGHTS) → 401',
    g3.code === 401, JSON.stringify(g3.message));

  // I9 — HMAC body envelope (data '')
  const ts9 = nowSec();
  const nonce9 = 'p3nonce' + String(++nonceSeq).padStart(24, '0');
  const sig9 = hmacHex(sec, canonical('INSIGHTS', ts9, nonce9, dev, ''));
  const r9 = doPost(env, {
    action: 'INSIGHTS',
    auth: { method: 'HMAC-SHA256', timestamp: ts9, nonce: nonce9, deviceId: dev, signature: sig9 },
    data: '',
  });
  check('I9 doPost INSIGHTS over HMAC body envelope (data "")',
    r9.code === 200 && r9.data && r9.data.success === true, JSON.stringify(r9).slice(0, 160));

  // I10 — unregistered device is rejected (registration gate)
  const ts10 = nowSec();
  const nonce10 = 'p3nonce' + String(++nonceSeq).padStart(24, '0');
  const sig10 = hmacHex('unknown-device-secret', canonical('INSIGHTS', ts10, nonce10, 'PLTS_GHOST_01', ''));
  const r10 = doPost(env, {
    action: 'INSIGHTS',
    auth: { method: 'HMAC-SHA256', timestamp: ts10, nonce: nonce10, deviceId: 'PLTS_GHOST_01', signature: sig10 },
    data: '',
  });
  check('I10 unknown device → 401 (fail-closed)', r10.code === 401, JSON.stringify(r10.message));
}

// == OTA_STATUS vocabulary =====================================================
console.log('[OTA_STATUS vocabulary]');
{
  const states = ['ACCEPTED', 'DOWNLOADING', 'VERIFIED', 'FLASHED', 'FAILED',
                  'ACTIVATED', 'ROLLBACK'];
  let allOk = true, bad = '';
  for (const s of states) {
    const r = doPost(env, { action: 'OTA_STATUS', token: TOKEN, device_key: DEV,
                            event: s, version: '1.9.3', message: 'parity' });
    if (r.code !== 200) { allOk = false; bad += s + '=' + r.code + ' '; }
  }
  check('O1 modular lifecycle states accepted', allOk, bad);
  const r2 = doPost(env, { action: 'OTA_STATUS', token: TOKEN, device_key: DEV,
                           event: 'TOTALLY_BOGUS', version: '1.9.3' });
  check('O2 unknown event still refused (400)', r2.code === 400, JSON.stringify(r2.message));
  const evs = env.ss.sheets['OtaEvents'].rows;
  check('O2b OtaEvents rows persisted per event',
    evs.length >= states.length, 'rows=' + evs.length);
}

// == DAILY peaks ================================================================
console.log('[DAILY report]');
{
  // Current day rows: charging spike +5.4 A, discharge min -3.2 A
  postTelemetry(env, DEV, 40, { iBat: 5.4 });
  postTelemetry(env, DEV, 41, { iBat: -3.2 });
  const r = doPost(env, { action: 'DAILY', token: TOKEN, device_key: DEV, days: 1 });
  const day = r.data && r.data.days && r.data.days[r.data.days.length - 1];
  check('D1 peakChargeA = max positive current',
    r.code === 200 && day && day.peakChargeA === 5.4, JSON.stringify(day));
  check('D1b peakDischargeA = |min negative| current', day && day.peakDischargeA === 3.2, '');
  const r90 = doPost(env, { action: 'DAILY', token: TOKEN, device_key: DEV, days: 90 });
  check('D2 90-day window accepted (PWA monthly range)',
    r90.code === 200 && Array.isArray(r90.data.days), JSON.stringify(r90.code));
  const r99 = doPost(env, { action: 'DAILY', token: TOKEN, device_key: DEV, days: 99 });
  const got = r99.data.days.length;
  check('D2b days>90 clamped by cap (≤90 returned, no error)',
    r99.code === 200 && got <= 90, 'days=' + got);
}

// == PING firmware_type =========================================================
console.log('[PING firmware_type]');
{
  const r = doPost(env, { action: 'PING', token: TOKEN, device_key: DEV });
  check('P1 registered device reports firmware_type=modular',
    r.code === 200 && r.data.device_registered === true &&
    r.data.firmware_type === 'modular', JSON.stringify(r.data));
  env.ss.sheets['Devices'].appendRow(['PLTS_NOTYPED', 's', 'NoType', '', '', '']);
  const r2 = doPost(env, { action: 'PING', token: TOKEN, device_key: 'PLTS_NOTYPED' });
  check('P1b undeclared type → firmware_type null (honest)',
    r2.data.firmware_type === null, JSON.stringify(r2.data));
  const r3 = doPost(env, { action: 'PING', token: TOKEN, device_key: 'PLTS_NOBODY' });
  check('P1c unknown device → device_registered false',
    r3.data.device_registered === false, JSON.stringify(r3.data));
}

// == EMERGENCY_COMMAND guard + cooldown + piggyback ============================
console.log('[EMERGENCY fixes]');
{
  // E1 — no body device_key, HMAC-authenticated operator. admin_token and
  // command ride INSIDE the signed data string (dual acceptance).
  const ts = Math.floor(Date.now() / 1000);
  const nonce = 'e1nonce' + String(1).padStart(25, '0');
  const sec = 'secret_p3_0123456789abcdef';
  const dataE1 = JSON.stringify({ admin_token: ADMIN, command: 'ARM', note: 'parity test' });
  const sig = hmacHex(sec, canonical('EMERGENCY_COMMAND', ts, nonce, DEV, dataE1));
  const r = doPost(env, {
    action: 'EMERGENCY_COMMAND',
    auth: { method: 'HMAC-SHA256', timestamp: ts, nonce, deviceId: DEV, signature: sig },
    data: dataE1,
  });
  check('E1 no body device_key (HMAC) no longer 400s on "undefined"',
    r.code === 200 && r.data && r.data.command_id, JSON.stringify(r).slice(0, 200));

  // E1b — explicit MISMATCHING device_key still rejected
  const ts2 = Math.floor(Date.now() / 1000);
  const nonce2 = 'e1nonce' + String(2).padStart(25, '0');
  const dataE2 = JSON.stringify({ admin_token: ADMIN, command: 'ARM' });
  const sig2 = hmacHex(sec, canonical('EMERGENCY_COMMAND', ts2, nonce2, DEV, dataE2));
  const r2 = doPost(env, {
    action: 'EMERGENCY_COMMAND',
    auth: { method: 'HMAC-SHA256', timestamp: ts2, nonce: nonce2, deviceId: DEV, signature: sig2 },
    data: dataE2,
    device_key: 'PLTS_SOMEONE_ELSE',
  });
  check('E1b mismatching body device_key still 400', r2.code === 400, JSON.stringify(r2.code));

  // E1c — token-path caller with explicit top-level fields still works.
  const cmdTok = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN,
                               admin_token: ADMIN, command: 'DISARM', note: 'token path',
                               device_key: DEV });
  check('E1c token path unchanged (top-level fields)',
    cmdTok.code === 200 && cmdTok.data.command_id, JSON.stringify(cmdTok).slice(0, 160));

  // E2 — cooldown: EMERGENCY uses EMERGENCY_ALERT_COOLDOWN_MIN (2 min = 120 s)
  setConfig(env, 'LOW_BATTERY_ALERT_COOLDOWN_MIN', '30');
  setConfig(env, 'EMERGENCY_ALERT_COOLDOWN_MIN', '2');
  env.sandbox.invalidatePltsCache();
  env.telegram.calls.length = 0;
  const evt1 = doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV,
                             type: 'TRIP', reason: 'vbat low' });
  const tAfterFirst = env.telegram.calls.length;
  const evt2 = doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV,
                             type: 'TRIP', reason: 'vbat low again' });
  const tAfterSecond = env.telegram.calls.length;
  const cacheKey = 'PLTS_TG_EMG_EVT_' + DEV + '_TRIP';
  const entry = env.cacheStore.get(cacheKey);
  const ttlSec = entry ? Math.round((entry.exp - Date.now()) / 1000) : -1;
  check('E2 emergency alert cooldown = EMERGENCY_ALERT_COOLDOWN_MIN (≈120 s)',
    tAfterFirst === 1 && tAfterSecond === 1 && ttlSec > 60 && ttlSec <= 120,
    `calls=${tAfterSecond} ttl=${ttlSec}`);
  // E2b — LOW_BATTERY still keyed at 30 min (≈1800 s)
  postTelemetry(env, DEV, 50, { vBat: 40.0 });
  postTelemetry(env, DEV, 51, { vBat: 40.0 });
  const lbKey = 'PLTS_TG_LOWBATT_' + DEV;
  const lbEntry = env.cacheStore.get(lbKey);
  const lbTtl = lbEntry ? Math.round((lbEntry.exp - Date.now()) / 1000) : -1;
  check('E2b low-battery cooldown unchanged (≈1800 s)',
    lbTtl > 1500 && lbTtl <= 1800, `ttl=${lbTtl}`);

  // E3 — piggyback marks DELIVERED
  const cmd = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN,
                            command: 'ARM', note: 'piggyback', device_key: DEV });
  check('E3a emergency command queued', cmd.code === 200 && cmd.data.command_id, '');
  const qrows = env.ss.sheets['EmergencyQueue'].rows;
  const qrowIdx = qrows.findIndex((r) => String(r[0]) === String(cmd.data.command_id));
  const statusBefore = qrowIdx >= 0 ? String(qrows[qrowIdx][6]) : 'MISSING';
  const tel = postTelemetry(env, DEV, 60, {});
  const statusAfter = String(env.ss.sheets['EmergencyQueue'].rows[qrowIdx][6]);
  check('E3 piggyback delivery marks the row DELIVERED (was ' + statusBefore + ')',
    tel.data && tel.data.pendingEmergency &&
    tel.data.pendingEmergency.command_id === cmd.data.command_id &&
    statusAfter === 'DELIVERED', 'status=' + statusAfter);
}

// ---------------------------------------------------------------------------
console.log('\n============================================================');
console.log(`PARITY-3 GAS: ${passed} passed, ${failed} failed`);
if (failed) {
  failures.forEach((f) => console.log('  ✗ ' + f));
  process.exit(1);
}
console.log('ALL PARITY-3 GAS TESTS PASS');
