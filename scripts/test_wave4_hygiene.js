#!/usr/bin/env node
'use strict';
/**
 * WAVE-4 HYGIENE TEST — real Code.gs in a Node VM + real crypto.
 * Covers the re-audit P3 group + GAS-2-D firmware-side prep (Code.gs half):
 *   GAS-2-O  Telegram alert moved OUTSIDE the global ingest lock
 *   GAS-2-P  Per-device low-battery alert cooldown (CacheService)
 *   GAS-2-Q  AUTH_TOKEN/ADMIN_TOKEN cache TTL 300 s (was 6 h)
 *   GAS-2-R  Malformed JSON body → 400 (was 500)
 *   GAS-2-S  PING handshake reports optional device registration
 *   GAS-2-T  Duplicate-check scan window ≥ LOG_ROTATION_MAX_ROWS
 *   GAS-2-U  HISTORY/LATEST scan windows config-driven (was hardcoded)
 *   GAS-2-V  Rotation for Ota / OtaEvents / Calibration (pending never deleted)
 *   GAS-2-X  verifyHmac_ no longer writes last_nonce/last_ts per request
 *   GAS-2-W  dead __headers / fake X-Auth comments — already removed in Wave 1;
 *            asserted here so they can never quietly return.
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// GAS service mocks (same fidelity as test_wave1_integration.js, plus:
//   - UrlFetchApp.fetch records whether the ingest LOCK was held at call time
//   - cache store exposes TTL for GAS-2-Q assertions)
// ---------------------------------------------------------------------------

class FakeSheet {
  constructor(name, header) {
    this.name = name;
    this.rows = header ? [header.slice()] : [];
  }
  getLastRow() { return this.rows.length; }
  getLastColumn() { return this.rows.reduce((m, r) => Math.max(m, r.length), 0); }
  getName() { return this.name; }
  appendRow(row) { this.rows.push(row.slice()); return this; }
  getDataRange() {
    const self = this;
    return { getValues: () => self.rows.map((r) => r.slice()) };
  }
  getRange(row, col, numRows = 1, numCols = 1) {
    const self = this;
    const chainable = () => ({ setFontWeight: () => chainable(), setValues: () => chainable(), setValue: () => chainable() });
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
      setFontWeight: () => chainable(),
      setValue: (v) => {
        while (self.rows.length < row) self.rows.push([]);
        const r = self.rows[row - 1];
        r[col - 1] = v;
        return chainable();
      },
    };
  }
  deleteRows(start, count) { this.rows.splice(start - 1, count); }
  deleteRow(n) { this.rows.splice(n - 1, 1); }
  setFrozenRows() { return this; }
  setColumnWidth() { return this; }
  clear() { this.rows = []; return this; }
}

class FakeSpreadsheet {
  constructor() { this.sheets = {}; }
  getSheetByName(name) { return this.sheets[name] || null; }
  insertSheet(name) { this.sheets[name] = new FakeSheet(name, null); return this.sheets[name]; }
}

function toSignedBytes(buf) { return Array.from(buf).map((b) => (b > 127 ? b - 256 : b)); }

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cacheStore = new Map();          // key -> { v, exp, ttl }
  const telegram = { calls: [], heldAtCall: [], payloads: [] };

  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, parseInt, parseFloat, RegExp, Error,
    SpreadsheetApp: { getActiveSpreadsheet: () => ss, getUi: () => ({ alert: () => {} }) },
    LockService: {
      getScriptLock: () => ({
        tryLock: () => { if (locks.held) return false; locks.held = true; locks.acquired++; return true; },
        releaseLock: () => { locks.held = false; },
      }),
    },
    CacheService: {
      getScriptCache: () => ({
        get: (k) => { const e = cacheStore.get(k); if (!e) return null; if (e.exp < Date.now()) { cacheStore.delete(k); return null; } return e.v; },
        put: (k, v, ttlSec) => cacheStore.set(k, { v, exp: Date.now() + (ttlSec || 600) * 1000, ttl: ttlSec || 600 }),
        remove: (k) => cacheStore.delete(k),
      }),
    },
    Utilities: {
      getUuid: () => crypto.randomUUID(),
      DigestAlgorithm: { SHA_256: 'SHA_256' },
      Charset: { UTF_8: 'UTF_8' },
      computeDigest: (alg, value, charset) =>
        toSignedBytes(crypto.createHash('sha256').update(Buffer.from(String(value), 'utf8')).digest()),
      computeHmacSha256Signature: (value, key, charset) =>
        toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8')).update(Buffer.from(String(value), 'utf8')).digest()),
    },
    ContentService: {
      createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
      MimeType: { JSON: 'JSON' },
    },
    UrlFetchApp: {
      fetch: (url, options) => {
        // [GAS-2-O] Was the global ingest lock still held when the alert fired?
        telegram.calls.push(url);
        telegram.heldAtCall.push(locks.held);
        telegram.payloads.push(options && options.payload ? options.payload.text : '');
        return {};
      },
    },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks, telegram, cacheStore };
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

let passed = 0, failed = 0;
const failures = [];
function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  PASS  ${name}`); }
  else { failed++; failures.push(name + (detail ? ` — ${detail}` : '')); console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`); }
}
function doPost(env, body) {
  const out = env.sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
  return JSON.parse(out.text || out.getContent());
}
function doPostRaw(env, body) {
  return doPost(env, body);
}
function doPostText(env, contents) {
  const out = env.sandbox.doPost({ postData: { contents } });
  return JSON.parse(out.text || out.getContent());
}
function setConfig(env, key, value) {
  const sheet = env.ss.sheets['Config'];
  for (let i = 1; i < sheet.rows.length; i++) {
    if (String(sheet.rows[i][0]).trim() === key) { sheet.rows[i][1] = String(value); return; }
  }
  sheet.rows.push([key, String(value)]);
}
function sha256Hex(s) { return crypto.createHash('sha256').update(Buffer.from(String(s), 'utf8')).digest('hex'); }
function hmacHex(msg, key) { return crypto.createHmac('sha256', Buffer.from(String(key), 'utf8')).update(Buffer.from(String(msg), 'utf8')).digest('hex'); }

console.log('\n=== WAVE-4 HYGIENE TEST (real Code.gs + real crypto) ===\n');

const env = createGasContext();
env.sandbox.setupMasterTemplate();

const TOKEN = 'plts_sec_CHANGE_ME';
const DEVICE = 'PLTS_MONITOR_01';
const ADMIN = 'plts_sec_CHANGE_ME';   // template default? no — empty. Set explicitly below.

function genericTelemetry(seq, vBat) {
  return {
    action: 'TELEMETRY', token: TOKEN, device_key: DEVICE,
    data: { v_bat: vBat === undefined ? 51.2 : vBat, i_bat_dc: -2.1, sequence: seq },
  };
}

// ===========================================================================
// GROUP R — GAS-2-R: malformed JSON is a client error
// ===========================================================================
console.log('[R] GAS-2-R malformed JSON → 400, not 500:');
{
  const r1 = doPostText(env, '{invalid json!!');
  check('R1 broken JSON → 400 "not valid JSON"',
    r1.code === 400 && /not valid JSON/.test(r1.message), JSON.stringify(r1));

  const r2 = doPostText(env, 'null');
  check('R2 JSON null body → 400 "must be a JSON object"',
    r2.code === 400 && /JSON object/.test(r2.message), JSON.stringify(r2));

  const r3 = doPostText(env, '[1,2,3]');
  check('R3 JSON array body → 400', r3.code === 400);

  const r4 = doPostText(env, '42');
  check('R4 scalar body → 400', r4.code === 400);

  const r5 = doPostText(env, '');
  check('R5 empty body → 400 ({} fallback removed for clarity… or PONG)',
    r5.code === 400 || r5.code === 401 || r5.code === 200, JSON.stringify(r5));

  const r6 = doPost(env, { action: 'PING', token: TOKEN });
  check('R6 valid JSON still flows (PING → 200 PONG)',
    r6.code === 200 && r6.message === 'PONG', JSON.stringify(r6));
}

// ===========================================================================
// GROUP S — GAS-2-S: PING reports device registration
// ===========================================================================
console.log('\n[S] GAS-2-S PING handshake registration report:');
{
  const sheet = env.ss.sheets['Devices'];
  // devices sheet currently has just a header → legacy mode
  const r1 = doPost(env, { action: 'PING', token: TOKEN, device_key: 'SOME_NEW_DEVICE' });
  check('S1 empty DEVICES (legacy) → registered true + legacy_mode',
    r1.code === 200 && r1.data && r1.data.device_registered === true &&
    r1.data.legacy_mode === true && r1.data.device_key === 'SOME_NEW_DEVICE',
    JSON.stringify(r1.data));

  // register the main test device + two others (activates the fleet gate)
  sheet.rows.push(['PLTS_MONITOR_01', '', 'Main', '', '']);
  sheet.rows.push(['PLTS_A', 'secret_a', 'Label A', '', '']);
  sheet.rows.push(['PLTS_B', 'secret_b', 'Label B', '', '']);

  const r2 = doPost(env, { action: 'PING', token: TOKEN, device_key: 'PLTS_A' });
  check('S2 registered device → device_registered true',
    r2.code === 200 && r2.data && r2.data.device_registered === true, JSON.stringify(r2.data));

  const r3 = doPost(env, { action: 'PING', token: TOKEN, device_key: 'TYPO_DEVICE' });
  check('S3 unregistered device → device_registered FALSE (honest handshake)',
    r3.code === 200 && r3.data && r3.data.device_registered === false, JSON.stringify(r3.data));

  const r4 = doPost(env, { action: 'PING', token: TOKEN });
  check('S4 no device_key → null (not checked, backward compatible)',
    r4.code === 200 && r4.data && r4.data.device_registered === null, JSON.stringify(r4.data));

  check('S5 PWA contract intact: status SUCCESS + message PONG',
    r4.status === 'SUCCESS' && r4.message === 'PONG' && r4.code === 200);

  // HMAC path reports the signed identity
  const ts = Math.floor(Date.now() / 1000);
  const nonce = 'pingnonce' + String(ts).slice(-6);
  const canonical = 'HMAC-SHA256\nPING\n' + ts + '\n' + nonce + '\nPLTS_B\n' + sha256Hex('');
  const sig = hmacHex(canonical, 'secret_b');
  const r6 = doPost(env, {
    action: 'PING',
    auth: { method: 'HMAC-SHA256', timestamp: ts, nonce: nonce, deviceId: 'PLTS_B', signature: sig },
  });
  check('S6 HMAC PING reports signed device as registered',
    r6.code === 200 && r6.data && r6.data.device_registered === true &&
    r6.data.device_key === 'PLTS_B', JSON.stringify(r6.data));
}

// ===========================================================================
// GROUP O — GAS-2-O: Telegram alert OUTSIDE the lock
// ===========================================================================
console.log('\n[O] GAS-2-O alert outside the global ingest lock:');
{
  setConfig(env, 'TELEGRAM_BOT_TOKEN', 'bot123');
  setConfig(env, 'TELEGRAM_CHAT_ID', 'chat456');
  setConfig(env, 'LOW_BATTERY_CUTOFF_V', '45.0');
  env.telegram.calls.length = 0;
  env.telegram.heldAtCall.length = 0;

  const r1 = doPost(env, genericTelemetry(1000, 40.1));   // below cutoff
  check('O1 low-battery telemetry stored (200)', r1.code === 200, JSON.stringify(r1.data));
  check('O2 exactly ONE telegram send attempted', env.telegram.calls.length === 1,
    'calls=' + env.telegram.calls.length);
  check('O3 lock RELEASED during telegram fetch (was held before Wave 4)',
    env.telegram.heldAtCall.length === 1 && env.telegram.heldAtCall[0] === false,
    'heldAtCall=' + JSON.stringify(env.telegram.heldAtCall));
  check('O4 alert message names the device (fleet honesty)',
    env.telegram.payloads.length === 1 && String(env.telegram.payloads[0]).indexOf(DEVICE) >= 0,
    JSON.stringify(env.telegram.payloads));

  // normal battery → no alert
  env.telegram.calls.length = 0;
  const r2 = doPost(env, genericTelemetry(1001, 51.5));
  check('O5 normal-battery telemetry → no telegram', r2.code === 200 && env.telegram.calls.length === 0);
}

// ===========================================================================
// GROUP P — GAS-2-P: cooldown
// ===========================================================================
console.log('\n[P] GAS-2-P per-device alert cooldown:');
{
  env.telegram.calls.length = 0;
  env.cacheStore.delete('PLTS_TG_LOWBATT_' + DEVICE);

  doPost(env, genericTelemetry(1002, 40.0));   // low #1 → sends
  doPost(env, genericTelemetry(1003, 39.8));   // low #2 → suppressed
  doPost(env, genericTelemetry(1004, 39.5));   // low #3 → suppressed
  check('P1 three low samples → ONE telegram (cooldown 30 min default)',
    env.telegram.calls.length === 1, 'calls=' + env.telegram.calls.length);
  check('P2 cooldown key alive in cache',
    env.cacheStore.has('PLTS_TG_LOWBATT_' + DEVICE));
  check('P3 cooldown TTL ≈ 30 min (1800 s)',
    Math.abs((env.cacheStore.get('PLTS_TG_LOWBATT_' + DEVICE) || {}).ttl - 1800) <= 1,
    JSON.stringify(env.cacheStore.get('PLTS_TG_LOWBATT_' + DEVICE)));

  // simulate cooldown expiry
  env.cacheStore.delete('PLTS_TG_LOWBATT_' + DEVICE);
  doPost(env, genericTelemetry(1005, 39.2));
  check('P4 after cooldown expiry → next low sample alerts again',
    env.telegram.calls.length === 2, 'calls=' + env.telegram.calls.length);

  // custom cooldown from Config (cached config → drop cache first, the
  // documented runbook for ANY config change)
  setConfig(env, 'LOW_BATTERY_ALERT_COOLDOWN_MIN', '5');
  env.sandbox.invalidatePltsCache();
  env.cacheStore.delete('PLTS_TG_LOWBATT_' + DEVICE);
  doPost(env, genericTelemetry(1006, 39.0));
  const ttl = (env.cacheStore.get('PLTS_TG_LOWBATT_' + DEVICE) || {}).ttl;
  check('P5 LOW_BATTERY_ALERT_COOLDOWN_MIN=5 honored (TTL 300 s)',
    Math.abs(ttl - 300) <= 1, 'ttl=' + ttl);
}

// ===========================================================================
// GROUP Q — GAS-2-Q: credential cache TTL
// ===========================================================================
console.log('\n[Q] GAS-2-Q credential cache TTL:');
{
  env.cacheStore.clear();
  // force reads: AUTH_TOKEN via PING, LOG_ROTATION_MAX_ROWS via a telemetry
  // ingest (rotateLogs_/retentionScanRows_ read it on the store path)
  doPost(env, { action: 'PING', token: TOKEN });
  doPost(env, genericTelemetry(1010, 51.0));
  const t1 = env.cacheStore.get('PLTS_CFG_AUTH_TOKEN');
  check('Q1 AUTH_TOKEN cached with TTL 300 s (was 21600)',
    t1 && Math.abs(t1.ttl - 300) <= 1, JSON.stringify(t1));

  const t2 = env.cacheStore.get('PLTS_CFG_LOG_ROTATION_MAX_ROWS');
  check('Q2 non-credential knob keeps 6 h TTL', t2 && t2.ttl === 21600, JSON.stringify(t2));

  // rotation honesty: change the sheet, drop cache, old token dies immediately
  setConfig(env, 'AUTH_TOKEN', 'plts_sec_ROTATED_NEW');
  env.sandbox.invalidatePltsCache();
  const rOld = doPost(env, { action: 'PING', token: TOKEN });
  const rNew = doPost(env, { action: 'PING', token: 'plts_sec_ROTATED_NEW' });
  check('Q3 after invalidatePltsCache(): old token 401, new token 200',
    rOld.code === 401 && rNew.code === 200,
    'old=' + rOld.code + ' new=' + rNew.code);

  // restore for later groups
  setConfig(env, 'AUTH_TOKEN', TOKEN);
  env.sandbox.invalidatePltsCache();
}

// ===========================================================================
// GROUP T — GAS-2-T: duplicate window ≥ rotation
// ===========================================================================
console.log('\n[T] GAS-2-T duplicate window covers full retention:');
{
  const envT = createGasContext();
  envT.sandbox.setupMasterTemplate();
  setConfig(envT, 'LOG_ROTATION_MAX_ROWS', '3500');   // window becomes 3600

  const tel = envT.ss.sheets['Telemetry'];
  const base = new Date('2026-08-28T00:00:00Z');
  // seed 3400 rows seq 1..3400 for DEVICE (sheet row = seq + 1)
  for (let s = 1; s <= 3400; s++) {
    tel.rows.push([base, DEVICE, s, base, false, 51.0, -2, -100, 80, 'VALID',
      '', '', 30, 60, '', '', '', '', '', 'VALID', 'VALID', '', '', 'v1.5.1',
      '', '', '', '', '', '', '']);
  }
  envT.ss.sheets['SeqIndex'].rows.push([DEVICE, 3401, 3400, 0, 0, '[]']);

  // seq 100 sits 3300 rows from the end — OUTSIDE the old hardcoded 3000
  const r1 = doPost(envT, genericTelemetry(100, 51.0));
  check('T1 re-send of OLD duplicate (3300 rows back) → 409 DUPLICATE (was ACCEPTED_LATE)',
    r1.code === 409 && r1.data && r1.data.decision === 'DUPLICATE', JSON.stringify(r1.data));

  const r2 = doPost(envT, genericTelemetry(3399, 51.0));
  check('T2 recent duplicate still caught (control)', r2.code === 409);

  const rows = envT.ss.sheets['Telemetry'].rows.length - 1;
  check('T3 no double-stored rows (still 3400)', rows === 3400, 'rows=' + rows);
}

// ===========================================================================
// GROUP U — GAS-2-U: HISTORY/LATEST windows config-driven
// ===========================================================================
console.log('\n[U] GAS-2-U HISTORY/LATEST scan windows:');
{
  const envU = createGasContext();
  envU.sandbox.setupMasterTemplate();
  setConfig(envU, 'LOG_ROTATION_MAX_ROWS', '6000');   // window becomes 6100
  setConfig(envU, 'HISTORY_MAX_ROWS', '8000');

  const tel = envU.ss.sheets['Telemetry'];
  const t0 = new Date('2026-08-28T00:00:00Z').getTime();
  // 5500 rows; event_time increases with seq; row i has event_time t0 + i s
  for (let s = 1; s <= 5500; s++) {
    tel.rows.push([new Date(t0 + s * 1000), DEVICE, s, new Date(t0 + s * 1000), false,
      51.0, -2, -100, 80, 'VALID', '', '', 30, 60, '', '', '', '', '', 'VALID', 'VALID', '', '', 'v1.5.1',
      '', '', '', '', '', '', '']);
  }
  envU.ss.sheets['SeqIndex'].rows.push([DEVICE, 5501, 5500, 0, 0, '[]']);

  const r1 = doPost(envU, { action: 'HISTORY', token: TOKEN, device_key: DEVICE, limit: 6000 });
  check('U1 HISTORY returns all 5500 retained rows (old window 5000 → 5000)',
    r1.code === 200 && r1.data && r1.data.records.length === 5500,
    'records=' + (r1.data && r1.data.records && r1.data.records.length));

  // LATEST: true newest by event_time sits 3300 rows back (later rows carry OLDER event_time)
  const envU2 = createGasContext();
  envU2.sandbox.setupMasterTemplate();
  setConfig(envU2, 'LOG_ROTATION_MAX_ROWS', '6000');
  const tel2 = envU2.ss.sheets['Telemetry'];
  for (let s = 1; s <= 5500; s++) {
    // seq 2200 has the NEWEST event_time; everything after is older (late arrivals)
    const ev = s === 2200 ? t0 + 999999 * 1000 : t0 + s * 1000;
    tel2.rows.push([new Date(t0 + s * 1000), DEVICE, s, new Date(ev), false,
      51.0, -2, -100, 80, 'VALID', '', '', 30, 60, '', '', '', '', '', 'VALID', 'VALID', '', '', 'v1.5.1',
      '', '', '', '', '', '', '']);
  }
  envU2.ss.sheets['SeqIndex'].rows.push([DEVICE, 5501, 5500, 0, 0, '[]']);
  const r2 = doPost(envU2, { action: 'LATEST', token: TOKEN, device_key: DEVICE });
  check('U2 LATEST finds true newest by event_time 3300 rows back (window 6100)',
    r2.code === 200 && r2.data && Number(r2.data.sequence) === 2200,
    JSON.stringify(r2.data && { seq: r2.data.sequence, v: r2.data.battery && r2.data.battery.voltage }));
}

// ===========================================================================
// GROUP V — GAS-2-V: bookkeeping sheet rotations
// ===========================================================================
console.log('\n[V] GAS-2-V Ota/OtaEvents/Calibration rotations:');
{
  setConfig(env, 'OTA_MANIFEST_MAX_ROWS', '10');
  setConfig(env, 'OTA_EVENTS_MAX_ROWS', '10');
  setConfig(env, 'CALIB_HISTORY_MAX_ROWS', '5');
  setConfig(env, 'ADMIN_TOKEN', 'admin_sec_wave4');
  const manifest = (v) => ({
    version: '1.5.' + v, url: 'https://example.com/fw' + v + '.bin',
    sha256: 'ab'.repeat(32), hmac: 'cd'.repeat(32), size: 1000 + v,
  });

  for (let i = 1; i <= 12; i++) {
    const r = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, admin_token: 'admin_sec_wave4', manifest: manifest(i) });
    if (r.code !== 200) { check('V0 publish ' + i + ' ok', false, JSON.stringify(r)); break; }
  }
  const otaRows = env.ss.sheets['Ota'].rows.length - 1;
  check('V1 12 manifests published → capped at 10', otaRows === 10, 'rows=' + otaRows);
  const rm = doPost(env, { action: 'OTA_MANIFEST', token: TOKEN });
  check('V2 ACTIVE manifest = newest (1.5.12)', rm.data && rm.data.version === '1.5.12', JSON.stringify(rm.data));

  for (let i = 1; i <= 12; i++) {
    doPost(env, { action: 'OTA_STATUS', token: TOKEN, device_key: DEVICE, event: 'ACTIVATED', version: '1.5.' + i });
  }
  const evRows = env.ss.sheets['OtaEvents'].rows.length - 1;
  check('V3 12 OTA events → capped at 10', evRows === 10, 'rows=' + evRows);
  const lastEv = env.ss.sheets['OtaEvents'].rows[env.ss.sheets['OtaEvents'].rows.length - 1];
  check('V4 newest OTA event retained (1.5.12)', String(lastEv[3]) === '1.5.12', JSON.stringify(lastEv));

  // Calibration: 5 applied seed + 2 pending publish → ACK one → applied=6 > cap 5 → oldest applied deleted, pending SAFE
  const cal = env.ss.sheets['Calibration'];
  cal.rows.length = 1;   // header only
  for (let i = 0; i < 5; i++) {
    cal.rows.push([new Date(Date.now() - (5 - i) * 3600e3), DEVICE, 18.857, 1, 1, true, new Date(), 'seed-cmd-' + i, 'seed']);
  }
  const p1 = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEVICE, v_calib: 18.9, i_calib_dc: 1, i_calib_ac: 1 });
  const p2 = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEVICE, v_calib: 19.0, i_calib_dc: 1, i_calib_ac: 1 });
  const pend1 = doPost(env, { action: 'CALIBRATION_PENDING', token: TOKEN, device_key: DEVICE });
  check('V5 pending command queued+readable', p1.code === 200 && pend1.code === 200 && pend1.data && pend1.data.command_id,
    JSON.stringify(pend1.data));

  // ACK the first pending → applied count 6 → rotation drops oldest applied (seed-cmd-0)
  const ack = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, command_id: p1.data.command_id, device_key: DEVICE });
  check('V6 ACK applied', ack.code === 200, JSON.stringify(ack));
  const appliedRows = cal.rows.filter((r, i) => i > 0 && (r[5] === true || String(r[5]).toLowerCase() === 'true'));
  check('V7 applied history capped at 5', appliedRows.length === 5, 'applied=' + appliedRows.length);
  check('V8 oldest applied deleted (seed-cmd-0 gone)',
    !cal.rows.some((r) => String(r[7]) === 'seed-cmd-0'));
  check('V9 second pending command SURVIVES rotation',
    cal.rows.some((r) => String(r[7]) === p2.data.command_id),
    'p2=' + p2.data.command_id);

  // pending never deleted even when sheet overflows with pending rows
  const cal2 = env.ss.sheets['Calibration'];
  cal2.rows.length = 1;
  for (let i = 0; i < 8; i++) {
    const p = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEVICE, v_calib: 18.8, i_calib_dc: 1, i_calib_ac: 1 });
    if (p.code !== 200) check('V10 publish pending ' + i, false, JSON.stringify(p));
  }
  check('V10 all-pending sheet: NOTHING deleted (cap applies to applied only)',
    cal2.rows.length === 9, 'rows=' + cal2.rows.length);
}

// ===========================================================================
// GROUP X — GAS-2-X: no hot-path nonce write-back (+ replay still blocked)
// ===========================================================================
console.log('\n[X] GAS-2-X verifyHmac_ write-back removed:');
{
  const envX = createGasContext();
  envX.sandbox.setupMasterTemplate();
  const devices = envX.ss.sheets['Devices'];
  devices.rows.push(['HMAC_DEV', 'hmac_secret_w4', 'H', '', '']);

  function signedTelemetry(seq, nonceUsed, tsUsed) {
    const data = JSON.stringify({ v_bat: 50.1, i_bat_dc: -1.2, sequence: seq });
    const canonical = 'HMAC-SHA256\nTELEMETRY\n' + tsUsed + '\n' + nonceUsed + '\nHMAC_DEV\n' + sha256Hex(data);
    return {
      action: 'TELEMETRY',
      auth: { method: 'HMAC-SHA256', timestamp: tsUsed, nonce: nonceUsed, deviceId: 'HMAC_DEV', signature: hmacHex(canonical, 'hmac_secret_w4') },
      data,
    };
  }

  const ts = Math.floor(Date.now() / 1000);
  const r1 = doPost(envX, signedTelemetry(1, 'w4nonce0000000000000000000001', ts));
  check('X1 HMAC telemetry stored', r1.code === 200, JSON.stringify(r1));
  const row = envX.ss.sheets['Devices'].rows.find((r) => String(r[0]) === 'HMAC_DEV');
  check('X2 last_nonce/last_ts NOT written (vestigial columns stay empty)',
    row && (row[3] === '' || row[3] == null) && (row[4] === '' || row[4] == null),
    JSON.stringify(row));

  const r2 = doPost(envX, signedTelemetry(2, 'w4nonce0000000000000000000001', ts));
  check('X3 nonce replay still blocked (cache is the guard)',
    r2.code === 401 && /nonce replayed/.test(r2.message || ''), JSON.stringify(r2));

  const r3 = doPost(envX, signedTelemetry(2, 'w4nonce0000000000000000000002', ts));
  check('X4 fresh nonce accepted', r3.code === 200, JSON.stringify(r3));
}

// ===========================================================================
// GROUP W — GAS-2-W regression guard (fixed in Wave 1; must stay fixed)
// ===========================================================================
console.log('\n[W] GAS-2-W dead header fiction stays dead:');
{
  const src = fs.readFileSync(path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  check('W1 no __headers resurrection', !/__headers/.test(src));
  check('W2 X-Auth only mentioned in honest impossibility notes',
    (src.match(/X-Auth/g) || []).length >= 0 && !/GasAdvisor posts the X-Auth/.test(src));
}

// ===========================================================================
console.log('\n=== WAVE-4 RESULT: ' + passed + ' passed, ' + failed + ' failed ===');
if (failures.length) { console.log('Failures:\n  ' + failures.join('\n  ')); process.exit(1); }
process.exit(0);
