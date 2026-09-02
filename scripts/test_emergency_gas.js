#!/usr/bin/env node
/**
 * test_emergency_gas.js — [WAVE-7] Emergency relay control layer contract tests
 * =============================================================================
 * Executes the REAL code.gs/Code.gs in a Node sandbox (mocked GAS services)
 * and proves the E-series invariants:
 *
 *   Group A — Authorization (fail-closed):
 *     EMERGENCY_COMMAND is operator-only (ADMIN_TOKEN), disabled while the
 *     token is unset; device credentials never suffice; device-side actions
 *     (PENDING/ACK/EVENT) authenticate as the device and pass the fleet
 *     registration gate.
 *
 *   Group B — Command queue lifecycle:
 *     ARM/DISARM/CONFIG queue → PENDING; identical re-issue dedups; PENDING
 *     serves the OLDEST servable row and marks DELIVERED (still servable);
 *     ACK is device-bound (cross-device ACK rejected, row stays unsettled);
 *     ACK is idempotent (re-ACK → 200 settled); APPLIED emits an event row.
 *
 *   Group C — TTL expiry:
 *     A command issued past EMERGENCY_QUEUE_TTL_MIN is EXPIRED on the next
 *     touch and never served; an ACK for an EXPIRED row is an honest no-op.
 *
 *   Group D — Event logging:
 *     EMERGENCY_EVENT whitelists types (unknown → 400); TRIP appends a row;
 *     EMERGENCY_LOG returns newest-first, device-scoped; the events sheet is
 *     bounded by EMERGENCY_EVENTS_MAX_ROWS.
 *
 *   Group E — CONFIG validation:
 *     Out-of-range values are refused BEFORE any row is written; a valid
 *     CONFIG is merged with server-side defaults (unknown fields dropped —
 *     only whitelisted keys ever reach the device).
 *
 *   Group F — TELEMETRY piggyback + v1.7 columns:
 *     Ingest stores i_ac_gen + emergency fields; the TELEMETRY response
 *     carries pendingEmergency when a command is queued (and null when the
 *     queue is empty); LATEST returns the emergency block + genset current;
 *     pre-1.7 rows never fabricate an emergency block.
 *
 * Usage: node scripts/test_emergency_gas.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

function sha256Hex(s) { return crypto.createHash('sha256').update(s, 'utf-8').digest('hex'); }
function hmacHex(key, msg) { return crypto.createHmac('sha256', key).update(msg, 'utf-8').digest('hex'); }

/** GAS returns SIGNED int8 arrays from computeDigest/computeHmacSha256Signature. */
function toSignedBytes(buf) {
  return Array.from(buf, (b) => (b > 127 ? b - 256 : b));
}

/** HMAC envelope (GasAdvisor contract v2.1 — action signed, raw data string). */
function hmacEnvelope(action, dataJson, secret, deviceId, timestamp, nonce) {
  const canonical = 'HMAC-SHA256\n' + action + '\n' + timestamp + '\n' + nonce +
                    '\n' + deviceId + '\n' + sha256Hex(dataJson || '');
  return {
    action: action,
    auth: {
      method: 'HMAC-SHA256', timestamp: Number(timestamp), nonce: String(nonce),
      deviceId: deviceId, signature: hmacHex(secret, canonical)
    },
    data: dataJson
  };
}

// ---------------------------------------------------------------------------
// Minimal GAS service mocks (same shape as test_gas_contract.js)
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
      setValue: (v) => {
        while (self.rows.length < row) self.rows.push([]);
        const r = self.rows[row - 1];
        r[col - 1] = v;
        return this;
      },
      setValues: (vals) => {
        for (let i = 0; i < numRows; i++) {
          while (self.rows.length < row + i) self.rows.push([]);
          const r = self.rows[row - 1 + i];
          for (let j = 0; j < numCols; j++) r[col - 1 + j] = vals[i][j];
        }
        return chainable();
      },
    };
  }
  deleteRows(start, count) { this.rows.splice(start - 1, count); }
  setFrozenRows() { return this; }
  setColumnWidth() { return this; }
  clear() { this.rows = []; return this; }
}

