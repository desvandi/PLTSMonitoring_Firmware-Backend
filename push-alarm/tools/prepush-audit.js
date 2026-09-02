#!/usr/bin/env node
/*
 * prepush-audit.js - Gerbang audit terakhir SEBELUM push repositori.
 * =====================================================================
 * Memastikan repositori yang akan didorong ke remote:
 *   1. Bersih dari rahasia (kunci privat VAPID, token perangkat,
 *      URL deployment nyata/staging) - nilai EKSAKTIT maupun pola.
 *   2. Higienitas git: repo valid, semua berkas ter-commit, tidak ada
 *      berkas terlarang terlacak.
 *   3. Sinkron dengan templat kanonik paket rilis (bila paket tersedia):
 *      berkas kode identik byte-per-byte dengan sumbernya.
 *
 * Pemakaian:
 *   node prepush-audit.js                      # audit semua repo default
 *   node prepush-audit.js --repos <dir>        # akar folder repositori
 *   node prepush-audit.js <repoDir1> <repoDir2># audit repo tertentu
 *   node prepush-audit.js --package <dir>      # templat kanonik (default:
 *                                              # folder paket di samping
 *                                              # tools/ bila ada)
 *   node prepush-audit.js --secrets-dir <dir>  # sumber nilai rahasia
 *                                              # (default: <paket>/deploy
 *                                              # atau <repo>/secrets)
 *
 * Kode keluar: 0 = LULUS (aman di-push), 1 = BLOKIR (ada temuan).
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

/* ---------------- argumen ---------------- */
function argValue(names) {
  for (let i = 2; i < process.argv.length; i++) {
    if (names.includes(process.argv[i]) && process.argv[i + 1]) {
      return process.argv[i + 1];
    }
  }
  return null;
}

const SCRIPT_DIR = __dirname;
const DEFAULT_REPOS_ROOT = path.join(SCRIPT_DIR, '..', '..', 'repos');
const reposArg = argValue(['--repos']);
const positional = process.argv.slice(2).filter(function (a, i, arr) {
  return !a.startsWith('--') && !(i > 0 && arr[i - 1].startsWith('--'));
});
let repoDirs = positional.length ? positional.map(function (p) {
  return path.resolve(p);
}) : null;

const PKG_DIR = argValue(['--package']) || (function () {
  const pkg = path.join(SCRIPT_DIR, '..');
  const isPkg = fs.existsSync(path.join(pkg, 'gas', 'PushService.gs')) &&
    fs.existsSync(path.join(pkg, 'pwa-push-alarm', 'js', 'config.js'));
  return isPkg ? pkg : null;
})();

const WS_SCRIPTS = path.join(SCRIPT_DIR, '..', '..', 'scripts');
const wsScripsOk = fs.existsSync(path.join(WS_SCRIPTS, 'cross-audit-test.js'));

/* Peta sinkronisasi: [path-di-repo, [sumber, path-sumber]]
 * sumber 'dl' = paket rilis (download/), 'ws' = scripts/ ruang kerja. */
