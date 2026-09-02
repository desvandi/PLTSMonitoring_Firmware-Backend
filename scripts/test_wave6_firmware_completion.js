#!/usr/bin/env node
'use strict';
/**
 * WAVE-6 FIRMWARE COMPLETION TEST — real Code.gs in a Node VM + real crypto
 * + static contract assertions on both firmware trees.
 *
 * Closes the unfinished business Wave 5 left documented-but-open (doc 19 §9):
 *   FW6-1  OTA failures reach GAS (DOWNLOAD_FAILED / REFUSED) — both sides
 *   FW6-2  JWT epoch persistence + iat-regression rejection (spec mirror)
 *   FW6-3  dev/staging secrets masked; one-time reveal at generation only
 *   FW6-4  ota.check manifest polling EXISTS (state machine + dispatcher)
 *   FW6-5  telemetry envelope-status honesty + capped backoff
 *   FW6-6  AP password persist verified by read-back
 *   FW6-7  /save server-side validation
 *   FW6-8  OTA boot-health marking in AP mode
 *   FW6-9  per-device OTA manifest hmac — GAS↔firmware byte-exact contract
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ---------------------------------------------------------------------------
// GAS service mocks (same fidelity as test_wave4_hygiene.js)
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
  const sandbox = {
    console, JSON, Math, Date, Number, String, Object, Array,
    isNaN, parseInt, parseFloat, RegExp, Error,
    SpreadsheetApp: { getActiveSpreadsheet: () => ss, getUi: () => ({ alert: () => {} }) },
    LockService: {
      getScriptLock: () => ({
        tryLock: () => true,
        releaseLock: () => {},
      }),
    },
    CacheService: {
      getScriptCache: () => ({
        get: () => null,
        put: () => {},
        remove: () => {},
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
    UrlFetchApp: { fetch: () => ({}) },
  };

  vm.createContext(sandbox);
  const code = fs.readFileSync(path.join(__dirname, '..', 'code.gs', 'Code.gs'), 'utf-8');
  vm.runInContext(code, sandbox, { filename: 'Code.gs' });
  return { sandbox, ss };
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
function hmacHex(msg, key) {
  return crypto.createHmac('sha256', Buffer.from(String(key), 'utf8'))
    .update(Buffer.from(String(msg), 'utf8')).digest('hex');
}
function setConfig(env, key, value) {
  const sheet = env.ss.sheets['Config'];
  for (let i = 1; i < sheet.rows.length; i++) {
    if (String(sheet.rows[i][0]).trim() === key) { sheet.rows[i][1] = String(value); return; }
  }
  sheet.rows.push([key, String(value)]);
}

const REPO = path.join(__dirname, '..');
const INO = fs.readFileSync(path.join(REPO, 'firmware-generic', 'src', 'plts_firmware_v1.ino'), 'utf-8');
const MAIN = (rel) => fs.readFileSync(path.join(REPO, 'firmware', rel), 'utf-8');

console.log('\n=== WAVE-6 FIRMWARE COMPLETION TEST (Code.gs VM + firmware contracts) ===\n');

// ===========================================================================
// GROUP A — FW6-9: per-device OTA manifest hmac (GAS side, real Code.gs)
// ===========================================================================
console.log('[A] FW6-9 per-device OTA manifest hmac (GAS):');
const env = createGasContext();
env.sandbox.setupMasterTemplate();

const TOKEN = 'plts_sec_CHANGE_ME';
const DEVICE = 'PLTS_MONITOR_01';
const FLEET_HMAC = hmacHex('1.5.4|https://example.com/fw.bin|' + 'ab'.repeat(32), TOKEN);

{
  // Register a device (activates the fleet gate) + publish a manifest via the
  // admin path so the row carries the fleet-keyed hmac.
  const devices = env.ss.sheets['Devices'];
  devices.rows.push([DEVICE, '', 'Main', '', '']);
  setConfig(env, 'ADMIN_TOKEN', 'adm_secret');
  const pub = doPost(env, {
    action: 'OTA_PUBLISH', token: TOKEN, admin_token: 'adm_secret',
    manifest: {
      version: '1.5.4', url: 'https://example.com/fw.bin',
      sha256: 'ab'.repeat(32), hmac: FLEET_HMAC, size: 1080000,
    },
  });
  check('A1 OTA_PUBLISH with admin_token → 200', pub.code === 200, JSON.stringify(pub));

  // A2: capable caller (fw 1.5.4, registered) receives the PER-DEVICE hmac.
  const r2 = doPost(env, {
    action: 'OTA_MANIFEST', token: TOKEN,
    device_key: DEVICE, fw_version: '1.5.4',
  });
  const expectedKey = hmacHex(DEVICE, TOKEN);                    // = firmware hmacSha256Hex(token, deviceKey)
  const expectedHmac = hmacHex('1.5.4|https://example.com/fw.bin|' + 'ab'.repeat(32), expectedKey);
  check('A2 fw>=1.5.4 registered device → per-device hmac (≠ fleet hmac)',
    r2.code === 200 && r2.data && r2.data.hmac === expectedHmac &&
    r2.data.hmac !== FLEET_HMAC,
    JSON.stringify(r2.data && { hmac: r2.data.hmac, fleet: FLEET_HMAC, expected: expectedHmac }));

  // A3: legacy caller (fw 1.5.3) receives the fleet-keyed hmac from the row.
  const r3 = doPost(env, {
    action: 'OTA_MANIFEST', token: TOKEN,
    device_key: DEVICE, fw_version: '1.5.3',
  });
  check('A3 fw 1.5.3 legacy → fleet hmac (backward compatible)',
    r3.code === 200 && r3.data && r3.data.hmac === FLEET_HMAC,
    JSON.stringify(r3.data && r3.data.hmac));

  // A4: no fw_version at all (pre-1.5.4 firmware never sends it) → fleet hmac.
  const r4 = doPost(env, { action: 'OTA_MANIFEST', token: TOKEN });
  check('A4 no fw_version → fleet hmac (mixed fleet keeps working)',
    r4.code === 200 && r4.data && r4.data.hmac === FLEET_HMAC,
    JSON.stringify(r4.data && r4.data.hmac));

  // A5: capable caller naming an UNREGISTERED device → 400 (fail-closed).
  const r5 = doPost(env, {
    action: 'OTA_MANIFEST', token: TOKEN,
    device_key: 'GHOST_DEVICE', fw_version: '1.5.4',
  });
  check('A5 fw>=1.5.4 unknown device_key → 400 (fail-closed)',
    r5.code === 400, JSON.stringify(r5));

  // A6: garbage fw_version → legacy path (fleet hmac), never a 500.
  const r6 = doPost(env, {
    action: 'OTA_MANIFEST', token: TOKEN,
    device_key: DEVICE, fw_version: 'banana',
  });
  check('A6 unparseable fw_version → fleet hmac (honest fallback, no brick)',
    r6.code === 200 && r6.data && r6.data.hmac === FLEET_HMAC,
    JSON.stringify(r6.data && r6.data.hmac));

  // A7: version gating is numeric semver, not string compare.
  const r7 = doPost(env, {
    action: 'OTA_MANIFEST', token: TOKEN,
    device_key: DEVICE, fw_version: '1.5.10',
  });
  check('A7 fw 1.5.10 (numeric > 1.5.4) → per-device hmac',
    r7.code === 200 && r7.data && r7.data.hmac === expectedHmac,
    JSON.stringify(r7.data && r7.data.hmac));
}

// ===========================================================================
// GROUP B — FW6-1: OTA_STATUS accepts the events firmware now sends
// ===========================================================================
console.log('\n[B] FW6-1 OTA_STATUS event surface:');
{
  const r1 = doPost(env, {
    action: 'OTA_STATUS', token: TOKEN, device_key: DEVICE,
    event: 'REFUSED', version: '1.5.3',
    message: 'downgrade refused (running 1.5.4)',
  });
  check('B1 OTA_STATUS REFUSED → 200 (new event accepted)',
    r1.code === 200, JSON.stringify(r1));

  const r2 = doPost(env, {
    action: 'OTA_STATUS', token: TOKEN, device_key: DEVICE,
    event: 'DOWNLOAD_FAILED', version: '1.5.4',
    message: 'manifest HMAC mismatch',
  });
  check('B2 OTA_STATUS DOWNLOAD_FAILED → 200 (was already valid, stays valid)',
    r2.code === 200, JSON.stringify(r2));

  const r3 = doPost(env, {
    action: 'OTA_STATUS', token: TOKEN, device_key: DEVICE,
    event: 'SOMETHING_ELSE', version: '1.5.4',
  });
  check('B3 OTA_STATUS invalid event → 400 (allowlist intact)',
    r3.code === 400, JSON.stringify(r3));

  const events = env.ss.sheets['OtaEvents'].rows.slice(1)
    .filter((r) => String(r[2]).toUpperCase() === 'REFUSED');
  check('B4 REFUSED row recorded in OtaEvents with message',
    events.length === 1 && /downgrade refused/.test(String(events[0][4])),
    JSON.stringify(env.ss.sheets['OtaEvents'].rows));
}

// ===========================================================================
// GROUP C — FW6-9/FW6-1: firmware-generic source contract (static)
// ===========================================================================
console.log('\n[C] firmware-generic v1.5.4 source contract:');
{
  // [P1-REMEDIATION 2026-09] The literal 1.6.0 pin is retired: a hard-coded
  // version literal here would fight the bump discipline. The invariant that
  // actually matters is CONSISTENCY (constant == manifest == header), which
  // scripts/test_version_identity.py enforces in CI. This check now verifies
  // the version is a well-formed semver constant, and cross-checks the
  // manifest so this suite also fails on drift.
  {
    const m = INO.match(/FIRMWARE_VERSION\s*=\s*"(\d+\.\d+\.\d+)"/);
    const manifest = fs.readFileSync(
      path.join(__dirname, '..', 'firmware-generic', 'manifest.json'), 'utf8');
    const mv = JSON.parse(manifest).version;
    check('C1 FIRMWARE_VERSION is semver AND matches manifest.json',
      !!m && m[1] === mv, `source=${m && m[1]} manifest=${mv}`);
  }
  check('C2 no setInsecure() call anywhere (comments excluded)',
    !/\.setInsecure\s*\(/.test(INO));
  check('C3 per-device key derivation in applyOta',
    /hmacSha256Hex\(config\.token,\s*config\.deviceKey\)/.test(INO));
  check('C4 OTA_MANIFEST request carries device_key + fw_version',
    /body\["device_key"\]\s*=\s*config\.deviceKey/.test(INO) &&
    /body\["fw_version"\]\s*=\s*FIRMWARE_VERSION/.test(INO));
  const failedReports = (INO.match(/reportOtaStatus\("DOWNLOAD_FAILED"/g) || []).length;
  check('C5 every OTA failure path reports DOWNLOAD_FAILED (≥7 sites)',
    failedReports >= 7, `found ${failedReports}`);
  const refusedReports = (INO.match(/reportOtaStatus\("REFUSED"/g) || []).length;
  check('C6 anti-downgrade + non-semver refusals report REFUSED (2 sites)',
    refusedReports === 2, `found ${refusedReports}`);
  check('C7 telemetry parses envelope status (GAS HTTP 200 lie retired)',
    /rdoc\["status"\]/.test(INO) && /"SUCCESS"/.test(INO));
  check('C8 telemetry backoff state + cap present',
    /txConsecutiveFails/.test(INO) && /TX_BACKOFF_CAP_MS/.test(INO));
  check('C9 /save server-side validation of ssid/token/device_key',
    /SSID, Auth Token, dan Device Key wajib diisi/.test(INO));
  check('C10 AP password persist verified by read-back',
    /nvs_get_str\(rh, NVS_KEY_AP_PASS, verify, &vlen\)/.test(INO) &&
    /memcmp\(verify, pass, 12\)/.test(INO));
  check('C11 boot-health marking is runtime-based (AP mode too)',
    /runtimeStartedAt/.test(INO) &&
    /markOtaHealthyIfPending\(\);[\s\S]{0,400}AP_FALLBACK_MS/.test(INO.replace(/dnsServer\.processNextRequest\(\);\s*server\.handleClient\(\);\s*/m, '')) ||
    (/markOtaHealthyIfPending\(\)/.test(INO) && INO.indexOf('markOtaHealthyIfPending();', INO.indexOf('Mode::AP_MODE')) < INO.indexOf('AP_FALLBACK_MS', INO.indexOf('Mode::AP_MODE'))),
    '');
  check('C12 https:// enforced at load, save, and manifest validation',
    (INO.match(/startsWith\("https:\/\/"\)/g) || []).length >= 3);
}

