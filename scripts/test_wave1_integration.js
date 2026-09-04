#!/usr/bin/env node
/**
 * test_wave1_integration.js — WAVE-1 REAL integration test (Level 2+ evidence)
 * =============================================================================
 * Re-audit 2026-08-28 (14_CODE_GS_V2_REAUDIT.md) proved the ingest pipeline
 * was DEAD end-to-end: firmware-generic lacked `sequence` (GAS-2-A → 400 on
 * every TELEMETRY) and GasAdvisor was broken on 3 layers (raw envelope → 401,
 * credentials in unreadable X-Auth-* headers, canonical string mismatch —
 * GAS-2-B). This test proves the WAVE-1 fixes ACTUALLY work, by executing the
 * REAL code.gs/Code.gs in a Node sandbox (mocked GAS services, REAL crypto)
 * against clients that mirror the patched firmware byte-for-byte:
 *
 *   • Group A — firmware-generic v1.5.0 (legacy token + sequence):
 *     ingest ACCEPTED, DUPLICATE 409 (one row only), GAP recorded honestly,
 *     ACCEPTED_LATE flagged, missing-sequence still 400 (explicit gate),
 *     LATEST no longer dead code, SEQ_STATUS honest.
 *
 *   • Group B — GasAdvisor v1.6.2 (HMAC body envelope, contract v2.1):
 *     ingest ACCEPTED (nested envelope → full 31-column row incl. BMS block),
 *     nonce replay 401, tampered data 401, action-confusion 401 (action is
 *     signed — GAS-2-C), stale timestamp 401, unknown device 401, wrong
 *     secret 401, data-as-object rejected (drift-proof contract), duplicate
 *     sequence 409 (idempotency on the HMAC path too), PING via HMAC.
 *
 *   • Group C — regression + fleet honesty:
 *     PWA contract (PING token) intact, wrong token 401, unknown device_key
 *     rejected once the fleet gate is armed, CALIBRATION_PENDING still works.
 *
 * The canonical string and envelope construction in this file are EXACT
 * mirrors of the patched sources:
 *   - firmware/AI/GasAdvisor.cpp  (_signRequest + _sendPost)
 *   - firmware-generic/src/plts_firmware_v1.ino (sendTelemetry)
 *   - code.gs/Code.gs (verifyHmac_ + doPost unwrap)
 * If either side drifts again, this test FAILS — that is its purpose.
 *
 * Usage: node scripts/test_wave1_integration.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// GAS service mocks (same conventions as test_gas_contract.js, plus REAL
// crypto in Utilities and a TTL-honoring cache).
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
  const cacheStore = new Map();   // key -> { v, exp }
  const telegram = { calls: [] };

  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, parseInt, parseFloat, RegExp, Error,
    // ---- GAS services ----
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
      // GAS enums referenced by Code.gs (values are opaque to the mock).
      DigestAlgorithm: { SHA_256: 'SHA_256' },
      Charset: { UTF_8: 'UTF_8' },
      // REAL SHA-256 over UTF-8 bytes (GAS semantics with explicit charset).
      computeDigest: (alg, value, charset) =>
        toSignedBytes(crypto.createHash('sha256')
          .update(Buffer.from(String(value), 'utf8')).digest()),
      // REAL HMAC-SHA256 (message, key) over UTF-8 bytes.
      computeHmacSha256Signature: (value, key, charset) =>
        toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
          .update(Buffer.from(String(value), 'utf8')).digest()),
    },
    ContentService: {
      createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
      MimeType: { JSON: 'JSON' },
    },
    UrlFetchApp: { fetch: (url) => { telegram.calls.push(url); return {}; } },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks, telegram };
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

/** doPost over a RAW string body (what the wire actually carries). */
function doPostRaw(env, raw) {
  const out = env.sandbox.doPost({ postData: { contents: raw } });
  return JSON.parse(out.text);
}

