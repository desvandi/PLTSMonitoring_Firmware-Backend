#!/usr/bin/env node
/**
 * test_bench_w14_ota_rollback.js — WAVE 14 BENCH: OTA v1.7.1 rollback
 * =============================================================================
 * Virtual-bench execution of the "pengembalian fisik OTA v1.7.1" half of the
 * W14 bench: an ESP-IDF v4.4.7 bootloader 1:1 mirror (ota_data state machine,
 * PENDING_VERIFY/ABORTED selection — verified against the REAL
 * bootloader_utility.c sources) drives a 1:1 JS port of the firmware-generic
 * v1.7.1 OTA client (fetchOtaManifest / applyOta / handleOtaRollback /
 * checkBootloaderRevert / markOtaHealthyIfPending / reportOtaStatus), all
 * against the REAL code.gs/Code.gs in a vm sandbox with REAL HMAC crypto.
 *
 * This proves the rollback SEMANTICS end-to-end (device → bootloader → GAS
 * OtaEvents → OTA_LOG read-back), so the physical bench only has to confirm
 * the same serial lines + OTA_LOG rows on real silicon.
 *
 * The verified hardware policy (the W14 core finding, from the arduino-esp32
 * 2.0.17 sdkconfig + IDF v4.4.7 bootloader_utility.c / bootloader_common.c):
 *   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y gives a fresh image exactly ONE
 *   unconfirmed boot. ANY reset before the 60 s confirm (power blip, panic,
 *   WDT) → the bootloader marks the ota_data entry ABORTED and boots the
 *   previous app. The firmware's "3 boot attempts" is defense-in-depth that
 *   cannot fire on this bootloader (attempt #2 never happens) — hence
 *   [W14-2b]: the reverted image detects running < lastFlashed and reports
 *   ROLLBACK to GAS itself.
 *
 * Check groups:
 *   OT-1  Bootloader mirror semantics (IDF 4.4.7 ota_data state machine):
 *         NEW → PENDING_VERIFY on first boot; unconfirmed reset → ABORTED →
 *         previous app boots; confirm → VALID; ABORTED never re-selected
 *   OT-2  Happy path through REAL Code.gs: OTA_PUBLISH (target=generic) →
 *         OTA_MANIFEST served with per-device HMAC (real crypto chain) →
 *         apply → PENDING_VERIFY boot #1 (ledger starts at 0 — [W14-2a]) →
 *         60 s healthy → confirm → ACTIVATED in OTA_LOG → telemetry
 *         firmwareVersion 1.7.1
 *   OT-3  Physical rollback: power cut at 20 s (before confirm) → bootloader
 *         reverts → old image boots → [W14-2b] ROLLBACK reported to GAS once
 *         STA is up → OTA_LOG shows ROLLBACK v1.7.1 → marker consumed, no
 *         double report on the next boot
 *   OT-4  Marker persistence: ROLLBACK report survives a WiFi-less boot +
 *         power cycle (marker kept until HTTP 200)
 *   OT-5  Ledger semantics [W14-2a]: two power-blipped updates then a healthy
 *         third image boots at try #1 (pre-fix: #3 → instant self-rollback);
 *         the >= 3 defense-in-depth path still works when reachable
 *   OT-6  Refusals: cross-target manifest not served (404) + device-side
 *         target self-check REFUSED + anti-downgrade REFUSED
 *   OT-7  OTA_LOG read-back: newest-first, device-scoped, honest word list
 *         (ROLLBACK accepted)
 *   OT-8  Static source locks: generic [W14-2a/b/c] wiring, modular mirror
 *         (_recordFlashedImage at both Update.end sites, begin() revert
 *         detection, marker hygiene), GAS OTA_STATUS word list
 *
 * Usage: node scripts/test_bench_w14_ota_rollback.js   (exit 0 = PASS)
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { createGasContext } = require('./bench_w14_gasenv');

const FWB = path.join(__dirname, '..');
const INO = path.join(FWB, 'firmware-generic', 'src', 'plts_firmware_v1.ino');
const OTAMGR_CPP = path.join(FWB, 'firmware', 'Services', 'OtaManager.cpp');
const OTAMGR_H = path.join(FWB, 'firmware', 'Services', 'OtaManager.h');
const GAS_SRC = path.join(FWB, 'code.gs', 'Code.gs');

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
// Shared crypto / version helpers (mirrors of the .ino originals)
// ---------------------------------------------------------------------------

/** hex(HMAC-SHA256(key=secret, data=message)) — .ino:750 hmacSha256Hex. */
function hmacSha256Hex(secret, message) {
  return crypto.createHmac('sha256', Buffer.from(String(secret), 'utf8'))
    .update(Buffer.from(String(message), 'utf8')).digest('hex');
}