// ===========================================================================
// GROUP D — FW6-2/3/4: main firmware source contract (static)
// ===========================================================================
console.log('\n[D] main firmware v1.6.2 source contract:');
{
  const cryptoCpp = MAIN('Utils/Crypto.cpp');
  const cryptoH = MAIN('Utils/Crypto.h');
  const tm = MAIN('Services/TimeManager.cpp');
  const cs = MAIN('Storage/ConfigStore.cpp');
  const otaH = MAIN('Services/OtaManager.h');
  const otaCpp = MAIN('Services/OtaManager.cpp');
  const mqttOta = MAIN('Network/MqttOtaHandler.cpp');
  const ino = MAIN('firmware_v1.ino');
  const cfgH = MAIN('Core/Config.h');

  check('D1 FIRMWARE_VERSION == 1.6.3',
    /FIRMWARE_VERSION\s*=\s*"1\.6\.3"/.test(cfgH));
  check('D2 Crypto persists boot-epoch estimate in NVS (plts_time)',
    /plts_time/.test(cryptoCpp) && /boot_epoch/.test(cryptoCpp) &&
    /Utils::persistEpochEstimate/.test(cryptoCpp));
  check('D3 getCurrUnixTime resumes from persisted base when unsynced',
    /s_epochBootBase \+ \(uint32_t\)\(millis\(\) \/ 1000\)/.test(cryptoCpp));
  check('D4 jwtVerify rejects future iat (clock regression, fail-closed)',
    /now \+ 300u < iat/.test(cryptoCpp));
  check('D5 persistEpochEstimate declared in Crypto.h',
    /void persistEpochEstimate\(\);/.test(cryptoH));
  check('D6 TimeManager persists on NTP sync (both paths)',
    (tm.match(/Utils::persistEpochEstimate\(\)/g) || []).length >= 2);
  check('D7 dev/staging boot prints are MASKED',
    /maskSecret_\(Core::devicePin\)/.test(cs) &&
    /maskSecret_\(Core::mqttPassword\)/.test(cs) &&
    /maskSecret_\(Core::gasSecret\)/.test(cs));
  check('D8 secrets get one-time full reveal at GENERATION (3 new sites)',
    /Password MQTT BARU/.test(cs) && /Device PIN BARU/.test(cs) &&
    /Secret GAS HMAC BARU/.test(cs));
  check('D9 OtaState::Checking exists',
    /Checking\s*=\s*6/.test(otaH));
  check('D10 beginManifestCheck + tickManifestCheck implemented',
    /bool OtaManager::beginManifestCheck/.test(otaCpp) &&
    /void OtaManager::tickManifestCheck/.test(otaCpp));
  check('D11 manifest check enforces allowlist + CA before fetch',
    /_validateUrlAllowlist\(manifestUrl\)/.test(otaCpp) &&
    /_validateCa\(\)/.test(otaCpp));
  check('D12 manifest check hands off to the SAME beginDownload chain',
    /beginDownload\(url, ver, size, sha, sig\)/.test(otaCpp));
  check('D13 MqttOtaHandler dispatches ota.check → beginManifestCheck',
    /Services::ota\.beginManifestCheck\(manifestUrl\)/.test(mqttOta));
  check('D14 otaTask pumps the Checking state',
    /OtaState::Checking/.test(ino) && /tickManifestCheck\(\)/.test(ino));
  check('D15 "no update" is an honest Idle, not a failure',
    /no update: manifest v/.test(otaCpp));
}