// ---------------------------------------------------------------------------
// Firmware mirrors — byte-identical to the patched C++ sources.
// ---------------------------------------------------------------------------

function sha256hex(s) {
  return crypto.createHash('sha256').update(Buffer.from(s, 'utf8')).digest('hex');
}

function hmacHex(secret, message) {
  return crypto.createHmac('sha256', Buffer.from(secret, 'utf8'))
    .update(Buffer.from(message, 'utf8')).digest('hex');
}

/**
 * EXACT mirror of GasAdvisor::_signRequest (WAVE-1 contract v2.1):
 *   canonical = 'HMAC-SHA256' \n action \n timestamp \n nonce \n deviceId \n sha256hex(dataJson)
 */
function gasAdvisorSign(action, ts, nonce, deviceId, dataJson, secret) {
  const dataDigest = sha256hex(dataJson);
  const canonical = 'HMAC-SHA256' + '\n' + action + '\n' + String(ts) + '\n' +
                    nonce + '\n' + deviceId + '\n' + dataDigest;
  return hmacHex(secret, canonical);
}

/**
 * EXACT mirror of GasAdvisor::_sendPost envelope construction:
 *   { action, auth:{method,timestamp,nonce,deviceId,signature}, data:'<raw string>' }
 * `data` is the RAW string (exact signed bytes) — JSON escaping round-trips
 * it losslessly, which is the core of the drift-proof contract.
 */
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

/** EXACT mirror of patched firmware-generic sendTelemetry() body (v1.5.0). */
function genericTelemetryBody(token, deviceKey, seq, opts = {}) {
  return JSON.stringify({
    action: 'TELEMETRY',
    token: token,
    device_key: deviceKey,
    data: {
      sequence: seq,
      v_bat: opts.vBat !== undefined ? opts.vBat : 51.42,
      i_bat_dc: opts.iDc !== undefined ? opts.iDc : -3.75,
      p_bat_dc: opts.pDc !== undefined ? opts.pDc : -192.8,
      i_ac_load: opts.iAc !== undefined ? opts.iAc : 1.85,
      ina219_ok: true,
      free_heap: 180000,
      rssi: -58,
      fw_version: '1.5.0',
    },
  });
}

/**
 * Mirror of BatteryStatusSerializer::serialize key order (v1.6.x) — the
 * nested canonical envelope GasAdvisor signs and ships as `data`.
 * Includes escaped quotes + UTF-8 in deviceName to prove the raw-string
 * round-trip (JSON escaping is lossless).
 */