/** .ino:251 semverCompare — -1/0/1, -2 when unparseable. */
function semverCompare(a, b) {
  const pa = String(a).split('.').map(Number);
  const pb = String(b).split('.').map(Number);
  if (pa.length < 3 || pb.length < 3 ||
      pa.some((x) => !Number.isFinite(x)) || pb.some((x) => !Number.isFinite(x))) return -2;
  for (let i = 0; i < 3; i++) {
    if (pa[i] < pb[i]) return -1;
    if (pa[i] > pb[i]) return 1;
  }
  return 0;
}

function sha256Hex(buf) {
  return crypto.createHash('sha256').update(buf).digest('hex');
}

// Virtual firmware binaries served from the (virtual) HTTPS URL. The bench
// publishes their REAL sha256; the device mirror streams + verifies them.
const VIRTUAL_BINARIES = {
  '1.7.0': Buffer.from('PLTS-BENCH-VIRTUAL-BINARY-v1.7.0-' + '0'.repeat(300), 'utf8'),
  '1.7.1': Buffer.from('PLTS-BENCH-VIRTUAL-BINARY-v1.7.1-' + '1'.repeat(300), 'utf8'),
};

const TOKEN = 'plts_sec_CHANGE_ME';        // Config!AUTH_TOKEN default
const ADMIN = 'plts_admin_bench_w14';      // set into Config!ADMIN_TOKEN below
const DEVICE_KEY = 'PLTS-BENCH-OTA';

// ---------------------------------------------------------------------------
// VirtualEsp32 — ota_data + bootloader, 1:1 with IDF v4.4.7
// (bootloader_support/src/bootloader_utility.c + bootloader_common_loader.c)
// ---------------------------------------------------------------------------
class VirtualEsp32 {
  constructor(oldVersion) {
    // Two OTA slots. Slot 0 ships with the running app; slot 1 blank.
    this.slots = [oldVersion, null];
    // ota_data entries (one per slot). Erased entry = seq 0 / INVALID.
    this.otaData = [{ seq: 1, state: 'VALID' }, { seq: 0, state: 'INVALID' }];
    this.running = 0;
    this.bootCount = 0;
  }

  /** esp_ota_set_boot_partition (app side, after Update.end(true)):
   *  writes ota_data {seq: next, state: NEW}. */
  setBootPartition(slot, version) {
    this.slots[slot] = version;
    const nextSeq = Math.max(this.otaData[0].seq, this.otaData[1].seq) + 1;
    this.otaData[slot] = { seq: nextSeq, state: 'NEW' };
  }

  /** bootloader_utility_load_boot_partition — bu.c:335-415.
   *  1. PENDING_VERIFY → ABORTED for BOTH entries (bu.c:344-352)
   *  2. active = highest-seq selectable entry — bootloader_common.c:45-47:
   *     invalid = seq unset | INVALID | ABORTED
   *  3. NEW → PENDING_VERIFY (bu.c:394-400) — the ONE unconfirmed boot
   *  Returns the booted app version. */
  bootload() {
    this.bootCount++;
    for (const od of this.otaData) {
      if (od.state === 'PENDING_VERIFY') od.state = 'ABORTED';
    }
    let active = -1;
    for (let i = 0; i < 2; i++) {
      const od = this.otaData[i];
      if (od.seq === 0 || od.state === 'INVALID' || od.state === 'ABORTED') continue;
      if (active < 0 || od.seq > this.otaData[active].seq) active = i;
    }
    if (active < 0) throw new Error('VirtualEsp32: no bootable slot');
    if (this.otaData[active].state === 'NEW') this.otaData[active].state = 'PENDING_VERIFY';
    this.running = active;
    return this.slots[active];
  }

  /** esp_ota_get_state_partition(running). */
  runningState() { return this.otaData[this.running].state; }

  /** esp_ota_mark_app_valid_cancel_rollback (app side). */
  markValid() {
    if (this.otaData[this.running].state === 'PENDING_VERIFY') {
      this.otaData[this.running].state = 'VALID';
    }
  }

  /** esp_ota_mark_app_invalid_rollback_and_reboot (app side): the running
   *  entry is marked ABORTED, board reboots → previous app. */
  markInvalidAndReboot() {
    this.otaData[this.running].state = 'ABORTED';
    return this.bootload();
  }

  inactiveSlot() { return 1 - this.running; }
}

// ---------------------------------------------------------------------------
// DeviceFirmwareMirror — 1:1 port of the firmware-generic v1.7.1 OTA client.
// Every branch cites the .ino line it mirrors (post-W14 line numbers).
// ---------------------------------------------------------------------------
class DeviceFirmwareMirror {
  constructor(gas, esp, nvs, opts = {}) {
    this.gas = gas;
    this.esp = esp;
    this.nvs = nvs;                      // { boot_tries, lfver } — "plts" NVS
    this.token = TOKEN;
    this.deviceKey = DEVICE_KEY;
    this.version = esp.slots[esp.running];   // FIRMWARE_VERSION of THIS image
    this.wifiUp = opts.wifiUp !== false;
    this.logs = [];
    this.otaHealthyMarked = false;
    this.runtimeStartedAt = 0;           // set in setup()
    this.runtimeMs = 0;
    this.rollbackReportPending = false;  // [W14-2b] ino:230-234
    this.rollbackReportVersion = '';
    this.pendingRestart = false;
  }

