#!/usr/bin/env node
/**
 * _wave1_local_server.js — internal helper: serve the REAL code.gs/Code.gs
 * over local HTTP so scripts/wave1_smoke_test.sh can be verified end-to-end
 * (curl + openssl canonical construction vs real verifyHmac_) before being
 * pointed at a live GAS deployment.
 *
 * Env:
 *   PORT            (default 8788)
 *   AUTH_TOKEN      (default 'plts_sec_local_smoke') — seeded into Config
 *   ADMIN_TOKEN     (default 'plts_admin_local_smoke') — seeded into Config
 *   DEVICE_ID       (default 'PLTS_MONITOR_01')      — registered in Devices
 *   DEVICE_SECRET   (default 'local_smoke_secret')   — per-device HMAC secret
 *
 * Usage: node scripts/_wave1_local_server.js &
 *        GAS_URL=http://localhost:8788 AUTH_TOKEN=... ./scripts/wave1_smoke_test.sh
 */
'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const http = require('http');
const crypto = require('crypto');

const PORT = Number(process.env.PORT || 8788);
const AUTH_TOKEN = process.env.AUTH_TOKEN || 'plts_sec_local_smoke';
const ADMIN_TOKEN = process.env.ADMIN_TOKEN || 'plts_admin_local_smoke';
const DEVICE_ID = process.env.DEVICE_ID || 'PLTS_MONITOR_01';
const DEVICE_SECRET = process.env.DEVICE_SECRET || 'local_smoke_secret';

// ---- GAS mocks (same as test_wave1_integration.js) ------------------------
class FakeSheet {
  constructor(name, header) {
    this.name = name; this.rows = header ? [header.slice()] : [];
  }
  getLastRow() { return this.rows.length; }
  getLastColumn() { return this.rows.reduce((m, r) => Math.max(m, r.length), 0); }
  appendRow(row) { this.rows.push(row.slice()); return this; }
  getDataRange() { const s = this; return { getValues: () => s.rows.map((r) => r.slice()) }; }
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
        self.rows[row - 1][col - 1] = v; return this;
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
  getSheetByName(n) { return this.sheets[n] || null; }
  insertSheet(n) { this.sheets[n] = new FakeSheet(n, null); return this.sheets[n]; }
}
const toSignedBytes = (buf) => Array.from(buf).map((b) => (b > 127 ? b - 256 : b));

const ss = new FakeSpreadsheet();
const cacheStore = new Map();
const sandbox = {
  console, JSON, Math, Date, Number, String, Object, Array,
  isNaN, parseInt, parseFloat, RegExp, Error,
  SpreadsheetApp: { getActiveSpreadsheet: () => ss, getUi: () => ({ alert: () => {} }) },
  LockService: { getScriptLock: () => ({ tryLock: () => true, releaseLock: () => {} }) },
  CacheService: {
    getScriptCache: () => ({
      get: (k) => { const e = cacheStore.get(k); if (!e) return null; if (e.exp < Date.now()) { cacheStore.delete(k); return null; } return e.v; },
      put: (k, v, ttl) => cacheStore.set(k, { v, exp: Date.now() + (ttl || 600) * 1000 }),
      remove: (k) => cacheStore.delete(k),
    }),
  },
  Utilities: {
    getUuid: () => crypto.randomUUID(),
    DigestAlgorithm: { SHA_256: 'SHA_256' },
    Charset: { UTF_8: 'UTF_8' },
    computeDigest: (alg, v) => toSignedBytes(crypto.createHash('sha256').update(Buffer.from(String(v), 'utf8')).digest()),
    computeHmacSha256Signature: (v, k) => toSignedBytes(crypto.createHmac('sha256', Buffer.from(String(k), 'utf8')).update(Buffer.from(String(v), 'utf8')).digest()),
  },
  ContentService: {
    createTextOutput: (s) => ({ text: s, setMimeType: function () { return this; } }),
    MimeType: { JSON: 'JSON' },
  },
  UrlFetchApp: { fetch: () => ({}) },
};

vm.createContext(sandbox);
const code = fs.readFileSync(path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
vm.runInContext(code, sandbox, { filename: 'Code.gs' });
sandbox.setupMasterTemplate();

// Seed credentials from env (mirrors an operator's real sheet)
ss.sheets['Config'].rows = ss.sheets['Config'].rows.map(
  (r) => (r[0] === 'AUTH_TOKEN' ? ['AUTH_TOKEN', AUTH_TOKEN]
        : r[0] === 'ADMIN_TOKEN' ? ['ADMIN_TOKEN', ADMIN_TOKEN]
        : r));
ss.sheets['Devices'].appendRow([DEVICE_ID, DEVICE_SECRET, 'local smoke device', '', '']);

const server = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    try {
      const out = sandbox.doPost({ postData: { contents: body } });
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(out.text);
    } catch (err) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'ERROR', code: 500, message: String(err) }));
    }
  });
});
server.listen(PORT, () => {
  console.log(`[wave1-local] Code.gs live on http://localhost:${PORT} ` +
    `(token=${AUTH_TOKEN}, admin=${ADMIN_TOKEN ? 'set' : 'unset'}, device=${DEVICE_ID})`);
});