function gasAdvisorDataJson(seq) {
  return JSON.stringify({
    protocolVersion: 2,
    firmwareVersion: '1.6.2',
    deviceId: 'PLTS_GAS_001',
    deviceName: 'Gudang "Utama" — 48V',   // escaped quotes + UTF-8 em dash
    sequence: seq,
    timestamp: '2026-08-28T08:00:00.000Z',
    timeQuality: 'VALID',
    uptimeSeconds: 3600,
    bootCount: 3,
    resetReason: 'POWERON',
    battery: {
      voltage: { value: 51.42, quality: 'VALID', source: 'ADC' },
      current: { value: -3.75, quality: 'VALID', source: 'INA219' },
      power: { value: -192.83, quality: 'DERIVED', source: 'CALC' },
      direction: 'CHARGING',
      soc: {
        value: 78.5, quality: 'ESTIMATED', source: 'SHUNT',
        method: 'ESTIMATED', lastSync: 0, confidence: 'MEDIUM',
        provenance: 'SHUNT',
      },
      remainingAh: 157.0, chargeAh: 12.4, dischargeAh: 8.2,
      chargeWh: 640.5, dischargeWh: 410.0, netWh: 230.5, efc: 42,
      estimatedUsableCapacityAh: 180.0, peakChargeCurrent: 10.2,
      peakDischargeCurrent: 25.1,
      bms: {
        connected: true, protocol: 'PYLONTECH_CAN', state: 'RUNNING',
        voltage: 51.4, current: -3.7, temperature: 29.5, soh: 98.0,
        cellVoltageMin: 3.41, cellVoltageMax: 3.44, cellCount: 15,
        chargeCurrentLimit: 50.0, dischargeCurrentLimit: 60.0,
        cycleCount: 120, faultFlags: 0, moduleCount: 1, lastSeenMs: 5000,
        currentMismatchA: 0.2,
      },
    },
    ac: {
      rmsCurrent: { value: 1.85, quality: 'VALID', source: 'ACS712' },
      peakCurrent: { value: 3.9, quality: 'VALID' },
      averageCurrent: { value: 1.7, quality: 'VALID' },
      estimatedPower: {
        value: 370.0, quality: 'ESTIMATED',
        assumptions: { voltage: 220.0, powerFactor: 0.9 },
      },
      signalQuality: 'CLEAN',
    },
    environment: {
      temperature: { value: 29.4, quality: 'VALID', source: 'SHT31' },
      humidity: { value: 72.5, quality: 'VALID', source: 'SHT31' },
      dewPoint: { value: 24.1, quality: 'DERIVED' },
      label: 'Ambient / Enclosure Temperature',
      condensationRisk: 'LOW',
    },
    health: {
      systemState: 'RUNNING',
      sensorHealth: {
        ina219: 'ONLINE', batteryAdc: 'ONLINE', acs712: 'ONLINE',
        sht31: 'ONLINE', bmsComm: 'ONLINE',
      },
      freeHeap: 165000, minFreeHeap: 150000, wifiRssi: -61,
      wifiReconnectCount: 1, mqttConnected: false, ntpSynced: true,
      storageOk: true, spoolSize: 0, highestAlarmSeverity: 'INFO',
    },
    activeAlarms: [],
    overallQuality: 'VALID',
  });
}

// =============================================================================
// EXECUTION
// =============================================================================

console.log('\n=== WAVE-1 INTEGRATION TEST (real Code.gs + real crypto) ===\n');

const env = createGasContext();
env.sandbox.setupMasterTemplate();

const TOKEN = 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';        // Config default set by template
const GENERIC_DEVICE = 'PLTS_MONITOR_01';
const GAS_DEVICE = 'PLTS_GAS_001';
const GAS_SECRET = 'gas_secret_unit_test_0123456789abcdef';

function nonce(n) { return 'nonce' + String(n).padStart(26, '0'); }  // 32 hex-ish chars
function nowSec() { return Math.floor(Date.now() / 1000); }

function telemetryRows(env) { return env.ss.sheets['Telemetry'].rows; }
function ledgerRows(env) { return env.ss.sheets['SeqIndex'].rows; }

// ---------------------------------------------------------------------------
// GROUP A — firmware-generic v1.5.0 (legacy token + sequence) [GAS-2-A]
// ---------------------------------------------------------------------------
console.log('[A] firmware-generic v1.5.0 — legacy token + sequence (GAS-2-A):');

