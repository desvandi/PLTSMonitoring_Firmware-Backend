#!/usr/bin/env node
/**
 * test_bench_w14_pzem.js — WAVE 14 BENCH: PZEM flag trial (uji coba flag PZEM)
 * =============================================================================
 * Virtual-bench execution of docs/bench/PANDUAN_VALIDASI.md §3 (the part that
 * can run WITHOUT a physical meter on this host): a VirtualPzem-004T v3 slave
 * wired to a 1:1 JS port of Drivers/Pzem004tDriver.cpp over a virtual UART,
 * driven exactly like firmware_v1.ino's measurementTask, serialized exactly
 * like Web/BatteryStatusSerializer.h, then ingested by the REAL code.gs/Code.gs
 * in a vm sandbox and read back through the PWA gasEnvelope meter contract.
 *
 * This is the "uji coba flag PZEM" half of the W14 bench: it proves the
 * PLTS_ENABLE_PZEM_AC=1 DATA PATH is contract-correct end-to-end (driver →
 * telemetry → GAS sheet → LATEST → PWA), so the only thing left for the
 * physical bench is ACCURACY (compare against a reference meter), not
 * plumbing. The flag itself stays 0 in the repo until that physical pass.
 *
 * Check groups:
 *   PZ-0  W14-1 regression lock: the driver speaks the REAL PZEM v3 framing
 *         (FC 0x04 input registers, byte-count 0x14 validated, decode from
 *         byte 3) — the pre-bench code could never read a physical meter
 *   PZ-1  Happy loop: 1 Hz poll, presence on first valid frame, values flow
 *   PZ-2  Honesty under fault: timeout retracts presence; CRC error; garbage
 *         address; out-of-range nulls the WHOLE reading (never partial trust)
 *   PZ-3  Recovery: meter back online → MEASURED again, errorCount is lifetime
 *   PZ-4  Meter power loss: energyWh is the meter's own counter (reset on
 *         power loss) and is NEVER folded into the DC energy counters
 *   PZ-5  Telemetry JSON contract: ac.meter shape matches the serializer
 *         (connected + finite fields only; NaN omitted — never 0)
 *         [folded into PZ-1g + PZ-2e]
 *   PZ-6  GAS ingest (REAL Code.gs): meter trio lands in Telemetry columns
 *         37-39 (p_ac_meter / meter_v / meter_connected, 1-based); absent
 *         meter → ''/false; connected=false → ''/false (never fabricated)
 *   PZ-7  LATEST read-back → PWA gasEnvelope mapping. GAS LATEST is
 *         NESTED-ONLY (rowToEnvelope_ ac.meter{connected,voltage,power} — W12
 *         X10c contract); the flat p_ac_meter trio is a PWA-side fallback for
 *         a hypothetical legacy backend (gasEnvelope.ts comment) — static +
 *         behavioral
 *   PZ-8  Static flag gates: default OFF everywhere, pins 18/19, Serial1
 *         exclusive (RTU on Serial2), estimate path untouched
 *   PZ-9  Legacy-shape postmortem: a meter speaking the OLD (pre-W14) frame
 *         shape (24 B, no byte-count) is honestly rejected via incomplete-
 *         frame timeout — never a shifted decode — proof the bench found +
 *         fixed the bug
 *
 * Usage: node scripts/test_bench_w14_pzem.js   (exit 0 = PASS)
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { createGasContext } = require('./bench_w14_gasenv');

const FWB = path.join(__dirname, '..');
const FW = path.join(FWB, 'firmware');
const PWA = process.env.PLTS_PWA_REPO
  ? process.env.PLTS_PWA_REPO
  : path.join(FWB, '..', 'PLTSMonitoring_PWA');

let passed = 0, failed = 0;
const failures = [];
function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  PASS  ${name}`); }
  else {
    failed++; failures.push(name + (detail ? ` — ${detail}` : ''));
    console.log(`  FAIL  ${name}${detail ? ' — ' + detail : ''}`);
  }
}

// ---------------------------------------------------------------------------
// 1:1 port of Pzem004tDriver (firmware/Drivers/Pzem004tDriver.cpp) — mirror,
// not re-implementation: every branch below cites the cpp line it mirrors.
// ---------------------------------------------------------------------------

const PZEM_CONST = { // firmware/Core/Config.h:147-150
  POLL_MS: 1000, TIMEOUT_MS: 400, BAUD: 9600, ADDR: 0x01,
};
const FRAME_LEN = 25;

function crc16(buf, len) {                       // cpp:160-170
  let crc = 0xFFFF;
  for (let i = 0; i < len; i++) {
    crc ^= buf[i];
    for (let b = 0; b < 8; b++) {
      if (crc & 1) { crc >>= 1; crc ^= 0xA001; } else { crc >>= 1; }
    }
  }
  return crc & 0xFFFF;
}

const PLAUSIBLE = {                              // cpp:25-29
  volt: (v) => Number.isFinite(v) && v >= 80.0 && v <= 270.0,
  amp: (a) => Number.isFinite(a) && a >= 0.0 && a <= 100.0,
  watt: (p) => Number.isFinite(p) && p >= 0.0 && p <= 30000.0,
  hz: (f) => Number.isFinite(f) && f >= 45.0 && f <= 65.0,
  pf: (pf) => Number.isFinite(pf) && pf >= 0.0 && pf <= 1.0,
};

function decodeRegisters(d, nowMs) {             // cpp:122-158
  const r = {
    voltageV: NaN, currentA: NaN, powerW: NaN, energyWh: 0,
    frequencyHz: NaN, powerFactor: NaN, alarmFlag: 0,
    status: 'Ok', timestampMs: nowMs, errorCount: 0,
  };
  const volt = (d[0] << 8) | d[1];
  const amp = (d[2] << 24 >>> 0) + (d[3] << 16) + (d[4] << 8) + d[5];
  const watt = (d[6] << 24 >>> 0) + (d[7] << 16) + (d[8] << 8) + d[9];
  const ener = (d[10] << 24 >>> 0) + (d[11] << 16) + (d[12] << 8) + d[13];
  const hz = (d[14] << 8) | d[15];
  const pf = (d[16] << 8) | d[17];
  const alrm = (d[18] << 8) | d[19];
  r.voltageV = volt * 0.1; r.currentA = amp * 0.001; r.powerW = watt * 0.1;
  r.energyWh = ener; r.frequencyHz = hz * 0.1; r.powerFactor = pf * 0.01;
  r.alarmFlag = alrm;
  if (!PLAUSIBLE.volt(r.voltageV) || !PLAUSIBLE.amp(r.currentA) ||
      !PLAUSIBLE.watt(r.powerW) || !PLAUSIBLE.hz(r.frequencyHz) ||
      !PLAUSIBLE.pf(r.powerFactor)) {
    r.voltageV = NaN; r.currentA = NaN; r.powerW = NaN;   // cpp:32-35
    r.frequencyHz = NaN; r.powerFactor = NaN;
    r.status = 'OutOfRange';
  }
  return r;
}

/**
 * VirtualPzem — a PZEM-004T v3 slave on the other end of the virtual UART,
 * speaking the REAL Peacefair framing: response = [addr, FC, 0x14 count,
 * data20, crc2] = 25 bytes.
 * fault: 'ok' | 'silent' | 'corruptCrc' | 'wrongAddr' | 'plausFail' | 'legacy24'
 */
