#!/usr/bin/env node
/**
 * bench_w14_gasenv.js — W14 bench: shared GAS sandbox (REAL Code.gs in vm)
 * =============================================================================
 * Loads code.gs/Code.gs into a Node vm context with faithful-enough GAS
 * service mocks. Unlike test_gas_contract.js (stub HMAC), this harness
 * implements Utilities.computeHmacSha256Signature with REAL crypto so the
 * W14 benches can exercise the per-device OTA manifest HMAC chain, the
 * OTA_PUBLISH/OTA_MANIFEST/OTA_STATUS/OTA_LOG round trip and the [W12-2]
 * PZEM meter ingest end-to-end against the production script text.
 *
 * Exported:
 *   createGasContext()  -> { sandbox, ss, doPost, doGet, setConfig,
 *                            registerDevice, rows, sheet }
 * Usage: const g = createGasContext(); g.sandbox.setupMasterTemplate();
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// FakeSheet / FakeSpreadsheet (GAS SpreadsheetApp mock)
// ---------------------------------------------------------------------------

class FakeSheet {
  constructor(name, header) {
    this.name = name;
    this.rows = header ? [header.slice()] : [];
  }
  getName() { return this.name; }
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
      setFontWeight: () => chainable(),
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

// ---------------------------------------------------------------------------
// createGasContext
// ---------------------------------------------------------------------------

function createGasContext() {
  const ss = new FakeSpreadsheet();
  const locks = { held: false, acquired: 0 };
  const cache = new Map();

  const sandbox = {
    console,
    JSON, Math, Date, Number, String, Object, Array, Boolean,
    isNaN, isFinite, parseInt, parseFloat, RegExp, Error, TypeError,
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
        put: (k, v) => cache.set(k, v),
        remove: (k) => cache.delete(k),
        removeAll: (keys) => keys.forEach((k) => cache.delete(k)),
      }),
    },
    // REAL crypto — the W14 benches verify real HMAC chains.
    Utilities: {
      getUuid: () => 'uuid-' + crypto.randomUUID().slice(0, 8),
      computeHmacSha256Signature: (message, key) =>
        Array.from(crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
          .update(Buffer.from(String(message), 'utf8')).digest()),
      base64Encode: (data) => Buffer.from(String(data), 'utf8').toString('base64'),
      base64Decode: (b64) => Array.from(Buffer.from(String(b64), 'base64')),
      Charset: { UTF_8: 'UTF_8', US_ASCII: 'US_ASCII' },
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
    UrlFetchApp: { fetch: () => ({ getContentText: () => '' }) },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(
    path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });

  // ---- helpers over the sandbox ----
  function doPost(body) {
    const out = sandbox.doPost({ postData: { contents: JSON.stringify(body) } });
    return JSON.parse(out.text);
  }
  function doGet(params) {
    const out = sandbox.doGet({ parameter: params || {} });
    return JSON.parse(out.text);
  }
  /** Mutate a Config key on the sheet + drop the whole config cache. */
  function setConfig(key, value) {
    const sheet = ss.getSheetByName('Config');
    if (!sheet) throw new Error('Config sheet missing — run setupMasterTemplate() first');
    for (let i = 0; i < sheet.rows.length; i++) {
      if (String(sheet.rows[i][0]).trim() === key) {
        sheet.rows[i][1] = String(value);
        cache.clear();
        return true;
      }
    }
    sheet.appendRow([key, String(value)]);
    cache.clear();
    return true;
  }
  /** Register a device in the DEVICES sheet (6-col firmware_type). */
  function registerDevice(deviceKey, firmwareType, secret) {
    const sheet = ss.getSheetByName('Devices');
    if (!sheet) throw new Error('Devices sheet missing');
    sheet.appendRow([deviceKey, secret || ('sec_' + deviceKey),
      'bench ' + deviceKey, '', '', firmwareType || '']);
    return true;
  }
  function sheet(name) { return ss.getSheetByName(name); }
  function rows(name) {
    const s = ss.getSheetByName(name);
    return s ? s.rows : null;
  }

  return { sandbox, ss, cache, doPost, doGet, setConfig, registerDevice, sheet, rows };
}

module.exports = { createGasContext, FakeSheet, FakeSpreadsheet };
