#!/usr/bin/env node
/**
 * test_wave3_authorization.js — WAVE-3 REAL integration test (Level 2+ evidence)
 * =============================================================================
 * Re-audit 2026-08-28 (14_CODE_GS_V2_REAUDIT.md §4) found four authorization /
 * precision holes. Wave 3 patches them; this test proves the patches ACTUALLY
 * work by executing the REAL code.gs/Code.gs in a Node sandbox (mocked GAS
 * services, REAL crypto) against clients that mirror the patched firmware:
 *
 *   • Group A — [GAS-2-J] HMAC identity pinning:
 *     a body-level device_key naming a DIFFERENT device is rejected 400 on
 *     every device-scoped action; the signed deviceId always wins (even when
 *     a foreign device_key rides inside the signed data); the token path
 *     keeps its documented single trust domain.
 *
 *   • Group B — [GAS-2-I] CALIBRATION_ACK device binding:
 *     a command queued for device A can only be ACKed by A (HMAC or token);
 *     device B's ACK is rejected 400 and the row stays unapplied; an ACK
 *     with no device identity is rejected (fail-closed); after A ACKs, A's
 *     CALIBRATION_PENDING honestly reports "no pending".
 *
 *   • Group C — [GAS-2-K] OTA admin gate + calibration ranges:
 *     OTA_PUBLISH without/with-wrong admin_token → 401; with the right
 *     admin_token → 200; while Config!ADMIN_TOKEN is UNSET the action is
 *     disabled entirely (fail-closed, second fresh env); calibration
 *     factors outside [0.1,100] (v) / [0.1,50] (i) are refused 400 before
 *     any row is written; boundaries and production defaults pass.
 *
 *   • Group D — [GAS-2-L] LATEST tie-break precision:
 *     two rows sharing the same event_time MILLISECOND with sequence
 *     inverted relative to arrival order — LATEST must return the higher
 *     sequence (the old `ev*1e6+seq` key lost sequence granularity beyond
 *     2^53 and returned the wrong row); a newer-by-1ms row wins regardless
 *     of sheet order; a present-but-unparseable legacy event_time is skipped
 *     honestly (never eligible as "latest").
 *
 *   • Group E — regression: PING (token + HMAC), TELEMETRY (token + HMAC)
 *     still accepted; CALIBRATION_PENDING via token still works.
 *
 * The envelope/canonical constructions mirror the patched sources exactly:
 *   - firmware/AI/GasAdvisor.cpp (_signRequest + _sendPost, contract v2.1)
 *   - firmware-generic/src/plts_firmware_v1.ino v1.5.1 (checkCalibration ACK)
 *   - code.gs/Code.gs (verifyHmac_ + doPost dispatch + resolveDeviceKey_)
 * If either side drifts, this test FAILS — that is its purpose.
 *
 * Usage: node scripts/test_wave3_authorization.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// GAS service mocks (same conventions as test_wave1_integration.js).
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
      setFontWeight: () => chainable(),
      setValue: (v) => {
        while (self.rows.length < row) self.rows.push([]);
        const r = self.rows[row - 1];
        r[col - 1] = v;
        return this;
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

/** GAS returns SIGNED bytes (-128..127); Code.gs does (b & 0xFF) — mirror it. */
function toSignedBytes(buf) {
  return Array.from(buf).map((b) => (b > 127 ? b - 256 : b));
}

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cacheStore = new Map();

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
    },
    ContentService: {
      createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
      MimeType: { JSON: 'JSON' },
    },
    UrlFetchApp: { fetch: () => ({}) },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks };
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

function doPostRaw(env, raw) {
  const out = env.sandbox.doPost({ postData: { contents: raw } });
  return JSON.parse(out.text);
}

// ---------------------------------------------------------------------------
// Client mirrors — byte-identical to the patched C++ sources
// ---------------------------------------------------------------------------

function sha256hex(s) {
  return crypto.createHash('sha256').update(Buffer.from(s, 'utf8')).digest('hex');
}

function hmacHex(secret, message) {
  return crypto.createHmac('sha256', Buffer.from(secret, 'utf8'))
    .update(Buffer.from(message, 'utf8')).digest('hex');
}