class VirtualPzem {
  constructor() {
    this.phys = { voltage: 226.2, current: 0.622, power: 141.0,
      energy: 5406, freq: 50.0, pf: 0.97, alarm: 0 };
    this.fault = 'ok';
    this.requestCount = 0;
    this.replied = 0;
    this.lastRequest = null;
  }
  setFault(f) { this.fault = f; }
  // Respond to a request frame; return response bytes or null (silent).
  respond(req) {
    this.requestCount++;
    this.lastRequest = Array.from(req);
    if (this.fault === 'silent') return null;
    const d = new Array(20).fill(0);
    const putU16 = (i, v) => { d[i] = (v >> 8) & 0xFF; d[i + 1] = v & 0xFF; };
    const putU32 = (i, v) => {
      d[i] = (v >> 24) & 0xFF; d[i + 1] = (v >> 16) & 0xFF;
      d[i + 2] = (v >> 8) & 0xFF; d[i + 3] = v & 0xFF;
    };
    putU16(0, Math.round(this.phys.voltage / 0.1));
    putU32(2, Math.round(this.phys.current / 0.001));
    putU32(6, Math.round(this.phys.power / 0.1));
    putU32(10, Math.round(this.phys.energy));
    putU16(14, Math.round(this.phys.freq / 0.1));
    putU16(16, Math.round(this.phys.pf / 0.01));
    putU16(18, this.phys.alarm);
    if (this.fault === 'plausFail') { d[0] = 0xFF; d[1] = 0xFF; }  // 6553.5 V

    let addr = 0x01, fc = 0x04;                    // echo FC 0x04 (real meter)
    if (this.fault === 'wrongAddr') addr = 0x02;
    // legacy24: the PRE-W14 (wrong) shape — [addr, fc, data20, crc2] = 24 bytes,
    // no byte-count. A real meter never sends this; kept to prove the fixed
    // driver rejects it honestly instead of decoding shifted garbage.
    const withCount = this.fault !== 'legacy24';
    const frame = [addr, fc];
    if (withCount) frame.push(0x14);               // Modbus byte count = 20
    frame.push(...d);
    let crc = crc16(frame, frame.length);
    if (this.fault === 'corruptCrc') crc ^= 0x00FF;   // flip low byte
    frame.push(crc & 0xFF, (crc >> 8) & 0xFF);
    this.replied++;
    return frame;
  }
}