const FILE_MAP = {
  'monitoriot-pwa': [
    ['index.html', ['dl', 'pwa-push-alarm/index.html']],
    ['manifest.json', ['dl', 'pwa-push-alarm/manifest.json']],
    ['sw.js', ['dl', 'pwa-push-alarm/sw.js']],
    ['css/style.css', ['dl', 'pwa-push-alarm/css/style.css']],
    ['js/config.js', ['dl', 'pwa-push-alarm/js/config.js']],
    ['js/push-manager.js', ['dl', 'pwa-push-alarm/js/push-manager.js']],
    ['js/app.js', ['dl', 'pwa-push-alarm/js/app.js']],
    ['icons/icon-192.png', ['dl', 'pwa-push-alarm/icons/icon-192.png']],
    ['icons/icon-512.png', ['dl', 'pwa-push-alarm/icons/icon-512.png']],
    ['icons/badge-72.png', ['dl', 'pwa-push-alarm/icons/badge-72.png']],
    ['icons/maskable-512.png', ['dl', 'pwa-push-alarm/icons/maskable-512.png']],
    ['tools/verify-deployment.js', ['dl', 'tools/verify-deployment.js']],
  ],
  'monitoriot-gas': [
    ['PushService.gs', ['dl', 'gas/PushService.gs']],
    ['WebPushCore.gs', ['dl', 'gas/webpush-core.js']],
    ['tools/generate-vapid-keys.js', ['dl', 'tools/generate-vapid-keys.js']],
  ],
  'monitoriot-firmware': [
    ['MonitorIoT_Firmware/MonitorIoT_Firmware.ino', ['dl', 'firmware/MonitorIoT_Firmware.ino']],
    ['tools/roots.pem', ['ws', 'roots.pem']],
  ],
  'monitoriot-deploy': [
    ['tools/apply-deploy-config.js', ['dl', 'tools/apply-deploy-config.js']],
    ['tools/verify-deployment.js', ['dl', 'tools/verify-deployment.js']],
    ['tools/generate-vapid-keys.js', ['dl', 'tools/generate-vapid-keys.js']],
    ['tools/prep-deploy-secrets.js', ['dl', 'tools/prep-deploy-secrets.js']],
    ['tools/prepush-audit.js', ['dl', 'tools/prepush-audit.js']],
    ['tests/test-webpush-core.js', ['ws', 'test-webpush-core.js']],
    ['tests/smoke-test-pwa.js', ['ws', 'smoke-test-pwa.js']],
    ['tests/cross-audit-test.js', ['ws', 'cross-audit-test.js']],
  ],
  /* Repositori target publik (GitHub desvandi) - layout 2-repo:
   * PWA di subfolder pwa-push-alarm/ repo Next.js; backend+firmware+
   * toolkit+tests di subfolder push-alarm/ repo firmware-code.gs.
   * gas/Code.gs = rename PushService.gs (bukan byte-sync; kesetaraan
   * fungsional dibuktikan suite regresi 201 asersi yang MENJALANKAN kode). */
  'plts_monitor_PWA_only': [
    ['pwa-push-alarm/index.html', ['dl', 'pwa-push-alarm/index.html']],
    ['pwa-push-alarm/manifest.json', ['dl', 'pwa-push-alarm/manifest.json']],
    ['pwa-push-alarm/sw.js', ['dl', 'pwa-push-alarm/sw.js']],
    ['pwa-push-alarm/css/style.css', ['dl', 'pwa-push-alarm/css/style.css']],
    ['pwa-push-alarm/js/config.js', ['dl', 'pwa-push-alarm/js/config.js']],
    ['pwa-push-alarm/js/push-manager.js', ['dl', 'pwa-push-alarm/js/push-manager.js']],
    ['pwa-push-alarm/js/app.js', ['dl', 'pwa-push-alarm/js/app.js']],
    ['pwa-push-alarm/icons/icon-192.png', ['dl', 'pwa-push-alarm/icons/icon-192.png']],
    ['pwa-push-alarm/icons/icon-512.png', ['dl', 'pwa-push-alarm/icons/icon-512.png']],
    ['pwa-push-alarm/icons/badge-72.png', ['dl', 'pwa-push-alarm/icons/badge-72.png']],
    ['pwa-push-alarm/icons/maskable-512.png', ['dl', 'pwa-push-alarm/icons/maskable-512.png']],
    ['pwa-push-alarm/tools/verify-deployment.js', ['dl', 'tools/verify-deployment.js']],
    /* Panduan deploy PDF di AKAR repo (struktur ramping 2026-09-01):
     * salinan identik di kedua repositori - dicek byte-per-byte. */
    ['Panduan_Deploy_Production_MonitorIoT.pdf', ['dl', 'Panduan_Deploy_Production_MonitorIoT.pdf']],
  ],
  'plts_monitor_firmware-code.gs-etc': [
    ['push-alarm/gas/WebPushCore.gs', ['dl', 'gas/webpush-core.js']],
    ['push-alarm/firmware/MonitorIoT_Firmware/MonitorIoT_Firmware.ino', ['dl', 'firmware/MonitorIoT_Firmware.ino']],
    ['push-alarm/firmware/tools/roots.pem', ['ws', 'roots.pem']],
    ['push-alarm/tools/apply-deploy-config.js', ['dl', 'tools/apply-deploy-config.js']],
    ['push-alarm/tools/verify-deployment.js', ['dl', 'tools/verify-deployment.js']],
    ['push-alarm/tools/generate-vapid-keys.js', ['dl', 'tools/generate-vapid-keys.js']],
    ['push-alarm/tools/prep-deploy-secrets.js', ['dl', 'tools/prep-deploy-secrets.js']],
    ['push-alarm/tools/prepush-audit.js', ['dl', 'tools/prepush-audit.js']],
    ['push-alarm/tests/test-webpush-core.js', ['ws', 'test-webpush-core.js']],
    ['push-alarm/tests/smoke-test-pwa.js', ['ws', 'smoke-test-pwa.js']],
    ['push-alarm/tests/cross-audit-test.js', ['ws', 'cross-audit-test.js']],
    ['Panduan_Deploy_Production_MonitorIoT.pdf', ['dl', 'Panduan_Deploy_Production_MonitorIoT.pdf']],
  ],
};

