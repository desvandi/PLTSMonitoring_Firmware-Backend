#!/usr/bin/env node
/*
 * smoke-test-pwa.js - Uji asap PWA MonitorIoT di headless Chromium.
 * - Serve folder PWA di localhost
 * - Mock endpoint GAS (script.google.com) via Playwright route()
 * - Verifikasi: halaman render, SW aktif, manifest valid, sensor render,
 *   alarm render, tombol push tersedia, tanpa error konsol JS.
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
  const page = await browser.newPage({ viewport: { width: 390, height: 844 } }); // ukuran ponsel

  const consoleErrors = [];
  page.on('console', (msg) => {
    if (msg.type() === 'error') consoleErrors.push(msg.text());
  });
  page.on('pageerror', (err) => consoleErrors.push('PAGEERROR: ' + err.message));

  // ---- Mock endpoint GAS ----
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
  await page.route('**/exec**', (route) => {
    return route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify(snapshot)
    });
  });

  let pass = 0, fail = 0;
  const check = (name, cond) => {
    if (cond) { pass++; console.log('  PASS ' + name); }
    else { fail++; console.log('  FAIL ' + name); }
  };

  // ---- Muat halaman ----
  await page.goto('http://127.0.0.1:8787/', { waitUntil: 'load' });
  await page.waitForTimeout(1500);

  check('judul halaman', (await page.title()).includes('MonitorIoT'));

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

  // ---- Render data mock ----
  const sensorCards = await page.locator('.sensor-card').count();
  check('3 kartu sensor ter-render', sensorCards === 3);
  const alarmItems = await page.locator('.alarm-item').count();
  check('2 alarm ter-render', alarmItems === 2);
  const alarmText = await page.locator('#alarm-list').innerText();
  check('alarm kritis tampil', alarmText.includes('SUHU KRITIS'));
  const sensorText = await page.locator('#sensor-grid').innerText();
  check('nilai sensor tampil (41.2 C)', sensorText.includes('41.2'));
  check('status koneksi online', (await page.locator('#conn-label').innerText()) === 'Terhubung');

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
  await browser.close();
  server.close();

  console.log('\nSMOKE TEST: ' + pass + ' pass, ' + fail + ' fail');
  process.exit(fail > 0 ? 1 : 0);
}

main().catch((e) => { console.error(e); server.close(); process.exit(1); });