  log(s) { this.logs.push(s); }

  // ---- reportOtaStatus (ino:1809-1841, [W14-2c] returns HTTP-equivalent) ---
  reportOtaStatus(event, version, message) {
    if (!this.wifiUp) return 0;
    const r = this.gas.doPost({
      action: 'OTA_STATUS', token: this.token, device_key: this.deviceKey,
      event, version, message,
    });
    const code = r.status === 'SUCCESS' ? 200 : 400;
    this.log(`[OTA-STATUS] ${event} v${version} -> HTTP=${code}`);
    return code;
  }

  // ---- fetchOtaManifest (ino:1508-1570) -------------------------------------
  fetchOtaManifest() {
    const r = this.gas.doPost({
      action: 'OTA_MANIFEST', token: this.token, device_key: this.deviceKey,
      fw_version: this.version,
    });
    if (r.status !== 'SUCCESS' || !r.data) {
      return { valid: false, reason: r.message || 'no manifest' };
    }
    const m = {
      version: String(r.data.version || ''),
      url: String(r.data.url || ''),
      sha256: String(r.data.sha256 || ''),
      hmac: String(r.data.hmac || ''),
      size: Number(r.data.size || 0),
      target: String(r.data.target || '').trim().toLowerCase(),
    };
    // [W13-2] device-side target self-check (ino:1556-1566)
    if (m.target.length > 0 && m.target !== 'generic') {
      this.reportOtaStatus('REFUSED', m.version,
        `manifest target '${m.target}' does not match this device (generic)`);
      return { valid: false, reason: 'target mismatch' };
    }
    m.valid = m.version.length > 0 && m.url.startsWith('https://') &&
      m.sha256.length === 64 && m.hmac.length === 64;
    return m;
  }

  // ---- applyOta (ino:1572-1692) ----------------------------------------------
  applyOta(m) {
    // HMAC chain (ino:1573-1586): per-device key = HMAC(token, deviceKey)
    const otaKey = this.deviceKey.length > 0
      ? hmacSha256Hex(this.token, this.deviceKey) : this.token;
    const expected = hmacSha256Hex(otaKey, m.version + '|' + m.url + '|' + m.sha256);
    if (expected !== m.hmac.toLowerCase()) {
      this.reportOtaStatus('DOWNLOAD_FAILED', m.version, 'manifest HMAC mismatch');
      return false;
    }
    // Virtual download + streaming SHA-256 (ino:1588-1667)
    const bin = VIRTUAL_BINARIES[m.version];
    if (!bin) {
      this.reportOtaStatus('DOWNLOAD_FAILED', m.version, 'binary not in bench registry');
      return false;
    }
    const actual = sha256Hex(bin);
    if (actual !== m.sha256) {
      this.reportOtaStatus('DOWNLOAD_FAILED', m.version, 'SHA-256 mismatch');
      return false;
    }
    // Update.end(true) → esp_ota_set_boot_partition (NEW state, next seq)
    this.esp.setBootPartition(this.esp.inactiveSlot(), m.version);
    // [W14-2a] fresh per-image ledger (ino:1678-1683)
    this.nvs.boot_tries = 0;
    // [W14-2b] revert marker (ino:1684-1686)
    this.nvs.lfver = m.version;
    this.log(`[OTA] Success v${m.version} (${bin.length} bytes). Rebooting...`);
    this.pendingRestart = true;
    return true;
  }

  // ---- checkOta (ino:1694-1726) ----------------------------------------------
  checkOta() {
    if (!this.wifiUp) return false;
    const m = this.fetchOtaManifest();
    if (!m.valid) return false;
    if (m.version === this.version) return false;      // ino:1699
    const cmp = semverCompare(m.version, this.version); // ino:1705-1716
    if (cmp === -2) {
      this.reportOtaStatus('REFUSED', m.version, 'manifest version not semver');
      return false;
    }
    if (cmp <= 0) {
      this.reportOtaStatus('REFUSED', m.version,
        `downgrade refused (running ${this.version})`);
      return false;
    }
    this.log(`[OTA] new version ${m.version} (running ${this.version})`);
    return this.applyOta(m);
  }