/* Nilai rahasia eksaklit yang TIDAK BOLEH ada di repo mana pun. */
function collectSecretValues() {
  const candidates = [];
  if (PKG_DIR) candidates.push(path.join(PKG_DIR, 'deploy'));
  candidates.push(path.join(SCRIPT_DIR, '..', 'secrets'));
  if (argValue(['--secrets-dir'])) candidates.unshift(argValue(['--secrets-dir']));
  for (const dir of candidates) {
    const keysPath = path.join(dir, 'vapid-keys.json');
    const tokenPath = path.join(dir, 'device-token.txt');
    if (fs.existsSync(keysPath) && fs.existsSync(tokenPath)) {
      const keys = JSON.parse(fs.readFileSync(keysPath, 'utf8'));
      // URL staging pipa rilis (bukan nilai produksi, tetapi tidak boleh
      // bocor ke repo - akan tampak seperti deployment nyata). Dibangun
      // per-fragmen agar TIDAK muncul literal di berkas alat ini sendiri
      // (positif-palsu self-referential).
      const stagingUrl = 'AKfycbUjiStag' + 'ingBukanDeploy' + 'mentNyata';
      return {
        dir: dir,
        values: [
          String(keys.privateKey).trim(),
          String(keys.publicKey).trim(),
          fs.readFileSync(tokenPath, 'utf8').trim(),
          stagingUrl,
        ],
        names: ['VAPID_PRIVATE_KEY', 'VAPID_PUBLIC_KEY', 'FW_DEVICE_TOKEN', 'URL staging'],
      };
    }
  }
  return null;
}

const TEXT_EXT = new Set(['.js', '.gs', '.md', '.json', '.html', '.css',
  '.sh', '.ino', '.pem', '.txt', '.c', '.conf', '.example', '.yml', '.yaml']);
const FORBIDDEN_NAMES = new Set(['vapid-keys.json', 'device-token.txt',
  'script-properties.txt', '.clasp.json', '.env', '.npmrc', 'credentials.json']);
// Token rahasia (kunci/token VAPID) = tepat 43 karakter base64url yang
// BERBATAS (bukan potongan hex 64-karakter seperti sidik jari sertifikat).
// Batas juga mengecualikan '+' '/' '=' sehingga jendela 43-karakter di
// DALAM blob base64-standar (mis. hash integritas npm "sha512-...") tidak
// dianggap token base64url mandiri.
const B64URL43 = /(?<![A-Za-z0-9_+/=])[A-Za-z0-9_-]{43}(?![A-Za-z0-9_+/=])/g;
const PEM_PRIVATE = /-----BEGIN (RSA |EC |DSA |OPENSSH )?PRIVATE KEY( BLOCK)?-----/;
const REAL_GAS_URL = /script\.google\.com\/macros\/s\/(?!.*GANTI)[A-Za-z0-9_-]{20,}/;

