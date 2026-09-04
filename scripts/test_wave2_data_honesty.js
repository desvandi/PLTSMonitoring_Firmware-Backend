#!/usr/bin/env node
/**
 * test_wave2_data_honesty.js — WAVE-2 data-honesty tests (Level 2+ evidence)
 * =============================================================================
 * Re-audit 2026-08-28 findings fixed by Wave 2 (docs 16_WAVE2_DATA_HONESTY.md):
 *
 *   GAS-2-E  rowToEnvelope_ fabricated quality:'VALID' for null values and
 *            for channels whose quality was never persisted → now:
 *            null → NOT_AVAILABLE, present+overall-VALID → semantic label,
 *            present+degraded → UNVERIFIED (never a guess dressed as a label).
 *   GAS-2-F  DAILY bucketed by UTC date slice → Jakarta days ran 07:00→07:00
 *            WIB. Now bucketed via Config TIMEZONE (default Asia/Jakarta).
 *   GAS-2-G  DAILY energy = last−first → counter reset produced NEGATIVE
 *            daily energy. Now: per-segment positive-delta sum + the day is
 *            flagged energyQuality='COUNTER_RESET'.
 *   GAS-2-M  Unparseable event_time reached the sheet as "Invalid Date"
 *            (NaN sort keys, non-deterministic HISTORY). Now validated at
 *            ingest: fall back to server time + timeQuality='DEGRADED',
 *            NaN-safe HISTORY comparator.
 *
 * Runs the REAL code.gs/Code.gs in a Node sandbox (mocked GAS services, REAL
 * crypto + REAL timezone conversion via Intl) — same harness conventions as
 * test_wave1_integration.js.
 *
 * Usage: node scripts/test_wave2_data_honesty.js   (exit 0 = PASS)
 * =============================================================================
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// GAS service mocks (identical conventions to test_wave1_integration.js)
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
        self.rows[row - 1][col - 1] = v;
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

function toSignedBytes(buf) {
  return Array.from(buf).map((b) => (b > 127 ? b - 256 : b));
}

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cacheStore = new Map();

  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, parseInt, parseFloat, RegExp, Error,
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
      computeDigest: (alg, value) =>
        toSignedBytes(crypto.createHash('sha256')
          .update(Buffer.from(String(value), 'utf8')).digest()),
      computeHmacSha256Signature: (value, key) =>
        toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
          .update(Buffer.from(String(value), 'utf8')).digest()),
      // [GAS-2-F] REAL timezone conversion (closure over host Intl).
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
  return { sandbox, ss, locks };
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
    console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`);
  }
}

function doPost(env, body) {
  const out = env.sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
  return JSON.parse(out.text);
}

const TOKEN = 'TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE';

/** Nested canonical envelope with per-test overrides. */
function envelope(seq, ts, opts = {}) {
  return {
    protocolVersion: 2,
    firmwareVersion: '1.6.2',
    deviceId: 'W2-DEV',
    sequence: seq,
    timestamp: ts,
    timeQuality: 'VALID',
    battery: {
      voltage: { value: 51.4, quality: 'VALID' },
      current: { value: -3.7, quality: 'VALID' },
      power: { value: -190.2, quality: 'DERIVED' },
      soc: { value: 78.0, quality: 'ESTIMATED' },
      chargeWh: 1000, dischargeWh: 2000, chargeAh: 20, dischargeAh: 40,
    },
    ac: { rmsCurrent: { value: 1.8 }, estimatedPower: { value: 356.4 } },
    environment: { temperature: { value: 30.1 }, humidity: { value: 70.0 } },
    health: { freeHeap: 170000, rssi: -60, sensorHealth: { ina219: 'ONLINE' } },
    overallQuality: 'VALID',
    ...opts,
  };
}

// =============================================================================
// EXECUTION
// =============================================================================

console.log('\n=== WAVE-2 DATA HONESTY TESTS (real Code.gs + real tz conversion) ===\n');