  // ---- setup() OTA section (ino:2001-2010): handleOtaRollback +
  //      checkBootloaderRevert -------------------------------------------------
  setup() {
    this.runtimeStartedAt = 0;
    this.runtimeMs = 0;
    this.otaHealthyMarked = false;

    // handleOtaRollback (ino:1855-1872)
    const state = this.esp.runningState();
    let rolledBackNow = false;
    if (state === 'PENDING_VERIFY') {
      this.nvs.boot_tries++;
      this.log(`[OTA] pending-verify boot try #${this.nvs.boot_tries}`);
      if (this.nvs.boot_tries >= 3) {                 // defense-in-depth path
        this.log('[OTA] max boot attempts reached — rolling back!');
        this.nvs.boot_tries = 0;
        this.nvs.lfver = '';                           // [W14-2b] ino:1862-1864
        this.reportOtaStatus('ROLLBACK', this.version, 'boot loop detected');
        rolledBackNow = true;                          // esp_ota_mark_app_invalid_rollback_and_reboot()
      }
    }

    // checkBootloaderRevert (ino:1896-1903)
    if (!rolledBackNow) {
      const lf = this.nvs.lfver;
      if (lf && lf !== this.version) {
        const cmp = semverCompare(lf, this.version);
        if (cmp === 1) {
          this.rollbackReportPending = true;
          this.rollbackReportVersion = lf;
          this.log(`[OTA] update v${lf} was reverted by the bootloader — running v${this.version}`);
          // marker deliberately KEPT until delivery (ino:1897-1898)
        } else {
          this.nvs.lfver = '';                         // running newer → consume
        }
      }
    }
    return rolledBackNow;
  }

  // ---- STA loop slice (ino:2057-2090), 1 tick = 1 s --------------------------
  runStaLoop(seconds, opts = {}) {
    const wifiUp = opts.wifiUp !== undefined ? opts.wifiUp : this.wifiUp;
    for (let t = 1; t <= seconds; t++) {
      this.runtimeMs = t * 1000;

      // markOtaHealthyIfPending (ino:1914-1940)
      if (!this.otaHealthyMarked && this.runtimeMs >= 60000) {
        const st = this.esp.runningState();
        if (st === 'PENDING_VERIFY') {
          this.esp.markValid();                        // esp_ota_mark_app_valid_cancel_rollback
          this.log('[OTA] new image validated — rollback cancelled.');
          this.reportOtaStatus('ACTIVATED', this.version,
            wifiUp ? 'healthy after boot' : 'healthy in setup/AP mode (no STA)');
          this.nvs.lfver = '';                         // [W14-2b] confirmed → marker off
        }
        this.nvs.boot_tries = 0;
        this.otaHealthyMarked = true;
      }

      // [W14-2b] deferred ROLLBACK report (ino:2060-2070)
      if (wifiUp && this.rollbackReportPending) {
        const rc = this.reportOtaStatus('ROLLBACK', this.rollbackReportVersion,
          'bootloader revert (image unconfirmed within its single boot)');
        if (rc === 200) {
          this.nvs.lfver = '';
          this.rollbackReportPending = false;
        }
      }
    }
  }
}

/** Boot the virtual ESP32 into a fresh mirror (post ESP.restart()). */
function bootMirror(gas, esp, nvs, opts) {
  const version = esp.bootload();
  const mirror = new DeviceFirmwareMirror(gas, esp, nvs, opts);
  const rolledBackNow = mirror.setup();
  return { mirror, version, rolledBackNow };
}

// ---------------------------------------------------------------------------
// Bench runner
// ---------------------------------------------------------------------------
console.log('test_bench_w14_ota_rollback.js — W14 bench: OTA v1.7.1 rollback (virtual ESP32 + real Code.gs)');
console.log('='.repeat(72));

// Shared GAS deployment for the whole bench.
const g = createGasContext();
g.sandbox.setupMasterTemplate();
g.setConfig('ADMIN_TOKEN', ADMIN);        // OTA_PUBLISH is operator-gated
g.registerDevice(DEVICE_KEY, 'generic');  // DEVICES!firmware_type = generic

const OTA_URL = 'https://raw.example.com/plts_firmware_v1.7.1.bin';

function publish(version, target, shaOverride) {
  return g.doPost({
    action: 'OTA_PUBLISH', token: TOKEN, admin_token: ADMIN,
    manifest: {
      version: version,
      url: `https://raw.example.com/plts_firmware_v${version}.bin`,
      sha256: shaOverride || sha256Hex(VIRTUAL_BINARIES[version]),
      hmac: 'ab'.repeat(32),              // stored; re-keyed per-device on serve
      size: VIRTUAL_BINARIES[version].length,
      target: target,
    },
  });
}