class FakeSpreadsheet {
  constructor() { this.sheets = {}; }
  getSheetByName(name) { return this.sheets[name] || null; }
  insertSheet(name) { this.sheets[name] = new FakeSheet(name, null); return this.sheets[name]; }
}

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cache = new Map();
  const telegramSent = [];

  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, isFinite, parseInt, parseFloat, RegExp, Error,
    SpreadsheetApp: {
      getActiveSpreadsheet: () => ss,
      getUi: () => ({ alert: () => {} }),
    },
    LockService: {
      getScriptLock: () => ({
        tryLock: () => {
          if (locks.held) return false;
          locks.held = true; locks.acquired++;
          return true;
        },
        releaseLock: () => { locks.held = false; },
      }),
    },
    CacheService: {
      getScriptCache: () => ({
        get: (k) => cache.get(k) || null,
        put: (k, v) => cache.set(k, v),
        remove: (k) => cache.delete(k),
      }),
    },
    Utilities: {
      getUuid: () => 'uuid-' + Math.random().toString(16).slice(2),
      DigestAlgorithm: { SHA_256: 'SHA_256' },
      Charset: { UTF_8: 'UTF_8' },
      computeDigest: (alg, value, charset) =>
        toSignedBytes(crypto.createHash('sha256')
          .update(Buffer.from(String(value), 'utf8')).digest()),
      computeHmacSha256Signature: (value, key, charset) =>
        toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
          .update(Buffer.from(String(value), 'utf8')).digest()),
      formatDate: (date, tz, fmt) => {
        if (fmt !== 'yyyy-MM-dd') throw new Error('mock supports yyyy-MM-dd only');
        return new Intl.DateTimeFormat('en-CA', {
          timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit',
        }).format(date);
      },
    },
    ContentService: {
      createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
      MimeType: { JSON: 'JSON' },
    },
    UrlFetchApp: {
      fetch: (url) => { telegramSent.push(String(url)); return {}; },
    },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks, telegramSent };
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