/** Virtual UART: driver TX → meter (instant), meter RX → driver after latency. */
class VirtualUart {
  constructor(meter, latencyMs = 30) {
    this.meter = meter;
    this.latencyMs = latencyMs;
    this.rxQueue = [];   // {readyAtMs, bytes}
  }
  write(bytes, nowMs) {
    const resp = this.meter.respond(Array.from(bytes));
    if (resp) this.rxQueue.push({ readyAtMs: nowMs + this.latencyMs, bytes: resp });
  }
  available(nowMs) {
    return this.rxQueue.some((p) => p.readyAtMs <= nowMs);
  }
  read(nowMs) {
    const idx = this.rxQueue.findIndex((p) => p.readyAtMs <= nowMs);
    if (idx < 0) return null;
    return this.rxQueue.splice(idx, 1)[0].bytes;
  }
}

/** Driver mirror — Pzem004tDriver.cpp 1:1 (see line cites). */
class PzemDriverMirror {
  constructor(uart) {
    this.uart = uart;
    this._addr = PZEM_CONST.ADDR;
    this._available = false;   // cpp:43 — presence unproven until first frame
    this._begun = false;
    this._awaiting = false;
    this._awaitStartMs = 0;
    this._lastReqMs = 0;
    this._buf = [];
    this._reading = { voltageV: NaN, currentA: NaN, powerW: NaN, energyWh: 0,
      frequencyHz: NaN, powerFactor: NaN, alarmFlag: 0, status: 'NotInitialized',
      timestampMs: 0, errorCount: 0 };
    this.detectedLog = false;
  }
  begin() { this._begun = true; this._available = false; return true; }
  isAvailable() { return this._available; }
  getReading() { return this._reading; }

  tick(nowMs) {                                   // cpp:50-60
    if (!this._awaiting) {
      if (nowMs - this._lastReqMs >= PZEM_CONST.POLL_MS) this._sendRequest(nowMs);
      return;
    }
    this._pollResponse(nowMs);
  }
  _sendRequest(nowMs) {                           // cpp:62-81
    const frame = [this._addr, 0x04, 0x00, 0x00, 0x00, 0x0A];   // FC 04 [W14-1]
    const crc = crc16(frame, 6);
    frame.push(crc & 0xFF, crc >> 8);             // LOW byte first (cpp:73)
    this.uart.write(frame, nowMs);
    this._awaiting = true;
    this._awaitStartMs = nowMs;
    this._buf = [];
    this._lastReqMs = nowMs;
  }
  _pollResponse(nowMs) {                          // cpp:83-130
    for (;;) {
      const bytes = this.uart.read(nowMs);
      if (!bytes) break;
      for (const b of bytes) {
        if (this._buf.length < FRAME_LEN) this._buf.push(b);
      }
    }
    if (this._buf.length >= FRAME_LEN) {
      this._awaiting = false;
      if (this._buf[0] !== this._addr || this._buf[1] !== 0x04 ||
          this._buf[2] !== 0x14) {                // cpp:95-100 [W14-1] count check
        this._reading.errorCount++;
        this._resetFields();
        this._reading.status = 'CrcError';
        return;
      }
      const crcCalc = crc16(this._buf, FRAME_LEN - 2);
      const crcRecv = (this._buf[FRAME_LEN - 2]) | (this._buf[FRAME_LEN - 1] << 8);
      if (crcCalc !== crcRecv) {                                    // cpp:101-108
        this._reading.errorCount++;
        this._resetFields();
        this._reading.status = 'CrcError';
        return;
      }
      const r = decodeRegisters(this._buf.slice(3), nowMs);   // cpp:110 [W14-1]
      r.errorCount = this._reading.errorCount;     // cpp:111 keep lifetime counter
      this._reading = r;
      if (r.status === 'Ok' && !this._available) { // cpp:113-118
        this._available = true;
        this.detectedLog = true;
      }
      return;
    }
    if (nowMs - this._awaitStartMs >= PZEM_CONST.TIMEOUT_MS) {      // cpp:112-118
      this._awaiting = false;
      this._reading.errorCount++;
      this._resetFields();
      this._reading.status = 'Timeout';
      this._available = false;                     // cpp:117 presence retracted
    }
  }
  _resetFields() {                                 // cpp:32-35
    this._reading.voltageV = NaN; this._reading.currentA = NaN;
    this._reading.powerW = NaN; this._reading.frequencyHz = NaN;
    this._reading.powerFactor = NaN;
    if (this._reading.status !== 'CrcError' && this._reading.status !== 'Timeout') {
      this._reading.status = 'NotInitialized';
    }
  }
}

