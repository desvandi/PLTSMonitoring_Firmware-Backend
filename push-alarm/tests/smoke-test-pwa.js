#!/usr/bin/env node
/*
 * smoke-test-pwa.js - Uji asap PWA MonitorIoT di headless Chromium.
 * - Serve folder PWA di localhost
 * - Mock endpoint GAS (script.google.com) via Playwright route()
 * - Verifikasi: halaman render, SW aktif, manifest valid, sensor render,
 *   alarm render, tombol push tersedia, tanpa error konsol JS.
 *
 * [REMEDIASI P0-1/p.493 2026-09-16] Arsitektur provisioning runtime:
 * build produksi menyajikan config.js KOSONG yang jujur
 * (APP_PROVISIONED=false) — dashboard sensor hanya muncul SETELAH
 * pengguna mem-provision lewat layar setup (URL GAS / VAPID /
 * deviceId / push token -> sessionStorage, BUKAN localStorage).
 * Test ini kini tiga fase:
 *   A. Kejujuran: tanpa provisioning -> layar setup tampil, dashboard
 *      disembunyikan, form lengkap.
 *   B. Provisioning via UI nyata: isi form + submit -> reload.
 *   C. Dashboard: data mock ter-render, status online, dan kredensial
 *      TIDAK pernah menulis ke localStorage (p.493 end-to-end).
 * Override: env MONITORIOT_PWA (folder pwa-push-alarm).
 */
'use strict';

const path = require('path');
const http = require('http');
const fs = require('fs');

const ROOT = path.join(__dirname, '..');
// Sadar-layout: paket rilis (.../download/pwa-push-alarm) atau clone
// multi-repo (ROOT/pwa-push-alarm, atau ROOT itu sendiri bila repo PWA
// di-clone sendiri). Override: env MONITORIOT_PWA.
function findPwaDir() {
  if (process.env.MONITORIOT_PWA) return process.env.MONITORIOT_PWA;
  const cands = [
    path.join(ROOT, 'download', 'pwa-push-alarm'),
    path.join(ROOT, 'pwa-push-alarm'),
    ROOT
  ];
  for (const c of cands) {
    if (fs.existsSync(path.join(c, 'js', 'config.js')) &&
        fs.existsSync(path.join(c, 'sw.js'))) return c;
  }
  return cands[0];
}
const PWA_DIR = findPwaDir();

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png'
};

// ---- Server statis kecil ----
const server = http.createServer((req, res) => {
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath === '/') urlPath = '/index.html';
  const filePath = path.join(PWA_DIR, urlPath);
  if (!filePath.startsWith(PWA_DIR) || !fs.existsSync(filePath) || fs.statSync(filePath).isDirectory()) {
    res.writeHead(404); res.end('not found'); return;
  }
  const ext = path.extname(filePath).toLowerCase();
  res.writeHead(200, {
    'Content-Type': MIME[ext] || 'application/octet-stream',
    // SW wajib konteks aman + no-cache agar update cepat terlihat
    'Cache-Control': 'no-cache'
  });
  fs.createReadStream(filePath).pipe(res);
});