const env = createGasContext();
env.sandbox.setupMasterTemplate();

// ---------------------------------------------------------------------------
// GROUP E — honest per-channel quality on read (GAS-2-E)
// ---------------------------------------------------------------------------
console.log('[E] Per-channel quality — no fabricated VALID (GAS-2-E):');

{
  // E1: happy path — everything VALID in, semantic labels out
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'W2-E', data: envelope(1, '2026-08-27T02:00:00Z') });
  const h = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'W2-E' });
  const r1 = h.data.records[0];
  check('E1 valid day: voltage quality VALID', r1.battery.voltage.quality === 'VALID');
  check('E1 valid day: power quality DERIVED (semantic label kept)',
    r1.battery.power.quality === 'DERIVED');
  check('E1 valid day: ac estimatedPower quality ESTIMATED',
    r1.ac.estimatedPower.quality === 'ESTIMATED');
  check('E1 valid day: energy quality VALID', r1.energy.quality === 'VALID');

  // E2: SENSOR_ERROR day — voltage null, others present
  doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-E',
    data: envelope(2, '2026-08-27T03:00:00Z', {
      overallQuality: 'SENSOR_ERROR',
      battery: {
        voltage: { value: null, quality: 'SENSOR_ERROR' },
        current: { value: -3.7, quality: 'VALID' },
        power: { value: null, quality: 'SENSOR_ERROR' },
        soc: { value: 78.0, quality: 'ESTIMATED' },
        chargeWh: 1100, dischargeWh: 2050, chargeAh: 21, dischargeAh: 41,
      },
      ac: { rmsCurrent: { value: null }, estimatedPower: { value: null } },
      environment: { temperature: { value: null }, humidity: { value: null } },
    }),
  });
  const h2 = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'W2-E' });
  const r2 = h2.data.records.find((r) => r.sequence === 2);
  check('E2 null value → quality NOT_AVAILABLE (was fabricated VALID before Wave 2)',
    r2.battery.voltage.value === null && r2.battery.voltage.quality === 'NOT_AVAILABLE',
    JSON.stringify(r2.battery.voltage));
  check('E2 present value + overall SENSOR_ERROR → UNVERIFIED (not VALID)',
    r2.battery.current.quality === 'UNVERIFIED');
  check('E2 null power → NOT_AVAILABLE', r2.battery.power.quality === 'NOT_AVAILABLE');
  check('E2 null AC/environment → NOT_AVAILABLE',
    r2.ac.rmsCurrent.quality === 'NOT_AVAILABLE' &&
    r2.environment.temperature.quality === 'NOT_AVAILABLE' &&
    r2.environment.humidity.quality === 'NOT_AVAILABLE');
  check('E2 counters present + degraded overall → energy UNVERIFIED',
    r2.energy.quality === 'UNVERIFIED');
  check('E2 soc quality still round-trips honestly (ESTIMATED)',
    r2.battery.soc.quality === 'ESTIMATED');

  // E3: legacy flat row, ina219 offline → overall SUSPECT → present channels UNVERIFIED
  doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-E',
    data: { sequence: 3, v_bat: 51.0, i_bat_dc: null, p_bat_dc: null,
            i_ac_load: null, ina219_ok: false, free_heap: 165000, rssi: -61,
            fw_version: '1.5.0' },
  });
  const h3 = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'W2-E' });
  const r3 = h3.data.records.find((r) => r.sequence === 3);
  check('E3 flat + SUSPECT: present v_bat → UNVERIFIED (was VALID — the audit lie)',
    r3.battery.voltage.value === 51.0 && r3.battery.voltage.quality === 'UNVERIFIED');
  check('E3 flat + SUSPECT: null i_bat → NOT_AVAILABLE',
    r3.battery.current.quality === 'NOT_AVAILABLE');
  check('E3 flat + SUSPECT: energy all-null → NOT_AVAILABLE',
    r3.energy.quality === 'NOT_AVAILABLE');

  // E4: legacy flat, ina219 online → overall VALID → semantic labels
  doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-E',
    data: { sequence: 4, v_bat: 51.2, i_bat_dc: -3.0, p_bat_dc: -153.6,
            i_ac_load: 1.5, ina219_ok: true, free_heap: 165000, rssi: -61,
            fw_version: '1.5.0' },
  });
  const h4 = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'W2-E' });
  const r4 = h4.data.records.find((r) => r.sequence === 4);
  check('E4 flat + VALID: v_bat → VALID (sound inference, not fabrication)',
    r4.battery.voltage.quality === 'VALID');
  check('E4 flat + VALID: p_bat → DERIVED', r4.battery.power.quality === 'DERIVED');
}