/** measurementTask integration mirror — firmware_v1.ino:805-817. */
class MeterStatusMirror {
  constructor() { this.meter = { connected: false, voltage: NaN, current: NaN,
    power: NaN, energy: NaN, frequency: NaN, powerFactor: NaN }; }
  update(driver) {
    const r = driver.getReading();
    const meterOk = driver.isAvailable() && r.status === 'Ok';   // ino:808-809
    this.meter.connected = meterOk;
    this.meter.voltage = meterOk ? r.voltageV : NaN;
    this.meter.current = meterOk ? r.currentA : NaN;
    this.meter.power = meterOk ? r.powerW : NaN;
    this.meter.energy = meterOk ? r.energyWh : NaN;
    this.meter.frequency = meterOk ? r.frequencyHz : NaN;
    this.meter.powerFactor = meterOk ? r.powerFactor : NaN;
  }
}

/** BatteryStatusSerializer.h mirror — ac.meter JSON (finite fields only). */
function serializeMeter(acMeterStatus) {
  const out = { connected: acMeterStatus.connected === true };
  const valid = (v) => Number.isFinite(v);
  if (valid(acMeterStatus.voltage)) out.voltage = acMeterStatus.voltage;
  if (valid(acMeterStatus.current)) out.current = acMeterStatus.current;
  if (valid(acMeterStatus.power)) out.power = acMeterStatus.power;
  if (valid(acMeterStatus.energy)) out.energy = acMeterStatus.energy;
  if (valid(acMeterStatus.frequency)) out.frequency = acMeterStatus.frequency;
  if (valid(acMeterStatus.powerFactor)) out.powerFactor = acMeterStatus.powerFactor;
  return out;
}

// ---------------------------------------------------------------------------
// Bench runner
// ---------------------------------------------------------------------------

console.log('test_bench_w14_pzem.js — W14 bench: PZEM flag trial (virtual meter)');
console.log('='.repeat(72));

// ---- PZ-1: happy loop -------------------------------------------------------
console.log('\n[PZ-1] Happy loop — 10 s @ 1 Hz, virtual meter online:');
{
  const meter = new VirtualPzem();
  const uart = new VirtualUart(meter, 30);
  const drv = new PzemDriverMirror(uart);
  const status = new MeterStatusMirror();
  drv.begin();
  let detectedAt = null;
  for (let t = 0; t <= 10000; t += 10) {
    drv.tick(t);
    status.update(drv);
    if (drv.isAvailable() && detectedAt === null) detectedAt = t;
  }
  check('PZ-1a presence proven by the FIRST valid frame (not UART open)',
    detectedAt !== null && detectedAt <= 1070, `detectedAt=${detectedAt}ms`);
  check('PZ-1b connected=true after detection', status.meter.connected === true);
  check('PZ-1c values flow: 226.2 V / 0.622 A / 141.0 W / 50.0 Hz / PF 0.97',
    Math.abs(status.meter.voltage - 226.2) < 1e-6 &&
    Math.abs(status.meter.current - 0.622) < 1e-6 &&
    Math.abs(status.meter.power - 141.0) < 1e-6 &&
    Math.abs(status.meter.frequency - 50.0) < 1e-6 &&
    Math.abs(status.meter.powerFactor - 0.97) < 1e-6,
    JSON.stringify(status.meter));
  check('PZ-1d poll rate ≈1 Hz (request every PZEM_POLL_MS)',
    Math.abs(meter.requestCount - 11) <= 1, `requests=${meter.requestCount}`);
  check('PZ-1e every request answered (no stuck awaiting)',
    meter.replied === meter.requestCount, `${meter.replied}/${meter.requestCount}`);
  check('PZ-1f zero errors while healthy', drv.getReading().errorCount === 0);
  const json = serializeMeter(status.meter);
  // [fix] epsilon compare: the JS mirror computes 2262*0.1 = 226.20000000000002
  // (IEEE-754 double); the C++ target computes (float)2262*0.1f and ArduinoJson
  // prints 226.2. Strict === on the mirror's double is a host artifact.
  const eq = (a, b) => Math.abs(a - b) < 1e-9;
  const shapeOk = json.connected === true && json.voltage !== undefined &&
    json.current !== undefined && json.power !== undefined &&
    json.energy !== undefined && json.frequency !== undefined &&
    json.powerFactor !== undefined;
  check('PZ-1g serializer shape: connected+voltage+current+power+energy+frequency+powerFactor',
    shapeOk && eq(json.voltage, 226.2) && eq(json.current, 0.622) &&
    eq(json.power, 141.0) && json.energy === 5406 && eq(json.frequency, 50.0) &&
    eq(json.powerFactor, 0.97), JSON.stringify(json));
}