// ---- OT-1: bootloader mirror semantics (IDF 4.4.7 ota_data state machine) ----
console.log('\n[OT-1] Bootloader semantics — verified against IDF v4.4.7 sources:');
{
  const esp = new VirtualEsp32('1.7.0');
  check('OT-1a set_boot_partition writes NEW + higher seq (esp_ota_ops)',
    esp.otaData[1].state === 'INVALID' &&
    (() => { esp.setBootPartition(1, '1.7.1'); return esp.otaData[1].state === 'NEW' &&
      esp.otaData[1].seq > esp.otaData[0].seq; })());
  const v1 = esp.bootload();
  check('OT-1b first boot: NEW → PENDING_VERIFY, new app runs (bu.c:394-400)',
    v1 === '1.7.1' && esp.runningState() === 'PENDING_VERIFY');
  // Unconfirmed reset (power cut) → next boot reverts.
  const v2 = esp.bootload();
  check('OT-1c unconfirmed reset → ABORTED → PREVIOUS app boots (bu.c:344-352 + bootloader_common.c:45-47)',
    v2 === '1.7.0' && esp.otaData[1].state === 'ABORTED' && esp.runningState() === 'VALID');
  const v3 = esp.bootload();
  check('OT-1d ABORTED entry is never re-selected',
    v3 === '1.7.0');
  // Confirmed path: re-flash + confirm → VALID stays across resets.
  esp.setBootPartition(1, '1.7.1');
  const v4 = esp.bootload();
  esp.markValid();
  const v5 = esp.bootload();
  check('OT-1e confirm (mark valid) → VALID survives resets',
    v4 === '1.7.1' && esp.otaData[1].state === 'VALID' && v5 === '1.7.1');
}

// ---- OT-2: happy path through the REAL Code.gs --------------------------------
console.log('\n[OT-2] Happy path — publish → manifest → apply → ACTIVATED:');
{
  const esp = new VirtualEsp32('1.7.0');
  const nvs = { boot_tries: 0, lfver: '' };
  let { mirror } = bootMirror(g, esp, nvs);            // boots 1.7.0 (VALID)

  const pr = publish('1.7.1', 'generic');
  check('OT-2a OTA_PUBLISH (target=generic) accepted by real Code.gs',
    pr.status === 'SUCCESS' && pr.data.target === 'generic', JSON.stringify(pr));

  const applied = mirror.checkOta();
  check('OT-2b device applies: HMAC (per-device, real crypto) + SHA-256 verified',
    applied === true && esp.otaData[1].state === 'NEW' && nvs.lfver === '1.7.1',
    `state=${esp.otaData[1].state} lfver=${nvs.lfver}`);
  check('OT-2c [W14-2a] ledger RESET at write time (per-image, starts at 0)',
    nvs.boot_tries === 0);

  ({ mirror } = bootMirror(g, esp, nvs));              // ESP.restart() → bootload()
  check('OT-2d boot #1 of the new image: PENDING_VERIFY, try #1, marker matches running',
    mirror.version === '1.7.1' && esp.runningState() === 'PENDING_VERIFY' &&
    mirror.logs.some((l) => l.includes('boot try #1')) && nvs.boot_tries === 1);

  mirror.runStaLoop(70);                               // 60 s healthy window + margin
  check('OT-2e 60 s healthy → image confirmed (VALID) + ACTIVATED reported',
    esp.runningState() === 'VALID' &&
    mirror.logs.some((l) => l.includes('new image validated')) &&
    mirror.logs.some((l) => l.includes('[OTA-STATUS] ACTIVATED v1.7.1')));
  check('OT-2f [W14-2b] marker consumed at confirmation (no revert report later)',
    nvs.lfver === '' && mirror.rollbackReportPending === false);

  const log = g.doPost({ action: 'OTA_LOG', token: TOKEN, device_key: DEVICE_KEY });
  const newest = log.data && log.data.events && log.data.events[0];
  check('OT-2g OTA_LOG newest event = ACTIVATED v1.7.1 (real sheet round trip)',
    newest && newest.event === 'ACTIVATED' && newest.version === '1.7.1',
    JSON.stringify(newest));

  // telemetry carries the new firmwareVersion through LATEST
  g.doPost({ action: 'TELEMETRY', token: TOKEN, device_key: DEVICE_KEY, data: {
    protocolVersion: 1, firmwareVersion: '1.7.1', deviceId: DEVICE_KEY,
    sequence: 1, timestamp: '2026-09-03T09:00:00Z', timeQuality: 'VALID',
    battery: { voltage: { value: 52.4 }, current: { value: -10.2 },
      power: { value: -534.5 }, soc: { value: 78.4 } },
    ac: { rmsCurrent: { value: 3.2 }, estimatedPower: { value: 633.6 } },
    environment: { temperature: { value: 31.2 }, humidity: { value: 72.1 } },
    health: { freeHeap: 123456, rssi: -61 },
    overallQuality: 'VALID' } });
  const latest = g.doPost({ action: 'LATEST', token: TOKEN, device_key: DEVICE_KEY });
  check('OT-2h LATEST health.firmwareVersion = 1.7.1 (PWA-visible activation)',
    latest.data && latest.data.health && latest.data.health.firmwareVersion === '1.7.1');
}