/* Coret blok sertifikat PUBLIK (PEM) dari teks sebelum pemindaian pola:
 * isi base64 di dalamnya bisa memicu positif-palsu pola token 43-karakter.
 * Rahasia tidak pernah berada di dalam blok sertifikat; cek nilai eksaklit
 * tetap dilakukan pada teks penuh sebelum pencoretan. */
function stripPublicCertificates(text) {
  return text.replace(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g, ' ');
}

/* ---------------- util ---------------- */
let gagalTotal = 0, okTotal = 0;

function ok(msg) { okTotal++; console.log('  [OK]     ' + msg); }
function gagal(msg) { gagalTotal++; console.log('  [GAGAL]  ' + msg); }
function info(msg) { console.log('  [..]     ' + msg); }

function git(repoDir, args) {
  return execFileSync('git', ['-C', repoDir].concat(args),
    { encoding: 'utf8' });
}

function scanRepo(repoDir, secrets) {
  const name = path.basename(repoDir);
  console.log('\n== Audit repo: ' + name + ' ==');

  /* 1. Repo git valid + bersih */
  if (!fs.existsSync(path.join(repoDir, '.git'))) {
    gagal('bukan repositori git (.git tidak ada) - jalankan build/init dulu');
    return;
  }
  ok('repositori git valid');
  const status = git(repoDir, ['status', '--porcelain']).trim();
  if (status) {
    gagal('ada perubahan belum ter-commit / berkas tak terlacak:\n' +
      status.split('\n').map(function (l) { return '          ' + l; }).join('\n'));
  } else {
    ok('semua berkas ter-commit (git status bersih)');
  }

  const files = git(repoDir, ['ls-files']).split('\n')
    .filter(function (f) { return f.length > 0; });
  info(files.length + ' berkas terlacak');

  /* 2. Nama berkas terlarang */
  const forbidden = files.filter(function (f) {
    return FORBIDDEN_NAMES.has(path.basename(f));
  });
  if (forbidden.length) gagal('berkas rahasia terlacak: ' + forbidden.join(', '));
  else ok('tidak ada berkas rahasia terlarang terlacak');

  /* 3. Pindai isi berkas */
  const patternHits = [];
  const exactHits = [];
  for (const f of files) {
    const full = path.join(repoDir, f);
    const buf = fs.readFileSync(full);
    const asText = buf.toString('latin1');
    const isText = TEXT_EXT.has(path.extname(f)) ||
      path.basename(f).startsWith('.gitignore') ||
      !path.extname(f);
    if (secrets) {
      for (let i = 0; i < secrets.values.length; i++) {
        if (asText.indexOf(secrets.values[i]) >= 0) {
          exactHits.push(f + ' (' + secrets.names[i] + ')');
        }
      }
    }
    if (isText) {
      const scanText = stripPublicCertificates(asText);
      if (PEM_PRIVATE.test(scanText)) patternHits.push(f + ' (blok PRIVATE KEY PEM)');
      if (REAL_GAS_URL.test(scanText)) patternHits.push(f + ' (URL deployment GAS nyata)');
      const m43 = (scanText.match(B64URL43) || []).filter(function (m) {
        // 43 karakter identik = garis pemisah komentar, bukan token nyata
        // (positif-palsu pada file warisan repo lama).
        return new Set(m).size > 1;
      });
      if (m43.length) {
        for (const m of m43) patternHits.push(f + ' (token 43-karakter base64url: ' +
          m.slice(0, 8) + '...)');
      }
    } else {
      info('pindai pola dilewati (biner): ' + f);
    }
  }
  if (exactHits.length) {
    gagal('nilai rahasia EKSAKLIT ditemukan: ' + exactHits.join(', '));
  } else if (secrets) {
    ok('nilai rahasia eksaklit (privat/publik/token/staging) tidak ditemukan');
  } else {
    info('cek nilai eksaklit dilewati (secrets tidak ditemukan - isi ' +
      '--secrets-dir bila ingin cek penuh)');
  }
  if (patternHits.length) {
    gagal('pola mencurigakan ditemukan:\n' +
      patternHits.map(function (h) { return '          ' + h; }).join('\n'));
  } else {
    ok('bebas pola mencurigakan (PRIVATE KEY, URL GAS nyata, token base64url)');
  }

  /* 4. Sinkronisasi dengan templat kanonik */
  const map = FILE_MAP[name];
  if (map && PKG_DIR) {
    let drift = 0, checked = 0, skipped = 0;
    for (const pair of map) {
      const repoFile = path.join(repoDir, pair[0]);
      const srcRoot = pair[1][0] === 'dl' ? PKG_DIR : WS_SCRIPTS;
      if (pair[1][0] === 'ws' && !wsScripsOk) { skipped++; continue; }
      const srcFile = path.join(srcRoot, pair[1][1]);
      if (!fs.existsSync(repoFile) || !fs.existsSync(srcFile)) {
        gagal('sinkronisasi: berkas hilang ' + pair[0]);
        drift++;
        continue;
      }
      const a = fs.readFileSync(repoFile);
      const b = fs.readFileSync(srcFile);
      checked++;
      if (!a.equals(b)) {
        gagal('sinkronisasi: ' + pair[0] + ' BERBEDA dari templat kanonik');
        drift++;
      }
    }
    if (!drift && checked) {
      ok('sinkron dengan templat kanonik (' + checked + ' berkas dicek' +
        (skipped ? ', ' + skipped + ' dilewati' : '') + ')');
    }
  } else if (map && !PKG_DIR) {
    info('cek sinkronisasi dilewati (paket rilis tidak tersedia di mesin ini)');
  }

  /* 5. Info remote */
  const remotes = git(repoDir, ['remote']).trim();
  if (remotes) info('remote terpasang: ' + remotes.replace(/\n/g, ', '));
  else info('belum ada remote (siap di-push lewat push-all-repos.sh)');
}