// ---- PZ-2: honesty under fault ------------------------------------------------
console.log('\n[PZ-2] Honesty under fault (single instrument, single verdict):');
{
  // Timeout: meter goes silent mid-stream
  const meter = new VirtualPzem();
  const uart = new VirtualUart(meter, 30);
  const drv = new PzemDriverMirror(uart);
  const status = new MeterStatusMirror();
  drv.begin();
  for (let t = 0; t <= 3000; t += 10) { drv.tick(t); status.update(drv); }
  const hadPresence = drv.isAvailable();
  meter.setFault('silent');
  for (let t = 3010; t <= 8000; t += 10) { drv.tick(t); status.update(drv); }
  check('PZ-2a presence WAS true before silence', hadPresence === true);
  check('PZ-2b timeout (400 ms) retracts presence → connected=false',
    drv.isAvailable() === false && status.meter.connected === false);
  check('PZ-2c values NaN (never 0, never stale)',
    Number.isNaN(status.meter.voltage) && Number.isNaN(status.meter.power));
  check('PZ-2d timeout counted as error', drv.getReading().errorCount >= 1,
    `errorCount=${drv.getReading().errorCount}`);
  const json = serializeMeter(status.meter);
  check('PZ-2e serializer: only {connected:false} — no fabricated fields',
    JSON.stringify(json) === '{"connected":false}', JSON.stringify(json));

  // CRC corruption
  const meter2 = new VirtualPzem();
  const drv2 = new PzemDriverMirror(new VirtualUart(meter2, 30));
  const status2 = new MeterStatusMirror();
  drv2.begin();
  for (let t = 0; t <= 2000; t += 10) { drv2.tick(t); status2.update(drv2); }
  const errsBefore = drv2.getReading().errorCount;
  meter2.setFault('corruptCrc');
  for (let t = 2010; t <= 4000; t += 10) { drv2.tick(t); status2.update(drv2); }
  check('PZ-2f CRC error → status CrcError, values nulled, errorCount++',
    drv2.getReading().status === 'CrcError' &&
    drv2.getReading().errorCount > errsBefore,
    `status=${drv2.getReading().status} errs=${drv2.getReading().errorCount}`);
  check('PZ-2g CRC error does NOT retract presence by itself (timeout does)',
    drv2.isAvailable() === true && status2.meter.connected === false,
    `available=${drv2.isAvailable()} connected=${status2.meter.connected}`);

  // Wrong address echo
  const meter3 = new VirtualPzem();
  const drv3 = new PzemDriverMirror(new VirtualUart(meter3, 30));
  drv3.begin();
  for (let t = 0; t <= 2000; t += 10) drv3.tick(t);
  meter3.setFault('wrongAddr');
  for (let t = 2010; t <= 4000; t += 10) drv3.tick(t);
  check('PZ-2h address/FC mismatch → CrcError branch (cpp:87-92)',
    drv3.getReading().status === 'CrcError');

  // Out-of-range: impossible voltage nulls the WHOLE reading
  const meter4 = new VirtualPzem();
  const drv4 = new PzemDriverMirror(new VirtualUart(meter4, 30));
  const status4 = new MeterStatusMirror();
  drv4.begin();
  for (let t = 0; t <= 2000; t += 10) { drv4.tick(t); status4.update(drv4); }
  meter4.setFault('plausFail');
  for (let t = 2010; t <= 4000; t += 10) { drv4.tick(t); status4.update(drv4); }
  check('PZ-2i impossible voltage (6553.5 V) → OutOfRange, whole reading nulled',
    drv4.getReading().status === 'OutOfRange' &&
    Number.isNaN(drv4.getReading().voltageV) &&
    Number.isNaN(drv4.getReading().powerW) &&
    Number.isNaN(drv4.getReading().frequencyHz));
  check('PZ-2j a REAL anomaly (250 V / 15 A) is still REPORTED (gate=possible, not safe)',
    PLAUSIBLE.volt(250.0) && PLAUSIBLE.amp(15.0));
}