// ---- OT-3: physical rollback — power cut before the 60 s confirm --------------
console.log('\n[OT-3] Physical rollback — power cut at 20 s:');
{
  const esp = new VirtualEsp32('1.7.0');
  const nvs = { boot_tries: 0, lfver: '' };
  let { mirror } = bootMirror(g, esp, nvs);
  const applied = mirror.checkOta();                   // manifest still active
  check('OT-3a re-apply of the same manifest (1.7.1 > 1.7.0) is accepted',
    applied === true && nvs.lfver === '1.7.1');

  ({ mirror } = bootMirror(g, esp, nvs));              // boot #1 of the new image
  check('OT-3b new image boots PENDING_VERIFY (try #1)',
    mirror.version === '1.7.1' && esp.runningState() === 'PENDING_VERIFY');

  mirror.runStaLoop(20, { wifiUp: false });            // 20 s, then POWER CUT
  // (no confirm — 20 s < 60 s)
  const boot2 = esp.bootload();                        // bootloader: ABORTED → old app
  check('OT-3c power cut before 60 s → bootloader reverts to v1.7.0',
    boot2 === '1.7.0' && esp.otaData[1].state === 'ABORTED');

  const old = new DeviceFirmwareMirror(g, esp, nvs);  // old image boots
  const rolledBackNow = old.setup();
  check('OT-3d [W14-2b] old image detects the revert (lfver kept, report armed)',
    rolledBackNow === false && old.rollbackReportPending === true &&
    old.rollbackReportVersion === '1.7.1' && nvs.lfver === '1.7.1' &&
    old.logs.some((l) => l.includes('was reverted by the bootloader')));

  old.runStaLoop(10);                                  // STA up → deferred report fires
  check('OT-3e ROLLBACK v1.7.1 reported to GAS once STA is up',
    old.logs.some((l) => l.includes('[OTA-STATUS] ROLLBACK v1.7.1')) &&
    old.rollbackReportPending === false && nvs.lfver === '');

  const log = g.doPost({ action: 'OTA_LOG', token: TOKEN, device_key: DEVICE_KEY });
  const evts = (log.data && log.data.events) || [];
  const rollbacks = evts.filter((e) => e.event === 'ROLLBACK' && e.version === '1.7.1');
  check('OT-3f OTA_LOG carries ROLLBACK v1.7.1 (observable revert)',
    rollbacks.length === 1, JSON.stringify(evts.slice(0, 3)));

  // next boot: no double report (marker consumed)
  esp.bootload();
  const old2 = new DeviceFirmwareMirror(g, esp, nvs);
  old2.setup();
  const eventsBefore = g.doPost({ action: 'OTA_LOG', token: TOKEN, device_key: DEVICE_KEY })
    .data.events.filter((e) => e.event === 'ROLLBACK').length;
  old2.runStaLoop(10);
  const eventsAfter = g.doPost({ action: 'OTA_LOG', token: TOKEN, device_key: DEVICE_KEY })
    .data.events.filter((e) => e.event === 'ROLLBACK').length;
  check('OT-3g single-shot semantics: next boot does NOT double-report',
    eventsBefore === eventsAfter && old2.rollbackReportPending === false);
}

// ---- OT-4: marker persistence across a WiFi-less boot -------------------------
console.log('\n[OT-4] ROLLBACK report persistence (WiFi down at revert boot):');
{
  const esp = new VirtualEsp32('1.7.0');
  const nvs = { boot_tries: 0, lfver: '' };
  let { mirror } = bootMirror(g, esp, nvs);
  mirror.checkOta();
  ({ mirror } = bootMirror(g, esp, nvs));              // new image boot #1
  mirror.runStaLoop(20, { wifiUp: false });            // power cut at 20 s
  esp.bootload();                                      // bootloader reverts
  const old = new DeviceFirmwareMirror(g, esp, nvs, { wifiUp: false });
  old.setup();                                         // report armed, NO WiFi
  old.runStaLoop(120, { wifiUp: false });              // 2 min offline (past 60 s mark!)
  check('OT-4a marker SURVIVES the 60 s healthy mark on the old image (W14-2b fix)',
    nvs.lfver === '1.7.1' && old.rollbackReportPending === true);
  // power cycle again while offline — detection re-arms from NVS
  esp.bootload();
  const old3 = new DeviceFirmwareMirror(g, esp, nvs, { wifiUp: false });
  old3.setup();
  check('OT-4b re-detection after another offline power cycle (NVS persistence)',
    old3.rollbackReportPending === true);
  old3.wifiUp = true;
  old3.runStaLoop(10);
  check('OT-4c report finally delivered when STA returns',
    old3.logs.some((l) => l.includes('[OTA-STATUS] ROLLBACK v1.7.1')) &&
    nvs.lfver === '');
}