{
  // A1 — first telemetry now carries sequence → ACCEPTED (was 400 before Wave 1)
  const r = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 1));
  check('A1 first TELEMETRY(sequence=1) → 200 ACCEPTED',
    r.code === 200 && r.data && r.data.decision === 'ACCEPTED', JSON.stringify(r));
  check('A1 one telemetry row persisted', telemetryRows(env).length === 2);   // header + row
  check('A1 ledger created (expectedNext=2)',
    ledgerRows(env).length === 2 && Number(ledgerRows(env)[1][1]) === 2);

  // A2 — exact replay → DUPLICATE 409, still ONE row
  const r2 = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 1));
  check('A2 replay (device_key,1) → 409 DUPLICATE',
    r2.code === 409 && r2.data && r2.data.decision === 'DUPLICATE');
  check('A2 no second row (idempotent insert)', telemetryRows(env).length === 2);
  check('A2 dup_count incremented', Number(ledgerRows(env)[1][3]) === 1);

  // A3 — jump to 4 → gap [2,3] recorded honestly, never fabricated
  const r3 = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 4));
  check('A3 TELEMETRY(sequence=4) → 200, gap recorded',
    r3.code === 200 && r3.data && r3.data.gapsOpen === 2, JSON.stringify(r3.data));
  const gaps = JSON.parse(ledgerRows(env)[1][5]);
  check('A3 ledger gap {from:2,to:3}', gaps.length === 1 && gaps[0].from === 2 && gaps[0].to === 3);
  check('A3 no synthetic rows for gap', telemetryRows(env).length === 3);

  // A4 — late arrival seq=3 (unseen, < expectedNext) → ACCEPTED_LATE + flag
  const r4 = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 3));
  check('A4 late TELEMETRY(sequence=3) → 200 ACCEPTED_LATE',
    r4.code === 200 && r4.data && r4.data.decision === 'ACCEPTED_LATE');
  const lateRow = telemetryRows(env).find((r) => Number(r[2]) === 3);
  check('A4 row flagged is_late=true', lateRow && lateRow[4] === true);

  // A5 — missing sequence still 400 (explicit gate; only pre-1.5.0 hits this)
  const noSeq = { action: 'TELEMETRY', token: TOKEN, device_key: GENERIC_DEVICE,
                  data: { v_bat: 51.4, i_bat_dc: -3.7 } };
  const r5 = doPost(env, noSeq);
  check('A5 TELEMETRY without sequence → 400 explicit',
    r5.code === 400 && /sequence/i.test(r5.message), r5.message);

  // A6 — LATEST no longer dead code in production
  doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 5, { vBat: 50.9 }));
  const r6 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: GENERIC_DEVICE });
  check('A6 LATEST → 200 with envelope',
    r6.code === 200 && r6.data && r6.data.sequence === 5, JSON.stringify(r6.data && r6.data.sequence));
  check('A6 envelope voltage round-trip (adapter output)',
    r6.data && r6.data.battery && r6.data.battery.voltage.value === 50.9);
  check('A6 flat adapter marks soc UNKNOWN (honest, no fabrication)',
    r6.data && r6.data.battery && r6.data.battery.soc.value === null);

  // A7 — SEQ_STATUS honest gap report
  const r7 = doPost(env, { action: 'SEQ_STATUS', token: TOKEN, device_key: GENERIC_DEVICE });
  check('A7 SEQ_STATUS → gapCount=2, gaps[{2,3}]',
    r7.code === 200 && r7.data && r7.data.gapCount === 2 &&
    r7.data.gaps.length === 1 && r7.data.gaps[0].from === 2);

  // A8 — mutations ran under LockService (P1-016 serialization intact)
  check('A8 LockService acquired for mutations', env.locks.acquired >= 5);
}

// ---------------------------------------------------------------------------
// GROUP B — GasAdvisor v1.6.2 (HMAC body envelope, contract v2.1)
//           [GAS-2-B lapis 1+2+3 + GAS-2-C]
// ---------------------------------------------------------------------------
console.log('\n[B] GasAdvisor v1.6.2 — HMAC body envelope (GAS-2-B/C):');

// Arm the fleet gate: register BOTH devices (as an operator would in the
// Devices sheet). From here on requireRegisteredDevice_ is fail-closed.
env.ss.sheets['Devices'].appendRow([GENERIC_DEVICE, '', 'Generic v1.5.0', '', '']);
env.ss.sheets['Devices'].appendRow([GAS_DEVICE, GAS_SECRET, 'Produksi 48V', '', '']);