// ---- PZ-3: recovery -----------------------------------------------------------
console.log('\n[PZ-3] Recovery after reconnect:');
{
  const meter = new VirtualPzem();
  const drv = new PzemDriverMirror(new VirtualUart(meter, 30));
  const status = new MeterStatusMirror();
  drv.begin();
  for (let t = 0; t <= 2000; t += 10) { drv.tick(t); status.update(drv); }
  meter.setFault('silent');
  for (let t = 2010; t <= 6000; t += 10) { drv.tick(t); status.update(drv); }
  const errsAtOutage = drv.getReading().errorCount;
  check('PZ-3a outage registered (errors grew)', errsAtOutage > 0);
  meter.setFault('ok');
  for (let t = 6010; t <= 9000; t += 10) { drv.tick(t); status.update(drv); }
  check('PZ-3b reconnect → presence re-proven, MEASURED again',
    drv.isAvailable() === true && status.meter.connected === true &&
    Math.abs(status.meter.voltage - 226.2) < 1e-6);
  check('PZ-3c errorCount is a LIFETIME counter (not reset by recovery)',
    drv.getReading().errorCount >= errsAtOutage);
}

// ---- PZ-4: energy counter semantics --------------------------------------------
console.log('\n[PZ-4] Energy counter semantics (meter power loss):');
{
  const meter = new VirtualPzem();
  const drv = new PzemDriverMirror(new VirtualUart(meter, 30));
  const status = new MeterStatusMirror();
  drv.begin();
  for (let t = 0; t <= 2000; t += 10) { drv.tick(t); status.update(drv); }
  check('PZ-4a energy reported from the meter counter (5406 Wh)',
    status.meter.energy === 5406);
  // Meter loses power: its cumulative counter RESETS to 0 (documented honesty)
  meter.phys.energy = 0;
  meter.phys.power = 0;
  for (let t = 2010; t <= 5000; t += 10) { drv.tick(t); status.update(drv); }
  check('PZ-4b meter counter reset is REPORTED (0 Wh), not smoothed/hidden',
    status.meter.energy === 0 && status.meter.connected === true);
  const energyCountersCpp = fs.readFileSync(
    path.join(FW, 'Services', 'EnergyCounters.cpp'), 'utf-8');
  const inoSrc = fs.readFileSync(path.join(FW, 'firmware_v1.ino'), 'utf-8');
  check('PZ-4c PZEM energy is NEVER integrated into the DC energy counters',
    !/pzem|meter/i.test(energyCountersCpp) &&
    !/energyWh\s*\+\=|chargeWh\s*=\s*.*meter/i.test(inoSrc));
}