// ---------------------------------------------------------------------------
// GROUP F — DAILY buckets in deployment timezone (GAS-2-F)
// ---------------------------------------------------------------------------
console.log('\n[F] DAILY bucketing — Asia/Jakarta calendar days (GAS-2-F):');

{
  // Samples chosen around the WIB midnight boundary:
  //   2026-08-27T16:59:59Z = 2026-08-27 23:59:59 WIB  → bucket 2026-08-27
  //   2026-08-27T17:00:00Z = 2026-08-28 00:00:00 WIB  → bucket 2026-08-28
  //   2026-08-27T20:00:00Z = 2026-08-28 03:00:00 WIB  → bucket 2026-08-28
  // (the old UTC slice put ALL of these in 2026-08-27)
  const samples = [
    envelope(10, '2026-08-27T16:59:59Z'),
    envelope(11, '2026-08-27T17:00:00Z'),
    envelope(12, '2026-08-27T20:00:00Z'),
  ];
  for (const s of samples) {
    doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'W2-F', data: s });
  }
  const rep = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'W2-F', days: 7 });
  const dates = rep.data.days.map((d) => d.date);
  check('F1 three samples split into exactly 2 Jakarta days',
    rep.data.days.length === 2, JSON.stringify(dates));
  check('F2 23:59:59 WIB stays on 2026-08-27', dates.includes('2026-08-27'));
  check('F3 00:00 WIB + 03:00 WIB land on 2026-08-28',
    dates.includes('2026-08-28') &&
    rep.data.days.find((d) => d.date === '2026-08-28').samples === 2,
    JSON.stringify(rep.data.days));
  check('F4 days sorted ascending', dates[0] < dates[1]);

  // F5: TIMEZONE config is honored (proves the value is READ, not hardcoded
  // — switch to UTC and the bucketing must follow)
  const cfg = env.ss.sheets['Config'];
  cfg.rows = cfg.rows.map((r) => (r[0] === 'TIMEZONE' ? ['TIMEZONE', 'UTC'] : r));
  env.sandbox.invalidatePltsCache();   // config cache TTL is 6 h — bust it
  const repUtc = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'W2-F', days: 7 });
  check('F5 TIMEZONE=UTC honored → all 3 samples back in one 2026-08-27 bucket',
    repUtc.data.days.length === 1 && repUtc.data.days[0].date === '2026-08-27' &&
    repUtc.data.days[0].samples === 3,
    JSON.stringify(repUtc.data.days.map((d) => [d.date, d.samples])));
  // restore default for later groups
  cfg.rows = cfg.rows.map((r) => (r[0] === 'TIMEZONE' ? ['TIMEZONE', 'Asia/Jakarta'] : r));
  env.sandbox.invalidatePltsCache();
}

// ---------------------------------------------------------------------------
// GROUP G — counter-reset detection in DAILY (GAS-2-G)
// ---------------------------------------------------------------------------
console.log('\n[G] DAILY counter reset — honest segments (GAS-2-G):');