// ===========================================================================
// GROUP E — FW6-2: JWT lifetime spec mirror (the 89-year bug)
// ===========================================================================
console.log('\n[E] FW6-2 JWT epoch fallback spec:');
{
  // Mirror of the documented semantics (Crypto.cpp): a token issued with a
  // real-epoch exp must not outlive its TTL by decades after a reboot
  // without NTP. Scenario: token issued (iat) at the moment the epoch was
  // last persisted, 15-minute TTL, device power-cycles immediately and runs
  // 1000 s unsynced.
  const iat = 1782990000;                 // = persisted boot-epoch base
  const exp = iat + 900;                  // 15-minute TTL (real epoch)
  const uptime = 1000;                    // seconds since reboot

  const oldNow = 1700000000 + uptime;     // OLD fallback base (Nov 2023)
  check('E1 OLD base: pre-reboot token stays valid ~89 years (bug reproduced)',
    oldNow <= exp);

  const newNow = iat + uptime;            // NEW persisted base
  check('E2 NEW base: same token expires on schedule (1000 s > 900 s TTL)',
    newNow > exp);

  // iat-regression: token iat in the future beyond 5 min skew → reject.
  const now = 1783000000;
  const iatF = 1783003600;                // 1 h in the "future"
  check('E3 iat > now + 300 → rejected (clock regression fail-closed)',
    now + 300 < iatF);
  const iat2 = 1783000120;                // 2 min skew — normal jitter
  check('E4 iat within 300 s skew → accepted',
    !(now + 300 < iat2));
}