// ---- PZ-6 (GAS ingest runs before static so we can reuse the env) --------------
console.log('\n[PZ-6] GAS ingest — REAL Code.gs in vm sandbox:');
const TOKEN = 'plts_sec_CHANGE_ME';
{
  const g = createGasContext();
  g.sandbox.setupMasterTemplate();
  g.registerDevice('PLTS-BENCH-PZEM', 'modular');
  const envelope = (seq, ts, acBlock) => ({
    protocolVersion: 1, firmwareVersion: '1.7.1', deviceId: 'PLTS-BENCH-PZEM',
    sequence: seq, timestamp: ts, timeQuality: 'VALID',
    battery: { voltage: { value: 52.4, quality: 'VALID', source: 'MEASURED' },
      current: { value: -10.2, quality: 'VALID', source: 'MEASURED' },
      power: { value: -534.5, quality: 'DERIVED', source: 'DERIVED' },
      soc: { value: 78.4, quality: 'ESTIMATED' },
      chargeWh: 1000, dischargeWh: 2000, chargeAh: 20, dischargeAh: 40 },
    ac: { rmsCurrent: { value: 3.2 }, estimatedPower: { value: 633.6 }, ...acBlock },
    environment: { temperature: { value: 31.2 }, humidity: { value: 72.1 } },
    health: { freeHeap: 123456, rssi: -61, sensorHealth: { ina219: 'ONLINE' } },
    overallQuality: 'VALID',
  });

  // healthy meter
  let r = g.doPost({ action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BENCH-PZEM',
    data: envelope(1, '2026-09-03T08:00:00Z',
      { meter: { connected: true, voltage: 226.2, current: 0.622, power: 141.0,
        energy: 5406, frequency: 50.0, powerFactor: 0.97 } }) });
  check('PZ-6a healthy meter telemetry ACCEPTED',
    r.status === 'SUCCESS' && r.data.decision === 'ACCEPTED', JSON.stringify(r));
  let row = g.rows('Telemetry').find((x) => Number(x[2]) === 1);
  const TELEMETRY_HEADER = g.rows('Telemetry')[0];
  const idxMeter = TELEMETRY_HEADER.indexOf('p_ac_meter');
  const idxV = TELEMETRY_HEADER.indexOf('meter_v');
  const idxC = TELEMETRY_HEADER.indexOf('meter_connected');
  check('PZ-6b sheet header carries the meter trio at the documented columns',
    idxMeter === 36 && idxV === 37 && idxC === 38,
    `p_ac_meter@${idxMeter} meter_v@${idxV} meter_connected@${idxC}`);
  check('PZ-6c healthy row: p_ac_meter=141, meter_v=226.2, meter_connected=TRUE',
    Number(row[idxMeter]) === 141.0 && Number(row[idxV]) === 226.2 &&
    String(row[idxC]).toUpperCase() === 'TRUE',
    `meter=[${row[idxMeter]},${row[idxV]},${row[idxC]}]`);

  // disconnected meter (present but connected=false)
  r = g.doPost({ action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BENCH-PZEM',
    data: envelope(2, '2026-09-03T08:00:10Z', { meter: { connected: false } }) });
  row = g.rows('Telemetry').find((x) => Number(x[2]) === 2);
  check('PZ-6d meter present but disconnected → \'\'/false (never fabricated)',
    r.status === 'SUCCESS' && row[idxMeter] === '' && row[idxV] === '' &&
    String(row[idxC]).toUpperCase() === 'FALSE',
    `meter=[${row[idxMeter]},${row[idxV]},${row[idxC]}]`);

  // no meter block at all (generic firmware / flag OFF)
  r = g.doPost({ action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BENCH-PZEM',
    data: envelope(3, '2026-09-03T08:00:20Z', {}) });
  row = g.rows('Telemetry').find((x) => Number(x[2]) === 3);
  check('PZ-6e meter block ABSENT (flag OFF / generic) → \'\'/false (honest no-meter)',
    r.status === 'SUCCESS' && row[idxMeter] === '' && row[idxV] === '' &&
    String(row[idxC]).toUpperCase() === 'FALSE');
  check('PZ-6f estimate stays the headline when no meter (p_ac_est column filled)',
    row[TELEMETRY_HEADER.indexOf('p_ac_est')] !== '');

  // seq 4 — healthy meter again, NEWEST event time. LATEST = newest by
  // event time (P1-003), so the read-back must project THIS row's meter.
  r = g.doPost({ action: 'TELEMETRY', token: TOKEN, device_key: 'PLTS-BENCH-PZEM',
    data: envelope(4, '2026-09-03T08:00:30Z',
      { meter: { connected: true, voltage: 226.2, current: 0.622, power: 141.0,
        energy: 5406, frequency: 50.0, powerFactor: 0.97 } }) });
  if (r.status !== 'SUCCESS') throw new Error('seq-4 ingest failed: ' + JSON.stringify(r));

  // LATEST read-back (nested envelope out) — the PWA-facing contract
  const latest = g.doPost({ action: 'LATEST', token: TOKEN,
    device_key: 'PLTS-BENCH-PZEM' });
  const acMeter = latest && latest.data && latest.data.ac && latest.data.ac.meter;
  check('PZ-7a LATEST returns the nested ac.meter block for the newest row (seq 4)',
    acMeter && acMeter.connected === true &&
    Math.abs(acMeter.power - 141.0) < 1e-9 &&
    Math.abs(acMeter.voltage - 226.2) < 1e-9,
    JSON.stringify(latest.data && latest.data.ac));
  // [fix] GAS LATEST is NESTED-ONLY (W12 X10c: rowToEnvelope_ emits
  // ac.meter{connected,voltage,power}); the flat p_ac_meter/meter_v/
  // meter_connected trio lives on the PWA side as a fallback for a
  // hypothetical legacy backend (gasEnvelope.ts comment). Lock both sides
  // of that contract: nested present, flat absent (PWA maps nested first).
  const flatKeys = latest && latest.data ? Object.keys(latest.data) : [];
  check('PZ-7b LATEST contract: nested ac.meter only (flat trio is PWA-side fallback)',
    acMeter && acMeter.connected === true &&
    flatKeys.indexOf('p_ac_meter') === -1 &&
    flatKeys.indexOf('meter_v') === -1 &&
    flatKeys.indexOf('meter_connected') === -1,
    `meter=${JSON.stringify(acMeter)} flatKeys should be absent`);
}

// ---- PZ-7 static: PWA gasEnvelope meter contract -------------------------------
console.log('\n[PZ-7] PWA gasEnvelope meter mapping (source contract):');
{
  const ge = fs.readFileSync(path.join(PWA, 'src', 'lib', 'gasEnvelope.ts'), 'utf-8');
  check('PZ-7c gasEnvelope: nested ac.meter first, flat fallback (W12)',
    /meter\.power\s*\?\?\s*d\.p_ac_meter/.test(ge) &&
    /meter\.voltage\s*\?\?\s*d\.meter_v/.test(ge) &&
    /meter\.connected/.test(ge));
  check('PZ-7d gasEnvelope: \'TRUE\' string tolerated (sheet boolean)',
    /meter\.connected === true \|\| meter\.connected === "TRUE"/.test(ge) ||
    /d\.meter_connected === true \|\| d\.meter_connected === "TRUE"/.test(ge));
  // [fix] the meter trio is declared on the GAS envelope DTO
  // (src/lib/gasEnvelope.ts:29-31), not src/lib/types.ts.
  const geTypes = fs.readFileSync(path.join(PWA, 'src', 'lib', 'gasEnvelope.ts'), 'utf-8');
  check('PZ-7e PWA gasEnvelope DTO: p_ac_meter/meter_v/meter_connected nullable (no meter → null)',
    /p_ac_meter: number \| null/.test(geTypes) &&
    /meter_v: number \| null/.test(geTypes) &&
    /meter_connected: boolean \| null/.test(geTypes));
}