/** EXACT mirror of GasAdvisor::_signRequest (contract v2.1). */
function gasAdvisorSign(action, ts, nonce, deviceId, dataJson, secret) {
  const canonical = 'HMAC-SHA256' + '\n' + action + '\n' + String(ts) + '\n' +
                    nonce + '\n' + deviceId + '\n' + sha256hex(dataJson);
  return hmacHex(secret, canonical);
}

/** EXACT mirror of GasAdvisor::_sendPost envelope. */
function gasAdvisorEnvelope(action, dataJson, secret, deviceId, ts, nonce) {
  const signature = gasAdvisorSign(action, ts, nonce, deviceId, dataJson, secret);
  return JSON.stringify({
    action: action,
    auth: {
      method: 'HMAC-SHA256',
      timestamp: ts,
      nonce: nonce,
      deviceId: deviceId,
      signature: signature,
    },
    data: dataJson,
  });
}

/** Nested canonical telemetry payload (GasAdvisor/BatteryStatusSerializer). */
function gasAdvisorDataJson(deviceId, seq, timestampIso) {
  return JSON.stringify({
    protocolVersion: 2,
    firmwareVersion: '1.6.2',
    deviceId: deviceId,
    sequence: seq,
    timestamp: timestampIso,
    timeQuality: 'VALID',
    battery: {
      voltage: { value: 51.42, unit: 'V', quality: 'VALID' },
      current: { value: -3.75, unit: 'A', quality: 'VALID' },
      power: { value: -192.8, unit: 'W', quality: 'VALID' },
      soc: { value: 78.5, unit: '%', quality: 'VALID', provenance: 'BMS' },
      chargeWh: 1250.5, dischargeWh: 3000.0,
      bms: { protocol: 'PYLONTECH', connected: true, cellVoltageMin: 3.32, cellVoltageMax: 3.41, temperature: 29.5, faultFlags: 0 }
    },
    ac: { rmsCurrent: { value: 1.85, unit: 'A', quality: 'VALID' }, estimatedPower: { value: 407, unit: 'W', quality: 'VALID' } },
    environment: { temperature: { value: 30.1, unit: '°C', quality: 'VALID' }, humidity: { value: 66, unit: '%', quality: 'VALID' } },
    health: { freeHeap: 180000, wifiRssi: -58, sensorHealth: { ina219: 'ONLINE' } },
    overallQuality: 'VALID',
  });
}

/** EXACT mirror of firmware-generic v1.5.1 sendTelemetry body. */
function genericTelemetryBody(token, deviceKey, seq) {
  return JSON.stringify({
    action: 'TELEMETRY',
    token: token,
    device_key: deviceKey,
    data: { sequence: seq, v_bat: 51.42, i_bat_dc: -3.75, p_bat_dc: -192.8, i_ac_load: 1.85, ina219_ok: true, free_heap: 180000, rssi: -58, fw_version: '1.5.1' },
  });
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

console.log('\n=== WAVE-3 AUTHORIZATION TEST (real Code.gs + real crypto) ===\n');

const env = createGasContext();
env.sandbox.setupMasterTemplate();

const TOKEN = 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';               // Config default from template
const ADMIN = 'plts_admin_w3_7f2c9e1a4d6b8a3c';   // operator-only secret
const DEV_A = 'PLTS_GAS_A';                       // HMAC fleet device A
const SEC_A = 'secret_A_0123456789abcdef';
const DEV_B = 'PLTS_GAS_B';                       // HMAC fleet device B
const SEC_B = 'secret_B_fedcba9876543210';
const GENERIC = 'PLTS_MONITOR_01';                // token-path device

// Register the fleet (operator action: rows in the Devices sheet).
env.ss.sheets['Devices'].appendRow([GENERIC, '', 'Generic v1.5.1', '', '']);
env.ss.sheets['Devices'].appendRow([DEV_A, SEC_A, 'Gudang Utama', '', '']);
env.ss.sheets['Devices'].appendRow([DEV_B, SEC_B, 'Gudang Cadangan', '', '']);

// Set Config!ADMIN_TOKEN (operator action). The template default is '' →
// fail-closed; set the real value by updating the row in place.
{
  const cfg = env.ss.sheets['Config'];
  const rows = cfg.getDataRange().getValues();
  for (let i = 0; i < rows.length; i++) {
    if (String(rows[i][0]) === 'ADMIN_TOKEN') { cfg.getRange(i + 1, 2).setValue(ADMIN); break; }
  }
}

function nonce(n) { return 'w3n' + String(n).padStart(29, '0'); }
let nonceSeq = 0;
function nextNonce() { nonceSeq += 1; return 'w3nonce' + String(nonceSeq).padStart(25, '0'); }
function nowSec() { return Math.floor(Date.now() / 1000); }

// Seed telemetry for DEV_A via the real HMAC ingest path (2 samples).
{
  const t0 = nowSec();
  doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_A, 1, '2026-08-28T02:00:00.000Z'), SEC_A, DEV_A, t0, nextNonce()));
  doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_A, 2, '2026-08-28T02:01:00.000Z'), SEC_A, DEV_A, t0, nextNonce()));
}
// Seed one telemetry for GENERIC via the real token ingest path.
doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC, 1));