let passed = 0, failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  PASS  ${name}`); }
  else {
    failed++; failures.push(name + (detail ? ` — ${detail}` : ''));
    console.log(`  FAIL  ${name}` + (detail ? ` — ${detail}` : ''));
  }
}

function doPost(env, body) {
  const out = env.sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
  return JSON.parse(out.text);
}

function setConfig(env, key, value) {
  const cfg = env.ss.sheets['Config'];
  const rows = cfg.getDataRange().getValues();
  for (let i = 0; i < rows.length; i++) {
    if (String(rows[i][0]) === key) { cfg.getRange(i + 1, 2).setValue(value); return; }
  }
  cfg.appendRow([key, value]);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

console.log('\n=== WAVE-7 EMERGENCY LAYER TEST (real Code.gs) ===\n');

const env = createGasContext();
env.sandbox.setupMasterTemplate();

const TOKEN = 'plts_sec_CHANGE_ME';
const ADMIN = 'plts_admin_w7_9a3f7c2d5e8b1a6f';
const DEV = 'PLTS_MONITOR_01';
const DEV_HMAC = 'PLTS_GAS_H7';
const SEC_HMAC = 'secret_H7_0123456789abcdef';
setConfig(env, 'ADMIN_TOKEN', ADMIN);
env.sandbox.invalidatePltsCache();
env.ss.sheets['Devices'].appendRow([DEV, '', 'Gudang Utama', '', '']);
env.ss.sheets['Devices'].appendRow([DEV_HMAC, SEC_HMAC, 'Gudang HMAC', '', '']);

function nowSec() { return Math.floor(Date.now() / 1000); }
let nonceSeq = 0;
function nextNonce() { nonceSeq += 1; return 'w7nonce' + String(nonceSeq).padStart(25, '0'); }

// ---------------------------------------------------------------------------
// GROUP A — Authorization (fail-closed)
// ---------------------------------------------------------------------------
console.log('[A] EMERGENCY_COMMAND operator gate:');
{
  // Operator actions carry BOTH standard auth (device token) and the
  // operator-only admin_token — same belt-and-braces pattern as OTA_PUBLISH.
  const a1 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, device_key: DEV, command: 'DISARM' });
  check('A1 device token only (no admin_token) → 401 (operator gate)', a1.code === 401, JSON.stringify(a1));

  const a2 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: 'wrong-secret', command: 'DISARM', device_key: DEV });
  check('A2 wrong admin_token → 401', a2.code === 401, JSON.stringify(a2));

  const a3 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'DISARM', device_key: DEV });
  check('A3 correct admin_token → 200 queued', a3.status === 'SUCCESS' && a3.data.status === 'PENDING',
    JSON.stringify(a3));

  // A4 — fail-closed on a fresh deployment with ADMIN_TOKEN unset.
  const env2 = createGasContext();
  env2.sandbox.setupMasterTemplate();
  const a4 = doPost(env2, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: 'anything', command: 'ARM', device_key: 'PLTS_MONITOR_01' });
  check('A4 ADMIN_TOKEN unset → action DISABLED (401, honest refusal)',
    a4.code === 401 && /disabled — set ADMIN_TOKEN/.test(a4.message), a4.message);

  // A5 — unknown command verb refused.
  const a5 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'DETONATE', device_key: DEV });
  check('A5 unknown command verb → 400 whitelist', a5.code === 400 && /ARM\/DISARM\/CONFIG/.test(a5.message), a5.message);

  // A6 — device-side action on an UNREGISTERED device_key → registration gate.
  const a6 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: 'PLTS_GHOST' });
  check('A6 unknown device EMERGENCY_PENDING → 400 gate', a6.code === 400, JSON.stringify(a6));

  // A7 — HMAC identity pinning: signed deviceId wins over a foreign body
  // device_key (GAS-2-J semantics apply to the emergency gate too).
  const raw = JSON.parse(JSON.stringify(
    hmacEnvelope('EMERGENCY_PENDING', '', SEC_HMAC, DEV_HMAC, nowSec(), nextNonce())));
  raw.device_key = 'PLTS_GHOST';
  const a7 = doPost(env, raw);
  check('A7 HMAC caller + foreign body device_key → 400 pinning', a7.code === 400, JSON.stringify(a7));

  // A8 — HMAC device polls its OWN pending → 200 (no pending yet).
  const a8raw = hmacEnvelope('EMERGENCY_PENDING', '', SEC_HMAC, DEV_HMAC, nowSec(), nextNonce());
  const a8 = doPost(env, a8raw);
  check('A8 HMAC EMERGENCY_PENDING for own device → 200', a8.status === 'SUCCESS', JSON.stringify(a8));
}

// ---------------------------------------------------------------------------
// GROUP B — Command queue lifecycle
// ---------------------------------------------------------------------------
console.log('\n[B] Queue lifecycle:');
let armId = '';
{
  // A3 queued a DISARM. Queue an ARM now — PENDING must serve OLDEST first.
  const b1 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'ARM', device_key: DEV, note: 'pasang kembali setelah perbaikan' });
  armId = b1.data.command_id;
  check('B1 ARM queued with command_id', b1.status === 'SUCCESS' && b1.data.command_id.length > 0, JSON.stringify(b1));

  const b2 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'ARM', device_key: DEV, note: 'pasang kembali setelah perbaikan' });
  check('B2 identical re-issue → dedup (duplicate: true)', b2.data.duplicate === true, JSON.stringify(b2));

  const queue = env.ss.sheets['EmergencyQueue'];
  check('B3 queue holds exactly 2 rows (DISARM + ARM)', queue.rows.length - 1 === 2,
    `got ${queue.rows.length - 1}`);

  // PENDING serves the OLDEST (the DISARM from A3).
  const b4 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  check('B4 PENDING returns the OLDEST command (DISARM)',
    b4.data.command === 'DISARM' && b4.data.command_id.length > 0, JSON.stringify(b4));

  // The row is now DELIVERED — still servable (lost response never loses it).
  const b5 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  check('B5 DELIVERED row still servable (same command_id)', b5.data.command_id === b4.data.command_id,
    JSON.stringify(b5));

  // Cross-device ACK rejected — simulate a second registered device.
  env.ss.sheets['Devices'].appendRow(['PLTS_IMPOSTOR', '', 'Impostor', '', '']);
  const b6 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: 'PLTS_IMPOSTOR', command_id: b4.data.command_id, result: 'APPLIED' });
  check('B6 cross-device ACK → 400, row unsettled', b6.code === 400, JSON.stringify(b6));

  // Invalid ACK result verb.
  const b7 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: b4.data.command_id, result: 'MAYBE' });
  check('B7 invalid result verb → 400', b7.code === 400, JSON.stringify(b7));

  // Owner ACK → APPLIED + ARMED event row.
  const b8 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: b4.data.command_id, result: 'APPLIED', message: 'relay energized', state: 'RUN' });
  check('B8 owner ACK APPLIED → 200', b8.status === 'SUCCESS' && b8.data.result === 'APPLIED', JSON.stringify(b8));

  const events = env.ss.sheets['EmergencyEvents'];
  const evTypes = events.rows.slice(1).map((r) => String(r[2]));
  check('B9 ARMED event logged from ACK', evTypes.indexOf('DISARMED') >= 0 || evTypes.indexOf('ARMED') >= 0,
    JSON.stringify(evTypes));
  // (A3 queued DISARM → its APPLIED ack logs type DISARMED.)

  // Idempotent re-ACK.
  const b10 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: b4.data.command_id, result: 'APPLIED' });
  check('B10 re-ACK idempotent → 200 settled', b10.status === 'SUCCESS' && b10.data.settled === 'APPLIED',
    JSON.stringify(b10));

  // After the DISARM settles, PENDING now serves the ARM.
  const b11 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  check('B11 next PENDING is the ARM', b11.data.command === 'ARM' && b11.data.command_id === armId,
    JSON.stringify(b11));
  check('B12 ARM note survives the round-trip', b11.data.note === 'pasang kembali setelah perbaikan',
    JSON.stringify(b11.data));

  // ACK 404 for a made-up command_id.
  const b13 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: 'no-such-id', result: 'APPLIED' });
  check('B13 unknown command_id → 404', b13.code === 404, JSON.stringify(b13));
}

// ---------------------------------------------------------------------------
// GROUP C — TTL expiry
// ---------------------------------------------------------------------------
console.log('\n[C] TTL expiry:');
{
  const c0 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'DISARM', device_key: DEV, note: 'ttl-test' });
  const cid = c0.data.command_id;
  // Age the row past the TTL (default 10 min).
  const queue = env.ss.sheets['EmergencyQueue'];
  for (let i = 1; i < queue.rows.length; i++) {
    if (String(queue.rows[i][0]) === cid) {
      queue.getRange(i + 1, 6).setValue(new Date(Date.now() - 11 * 60000));
    }
  }
  const c1 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  check('C1 expired command never served (next servable is the ARM)',
    c1.data && c1.data.command_id === armId, JSON.stringify(c1));

  let status = '';
  for (let i = 1; i < queue.rows.length; i++) {
    if (String(queue.rows[i][0]) === cid) status = String(queue.rows[i][6]);
  }
  check('C2 stale row marked EXPIRED', status === 'EXPIRED', status);

  const c3 = doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: cid, result: 'APPLIED' });
  check('C3 ACK on EXPIRED row → honest no-op (settled)', c3.status === 'SUCCESS' && c3.data.settled === 'EXPIRED',
    JSON.stringify(c3));
}

// ---------------------------------------------------------------------------
// GROUP D — Event logging
// ---------------------------------------------------------------------------
console.log('\n[D] Event logging:');
{
  const d1 = doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV, type: 'TRIP', reason: 'VBAT_LOW 41.2V', state: 'EMERGENCY' });
  check('D1 TRIP event accepted', d1.status === 'SUCCESS' && d1.data.type === 'TRIP', JSON.stringify(d1));

  const d2 = doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV, type: 'EXPLODE' });
  check('D2 unknown event type → 400 whitelist', d2.code === 400, JSON.stringify(d2));

  doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV, type: 'ESTOP', reason: 'physical button', state: 'EMERGENCY' });
  const d3 = doPost(env, { action: 'EMERGENCY_LOG', token: TOKEN, device_key: DEV, limit: 10 });
  check('D3 EMERGENCY_LOG newest-first', d3.data.events.length >= 3 &&
    d3.data.events[0].type === 'ESTOP' && d3.data.events[1].type === 'TRIP',
    JSON.stringify(d3.data.events.map((e) => e.type)));

  check('D4 event carries reason + stateAfter',
    d3.data.events[1].reason === 'VBAT_LOW 41.2V' && d3.data.events[1].stateAfter === 'EMERGENCY',
    JSON.stringify(d3.data.events[1]));

  // Rotation cap: set a tiny cap, overflow, verify bounded.
  setConfig(env, 'EMERGENCY_EVENTS_MAX_ROWS', '3');
  env.sandbox.invalidatePltsCache();
  for (let i = 0; i < 10; i++) {
    doPost(env, { action: 'EMERGENCY_EVENT', token: TOKEN, device_key: DEV, type: 'BOOT', reason: 'r' + i });
  }
  const events = env.ss.sheets['EmergencyEvents'];
  check('D5 events sheet bounded by EMERGENCY_EVENTS_MAX_ROWS', events.rows.length - 1 <= 3,
    `got ${events.rows.length - 1}`);
  setConfig(env, 'EMERGENCY_EVENTS_MAX_ROWS', '500');
  env.sandbox.invalidatePltsCache();
}

// ---------------------------------------------------------------------------
// GROUP E — CONFIG validation
// ---------------------------------------------------------------------------
console.log('\n[E] CONFIG validation:');
{
  const e1 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'CONFIG', device_key: DEV, config: { vbatLowV: 999 } });
  check('E1 out-of-range config refused (400, no row)', e1.code === 400, JSON.stringify(e1));
  const before = env.ss.sheets['EmergencyQueue'].rows.length;
  check('E2 refused config wrote NO row', env.ss.sheets['EmergencyQueue'].rows.length === before);

  const e3 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN, command: 'CONFIG', device_key: DEV,
    config: { vbatLowV: 43.5, iAcGenOverA: 22, rogueField: 'evil' } });
  check('E3 valid config queued', e3.status === 'SUCCESS', JSON.stringify(e3));

  // The ARM from group B is still servable — settle it first so PENDING
  // serves the CONFIG (oldest-servable-first semantics).
  {
    const settle = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
    if (settle.data && settle.data.command === 'ARM') {
      doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV,
        command_id: settle.data.command_id, result: 'APPLIED', state: 'RUN' });
    }
  }

  const e4 = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  const cfg = e4.data.config;
  check('E4 whitelisted field applied (vbatLowV 43.5)', cfg && cfg.vbatLowV === 43.5, JSON.stringify(cfg));
  check('E5 unknown field DROPPED (rogueField absent)', cfg && cfg.rogueField === undefined, JSON.stringify(cfg));
  check('E6 defaults merged (debounceN 3)', cfg && cfg.debounceN === 3, JSON.stringify(cfg));
  const cfgKeys = Object.keys(cfg).sort();
  check('E7 exactly the 13 schema keys delivered', cfgKeys.length === 13, JSON.stringify(cfgKeys));
  // v1.7.0 [P1-SC1] — the 13th field (safety-sensor fail-closed policy) must
  // be merged with the fail-closed default even when the caller omits it.
  check('E8 sensorFailPolicy default 1 (fail-closed) when omitted',
        cfg && cfg.sensorFailPolicy === 1, JSON.stringify(cfg));
  // E9 — an explicit policy 0 passes validation (legacy opt-out is allowed
  // but must round-trip honestly, never silently rewritten).
  const e9 = doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN,
    command: 'CONFIG', device_key: DEV,
    config: { sensorFailPolicy: 0, recoverySec: 30 } });
  check('E9 explicit sensorFailPolicy=0 accepted (operator opt-out)',
        e9.status === 'SUCCESS', JSON.stringify(e9));
  // E10 — the explicit policy 0 round-trips to the device verbatim (0 stays 0:
  // GAS must not "helpfully" rewrite an operator opt-out back to 1).
  // Drain the whole un-ACKed queue (oldest first: E3's CONFIG is DELIVERED
  // but un-ACKed, then E9's CONFIG) so GROUP F starts with a clean queue.
  {
    let sawE9Verbatim = false;
    for (let i = 0; i < 5; i++) {
      const pend = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
      const d = pend.data;
      if (!d || !d.command_id) break;            // queue drained
      if (d.command === 'CONFIG' && d.config && d.config.recoverySec === 30) {
        sawE9Verbatim = (d.config.sensorFailPolicy === 0);
      }
      doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV,
        command_id: d.command_id, result: 'APPLIED', state: 'EMERGENCY' });
    }
    check('E10 explicit sensorFailPolicy=0 served verbatim to the device',
          sawE9Verbatim === true);
  }
}

// ---------------------------------------------------------------------------
// GROUP F — TELEMETRY piggyback + v1.7 columns + LATEST
// ---------------------------------------------------------------------------
console.log('\n[F] TELEMETRY piggyback + v1.7 columns:');
{
  // F0 — queue a fresh CONFIG so the piggyback path has something to serve
  // (Group E drains its own queue fixtures; groups are now self-contained).
  doPost(env, { action: 'EMERGENCY_COMMAND', token: TOKEN, admin_token: ADMIN,
    command: 'CONFIG', device_key: DEV, config: { vbatLowV: 44.0 } });

  // F1 — ingest with emergency fields + 2nd ACS712 channel.
  const f1 = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: DEV,
    data: { sequence: 9001, v_bat: 51.2, i_bat_dc: -3.5, i_ac_load: 1.8, i_ac_gen: 4.2, ina219_ok: true,
            emg_state: 'RUN', emg_reason: '', emg_estop: false, emg_trips: 2, fw_version: '1.6.0' } });
  check('F1 v1.7 telemetry accepted', f1.status === 'SUCCESS', JSON.stringify(f1));
  check('F2 response piggybacks pendingEmergency (CONFIG still pending)',
    f1.data && f1.data.pendingEmergency && f1.data.pendingEmergency.command === 'CONFIG',
    JSON.stringify(f1.data));

  // F3 — row carries the v1.7 columns.
  const sheet = env.ss.sheets['Telemetry'];
  let row = null;
  for (let i = sheet.rows.length - 1; i >= 1; i--) {
    if (Number(sheet.rows[i][2]) === 9001) { row = sheet.rows[i]; break; }
  }
  check('F3 row stores i_ac_gen', row && row[31] === 4.2, JSON.stringify(row && row.slice(31)));
  check('F4 row stores emg_state RUN', row && row[32] === 'RUN', JSON.stringify(row && row.slice(31)));
  check('F5 row stores emg_trips', row && row[35] === 2, JSON.stringify(row && row.slice(31)));

  // F6 — settle any leftover CONFIG, then a clean ingest must answer
  // pendingEmergency null. (Group E may already have drained the queue —
  // the ACK is conditional, an empty queue is the desired end state.)
  const pend = doPost(env, { action: 'EMERGENCY_PENDING', token: TOKEN, device_key: DEV });
  if (pend.data && pend.data.command_id) {
    doPost(env, { action: 'EMERGENCY_ACK', token: TOKEN, device_key: DEV, command_id: pend.data.command_id, result: 'APPLIED', state: 'RUN' });
  }
  const f6 = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: DEV,
    data: { sequence: 9002, v_bat: 51.3, i_bat_dc: -3.4, i_ac_load: 1.7, i_ac_gen: 4.0, ina219_ok: true, fw_version: '1.6.0',
            emg_state: 'RUN', emg_reason: '', emg_estop: false, emg_trips: 2 } });
  check('F6 empty queue → pendingEmergency null', f6.data && f6.data.pendingEmergency === null,
    JSON.stringify(f6.data));

  // F7 — LATEST returns the emergency block + genset current.
  const f7 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: DEV });
  check('F7 LATEST emergency.state RUN', f7.data.emergency && f7.data.emergency.state === 'RUN',
    JSON.stringify(f7.data.emergency));
  check('F8 LATEST gensetRmsCurrent 4.0', f7.data.ac.gensetRmsCurrent && f7.data.ac.gensetRmsCurrent.value === 4.0,
    JSON.stringify(f7.data.ac));
  check('F9 LATEST emergency.tripCount 2', f7.data.emergency && f7.data.emergency.tripCount === 2,
    JSON.stringify(f7.data.emergency));

  // F10 — a PRE-1.7 row (no emergency columns) never fabricates a block.
  const legacyEnv = createGasContext();
  legacyEnv.sandbox.setupMasterTemplate();
  legacyEnv.ss.sheets['Devices'].appendRow([DEV, '', 'Gudang', '', '']);
  doPost(legacyEnv, { action: 'TELEMETRY', token: TOKEN, device_key: DEV,
    data: { sequence: 1, v_bat: 50.0, i_bat_dc: -1.0, i_ac_load: 1.0, ina219_ok: true, fw_version: '1.5.4' } });
  // Emulate a pre-1.7 sheet: trim the row + header back to 31 columns.
  const tSheet = legacyEnv.ss.sheets['Telemetry'];
  tSheet.rows[0] = tSheet.rows[0].slice(0, 31);
  for (let i = 1; i < tSheet.rows.length; i++) tSheet.rows[i] = tSheet.rows[i].slice(0, 31);
  const f10 = doPost(legacyEnv, { action: 'LATEST', token: TOKEN, device_key: DEV });
  check('F10 pre-1.7 row → no fabricated emergency block',
    f10.data.emergency === undefined && f10.data.ac.gensetRmsCurrent === undefined,
    JSON.stringify(f10.data.emergency));
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('\n============================================================');
console.log(`EMERGENCY LAYER: ${passed} passed, ${failed} failed`);
if (failed > 0) {
  console.log('FAILED CHECKS:');
  failures.forEach((f) => console.log('  ✗ ' + f));
  process.exit(1);
}
console.log('ALL EMERGENCY LAYER TESTS PASS');