// ---- PZ-8: static flag gates ----------------------------------------------------
console.log('\n[PZ-8] Static flag gates (flag stays OFF until physical validation):');
{
  const conf = fs.readFileSync(path.join(FW, 'Core', 'Config.h'), 'utf-8');
  const ini = fs.readFileSync(path.join(FW, 'platformio.ini'), 'utf-8');
  const cpp = fs.readFileSync(path.join(FW, 'Drivers', 'Pzem004tDriver.cpp'), 'utf-8');
  const h = fs.readFileSync(path.join(FW, 'Drivers', 'Pzem004tDriver.h'), 'utf-8');
  const rtu = fs.readFileSync(path.join(FW, 'Comm', 'ModbusRtuClient.cpp'), 'utf-8');
  const acMeas = fs.readFileSync(path.join(FW, 'Services', 'AcMeasurement.h'), 'utf-8');
  check('PZ-8a PLTS_ENABLE_PZEM_AC defaults 0 (Config.h)',
    /#define\s+PLTS_ENABLE_PZEM_AC\s+0/.test(conf));
  check('PZ-8b flag 0 in platformio.ini build flags',
    /-DPLTS_ENABLE_PZEM_AC=0/.test(ini));
  check('PZ-8c pins RX=18 TX=19, 9600 8N1 fixed',
    /PIN_PZEM_RX\s*=\s*18/.test(conf) && /PIN_PZEM_TX\s*=\s*19/.test(conf) &&
    /PZEM_BAUD\s*=\s*9600/.test(conf));
  check('PZ-8d driver fully flag-guarded (.h + .cpp)',
    /#if PLTS_ENABLE_PZEM_AC/.test(h) && /#if PLTS_ENABLE_PZEM_AC/.test(cpp));
  check('PZ-8e include order: Config.h BEFORE the guard (W12 fix holds)',
    h.indexOf('#include "../Core/Config.h"') <
    h.indexOf('#if PLTS_ENABLE_PZEM_AC'));
  check('PZ-8f no UART collision: PZEM on Serial1, Modbus RTU on Serial2',
    /Serial1\.begin/.test(cpp) && !/Serial2/.test(cpp) && /Serial2/.test(rtu));
  check('PZ-8g estimate path untouched (AcMeasurement keeps ACS712 estimation)',
    /estimated/i.test(acMeas) || /acs712/i.test(acMeas));
}

// ---- PZ-9: legacy-shape postmortem (the bug the W14 bench found) ---------------
console.log('\n[PZ-9] Legacy-shape postmortem — pre-W14 24 B frame rejected honestly:');
{
  const meter = new VirtualPzem();
  const uart = new VirtualUart(meter, 30);
  const drv = new PzemDriverMirror(uart);
  drv.begin();
  for (let t = 0; t <= 2000; t += 10) drv.tick(t);   // healthy 25 B frames first
  const healthyBefore = drv.isAvailable();
  meter.setFault('legacy24');                        // 24 B, no byte-count
  const errsBefore = drv.getReading().errorCount;
  for (let t = 2010; t <= 5000; t += 10) drv.tick(t);
  const r = drv.getReading();
  check('PZ-9a pre-W14 shape (24 B) never completes the 25 B frame — timeout, not decode',
    healthyBefore === true && r.errorCount > errsBefore &&
    (r.status === 'Timeout' || r.status === 'CrcError'),
    `status=${r.status} errs=${r.errorCount}`);
  check('PZ-9b no shifted decode: measurement fields stay NaN (never garbage)',
    Number.isNaN(r.voltageV) && Number.isNaN(r.powerW) &&
    Number.isNaN(r.frequencyHz));
  check('PZ-9c presence retracted under the legacy shape',
    drv.isAvailable() === false, `available=${drv.isAvailable()}`);
}

// ---- summary --------------------------------------------------------------------
console.log('\n' + '='.repeat(72));
console.log(`RESULT: ${passed} passed, ${failed} failed`);
if (failures.length) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
}
process.exit(failed === 0 ? 0 : 1);