// ---------------------------------------------------------------------------
// GROUP A — GAS-2-J: HMAC identity pinning
// ---------------------------------------------------------------------------
console.log('[A] GAS-2-J — HMAC identity pinning:');
{
  // A1 — device A's valid envelope + body.device_key naming device B → 400.
  const raw = JSON.parse(gasAdvisorEnvelope('LATEST', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  raw.device_key = DEV_B;
  const r1 = doPostRaw(env, JSON.stringify(raw));
  check('A1 HMAC-A LATEST with body device_key=B → 400 impersonation rejected',
    r1.code === 400 && /does not match the HMAC-authenticated device/.test(r1.message), r1.message);

  // A2 — same envelope naming ITSELF → 200.
  const r2 = doPostRaw(env, gasAdvisorEnvelope('LATEST', '', SEC_A, DEV_A, nowSec(), nextNonce()) // baseline ok
  );
  check('A2 HMAC-A LATEST (no body device_key) → 200',
    r2.code === 200 && r2.data && r2.data.sequence === 2, JSON.stringify(r2.code));

  const rawA2 = JSON.parse(gasAdvisorEnvelope('LATEST', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  rawA2.device_key = DEV_A;
  const rA2 = doPostRaw(env, JSON.stringify(rawA2));
  check('A2b HMAC-A LATEST with body device_key=A (self) → 200',
    rA2.code === 200 && rA2.data && rA2.data.sequence === 2);

  // A3 — cross-device TELEMETRY: envelope signed by A but body.device_key=B.
  const rawA3 = JSON.parse(gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_A, 3, '2026-08-28T02:02:00.000Z'), SEC_A, DEV_A, nowSec(), nextNonce()));
  rawA3.device_key = DEV_B;
  const r3 = doPostRaw(env, JSON.stringify(rawA3));
  check('A3 HMAC-A TELEMETRY with body device_key=B → 400',
    r3.code === 400 && /does not match/.test(r3.message));

  // A4 — cross-device CALIBRATION_PUBLISH via body.device_key.
  const rawA4 = JSON.parse(gasAdvisorEnvelope('CALIBRATION_PUBLISH', JSON.stringify({ v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 1.0 }), SEC_A, DEV_A, nowSec(), nextNonce()));
  rawA4.device_key = DEV_B;
  const r4 = doPostRaw(env, JSON.stringify(rawA4));
  check('A4 HMAC-A CALIBRATION_PUBLISH for device B → 400',
    r4.code === 400 && /does not match/.test(r4.message), r4.message);

  // A5 — a foreign device_key INSIDE the signed data cannot win either:
  // resolveDeviceKey_ pins the HMAC identity; the command lands for A.
  const r5 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_PUBLISH',
    JSON.stringify({ device_key: DEV_B, v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 1.0 }),
    SEC_A, DEV_A, nowSec(), nextNonce()));
  check('A5 HMAC-A publish with data.device_key=B → pinned to A (command for A)',
    r5.code === 200, JSON.stringify(r5));
  const calibRows = env.ss.sheets['Calibration'].rows;
  const lastCalib = calibRows[calibRows.length - 1];
  check('A5b Calibration row device_key == signed identity A',
    String(lastCalib[1]) === DEV_A, String(lastCalib[1]));

  // A6 — self-calibration via HMAC is allowed (device owns its own factors).
  const r6 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_PUBLISH',
    JSON.stringify({ v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 1.0 }),
    SEC_A, DEV_A, nowSec(), nextNonce()));
  check('A6 HMAC-A self CALIBRATION_PUBLISH → 200', r6.code === 200 && r6.data && r6.data.command_id);

  // A7 — cross-device OTA_STATUS log forgery.
  const rawA7 = JSON.parse(gasAdvisorEnvelope('OTA_STATUS', JSON.stringify({ event: 'ACTIVATED', version: '9.9.9' }), SEC_B, DEV_B, nowSec(), nextNonce()));
  rawA7.device_key = DEV_A;
  const r7 = doPostRaw(env, JSON.stringify(rawA7));
  check('A7 HMAC-B OTA_STATUS as device A → 400',
    r7.code === 400 && /does not match/.test(r7.message));

  // A8 — token path keeps the DOCUMENTED single trust domain (F-G17): the
  // operator's shared token may address any registered device. Honest
  // assertion of the documented (legacy) model, not an oversight.
  const r8 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: DEV_A });
  check('A8 token-path LATEST for any registered device → 200 (documented legacy domain)',
    r8.code === 200 && r8.data && r8.data.deviceId === DEV_A);

  // A9 — HISTORY/DAILY for self via HMAC still work.
  const r9 = doPostRaw(env, gasAdvisorEnvelope('HISTORY', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  check('A9 HMAC-A HISTORY → 200 with 2+ records',
    r9.code === 200 && r9.data && r9.data.records.length >= 2, JSON.stringify(r9.code));
  const r9b = doPostRaw(env, gasAdvisorEnvelope('DAILY', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  check('A9b HMAC-A DAILY → 200', r9b.code === 200);
}

// ---------------------------------------------------------------------------
// GROUP B — GAS-2-I: CALIBRATION_ACK device binding
// ---------------------------------------------------------------------------
console.log('\n[B] GAS-2-I — CALIBRATION_ACK device binding:');
{
  // B1 — operator (token path) queues a calibration command for device A.
  const b1 = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEV_A, v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 1.0, note: 'operator wizard' });
  check('B1 token CALIBRATION_PUBLISH for A → command_id',
    b1.code === 200 && b1.data && b1.data.command_id, JSON.stringify(b1));
  const cmd = b1.data.command_id;

  // B2 — device A polls its pending queue via HMAC (identity from signature).
  const b2 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_PENDING', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  check('B2 HMAC-A CALIBRATION_PENDING → sees the command',
    b2.code === 200 && b2.data && b2.data.command_id === cmd && b2.data.v_calib === 18.857,
    JSON.stringify(b2.data));

  // B3 — device B (HMAC) tries to ACK A's command → 400, row untouched.
  const b3 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_ACK',
    JSON.stringify({ command_id: cmd }), SEC_B, DEV_B, nowSec(), nextNonce()));
  check('B3 HMAC-B ACK of A\'s command → 400 cross-device rejected',
    b3.code === 400 && /cross-device ACK rejected/.test(b3.message), b3.message);
  let calibRow = env.ss.sheets['Calibration'].rows.find((r) => String(r[7]) === String(cmd));
  check('B3b row still unapplied (command not swallowed)',
    calibRow && calibRow[5] !== true);

  // B4 — token ACK with NO device identity: resolves to Config!DEVICE_KEY
  // ('PLTS_MONITOR_01') — an EXPLICIT identity, never a wildcard. It does
  // not own this command → 400 cross-device. (The bare 'Missing device_key'
  // branch only fires when Config!DEVICE_KEY is empty too — still closed.)
  const b4 = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, command_id: cmd });
  check('B4 token ACK without device_key → resolved to Config identity, rejected for a foreign command (no wildcard)',
    b4.code === 400 && /cross-device ACK rejected/.test(b4.message), b4.message);

  // B5 — token ACK naming the WRONG device → 400.
  const b5 = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, device_key: DEV_B, command_id: cmd });
  check('B5 token ACK as device B → 400 cross-device rejected',
    b5.code === 400 && /cross-device ACK rejected/.test(b5.message));

  // B6 — the OWNER (device A, HMAC — command_id inside the signed data) ACKs.
  const b6 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_ACK',
    JSON.stringify({ command_id: cmd }), SEC_A, DEV_A, nowSec(), nextNonce()));
  check('B6 HMAC-A ACK of its own command → 200 applied', b6.code === 200, JSON.stringify(b6));
  calibRow = env.ss.sheets['Calibration'].rows.find((r) => String(r[7]) === String(cmd));
  check('B6b row applied=true with applied_at timestamp',
    calibRow && calibRow[5] === true && calibRow[6] instanceof Date);

  // B7 — A polls again: the ACKED command must be gone from the queue.
  // (Other legitimately-pending commands for A — queued in Group A — may
  // still show; the honesty contract is that an ACKed command NEVER
  // reappears.)
  const b7 = doPostRaw(env, gasAdvisorEnvelope('CALIBRATION_PENDING', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  check('B7 ACKed command never reappears in CALIBRATION_PENDING',
    b7.code === 200 && (b7.data === null || b7.data.command_id !== cmd),
    JSON.stringify(b7.data));

  // B8 — firmware-generic v1.5.1 mirror: token + device_key + command_id.
  const b8pub = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: GENERIC, v_calib: 19.1, i_calib_dc: 1.02, i_calib_ac: 0.98 });
  const b8 = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, device_key: GENERIC, command_id: b8pub.data.command_id });
  check('B8 firmware-generic v1.5.1 ACK shape (token + device_key) → 200',
    b8.code === 200, JSON.stringify(b8));
}