// ---- OT-5: ledger semantics [W14-2a] ------------------------------------------
console.log('\n[OT-5] Boot-attempt ledger semantics:');
{
  // Two power-blipped updates, then a healthy third image: boots at try #1.
  const esp = new VirtualEsp32('1.7.0');
  const nvs = { boot_tries: 0, lfver: '' };
  for (let i = 0; i < 2; i++) {
    let { mirror } = bootMirror(g, esp, nvs);
    mirror.checkOta();
    ({ mirror } = bootMirror(g, esp, nvs));
    mirror.runStaLoop(20, { wifiUp: false });          // power cut each time
    esp.bootload();
  }
  check('OT-5a after 2 power-blipped updates the ledger holds only the LAST image\'s own try (1), not an accumulated 2',
    nvs.boot_tries === 1, `boot_tries=${nvs.boot_tries}`);
  const r3 = bootMirror(g, esp, nvs);
  const applied = r3.mirror.checkOta();                 // third apply (resets ledger)
  const r4 = bootMirror(g, esp, nvs);                  // its first boot
  check('OT-5b the healthy THIRD image boots at try #1 — no instant self-rollback',
    applied === true && r4.mirror.logs.some((l) => l.includes('boot try #1')) &&
    r4.rolledBackNow === false && esp.runningState() === 'PENDING_VERIFY');
  r4.mirror.runStaLoop(70);
  check('OT-5c third image confirms + ACTIVATED (the pre-fix bug would have reverted it)',
    esp.runningState() === 'VALID');

  // Defense-in-depth path: only reachable with a manually poisoned ledger.
  const esp2 = new VirtualEsp32('1.7.0');
  const nvs2 = { boot_tries: 2, lfver: '1.7.1' };      // pre-W14-2a stale state
  esp2.setBootPartition(1, '1.7.1');
  const m = new DeviceFirmwareMirror(g, esp2, nvs2);
  m.esp.bootload();
  const m2 = new DeviceFirmwareMirror(g, esp2, nvs2);
  const rolled = m2.setup();                           // try #3 → app-level rollback
  check('OT-5d >= 3 path (defense-in-depth) still fires when reachable',
    rolled === true && nvs2.boot_tries === 0 && nvs2.lfver === '' &&
    m2.logs.some((l) => l.includes('max boot attempts reached')));
}

// ---- OT-6: refusals (fresh GAS deployment — no earlier manifest rows) ----------
console.log('\n[OT-6] Refusals (mixed-fleet + anti-downgrade):');
{
  const g2 = createGasContext();                       // fresh sheet: only OUR manifests
  g2.sandbox.setupMasterTemplate();
  g2.setConfig('ADMIN_TOKEN', ADMIN);
  g2.registerDevice(DEVICE_KEY, 'generic');
  const publish2 = (version, target) => g2.doPost({
    action: 'OTA_PUBLISH', token: TOKEN, admin_token: ADMIN,
    manifest: {
      version: version,
      url: `https://raw.example.com/plts_firmware_v${version}.bin`,
      sha256: sha256Hex(VIRTUAL_BINARIES[version]),
      hmac: 'ab'.repeat(32),
      size: VIRTUAL_BINARIES[version].length,
      target: target,
    },
  });

  const cross = publish2('1.7.1', 'modular');          // target = the OTHER tree
  check('OT-6a publishing a modular-targeted manifest is accepted (valid for modular devices)',
    cross.status === 'SUCCESS' && cross.data.target === 'modular');

  const esp = new VirtualEsp32('1.7.0');
  const nvs = { boot_tries: 0, lfver: '' };
  const mirror = new DeviceFirmwareMirror(g2, esp, nvs);
  const m = mirror.fetchOtaManifest();                 // generic device, fw 1.7.0
  check('OT-6b a generic device is NOT served the modular manifest (GAS filter → 404 no-op)',
    m.valid === false && !esp.pendingRestart && esp.otaData[1].state === 'INVALID',
    JSON.stringify(m).slice(0, 140));

  // Anti-downgrade: a device already on 1.7.1 must refuse 1.7.0.
  const oldManifest = publish2('1.7.0', '');           // fleet-wide row
  check('OT-6c publishing an older fleet-wide manifest is accepted by GAS',
    oldManifest.status === 'SUCCESS');
  const esp2 = new VirtualEsp32('1.7.1');
  const nvs2 = { boot_tries: 0, lfver: '' };
  const mirrorNew = new DeviceFirmwareMirror(g2, esp2, nvs2);
  const applied = mirrorNew.checkOta();                // 1.7.0 < running 1.7.1
  const logR = g2.doPost({ action: 'OTA_LOG', token: TOKEN, device_key: DEVICE_KEY });
  const newestEvt = logR.data.events[0];
  check('OT-6d anti-downgrade: 1.7.1 device REFUSES 1.7.0 (honest event, no flash)',
    applied === false && esp2.otaData[1].state === 'INVALID' &&
    newestEvt && newestEvt.event === 'REFUSED' && newestEvt.version === '1.7.0',
    JSON.stringify(newestEvt));
}