{
  const ts = nowSec();

  // B1 — full envelope: data as RAW STRING, canonical binds action + digest
  const dataJson = gasAdvisorDataJson(101);
  const raw = gasAdvisorEnvelope('TELEMETRY', dataJson, GAS_SECRET, GAS_DEVICE, ts, nonce(1));
  const r = doPostRaw(env, raw);
  check('B1 HMAC TELEMETRY → 200 ACCEPTED (envelope + canonical synced)',
    r.code === 200 && r.data && r.data.decision === 'ACCEPTED', JSON.stringify(r));

  const row = telemetryRows(env).find((x) => String(x[1]) === GAS_DEVICE);
  check('B1 nested envelope → full row (v_bat 51.42, soc 78.5, soc_source SHUNT)',
    row && row[5] === 51.42 && row[8] === 78.5 && row[24] === 'SHUNT',
    row ? JSON.stringify(row.slice(0, 10)) + ' src=' + row[24] : 'no row');
  check('B1 BMS block persisted (PYLONTECH_CAN, connected, cellV 3.41/3.44)',
    row && row[25] === 'PYLONTECH_CAN' && row[26] === true &&
    row[27] === 3.41 && row[28] === 3.44);
  check('B1 energy counters persisted (chargeWh 640.5)',
    row && row[14] === 640.5);
  check('B1 fw_version + ina219 + environment persisted',
    row && row[23] === '1.6.2' && row[18] === true && row[12] === 29.4 && row[13] === 72.5);

  // B2 — UTF-8 + escaped quotes survived the raw-string round-trip
  //      (if GAS had re-serialized instead of hashing received bytes, the
  //      signature would have mismatched — this row would not exist)
  check('B2 raw-string contract survived UTF-8/escaped deviceName (digest matched)',
    r.code === 200);

  // B3 — exact replay (same nonce) → 401 nonce replayed
  const r3 = doPostRaw(env, raw);
  check('B3 replay same nonce → 401 nonce replayed',
    r3.code === 401 && /nonce replayed/i.test(r3.message), r3.message);

  // B4 — tampered data, valid signature over the ORIGINAL data + fresh nonce
  //      → 401 signature mismatch (the digest binds the exact data bytes)
  const b4ts = nowSec(), b4nonce = nonce(11);
  const validSig = gasAdvisorSign('TELEMETRY', b4ts, b4nonce, GAS_DEVICE, dataJson, GAS_SECRET);
  const tamperedInner = JSON.parse(dataJson);
  tamperedInner.battery.voltage.value = 99.99;      // attacker edits a value
  const r4 = doPostRaw(env, JSON.stringify({
    action: 'TELEMETRY',
    auth: { method: 'HMAC-SHA256', timestamp: b4ts, nonce: b4nonce,
            deviceId: GAS_DEVICE, signature: validSig },
    data: JSON.stringify(tamperedInner),
  }));
  check('B4 tampered data + valid signature → 401 signature mismatch',
    r4.code === 401 && /signature mismatch/i.test(r4.message), r4.message);

  // B5 — action confusion: signature issued for TELEMETRY, body says LATEST
  //      (GAS-2-C: action is INSIDE the canonical string)
  const confused = JSON.parse(gasAdvisorEnvelope('TELEMETRY', dataJson, GAS_SECRET, GAS_DEVICE, nowSec(), nonce(2)));
  confused.action = 'LATEST';
  confused.device_key = GAS_DEVICE;
  const r5 = doPostRaw(env, JSON.stringify(confused));
  check('B5 action confusion (sig=TELEMETRY, body=LATEST) → 401',
    r5.code === 401 && /signature mismatch/i.test(r5.message), r5.message);

  // B6 — stale timestamp (outside ±300 s window)
  const staleTs = nowSec() - 400;
  const r6 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', dataJson, GAS_SECRET, GAS_DEVICE, staleTs, nonce(3)));
  check('B6 stale timestamp (now-400s) → 401 replay window',
    r6.code === 401 && /replay window/i.test(r6.message), r6.message);

  // B7 — unknown device
  const r7 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', dataJson, GAS_SECRET, 'GHOST_DEVICE', nowSec(), nonce(4)));
  check('B7 unknown deviceId → 401 unknown device',
    r7.code === 401 && /unknown device/i.test(r7.message), r7.message);

  // B8 — wrong secret (valid-looking envelope, key mismatch)
  const r8 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', dataJson, 'wrong_secret', GAS_DEVICE, nowSec(), nonce(5)));
  check('B8 wrong secret → 401 signature mismatch',
    r8.code === 401 && /signature mismatch/i.test(r8.message), r8.message);

  // B9 — data as JSON OBJECT (old drift-prone form) → explicit rejection
  const objEnvelope = JSON.parse(gasAdvisorEnvelope('TELEMETRY', dataJson, GAS_SECRET, GAS_DEVICE, nowSec(), nonce(6)));
  objEnvelope.data = JSON.parse(objEnvelope.data);
  const r9 = doPostRaw(env, JSON.stringify(objEnvelope));
  check('B9 data as OBJECT (not raw string) → 401 explicit',
    r9.code === 401 && /raw JSON string/i.test(r9.message), r9.message);

  // B10 — duplicate sequence via HMAC (fresh nonce, seq=101 again) → 409
  const dataJson2 = gasAdvisorDataJson(101);       // same sequence, new nonce
  const r10 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', dataJson2, GAS_SECRET, GAS_DEVICE, nowSec(), nonce(7)));
  check('B10 duplicate sequence (new nonce) → 409 DUPLICATE on HMAC path',
    r10.code === 409 && r10.data && r10.data.decision === 'DUPLICATE', JSON.stringify(r10));
  check('B10 still exactly ONE row for PLTS_GAS_001',
    telemetryRows(env).filter((x) => String(x[1]) === GAS_DEVICE).length === 1);

  // B11 — gap on HMAC path: jump to 105 → gap [102,104] recorded
  const dataJson3 = gasAdvisorDataJson(105);
  const r11 = doPostRaw(env, gasAdvisorEnvelope('TELEMETRY', dataJson3, GAS_SECRET, GAS_DEVICE, nowSec(), nonce(8)));
  check('B11 HMAC gap (jump 101→105) recorded honestly',
    r11.code === 200 && r11.data && r11.data.gapsOpen === 3, JSON.stringify(r11.data));

  // B12 — PING via HMAC (action without data → sha256hex(''))
  const pingSig = gasAdvisorSign('PING', nowSec(), nonce(9), GAS_DEVICE, '', GAS_SECRET);
  const r12 = doPostRaw(env, JSON.stringify({
    action: 'PING',
    auth: { method: 'HMAC-SHA256', timestamp: nowSec(), nonce: nonce(9), deviceId: GAS_DEVICE, signature: pingSig },
  }));
  check('B12 PING via HMAC (empty data digest) → 200 PONG',
    r12.code === 200 && r12.message === 'PONG', JSON.stringify(r12));

  // B13 — LATEST via HMAC for own device (auth.deviceKey fallback)
  const r13 = doPostRaw(env, gasAdvisorEnvelope('LATEST', '', GAS_SECRET, GAS_DEVICE, nowSec(), nonce(10)));
  check("B13 LATEST via HMAC (data='') → 200, nested envelope with provenance",
    r13.code === 200 && r13.data && r13.data.battery &&
    r13.data.battery.soc.provenance === 'SHUNT' &&
    r13.data.battery.bms && r13.data.battery.bms.protocol === 'PYLONTECH_CAN',
    JSON.stringify(r13.data && r13.data.battery && r13.data.battery.soc));
}