{
  // G1: mid-day counter reset. chargeWh: 1000 → 1500 → reset(10) → 200.
  //     Old math: 200-1000 = -800 (a NEGATIVE energy day — the audit bug).
  //     Honest:   (1500-1000) + (200-10) = 690 + COUNTER_RESET flag.
  const day = [
    ['2026-08-27T01:00:00Z', 1000, 5000],   // 08:00 WIB
    ['2026-08-27T04:00:00Z', 1500, 6500],   // 11:00 WIB
    ['2026-08-27T07:00:00Z', 10,   20],     // 14:00 WIB — device rebooted, counters reset
    ['2026-08-27T10:00:00Z', 200,  210],    // 17:00 WIB
  ];
  day.forEach((d, i) => {
    doPost(env, {
      action: 'TELEMETRY', token: TOKEN, device_key: 'W2-G',
      data: envelope(20 + i, d[0], {
        battery: {
          voltage: { value: 51.4, quality: 'VALID' },
          current: { value: -3.7, quality: 'VALID' },
          power: { value: -190.2, quality: 'DERIVED' },
          soc: { value: 78.0, quality: 'ESTIMATED' },
          chargeWh: d[1], dischargeWh: d[2], chargeAh: d[1] / 50, dischargeAh: d[2] / 50,
        },
      }),
    });
  });
  const rep = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'W2-G', days: 7 });
  const d27 = rep.data.days.find((d) => d.date === '2026-08-27');
  check('G1 reset day: chargeWh = 690 (segments 500+190), never -800',
    d27 && d27.chargeWh === 690, JSON.stringify(d27));
  check('G1 reset day: dischargeWh = 1500+190 = 1690 (segments, never -4790)',
    d27 && d27.dischargeWh === 1690);
  check('G1 reset day: energyQuality = COUNTER_RESET (visible to operator)',
    d27 && d27.energyQuality === 'COUNTER_RESET');
  check('G1 reset day: chargeAh aggregated per-segment too (10+3.8 = 13.8 = 690/50)',
    d27 && d27.chargeAh === 13.8, JSON.stringify(d27 && d27.chargeAh));

  // G2: clean day, no reset → same value as the old first-minus-last math
  const clean = [
    ['2026-08-26T01:00:00Z', 0, 0],
    ['2026-08-26T04:00:00Z', 400, 900],
    ['2026-08-26T10:00:00Z', 900, 2100],
  ];
  clean.forEach((d, i) => {
    doPost(env, {
      action: 'TELEMETRY', token: TOKEN, device_key: 'W2-G2',
      data: envelope(30 + i, d[0], {
        battery: {
          voltage: { value: 51.4, quality: 'VALID' },
          current: { value: -3.7, quality: 'VALID' },
          power: { value: -190.2, quality: 'DERIVED' },
          soc: { value: 78.0, quality: 'ESTIMATED' },
          chargeWh: d[1], dischargeWh: d[2], chargeAh: d[1] / 50, dischargeAh: d[2] / 50,
        },
      }),
    });
  });
  const rep2 = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'W2-G2', days: 7 });
  const d26 = rep2.data.days.find((d) => d.date === '2026-08-26');
  check('G2 clean day: chargeWh = 900 (identical to old math)', d26 && d26.chargeWh === 900);
  check('G2 clean day: dischargeWh = 2100', d26 && d26.dischargeWh === 2100);
  check('G2 clean day: energyQuality PARTIAL (3 samples < 24) — no reset flag',
    d26 && d26.energyQuality === 'PARTIAL');

  // G3: day with NO counters at all → honest null + NO_DATA
  doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-G3',
    data: envelope(40, '2026-08-26T02:00:00Z', {
      battery: {
        voltage: { value: 51.4, quality: 'VALID' },
        current: { value: -3.7, quality: 'VALID' },
        power: { value: -190.2, quality: 'DERIVED' },
        soc: { value: 78.0, quality: 'ESTIMATED' },
      },
    }),
  });
  const rep3 = doPost(env, { action: 'DAILY', token: TOKEN, device_key: 'W2-G3', days: 7 });
  const g3 = rep3.data.days[0];
  check('G3 no counters: chargeWh null + energyQuality NO_DATA',
    g3 && g3.chargeWh === null && g3.energyQuality === 'NO_DATA', JSON.stringify(g3));
  check('G3 no counters: soc stats still honest (78/78)',
    g3 && g3.socMin === 78 && g3.socMax === 78);
}