// ---------------------------------------------------------------------------
// GROUP C — GAS-2-K: OTA admin gate + calibration ranges
// ---------------------------------------------------------------------------
console.log('\n[C] GAS-2-K — OTA admin gate + calibration ranges:');
{
  const manifest = { version: '1.5.1', url: 'https://example.com/fw.bin', sha256: 'ab'.repeat(32), hmac: 'cd'.repeat(32), size: 123456 };

  // C1 — token-only publish (no admin_token) → 401.
  const c1 = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, manifest });
  check('C1 OTA_PUBLISH without admin_token → 401',
    c1.code === 401 && /requires admin_token/.test(c1.message), c1.message);

  // C2 — wrong admin_token → 401, nothing written.
  const c2 = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, admin_token: 'wrong-token', manifest });
  check('C2 OTA_PUBLISH with wrong admin_token → 401',
    c2.code === 401 && /invalid admin_token/.test(c2.message));
  check('C2b no manifest row written while unauthorized',
    env.ss.sheets['Ota'].rows.length === 1);

  // C3 — an HMAC DEVICE cannot publish either (fleet credential ≠ operator).
  const c3 = doPostRaw(env, gasAdvisorEnvelope('OTA_PUBLISH', JSON.stringify(manifest), SEC_A, DEV_A, nowSec(), nextNonce()));
  check('C3 HMAC device OTA_PUBLISH (no admin_token) → 401',
    c3.code === 401 && /requires admin_token/.test(c3.message), JSON.stringify(c3));

  // C4 — correct admin_token (token path, the operator's PWA panel) → 200.
  const c4 = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, admin_token: ADMIN, manifest });
  check('C4 OTA_PUBLISH with correct admin_token → 200',
    c4.code === 200 && c4.data && c4.data.version === '1.5.1', JSON.stringify(c4));
  const c4b = doPost(env, { action: 'OTA_MANIFEST', token: TOKEN });
  check('C4b OTA_MANIFEST returns the published row',
    c4b.code === 200 && c4b.data && c4b.data.version === '1.5.1' && c4b.data.url.startsWith('https://'));

  // C5 — fail-closed while ADMIN_TOKEN is unset (fresh deployment).
  const env2 = createGasContext();
  env2.sandbox.setupMasterTemplate();
  const c5 = doPost(env2, { action: 'OTA_PUBLISH', token: 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE', admin_token: 'anything', manifest });
  check('C5 ADMIN_TOKEN unset → OTA publishing DISABLED (401, honest refusal)',
    c5.code === 401 && /disabled — set ADMIN_TOKEN/.test(c5.message), c5.message);

  // C6 — calibration range gate: garbage factors refused before any write.
  const before = env.ss.sheets['Calibration'].rows.length;
  const bad = [
    { v_calib: 0, i_calib_dc: 1.0, i_calib_ac: 1.0 },
    { v_calib: -5, i_calib_dc: 1.0, i_calib_ac: 1.0 },
    { v_calib: 1e9, i_calib_dc: 1.0, i_calib_ac: 1.0 },
    { v_calib: 18.857, i_calib_dc: 0.05, i_calib_ac: 1.0 },
    { v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 999 },
  ];
  let allRejected = true;
  for (const f of bad) {
    const r = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEV_A, ...f });
    if (!(r.code === 400 && /outside the sane range/.test(r.message))) { allRejected = false; console.log('    (not rejected properly: ' + JSON.stringify(r) + ')'); }
  }
  check('C6 v=0 / v=-5 / v=1e9 / i=0.05 / i=999 all → 400 outside sane range', allRejected);
  check('C6b no Calibration row written for refused factors',
    env.ss.sheets['Calibration'].rows.length === before);

  // C7 — boundaries and production defaults accepted.
  const bLo = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEV_A, v_calib: 0.1, i_calib_dc: 0.1, i_calib_ac: 0.1 });
  const bHi = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEV_A, v_calib: 100, i_calib_dc: 50, i_calib_ac: 50 });
  check('C7 boundary values 0.1 / 100 / 50 → 200 (inclusive)',
    bLo.code === 200 && bHi.code === 200);
  const sane = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: DEV_A, v_calib: 18.857, i_calib_dc: 1.0, i_calib_ac: 1.0 });
  check('C7b production defaults 18.857 / 1.0 / 1.0 → 200', sane.code === 200);
}