// ---------------------------------------------------------------------------
// GROUP C — regression + fleet honesty
// ---------------------------------------------------------------------------
console.log('\n[C] Regression + fleet honesty:');

{
  // C1 — PWA /setup handshake contract intact (token path)
  const r = doPost(env, { action: 'PING', token: TOKEN });
  check('C1 PING(token) → 200 PONG (PWA contract intact)',
    r.code === 200 && r.message === 'PONG');

  // C2 — wrong token
  const r2 = doPost(env, { action: 'PING', token: 'WRONG' });
  check('C2 wrong token → 401', r2.code === 401 && r2.status === 'ERROR');

  // C3 — fleet gate armed: unregistered device_key rejected (fail-closed)
  const r3 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'GHOST_DEVICE' });
  check('C3 LATEST for unregistered device → 400 Unknown device_key',
    r3.code === 400 && /Unknown device_key/i.test(r3.message), r3.message);

  // C4 — registered generic device still readable via token (fleet gate open for members)
  const r4 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: GENERIC_DEVICE });
  check('C4 LATEST for registered generic device → 200',
    r4.code === 200 && r4.data && r4.data.deviceId === GENERIC_DEVICE);

  // C5 — CALIBRATION_PENDING still works (dispatch regression)
  const r5 = doPost(env, { action: 'CALIBRATION_PENDING', token: TOKEN, device_key: GENERIC_DEVICE });
  check('C5 CALIBRATION_PENDING → 200 (no pending)',
    r5.code === 200 && /No pending/i.test(r5.message));

  // C6 — legacy generic TELEMETRY still accepted AFTER fleet arming
  //      (registered device + token → gate passes; sequence continues)
  const r6 = doPostRaw(env, genericTelemetryBody(TOKEN, GENERIC_DEVICE, 6));
  check('C6 generic TELEMETRY(seq=6) after fleet arming → 200',
    r6.code === 200 && r6.data && r6.data.decision === 'ACCEPTED', JSON.stringify(r6));

  // C7 — device separation: generic gaps never polluted GAS device ledger
  const gasLedger = ledgerRows(env).find((r) => String(r[0]) === GAS_DEVICE);
  const genericLedger = ledgerRows(env).find((r) => String(r[0]) === GENERIC_DEVICE);
  check('C7 per-device ledgers separate (generic gaps=2, gas gaps=3)',
    gasLedger && Number(gasLedger[4]) === 3 && genericLedger && Number(genericLedger[4]) === 2,
    'gas=' + JSON.stringify(gasLedger && gasLedger.slice(0, 5)) +
    ' generic=' + JSON.stringify(genericLedger && genericLedger.slice(0, 5)));

  // C8 — HISTORY ordering by event_time (regression, P1-003).
  //      firmware-generic sends NO device clock → event_time = server
  //      ingestion time. A hardcoded sequence list ([1,3,4,5,6]) is a
  //      millisecond RACE (only holds when seq 4 and late seq 3 land in the
  //      same ms and the tie breaks by sequence). The honest deterministic
  //      assertion is the documented ordering invariant itself.
  const r8 = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: GENERIC_DEVICE, limit: 10 });
  const recs8 = r8.data.records;
  const seqs8 = recs8.map((x) => x.sequence);
  check('C8 HISTORY returns all 5 records',
    r8.code === 200 && recs8.length === 5 &&
    [1, 3, 4, 5, 6].every((s) => seqs8.includes(s)), JSON.stringify(seqs8));
  let ordered8 = true;
  for (let i = 1; i < recs8.length; i++) {
    const ta = Date.parse(recs8[i - 1].eventTime);
    const tb = Date.parse(recs8[i].eventTime);
    if (tb < ta || (tb === ta && recs8[i].sequence < recs8[i - 1].sequence)) {
      ordered8 = false; break;
    }
  }
  check('C8 HISTORY ordered by (event_time, sequence) — deterministic invariant',
    ordered8, JSON.stringify(recs8.map((x) => [x.eventTime, x.sequence])));
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
console.log('\n=== SUMMARY: ' + passed + ' passed, ' + failed + ' failed ===');
if (failed > 0) {
  console.log('Failures:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exit(1);
}
process.exit(0);