// ---------------------------------------------------------------------------
// GROUP M — event_time validation at ingest (GAS-2-M)
// ---------------------------------------------------------------------------
console.log('\n[M] event_time validation — no Invalid Date rows (GAS-2-M):');

{
  // M1: nested envelope with garbage device clock
  const r1 = doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-M',
    data: envelope(50, 'not-a-date'),
  });
  check('M1 garbage timestamp still ACCEPTED (data is real; clock is not)',
    r1.code === 200 && r1.data.decision === 'ACCEPTED', JSON.stringify(r1));

  // M2: flat payload with garbage event_time
  const r2 = doPost(env, {
    action: 'TELEMETRY', token: TOKEN, device_key: 'W2-M',
    data: { sequence: 51, event_time: 'garbage!!', v_bat: 51.0, ina219_ok: true },
  });
  check('M2 flat garbage event_time ACCEPTED', r2.code === 200);

  // M3: valid samples around the corrupt ones
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'W2-M',
    data: envelope(49, '2026-08-27T01:00:00Z') });
  doPost(env, { action: 'TELEMETRY', token: TOKEN, device_key: 'W2-M',
    data: envelope(52, '2026-08-27T05:00:00Z') });

  const h = doPost(env, { action: 'HISTORY', token: TOKEN, device_key: 'W2-M' });
  const recs = h.data.records;
  check('M3 HISTORY returns all 4 records deterministically',
    h.code === 200 && recs.length === 4, JSON.stringify(recs.map((r) => r.sequence)));
  const bad = recs.filter((r) => r.sequence === 50 || r.sequence === 51);
  check('M4 corrupt-clock rows: timeQuality DEGRADED (visible, not hidden)',
    bad.every((r) => r.timeQuality === 'DEGRADED'),
    JSON.stringify(bad.map((r) => [r.sequence, r.timeQuality])));
  check('M5 no "Invalid Date" anywhere in eventTime',
    recs.every((r) => r.eventTime === null || isFinite(Date.parse(r.eventTime))),
    JSON.stringify(recs.map((r) => r.eventTime)));
  check('M6 corrupt rows fall back to server time (parseable ISO, honest ordering)',
    bad.every((r) => r.eventTime && isFinite(Date.parse(r.eventTime))));
  const good = recs.filter((r) => r.sequence === 49 || r.sequence === 52);
  check('M7 valid rows: timeQuality preserved (VALID), eventTime exact round-trip',
    good.every((r) => r.timeQuality === 'VALID' &&
      r.eventTime === (r.sequence === 49 ? '2026-08-27T01:00:00.000Z' : '2026-08-27T05:00:00.000Z')),
    JSON.stringify(good.map((r) => [r.eventTime, r.timeQuality])));

  // M8: LATEST is well-formed too (no Invalid Date poisoning)
  const latest = doPost(env, { action: 'LATEST', token: TOKEN, device_key: 'W2-M' });
  check('M8 LATEST eventTime parseable (NaN keys never win LATEST)',
    latest.code === 200 && isFinite(Date.parse(latest.data.eventTime)),
    JSON.stringify(latest.data.eventTime));

  // M9: sheet row itself never contains "Invalid Date" (the storage lie)
  const rows = env.ss.sheets['Telemetry'].rows.filter((r) => String(r[1]) === 'W2-M');
  check('M9 sheet rows store Date objects / valid values, never "Invalid Date"',
    rows.every((r) => r[3] instanceof Date && isFinite(r[3].getTime())),
    JSON.stringify(rows.map((r) => String(r[3]))));
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