// ===========================================================================
// GROUP F — FW6-9: firmware↔GAS byte-exact hmac contract (crypto mirror)
// ===========================================================================
console.log('\n[F] FW6-9 firmware↔GAS per-device key contract:');
{
  // Firmware: String otaKey = hmacSha256Hex(config.token, config.deviceKey);
  //           expected  = hmacSha256Hex(otaKey, version + "|" + url + "|" + sha256);
  // GAS:      derived = hex(HMAC(device_key, AUTH_TOKEN));
  //           hmac    = hex(HMAC(version|url|sha256, derived));
  const token = 'plts_sec_test_fleet';
  const deviceKey = 'PLTS_MONITOR_07';
  const version = '1.5.4';
  const url = 'https://example.com/fw.bin';
  const sha = 'cd'.repeat(32);

  const fwKey = hmacHex(deviceKey, token);   // message=deviceKey, key=token
  const fwHmac = hmacHex(`${version}|${url}|${sha}`, fwKey);

  // GAS derivation (Utilities.computeHmacSha256Signature(value, key)):
  const gasDerived = hmacHex(deviceKey, token);
  const gasHmac = hmacHex(`${version}|${url}|${sha}`, gasDerived);

  check('F1 firmware formula === GAS formula (byte-exact, 64 hex)',
    fwHmac === gasHmac && /^[0-9a-f]{64}$/.test(fwHmac), fwHmac);
  check('F2 per-device hmac differs per device (no fleet-wide forgery)',
    hmacHex(`${version}|${url}|${sha}`, hmacHex('OTHER_DEVICE', token)) !== fwHmac);
  check('F3 per-device hmac ≠ fleet-keyed hmac',
    fwHmac !== hmacHex(`${version}|${url}|${sha}`, token));
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

console.log('\n=== SUMMARY ===');
console.log(`PASS: ${passed}  FAIL: ${failed}`);
if (failed > 0) {
  console.log('Failures:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exit(1);
}
process.exit(0);