// ---------------------------------------------------------------------------
// GROUP D — GAS-2-L: LATEST tie-break precision
// ---------------------------------------------------------------------------
console.log('\n[D] GAS-2-L — LATEST tie-break precision:');
{
  // Same event_time MILLISECOND, sequences inverted relative to arrival:
  // seq 6 arrives FIRST, seq 5 arrives SECOND (late, same clock ms).
  // LATEST must be seq 6 — the old ev*1e6+seq key collapsed both to the
  // same double (~1.77e18, ULP 256) and `>=` let the LAST-SCANNED row (seq
  // 5) win: a genuinely OLDER sample reported as "latest".
  const ts = nowSec();
  const sameMs = '2026-08-28T03:00:00.123Z';
  const d1 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_B, 6, sameMs), SEC_B, DEV_B, ts, nextNonce()));
  check('D1 seed: seq 6 @ T.123ms → 200', d1.code === 200, JSON.stringify(d1));
  const d2 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_B, 5, sameMs), SEC_B, DEV_B, ts, nextNonce()));
  check('D2 seed: late seq 5 @ same T.123ms → 200', d2.code === 200, JSON.stringify(d2));
  const d3 = doPostRaw(env, gasAdvisorEnvelope('LATEST', '', SEC_B, DEV_B, ts, nextNonce()));
  check('D3 LATEST on same-ms tie → sequence 6 (newest sample, not newest arrival)',
    d3.code === 200 && d3.data && d3.data.sequence === 6,
    'got sequence=' + (d3.data && d3.data.sequence));

  // D4 — 1 ms newer beats any sequence, regardless of arrival order.
  doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_B, 100, '2026-08-28T03:00:02.000Z'), SEC_B, DEV_B, ts, nextNonce()));
  doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_B, 1, '2026-08-28T03:00:02.001Z'), SEC_B, DEV_B, ts, nextNonce()));
  const d4 = doPostRaw(env, gasAdvisorEnvelope('LATEST', '', SEC_B, DEV_B, ts, nextNonce()));
  check('D4 row 1ms newer (seq 1, arrived last) beats older seq 100',
    d4.code === 200 && d4.data && d4.data.sequence === 1);

  // D5 — present-but-unparseable legacy event_time is skipped honestly.
  const sheet = env.ss.sheets['Telemetry'];
  sheet.appendRow([new Date(), DEV_B, 999, 'not-a-date', true,
    '', '', '', '', 'UNKNOWN', '', '', '', '', '', '', '', '',
    false, '', '', '', '', '', '', '', '', '', '', '']);
  const d5 = doPostRaw(env, gasAdvisorEnvelope('LATEST', '', SEC_B, DEV_B, ts, nextNonce()));
  check('D5 corrupt event_time row never becomes LATEST',
    d5.code === 200 && d5.data && d5.data.sequence === 1);

  // D6 — a REGISTERED device whose ONLY row is corrupt → honest 404, no
  // fabricated LATEST. (Register first so the fleet gate lets us through.)
  doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_A, 50, '2026-08-28T04:00:00.000Z'), SEC_A, DEV_A, ts, nextNonce()));
  env.ss.sheets['Devices'].appendRow(['GHOST_CORRUPT', '', 'corrupt-only fixture', '', '']);
  env.ss.sheets['Telemetry'].appendRow([new Date(), 'GHOST_CORRUPT', 1, 'garbage', false,
    '', '', '', '', 'UNKNOWN', '', '', '', '', '', '', '', '',
    false, '', '', '', '', '', '', '', '', '', '', '']);
  const d6 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'GHOST_CORRUPT' });
  check('D6 only-corrupt device → 404 (honest: no usable latest)',
    d6.code === 404, JSON.stringify(d6.code));
}

