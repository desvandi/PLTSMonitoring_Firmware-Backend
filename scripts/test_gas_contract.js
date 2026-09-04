#!/usr/bin/env node
/**
 * test_gas_contract.js — GAS backend contract tests (Level 2 evidence)
 * =============================================================================
 * Runs code.gs/Code.gs in a Node sandbox with mocked GAS services
 * (SpreadsheetApp, LockService, CacheService, Utilities, ContentService,
 * UrlFetchApp) and asserts the P0/P1 contract invariants:
 *
 *   P0-001  API surface: PING/TELEMETRY/LATEST/HISTORY/DAILY/SEQ_STATUS/...
 *   P0-002  Canonical nested envelope in AND out (adapter round-trip)
 *   P0-007  48V_15S_LIFEPO4 config defaults
 *   P1-001  identity = (device_key, sequence)
 *   P1-002  duplicate sequence → DUPLICATE (409), exactly ONE row
 *   P1-003  out-of-order (late) sequence stored flagged; HISTORY ordered by
 *           event_time, not arrival
 *   P1-004  gap recorded in ledger; NO synthetic rows
 *   P1-016  mutations serialized under LockService
 *   §33     uniform response envelope {status, code, data, message, timestamp}
 *
 * Usage: node scripts/test_gas_contract.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

// ---------------------------------------------------------------------------
// Minimal GAS service mocks
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
    return {
      getValues: () => self.rows.map((r) => r.slice()),
    };
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

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cache = new Map();
  const telemetryReceived = [];

  const sandbox = {
    console,
    JSON,
    Math,
    Date,
    Number,
    String,
    Object,
    Array,
    isNaN,
    parseInt,
    parseFloat,
    RegExp,
    Error,
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
        get: (k) => cache.get(k) || null,
        put: (k, v, ttl) => cache.set(k, v),
        remove: (k) => cache.delete(k),
      }),
    },
    Utilities: {
      getUuid: () => 'uuid-' + Math.random().toString(16).slice(2),
      computeHmacSha256Signature: () => [0, 1, 2, 3],   // not used in token tests
      // [WAVE-2] real timezone conversion for DAILY bucketing (GAS-2-F).
      // Defined in module scope — Intl resolves lexically from the host,
      // independent of the sandbox context.
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
    UrlFetchApp: { fetch: () => ({}) },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss, locks, telemetryReceived };
}

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

let passed = 0, failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  PASS  ${name}`); }
  else { failed++; failures.push(name + (detail ? ` — ${detail}` : '')); console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`); }
}

function doPost(env, body) {
  const out = env.sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
  return JSON.parse(out.text);
}

// Canonical nested envelope helper (production firmware v1.5.0 shape)
function envelope(seq, ts, opts = {}) {
  return {
    protocolVersion: 1,
    firmwareVersion: '1.5.0',
    deviceId: 'PLTS-TEST01',
    sequence: seq,
    timestamp: ts,
    timeQuality: 'VALID',
    battery: {
      voltage: { value: 52.4, quality: 'VALID', source: 'MEASURED' },
      current: { value: -10.2, quality: 'VALID', source: 'MEASURED' },
      power: { value: -534.5, quality: 'DERIVED', source: 'DERIVED' },
      soc: { value: 78.4, quality: 'ESTIMATED' },
      chargeWh: 1000, dischargeWh: 2000, chargeAh: 20, dischargeAh: 40,
    },
    ac: { rmsCurrent: { value: 3.2 }, estimatedPower: { value: 633.6 } },
    environment: { temperature: { value: 31.2 }, humidity: { value: 72.1 } },
    health: { freeHeap: 123456, rssi: -61, sensorHealth: { ina219: 'ONLINE' } },
    overallQuality: 'VALID',
    ...opts,
  };
}

// =============================================================================

console.log('\n=== GAS CONTRACT TESTS (code.gs/Code.gs) ===\n');

// --- Setup: run setupMasterTemplate ----------------------------------------
const env = createGasContext();
env.sandbox.setupMasterTemplate();

// --- T1: P0-007 — 48V config defaults ---------------------------------------
console.log('[P0-007] Config defaults:');
{
  const cfg = env.sandbox.getPltsConfig('BATTERY_SYSTEM_TYPE');
  check('BATTERY_SYSTEM_TYPE = 48V_15S_LIFEPO4', cfg === '48V_15S_LIFEPO4', cfg);
  check('LOW_BATTERY_CUTOFF_V = 45.0', env.sandbox.getPltsConfig('LOW_BATTERY_CUTOFF_V') === '45.0');
}

// --- T2: PING with token ----------------------------------------------------
console.log('\n[P0-001] API surface:');
{
  const r = doPost(env, { action: 'PING', token: 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE' });
  check('PING → SUCCESS/PONG', r.status === 'SUCCESS' && r.message === 'PONG');
  check('PING envelope has code+timestamp', typeof r.code === 'number' && typeof r.timestamp === 'string');
}
{
  const r = doPost(env, { action: 'PING', token: 'WRONG' });
  check('PING bad token → 401', r.code === 401 && r.status === 'ERROR');
}
{
  const r = doPost(env, { action: 'NOPE', token: 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE' });
  check('Unknown action → 400', r.code === 400);
}

// --- T3: P1-001/P1-002 — telemetry identity + duplicate ----------------------
console.log('\n[P1-001/P1-002] Identity + duplicate handling:');
const TOKEN = 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';
{
  const r1 = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(100, '2026-08-27T10:00:00Z') });
  check('seq 100 → ACCEPTED', r1.status === 'SUCCESS' && r1.data.decision === 'ACCEPTED', JSON.stringify(r1));
  const r2 = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(100, '2026-08-27T10:00:05Z') });
  check('seq 100 again → DUPLICATE (409)', r2.code === 409 && r2.data.decision === 'DUPLICATE', JSON.stringify(r2));
  const sheet = env.ss.getSheetByName('Telemetry');
  const seq100rows = sheet.rows.filter((r) => String(r[1]) === 'PLTS-TEST01' && Number(r[2]) === 100);
  check('exactly ONE row for (device, seq=100)', seq100rows.length === 1, `got ${seq100rows.length}`);
}

// --- T4: P1-004 — gap recording, no synthetic rows ---------------------------
console.log('\n[P1-004] Gap handling:');
{
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(101, '2026-08-27T10:00:10Z') });
  const r = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(105, '2026-08-27T10:00:30Z') });
  check('seq 105 after 101 → ACCEPTED', r.status === 'SUCCESS');
  const st = doPost(env, { action: 'SEQ_STATUS', token: TOKEN, device_key: 'PLTS-TEST01' });
  check('gap 102-104 recorded', st.data.gapCount === 3, JSON.stringify(st.data.gaps));
  check('gap range from=102 to=104', st.data.gaps.length === 1 && st.data.gaps[0].from === 102 && st.data.gaps[0].to === 104);
  const sheet = env.ss.getSheetByName('Telemetry');
  const seqs = sheet.rows.filter((r) => String(r[1]) === 'PLTS-TEST01').map((r) => Number(r[2]));
  check('NO synthetic rows for 102-104', !seqs.includes(102) && !seqs.includes(103) && !seqs.includes(104), JSON.stringify(seqs));
  check('expectedNext = 106', st.data.expectedNext === 106);
}

// --- T5: P1-003 — out-of-order late arrival + HISTORY event_time ordering ----
console.log('\n[P1-003] Out-of-order handling:');
{
  const r = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(103, '2026-08-27T10:00:20Z') });
  check('late seq 103 → ACCEPTED_LATE', r.data.decision === 'ACCEPTED_LATE', JSON.stringify(r.data));
  const sheet = env.ss.getSheetByName('Telemetry');
  const lateRow = sheet.rows.find((r) => Number(r[2]) === 103);
  check('late row flagged is_late=true', lateRow[4] === true);

  const hist = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'PLTS-TEST01' });
  const seqs = hist.data.records.map((r) => r.sequence);
  check('HISTORY ordered by event_time (100,101,103,105)', JSON.stringify(seqs) === '[100,101,103,105]', JSON.stringify(seqs));
  const rec103 = hist.data.records.find((r) => r.sequence === 103);
  check('HISTORY record carries isLate=true', rec103.isLate === true);
}

// --- T6: P0-002 — canonical envelope round-trip ------------------------------
console.log('\n[P0-002] Canonical envelope round-trip:');
{
  const lat = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-TEST01' });
  const d = lat.data;
  check('LATEST SUCCESS', lat.status === 'SUCCESS');
  check('nested battery.voltage.value = 52.4', d.battery && d.battery.voltage.value === 52.4);
  check('nested battery.current.value = -10.2', d.battery.current.value === -10.2);
  check('nested battery.soc.value = 78.4 + quality ESTIMATED', d.battery.soc.value === 78.4 && d.battery.soc.quality === 'ESTIMATED');
  check('eventTime + ingestionTime distinct fields', d.eventTime !== null && d.ingestionTime !== null);
  check('sequence carried = 105', d.sequence === 105);
  check('energy counters nested', d.energy && d.energy.chargeWh === 1000 && d.energy.dischargeWh === 2000);
  check('health nested with firmwareVersion', d.health && d.health.firmwareVersion === '1.5.0');
  check('overallQuality carried', d.overallQuality === 'VALID');
}

// --- T7: Legacy flat adapter (firmware-generic v1.4.0) ------------------------
console.log('\n[P0-002b] Legacy flat adapter:');
{
  const r = doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-GENERIC',
    data: { v_bat: 51.2, i_bat_dc: -5.1, p_bat_dc: -261.1, i_ac_load: 2.2,
            ina219_ok: true, temp_celsius: 30.1, free_heap: 111111, rssi: -66,
            fw_version: '1.4.0', sequence: 1 },
  });
  check('flat telemetry ACCEPTED', r.status === 'SUCCESS', JSON.stringify(r));
  const lat = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-GENERIC' });
  check('flat → canonical v_bat becomes battery.voltage.value=51.2', lat.data.battery.voltage.value === 51.2);
  check('flat ina219_ok=true → health.ina219Online', lat.data.health.ina219Online === true);
}

// --- T7b: v1.6.0 SOC provenance + BMS block round-trip ------------------------
console.log('\n[v1.6.0] SOC provenance + BMS block:');
{
  // Firmware 1.6.0 envelope with a locked Pylontech CAN BMS.
  const env16 = envelope(200, '2026-08-27T11:00:00Z', {
    firmwareVersion: '1.6.0',
    battery: {
      voltage: { value: 52.9, quality: 'VALID', source: 'MEASURED' },
      current: { value: 12.4, quality: 'VALID', source: 'MEASURED' },
      power: { value: 656.0, quality: 'DERIVED', source: 'DERIVED' },
      soc: { value: 81.2, quality: 'VALID', source: 'MEASURED', provenance: 'BMS_DIRECT' },
      chargeWh: 1000, dischargeWh: 2000, chargeAh: 20, dischargeAh: 40,
      bms: {
        connected: true, protocol: 'PYLONTECH_CAN', state: 'LOCKED',
        voltage: 52.9, current: 12.4, temperature: 29.1, soh: 98,
        cellVoltageMin: 3.30, cellVoltageMax: 3.32, cellCount: 16,
        chargeCurrentLimit: 100, dischargeCurrentLimit: 80,
        cycleCount: 42, faultFlags: 0, moduleCount: 1,
        lastSeenMs: 12345, currentMismatchA: 0.12,
      },
    },
  });
  const r = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BMS01', data: env16 });
  check('v1.6 telemetry ACCEPTED', r.status === 'SUCCESS', JSON.stringify(r));
  const lat = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-BMS01' });
  const d = lat.data;
  check('soc.provenance = BMS_DIRECT round-trips',
        d.battery.soc.provenance === 'BMS_DIRECT', JSON.stringify(d.battery.soc));
  check('bms block reconstructed: protocol',
        d.battery.bms && d.battery.bms.protocol === 'PYLONTECH_CAN');
  check('bms block reconstructed: connected',
        d.battery.bms.connected === true);
  check('bms block reconstructed: cellVoltageMin/Max',
        d.battery.bms.cellVoltageMin === 3.30 && d.battery.bms.cellVoltageMax === 3.32);
  check('bms block reconstructed: temperature',
        d.battery.bms.temperature === 29.1);
  check('bms block reconstructed: faultFlags 0', d.battery.bms.faultFlags === 0);
  // Second reading: shunt fallback (BMS lost) — provenance must change visibly.
  const env16b = envelope(201, '2026-08-27T11:00:05Z', {
    firmwareVersion: '1.6.0',
    battery: {
      voltage: { value: 52.8, quality: 'VALID', source: 'MEASURED' },
      current: { value: -3.1, quality: 'VALID', source: 'MEASURED' },
      power: { value: -163.7, quality: 'DERIVED', source: 'DERIVED' },
      soc: { value: 81.0, quality: 'ESTIMATED', source: 'ESTIMATED', provenance: 'SHUNT_COULOMB' },
      chargeWh: 1000, dischargeWh: 2000, chargeAh: 20, dischargeAh: 40,
      bms: { connected: false, protocol: 'NONE', state: 'LOST' },
    },
  });
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BMS01', data: env16b });
  const lat2 = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-BMS01' });
  check('fallback provenance SHUNT_COULOMB round-trips',
        lat2.data.battery.soc.provenance === 'SHUNT_COULOMB');
  check('fallback bms.connected=false',
        lat2.data.battery.bms.connected === false);
  // Pre-1.6 rows (earlier PLTS-TEST01 data) must read back UNKNOWN — honest.
  const latOld = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-TEST01' });
  check('pre-1.6 row provenance = UNKNOWN (never guessed)',
        latOld.data.battery.soc.provenance === 'UNKNOWN',
        JSON.stringify(latOld.data.battery.soc));
  // A pre-1.6 device cannot claim a connected BMS — either absent or false.
  check('pre-1.6 row never claims connected BMS',
        !latOld.data.battery.bms || latOld.data.battery.bms.connected === false);
}

// --- T7c: v1.6.0 header migration of a pre-existing sheet ---------------------
console.log('\n[v1.6.0] Telemetry header migration:');
{
  // Fresh deployment with Config populated, then swap in a v1.5-shaped
  // Telemetry sheet (24-column header + one pre-1.6 row).
  const envOld = createGasContext();
  envOld.sandbox.setupMasterTemplate();
  const ss = envOld.sandbox.SpreadsheetApp.getActiveSpreadsheet();
  delete ss.sheets['Telemetry'];
  const sheet = ss.insertSheet('Telemetry');
  const headerV15 = [
    'timestamp', 'device_key', 'sequence', 'event_time', 'is_late',
    'v_bat', 'i_bat_dc', 'p_bat_dc', 'soc', 'soc_quality',
    'i_ac_load', 'p_ac_est', 'temp_celsius', 'humidity',
    'charge_wh', 'discharge_wh', 'charge_ah', 'discharge_ah',
    'ina219_ok', 'time_quality', 'overall_quality',
    'free_heap', 'rssi', 'fw_version',
  ];
  sheet.appendRow(headerV15);
  sheet.appendRow([new Date(), 'PLTS-OLD', 1, new Date(), false,
                   51.0, -5.0, -255.0, 77.0, 'ESTIMATED',
                   2.0, 396.0, 30.0, 70.0,
                   10, 20, 1, 2,
                   true, 'VALID', 'VALID',
                   99999, -60, '1.5.0']);
  // Touch the sheet through getOrCreateSheet_ → migration appends the columns.
  const r = doPost(envOld, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-OLD',
                             data: { v_bat: 51.1, i_bat_dc: -5.0, ina219_ok: true, sequence: 2 } });
  check('telemetry accepted after migration', r.status === 'SUCCESS', JSON.stringify(r));
  const hdr = sheet.rows[0];
  check('header extended to 39 columns (v1.7 + W12 meter trio)', hdr.length === 39, `got ${hdr.length}`);
  check('header v1.6 columns appended in order',
        hdr[24] === 'soc_source' && hdr[25] === 'bms_protocol' && hdr[26] === 'bms_connected' &&
        hdr[30] === 'bms_fault_flags');
  // [WAVE-7] v1.7.0 emergency columns appended in order after the BMS block.
  check('header v1.7 columns appended in order',
        hdr[31] === 'i_ac_gen' && hdr[32] === 'emg_state' && hdr[33] === 'emg_reason' &&
        hdr[34] === 'emg_estop' && hdr[35] === 'emg_trips',
        JSON.stringify(hdr.slice(31)));
  // [W12-2] PZEM meter trio appended after the emergency block.
  check('header W12 meter columns appended in order',
        hdr[36] === 'p_ac_meter' && hdr[37] === 'meter_v' && hdr[38] === 'meter_connected',
        JSON.stringify(hdr.slice(36)));
  const oldRow = sheet.rows[1];
  check('pre-existing row keeps v1.5 length (old indices intact)',
        oldRow.length === 24, `got ${oldRow.length}`);
  const newRow = sheet.rows[2];
  check('new row carries v1.7 columns (39 values)',
        newRow && newRow.length === 39, `got ${newRow && newRow.length}`);
  check('new row meter trio honest-empty for flat payload (no meter)',
        newRow[36] === '' && newRow[37] === '' && String(newRow[38]).toUpperCase() === 'FALSE',
        JSON.stringify(newRow.slice(36)));
  check('new row soc_source default UNKNOWN for flat payload', newRow[24] === 'UNKNOWN');
  check('new row emg_state default empty for flat payload without emergency',
        newRow[32] === '', `got ${JSON.stringify(newRow[32])}`);
  // Reading the migrated pre-1.6 ROW (seq 1, via HISTORY) → no fabricated
  // provenance/BMS/emergency: the 24-column row has no v1.6/v1.7 data to invent.
  const hist = doPost(envOld, { action: 'HISTORY', token: TOKEN, device_key: 'PLTS-OLD' });
  const rec1 = hist.data.records.find((x) => x.sequence === 1);
  check('migrated pre-1.6 row reads provenance UNKNOWN',
        rec1 && rec1.battery.soc.provenance === 'UNKNOWN',
        JSON.stringify(rec1 && rec1.battery.soc));
  check('migrated pre-1.6 row has no fabricated bms block',
        rec1 && rec1.battery.bms === undefined);
  check('migrated pre-1.6 row has no fabricated emergency block',
        rec1 && rec1.emergency === undefined);
}

// --- T8: DAILY aggregation honesty -------------------------------------------
console.log('\n[DAILY] Honest daily aggregation:');
{
  // Two days of cumulative counters: day1 10:00 → 14:00, day2 next day
  const day1a = envelope(200, '2026-08-26T10:00:00Z');
  day1a.battery.chargeWh = 1000; day1a.battery.dischargeWh = 5000;
  const day1b = envelope(201, '2026-08-26T14:00:00Z');
  day1b.battery.chargeWh = 1500; day1b.battery.dischargeWh = 6500;
  const day2a = envelope(202, '2026-08-27T10:00:00Z');
  day2a.battery.chargeWh = 1500; day2a.battery.dischargeWh = 6500;
  const day2b = envelope(203, '2026-08-27T14:00:00Z');
  day2b.battery.chargeWh = 2200; day2b.battery.dischargeWh = 8000;
  for (const d of [day1a, day1b, day2a, day2b]) {
    doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-DAILY', data: d });
  }
  const rep = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'PLTS-DAILY', days: 7 });
  check('DAILY SUCCESS with 2 days', rep.status === 'SUCCESS' && rep.data.days.length === 2, JSON.stringify(rep));
  const d1 = rep.data.days.find((d) => d.date === '2026-08-26');
  const d2 = rep.data.days.find((d) => d.date === '2026-08-27');
  check('day1 chargeWh = 500 (1500-1000)', d1 && d1.chargeWh === 500, JSON.stringify(d1));
  check('day1 dischargeWh = 1500 (6500-5000)', d1 && d1.dischargeWh === 1500);
  check('day2 chargeWh = 700 (2200-1500)', d2 && d2.chargeWh === 700);
  check('day2 dischargeWh = 1500 (8000-6500)', d2 && d2.dischargeWh === 1500);
  check('energyQuality = VALID/PARTIAL (never fabricated)', d1 && ['VALID', 'PARTIAL'].includes(d1.energyQuality));
}

// --- T9: P1-016 — LockService held during mutations ---------------------------
console.log('\n[P1-016] Atomicity:');
{
  const before = env.locks.acquired;
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(106, '2026-08-27T10:01:00Z') });
  check('LockService acquired for TELEMETRY', env.locks.acquired === before + 1);
  check('lock released after mutation', env.locks.held === false);
}

// --- T10: SEQ_STATUS + uniform envelope --------------------------------------
console.log('\n[§33] Uniform response envelope:');
{
  const st = doPost(env, { action: 'SEQ_STATUS', token: TOKEN, device_key: 'PLTS-TEST01' });
  check('SEQ_STATUS returns ledger', st.status === 'SUCCESS' && st.data.highestSeq === 106);
  check('envelope keys exactly {status,code,data,message,timestamp}',
    JSON.stringify(Object.keys(st).sort()) === '["code","data","message","status","timestamp"]');
}

// --- T11: Calibration + OTA contract (WAVE-3: ACK bound to device, OTA admin gate)
console.log('\n[Compat] Calibration + OTA:');
{
  const pub = doPost(env, { action: 'CALIBRATION_PUBLISH', token: TOKEN, device_key: 'PLTS-TEST01', v_calib: 18.9, i_calib_dc: 1.0, i_calib_ac: 1.0 });
  check('CALIBRATION_PUBLISH → command_id', pub.status === 'SUCCESS' && !!pub.data.command_id);
  const pend = doPost(env, { action: 'CALIBRATION_PENDING', token: TOKEN, device_key: 'PLTS-TEST01' });
  check('CALIBRATION_PENDING returns the queue head', pend.data && pend.data.v_calib === 18.9);
  // [WAVE-3 / GAS-2-I] ACK sekarang menyertakan device_key pemilik perintah
  // (mirror firmware-generic v1.5.1); tanpa itu → ditolak.
  const ackNoDk = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, command_id: pub.data.command_id });
  check('CALIBRATION_ACK tanpa device_key → 400 (fail-closed)', ackNoDk.code === 400);
  const ack = doPost(env, { action: 'CALIBRATION_ACK', token: TOKEN, device_key: 'PLTS-TEST01', command_id: pub.data.command_id });
  check('CALIBRATION_ACK (device_key pemilik) → applied', ack.status === 'SUCCESS');
  // [WAVE-3 / GAS-2-K] OTA_PUBLISH membutuhkan admin_token operator.
  const ADMIN = 'plts_admin_contract_test_secret';
  const cfg = env.ss.getSheetByName('Config');
  const cfgRows = cfg.getDataRange().getValues();
  for (let i = 0; i < cfgRows.length; i++) {
    if (String(cfgRows[i][0]) === 'ADMIN_TOKEN') { cfg.getRange(i + 1, 2).setValue(ADMIN); break; }
  }
  const omNoAdmin = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, manifest: { version: '1.6.0', url: 'https://github.com/x/fw.bin', sha256: 'ab'.repeat(32), hmac: 'cd'.repeat(32) } });
  check('OTA_PUBLISH tanpa admin_token → 401 (gerbang admin)', omNoAdmin.code === 401);
  const om = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, admin_token: ADMIN, manifest: { version: '1.6.0', url: 'https://github.com/x/fw.bin', sha256: 'ab'.repeat(32), hmac: 'cd'.repeat(32) } });
  check('OTA_PUBLISH https + admin_token → SUCCESS', om.status === 'SUCCESS');
  const omBad = doPost(env, { action: 'OTA_PUBLISH', token: TOKEN, admin_token: ADMIN, manifest: { version: '1.6.0', url: 'http://evil.example.com/f.bin', sha256: 'ab'.repeat(32), hmac: 'cd'.repeat(32) } });
  check('OTA_PUBLISH http → REJECTED (400)', omBad.code === 400);
}

// --- T12: [AUDIT 2026-08-28 F-G17] Device-registration gate -------------------
console.log('\n[F-G17] Device-registration gate (fleet mode):');
{
  // Register one device in the DEVICES sheet → gate switches to fleet mode.
  const devices = env.ss.getSheetByName('Devices');
  devices.appendRow(['PLTS-TEST01', 'f1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d7e8f9a0b1c2d3e4f5a6b7c8d9e0f1a2', 'Test Device', '', '']);
  const ok = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TEST01', data: envelope(9001, '2026-08-28T09:00:00Z') });
  check('registered device TELEMETRY → ACCEPTED', ok.status === 'SUCCESS' && ok.data.decision === 'ACCEPTED', JSON.stringify(ok));
  const lat = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-TEST01' });
  check('registered device LATEST → SUCCESS', lat.status === 'SUCCESS', JSON.stringify(lat));
  const bad = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'PLTS-INTRUDER' });
  check('unknown device_key LATEST → 400 gate', bad.code === 400 && /Unknown device_key/.test(bad.message), JSON.stringify(bad));
  const badW = doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-TYPO01', data: envelope(9002, '2026-08-28T09:00:10Z') });
  check('unknown device_key TELEMETRY → 400 gate', badW.code === 400 && /Unknown device_key/.test(badW.message), JSON.stringify(badW));
  const badH = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'PLTS-INTRUDER' });
  check('unknown device_key HISTORY → 400 gate', badH.code === 400 && /Unknown device_key/.test(badH.message), JSON.stringify(badH));
  const tele = env.ss.getSheetByName('Telemetry');
  const orphans = tele.rows.filter((r) => String(r[1]) === 'PLTS-TYPO01' || String(r[1]) === 'PLTS-INTRUDER');
  check('no orphan telemetry rows written for unknown keys', orphans.length === 0, `got ${orphans.length}`);
}

// =============================================================================

console.log('\n============================================================');
console.log(`GAS CONTRACT: ${passed} passed, ${failed} failed`);
if (failed) { failures.forEach((f) => console.log('  ✗ ' + f)); process.exit(1); }
console.log('ALL GAS CONTRACT TESTS PASS');