/* ---------------- main ---------------- */
console.log('==============================================================');
console.log(' PRE-PUSH AUDIT - MonitorIoT (pemindai rahasia + higienitas)');
console.log('==============================================================');
if (PKG_DIR) console.log('Paket kanonik : ' + PKG_DIR);
else console.log('Paket kanonik : (tidak tersedia - cek sinkronisasi dilewati)');
const secrets = collectSecretValues();
console.log('Sumber rahasia: ' + (secrets ? secrets.dir : '(tidak ditemukan)'));

if (!repoDirs) {
  if (reposArg) repoDirs = [path.resolve(reposArg)];
  else if (fs.existsSync(DEFAULT_REPOS_ROOT)) repoDirs = [DEFAULT_REPOS_ROOT];
  else {
    console.error('\nGAGAL: tidak ada repositori untuk diaudit. ' +
      'Tentukan --repos <dir> atau path repo sebagai argumen.');
    process.exit(1);
  }
}
if (repoDirs.length === 1 && fs.existsSync(repoDirs[0]) &&
  !fs.existsSync(path.join(repoDirs[0], '.git'))) {
  // argumen berupa akar folder repos
  repoDirs = fs.readdirSync(repoDirs[0]).filter(function (d) {
    return fs.existsSync(path.join(repoDirs[0], d, '.git'));
  }).map(function (d) { return path.join(repoDirs[0], d); });
}
if (!repoDirs.length) {
  console.error('GAGAL: tidak ada repositori git ditemukan.');
  process.exit(1);
}

for (const r of repoDirs) scanRepo(path.resolve(r), secrets);

console.log('\n==============================================================');
console.log(' RINGKASAN: ' + okTotal + ' lulus, ' + gagalTotal + ' gagal');
if (gagalTotal) {
  console.log(' PRE-PUSH: BLOKIR - perbaiki temuan di atas sebelum push.');
  console.log('==============================================================');
  process.exit(1);
}
console.log(' PRE-PUSH: LULUS - repositori aman untuk didorong ke remote.');
console.log('==============================================================');
process.exit(0);