async function main() {
  await new Promise((r) => server.listen(8787, '127.0.0.1', r));
  console.log('server: http://127.0.0.1:8787');

  const { chromium } = require('playwright');
  const browser = await chromium.launch({ headless: true });
  // [REMEDIASI P0-1] Route dipasang pada CONTEXT, bukan page: setelah
  // provisioning via form, reload membuat halaman dikendalikan service
  // worker, dan fetch GAS di-init dari dalam SW (network-only).
  // page.route() TIDAK mencegat fetch dari SW; context.route() YA.
  const context = await browser.newContext({ viewport: { width: 390, height: 844 } }); // ukuran ponsel
  const page = await context.newPage();

  const consoleErrors = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (err) => consoleErrors.push('PAGEERROR: ' + err.message));

  // ---- Mock endpoint GAS (di context: menangkap fetch dari page DAN SW;
  //     bertahan lintas reload) ----
  const snapshot = {
    ok: true,
    sensors: [
      { name: 'Suhu Greenhouse 1', value: 41.2, unit: 'C', alarm: true, status: 'melebihi ambang 38 C' },
      { name: 'Kelembapan', value: 71, unit: '%', alarm: false },
      { name: 'Kelembapan Tanah', value: 24, unit: '%', alarm: true, status: 'di bawah ambang 30%' }
    ],
    alarms: [
      { id: 'ALM-001', title: 'SUHU KRITIS', body: 'Suhu greenhouse 1 mencapai 41.2 C', severity: 'critical' },
      { id: 'ALM-002', title: 'Tanah Kering', body: 'Kelembapan tanah 24%', severity: 'warning' }
    ]
  };
  await context.route('**/exec**', (route) => {
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      headers: { 'Access-Control-Allow-Origin': '*' },
      body: JSON.stringify(snapshot)
    });
  });

  let pass = 0, fail = 0;
  const check = (name, cond) => {
    if (cond) { pass++; console.log('  PASS ' + name); }
    else { fail++; console.log('  FAIL ' + name); }
  };

  // ================================================================
  // FASE A — Kejujuran keadaan belum diprovision (P0-1)
  // ================================================================
  console.log('  -- Fase A: keadaan tanpa provisioning --');
  await page.goto('http://127.0.0.1:8787/', { waitUntil: 'load' });
  await page.waitForTimeout(800);

  check('judul halaman', (await page.title()).includes('MonitorIoT'));
  check('[A] layar setup tampil (belum diprovision = jujur)',
    await page.locator('#section-setup').isVisible());
  check('[A] dashboard sensor disembunyikan sebelum provisioning',
    !(await page.locator('#section-sensors').isVisible()));
  check('[A] form setup lengkap (apiBase/vapid/deviceId/token)',
    (await page.locator('#setup-api-base').count()) === 1 &&
    (await page.locator('#setup-vapid').count()) === 1 &&
    (await page.locator('#setup-device-id').count()) === 1 &&
    (await page.locator('#setup-device-token').count()) === 1);

  // ================================================================
  // FASE B — Provisioning runtime via UI nyata
  // ================================================================
  console.log('  -- Fase B: provisioning via form setup --');
  await page.fill('#setup-api-base', 'https://script.google.com/macros/s/MOCKSMOKETESTID/exec');
  // VAPID mock: base64url >= 80 karakter (validasi form: P-256 65-byte).
  await page.fill('#setup-vapid', 'B'.repeat(87));
  await page.fill('#setup-device-id', 'SMOKE-DEV-001');
  await page.fill('#setup-device-token', 'smoke-push-token-01');
  await Promise.all([
    page.waitForNavigation({ waitUntil: 'load', timeout: 15000 }),
    page.click('#setup-form button[type="submit"]')
  ]);
  await page.waitForTimeout(2000); // beri waktu polling pertama + render

  // ================================================================
  // FASE C — Dashboard ter-render dari data mock
  // ================================================================
  console.log('  -- Fase C: dashboard pasca-provisioning --');
  check('[C] layar setup tertutup setelah provisioning',
    !(await page.locator('#section-setup').isVisible()));
  check('[C] dashboard sensor tampil setelah provisioning',
    await page.locator('#section-sensors').isVisible());

  const sensorCards = await page.locator('.sensor-card').count();
  check('3 kartu sensor ter-render', sensorCards === 3);
  const alarmItems = await page.locator('.alarm-item').count();
  check('2 alarm ter-render', alarmItems === 2);
  const alarmText = await page.locator('#alarm-list').innerText();
  check('alarm kritis tampil', alarmText.includes('SUHU KRITIS'));
  const sensorText = await page.locator('#sensor-grid').innerText();
  check('nilai sensor tampil (41.2 C)', sensorText.includes('41.2'));
  check('status koneksi online', (await page.locator('#conn-label').innerText()) === 'Terhubung');

  // ---- p.493 end-to-end: kredensial HIDUP di sessionStorage, BUKAN localStorage ----
  const credStorage = await page.evaluate(() => ({
    localDeviceId: localStorage.getItem('push.deviceId'),
    localDeviceToken: localStorage.getItem('push.deviceToken'),
    sessionDeviceId: sessionStorage.getItem('push.deviceId'),
    sessionDeviceToken: sessionStorage.getItem('push.deviceToken')
  }));
  check('[p.493] localStorage TIDAK menyimpan deviceId',
    credStorage.localDeviceId === null);
  check('[p.493] localStorage TIDAK menyimpan deviceToken',
    credStorage.localDeviceToken === null);
  check('[p.493] kredensial tersimpan di sessionStorage (sesi saja)',
    credStorage.sessionDeviceId === 'SMOKE-DEV-001' &&
    credStorage.sessionDeviceToken === 'smoke-push-token-01');

  // ---- Manifest ----
  const manifestResp = await page.request.get('http://127.0.0.1:8787/manifest.json');
  const manifest = await manifestResp.json();
  check('manifest name/short_name', !!manifest.name && !!manifest.short_name);
  check('manifest display standalone', manifest.display === 'standalone');
  check('manifest start_url', !!manifest.start_url);
  check('manifest ikon 192+512+maskable',
    manifest.icons.some((i) => i.sizes === '192x192' && i.purpose === 'any') &&
    manifest.icons.some((i) => i.sizes === '512x512' && i.purpose === 'any') &&
    manifest.icons.some((i) => i.purpose === 'maskable'));

  // ---- Service worker ----
  const swState = await page.evaluate(async () => {
    if (!('serviceWorker' in navigator)) return { supported: false };
    const reg = await navigator.serviceWorker.getRegistration();
    return {
      supported: true,
      registered: !!reg,
      active: !!(reg && (reg.active || reg.installing || reg.waiting)),
      scope: reg ? reg.scope : null
    };
  });
  await page.waitForTimeout(2500); // beri waktu SW meng-instal
  const swState2 = await page.evaluate(async () => {
    const reg = await navigator.serviceWorker.getRegistration();
    return { registered: !!reg, active: !!(reg && reg.active) };
  });
  check('SW terdaftar', swState2.registered);
  check('SW aktif', swState2.active);

  // ---- UI Push ----
  const pushPermission = await page.evaluate(() =>
    typeof Notification !== 'undefined' ? Notification.permission : 'none');
  const btnEnable = page.locator('#btn-enable-push');
  check('tombol aktifkan push ada', await btnEnable.count() === 1);
  // Bila izin notifikasi ditolak (default headless), tombol HARUS nonaktif
  // + tampil panduan membuka izin -> itu perilaku yang dirancang.
  if (pushPermission === 'denied') {
    check('tombol disable saat izin ditolak (perilaku benar)', !(await btnEnable.isEnabled()));
  } else {
    check('tombol enable aktif (belum subscribed)', await btnEnable.isEnabled());
  }
  const pushStatus = await page.locator('#push-status').innerText();
  check('status push terisi', pushStatus.length > 0);

  // ---- Dukungan Push API di headless ----
  var pushSupport = await page.evaluate(() => ({
    push: 'PushManager' in window,
    notification: 'Notification' in window,
    notifPermission: typeof Notification !== 'undefined' ? Notification.permission : 'none',
    swShow: 'showNotification' in ServiceWorkerRegistration.prototype
  }));
  console.log('  [info] dukungan push headless:', JSON.stringify(pushSupport));
  check('PushManager tersedia (konteks aman)', pushSupport.push);

  // ---- Uji handler push SW secara langsung ----
  // (mensimulasikan event push dengan payload seperti kiriman GAS)
  const pushTest = await page.evaluate(async () => {
    const reg = await navigator.serviceWorker.getRegistration();
    if (!reg || !reg.active) return { ok: false, reason: 'sw-tidak-aktif' };
    const payload = {
      id: 'ALM-SMOKE', title: 'UJI SMOKE PUSH', body: 'Payload via postMessage',
      severity: 'critical', tag: 'smoke-1', url: './index.html?from=push'
    };
    return new Promise((resolve) => {
      const ch = new MessageChannel();
      ch.port1.onmessage = (ev) => resolve(ev.data);
      reg.active.postMessage({ __smokeTestPush: payload }, [ch.port2]);
      setTimeout(() => resolve({ ok: false, reason: 'timeout' }), 4000);
    });
  });
  console.log('  [info] hasil simulasi handler push:', JSON.stringify(pushTest));

  // ---- Error konsol (abaikan CORS jaringan mock) ----
  const realErrors = consoleErrors.filter((e) =>
    !/Failed to load resource|net::|ERR_FAILED/.test(e));
  check('tanpa error JS konsol', realErrors.length === 0);
  if (realErrors.length) console.log('  [errors]', JSON.stringify(realErrors, null, 2));

  await page.screenshot({ path: '/home/z/my-project/scripts/smoke-pwa.png', fullPage: false });
  await context.close();
  await browser.close();
  server.close();

  console.log('\nSMOKE TEST: ' + pass + ' pass, ' + fail + ' fail');
  process.exit(fail > 0 ? 1 : 0);
}

main().catch((e) => { console.error(e); server.close(); process.exit(1); });