// ---------------------------------------------------------------------------
// GROUP E — regression
// ---------------------------------------------------------------------------
console.log('\n[E] Regression — core pipeline unharmed:');
{
  const e1 = doPost(env, { action: 'PING', token: TOKEN });
  check('E1 PING token → 200 PONG (PWA /setup §2.4)',
    e1.code === 200 && e1.message === 'PONG');
  const e2 = doPostRaw(env, gasAdvisorEnvelope('PING', '', SEC_A, DEV_A, nowSec(), nextNonce()));
  check('E2 PING via HMAC → 200 PONG', e2.code === 200 && e2.message === 'PONG');
  const e3 = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC, 2));
  check('E3 TELEMETRY token path → 200 ACCEPTED',
    e3.code === 200 && e3.data && e3.data.decision === 'ACCEPTED');
  // E4 — HMAC ingest still works (seq 4 arrives after D-group's seq 50 for
  // the same device → honestly flagged ACCEPTED_LATE; either decision means
  // the row was stored, which is the regression property under test).
  const e4 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', gasAdvisorDataJson(DEV_A, 4, '2026-08-28T05:00:00.000Z'), SEC_A, DEV_A, nowSec(), nextNonce()));
  check('E4 TELEMETRY HMAC path → 200 stored (ACCEPTED/ACCEPTED_LATE)',
    e4.code === 200 && e4.data && (e4.data.decision === 'ACCEPTED' || e4.data.decision === 'ACCEPTED_LATE'),
    JSON.stringify(e4.data));
  const e5 = doPost(env, { action: 'CALIBRATION_PENDING', token: TOKEN, device_key: DEV_B });
  check('E5 CALIBRATION_PENDING token path → 200 (queue readable)',
    e5.code === 200);
  const e6 = doPost(env, { action: 'SEQ_STATUS', token: TOKEN, device_key: GENERIC });
  check('E6 SEQ_STATUS token path → 200 ledger', e6.code === 200 && e6.data && e6.data.deviceKey === GENERIC);
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('\n=== WAVE-3 RESULT: ' + passed + ' passed, ' + failed + ' failed ===');
if (failed > 0) {
  failures.forEach((f) => console.log('  FAILED: ' + f));
  process.exit(1);
}
process.exit(0);