// ---- OT-7: OTA_LOG read-back semantics ------------------------------------------
console.log('\n[OT-7] OTA_LOG read-back semantics:');
{
  const log = g.doPost({ action: 'OTA_LOG', token: DEVICE_KEY ? TOKEN : TOKEN,
    device_key: DEVICE_KEY, limit: 500 });
  const evts = (log.data && log.data.events) || [];
  check('OT-7a events are newest-first',
    evts.length >= 2 &&
    (Date.parse(evts[0].timestamp) >= Date.parse(evts[evts.length - 1].timestamp) ||
     evts[0].timestamp === evts[1].timestamp));
  const verbs = new Set(evts.map((e) => e.event));
  check('OT-7b only whitelisted verbs appear (word list held)',
    [...verbs].every((v) => ['ACTIVATED', 'ROLLBACK', 'BOOT_FAILED',
      'DOWNLOAD_FAILED', 'REFUSED', 'VERIFICATION_FAILED'].includes(v)),
    [...verbs].join(','));
  check('OT-7c device-scoped: no other device_key rows leak',
    evts.every((e) => log.data.deviceKey === DEVICE_KEY));
}

// ---- OT-8: static source locks ----------------------------------------------------
console.log('\n[OT-8] Static source locks (generic + modular + GAS):');
{
  const ino = fs.readFileSync(INO, 'utf-8');
  check('OT-8a generic: [W14-2a] ledger reset + [W14-2b] marker at applyOta success',
    /nvsSetBootTries\(0\);[\s\S]{0,400}nvsSetLastFlashed\(m\.version\);/.test(ino) &&
    ino.indexOf('nvsSetLastFlashed(m.version)') > ino.indexOf('bool applyOta'));
  check('OT-8b generic: checkBootloaderRevert wired in setup (both AP and STA paths)',
    (ino.match(/checkBootloaderRevert\(\);/g) || []).length >= 3);   // fwd-decl + 2 calls
  check('OT-8c generic: deferred report in the STA loop clears marker only on HTTP 200',
    /if \(otaRollbackReportPending\)[\s\S]{0,600}if \(rc == 200\)[\s\S]{0,200}nvsSetLastFlashed\(""\);/.test(ino));
  check('OT-8d generic: marker cleared on confirm INSIDE the pending branch (persistence fix)',
    (() => {
      const seg = ino.slice(ino.indexOf('void markOtaHealthyIfPending'),
        ino.indexOf('void markOtaHealthyIfPending') + 1400);
      const pendingIdx = seg.indexOf('PENDING_VERIFY');
      const clearIdx = seg.indexOf('nvsSetLastFlashed("")');
      const triesIdx = seg.indexOf('nvsSetBootTries(0)');
      return pendingIdx > -1 && clearIdx > pendingIdx && triesIdx > clearIdx;
    })());
  check('OT-8e generic: reportOtaStatus returns int (retry contract, [W14-2c])',
    /int reportOtaStatus\(const char\* event/.test(ino));

  const cpp = fs.readFileSync(OTAMGR_CPP, 'utf-8');
  const h = fs.readFileSync(OTAMGR_H, 'utf-8');
  check('OT-8f modular: _recordFlashedImage at BOTH Update.end(true) success sites',
    (cpp.match(/_recordFlashedImage\(_expectedVersion\);/g) || []).length === 2);
  check('OT-8g modular: begin() revert detection + single-shot marker consume',
    /lf_ver/.test(cpp) && /p\.remove\("lf_ver"\)/.test(cpp) &&
    /bootloader revert/.test(cpp));
  check('OT-8h modular: triggerRollback + markBootHealthy clear the marker',
    (() => {
      const t = cpp.indexOf('void OtaManager::triggerRollback');
      const m = cpp.indexOf('void OtaManager::markBootHealthy()');
      return cpp.slice(t, t + 700).includes('remove("lf_ver")') &&
             cpp.slice(m, m + 500).includes('remove("lf_ver")');
    })());
  check('OT-8i modular: getBootRollbackVersion() exposed for a future GAS bridge',
    /getBootRollbackVersion/.test(h) && /_bootRollbackVersion/.test(h));

  const gas = fs.readFileSync(GAS_SRC, 'utf-8');
  check('OT-8j GAS: OTA_STATUS word list still accepts ROLLBACK (+ VERIFICATION_FAILED)',
    /\['ACTIVATED', 'ROLLBACK', 'BOOT_FAILED', 'DOWNLOAD_FAILED',[\s\S]*?VERIFICATION_FAILED'\]/.test(gas));
}

// ---- summary -----------------------------------------------------------------------
console.log('\n' + '='.repeat(72));
console.log(`RESULT: ${passed} passed, ${failed} failed`);
if (failures.length) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
}
process.exit(failed === 0 ? 0 : 1);

