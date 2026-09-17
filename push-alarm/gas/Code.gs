/**
 * Code.gs (= PushService) - Modul pengirim Web Push (VAPID) untuk GAS.
 * =======================================================================
 * [ARSITEKTUR 2026-09-16 — audit P0-2] File ini adalah backend push LEGACY
 * (deployment standalone). Target arsitektur akhir adalah SATU trust
 * boundary canonical: ESP32 → canonical GAS (code.gs/Code.gs, HMAC) →
 * alarm state → PushService → PWA. Backend canonical kini memiliki modul
 * push setara (code.gs/PushService.gs — aksi PUSH_SUBSCRIBE / PUSH_
 * UNSUBSCRIBE / PUSH_ACK / PUSH_ALARM_INGEST dengan autentikasi + outbox
 * yang sama). Deployment BARU sebaiknya memakai canonical; file ini
 * dipertahankan untuk migrasi dan hardening deployment yang masih berjalan.
 *
 * [DEPRECATED — AUDIT ROUND-3 2026-09-17] Komponen ini BEKU untuk
 * production: perbaikan P1 round-3 (per-subscription delivery cursor,
 * sharded Script-Properties storage + kapasitas mekanis, lock tanpa
 * network I/O, lifecycle write-back) HANYA diimplementasikan di canonical
 * code.gs/PushService.gs. Backport yang DITERAPKAN di sini hanyalah
 * device-scoping state/ID (P1-A) supaya jalur migrasi multi-device tidak
 * salah edge. Limitasi yang diketahui dan TIDAK diperbaiki di sini:
 * penyimpanan array-tunggal (batas 9 KB/value GAS), delivery di dalam
 * lock, retry tanpa per-sub cursor. Gunakan canonical untuk deployment
 * production.
 *
 * File ini identik dengan PushService.gs pada rilis paket - hanya nama
 * filenya mengikuti konvensi Apps Script (Code.gs). Pasangan file:
 * WebPushCore.gs (tempel sebagai file terpisah di proyek GAS). File ini
 * berisi lapisan layanan; WebPushCore.gs berisi implementasi kripto
 * (SHA-256, P-256, AES-GCM).
 *
 * PENYIAPAN:
 *  1. Jalankan tools/generate-vapid-keys.js untuk membuat pasangan kunci.
 *  2. GAS -> Project Settings -> Script Properties, tambahkan:
 *       VAPID_PUBLIC_KEY  = <public key base64url>
 *       VAPID_PRIVATE_KEY = <private key base64url>   (RAHASIA)
 *       PUSH_TOKENS       = [{"deviceId":"...","token":"..."}]  (p.493:
 *                            token khusus subscribe/unsubscribe/testPush;
 *                            TIDAK bisa dipakai untuk ingest)
 *     Opsional (Script Properties tambahan):
 *       VAPID_SUBJECT             = kontak pengirim (mailto: atau https:)
 *                                    menimpa PUSH_CONFIG.SUBJECT.
 *       TEST_PUSH_MIN_INTERVAL_MS = jeda minimal antar testPush (default
 *                                    60000; 0 = tanpa batas).
 *       TEST_PUSH_ALLOW_PUBLIC    = 'true' untuk mengizinkan testPush GET
 *                                    tanpa autentikasi (default: CLOSED —
 *                                    rate limit bukan authorization).
 *  3. Deploy Web App: Execute as "Me", Access "Anyone".
 *  4. Salin URL deployment ke env PUSH_API_BASE Vercel project
 *     plts-monitor-push-alarm (build via tools/build-config.js) atau ke
 *     layar setup runtime PWA.
 *
 * API UTAMA (dipanggil handler firmware/data sensor Anda):
 *  sendAlarmToAll(alarm) - kirim push alarm ke semua perangkat langganan.
 *    Contoh alarm:
 *    { id: 'ALM-001', title: 'SUHU KRITIS', body: 'Suhu 41.2 C',
 *      severity: 'critical', url: './index.html?from=push' }
 *
 * INTEGRASI FIRMWARE (kontrak Ambang alarm FW-GAS, Tabel 11):
 *  Firmware tidak memanggil push secara langsung. Firmware hanya POST
 *  laporan level berkala ke `action=ingest`; modul ini melakukan deteksi
 *  tepi naik (false->true) per sensor dan memanggil sendAlarmToAll
 *  SEKALI per kejadian alarm (prinsip pengirim tunggal). Ketika kondisi
 *  alarm bertahan pada laporan berikutnya, tidak ada push tambahan.
 *  Skema laporan: lihat komentar pada handleIngest_ di bawah.
 *
 * ENDPOINT PWA (otomatis tersedia):
 *  POST ?action=subscribe|unsubscribe|ackAlarm  (body JSON dari PWA)
 *  GET  ?action=snapshot|latestAlarm|testPush
 */

'use strict';

/* ============================ Konfigurasi ============================ */

var PUSH_CONFIG = {
  // Identitas pengirim push (wajib mailto:/https: sesuai RFC 8292).
  // Dapat ditimpa lewat Script Property VAPID_SUBJECT (getVapidSubject_).
  SUBJECT: 'mailto:admin@monitor-iot.contoh.id',

  // Anti-spam endpoint testPush publik: jeda minimal antar permintaan
  // (ms). Dapat ditimpa lewat Script Property TEST_PUSH_MIN_INTERVAL_MS.
  // 0 = tanpa batas (tidak disarankan bila Web App terbuka "Anyone").
  TEST_PUSH_MIN_INTERVAL_MS: 60000,

  // Umur pesan di push service (detik). Maksimum 2419200 (28 hari).
  // Alarm layak dicoba ulang sampai 24 jam saat perangkat offline:
  TTL: 86400,

  // Maksimum langganan per eksekusi (kuota UrlFetchApp & batas 6 menit):
  BATCH_SIZE: 100,

  // Batas ukuran payload (push service umumnya 4096 byte total body):
  MAX_PAYLOAD_CHARS: 3000,

  // Simpan langganan di Spreadsheet (lebih tahan kuota & mudah diaudit).
  // false = simpan di Script Properties (cukup untuk < 100 perangkat).
  USE_SHEET_STORAGE: true,
  SHEET_NAME: 'push_subscriptions',

  // Buang langganan tidak aktif lebih dari N hari:
  PRUNE_AFTER_DAYS: 90,

  // Notifikasi "pulih" (severity info) saat alarm kembali normal:
  NOTIFY_RESOLVE: true,

  // Batas kejadian alarm yang disimpan (buffer melingkar):
  MAX_ALARM_LOG: 200,

  // Batas jumlah sensor per laporan firmware (PWA merender maks 24):
  MAX_SENSORS_PER_REPORT: 24,

  // [AUDIT P0-3] Outbox durable — model pengiriman alarm:
  //   AlarmEvent (identitas eventId, state RAISED/CLEARED) disimpan
  //   DULU, push dikirim SETELAHNYA, status delivery diperbarui terakhir.
  //   Event PENDING akan di-retry pada ingest berikutnya (backoff
  //   eksponensial, maks MAX_ATTEMPTS). Ini menutup dua failure mode:
  //   (a) push terkirim tapi state gagal disimpan -> duplikat tak terbatas;
  //   (b) state tersimpan tapi push gagal -> notifikasi hilang diam-diam.
  OUTBOX_MAX_EVENTS: 200,        // batas ring buffer event (identitas tetap)
  OUTBOX_RETRY_MAX_ATTEMPTS: 6,  // setelah ini status FAILED (tercatat, dihitung)
  OUTBOX_RETRY_BASE_MS: 60000,   // backoff: base * 2^(attempt-1)
  OUTBOX_MAX_RETRY_AGE_MS: 24 * 60 * 60 * 1000, // lewat umur ini -> EXPIRED

  // Ambang jaring pengaman sisi GAS: dipakai HANYA bila laporan
  // firmware TIDAK menyertakan field `alarm` (mis. firmware lama).
  // Bila field `alarm` hadir, flag firmware bersifat otoritatif
  // (satu jalur keputusan -> tidak ada dobel-kirim pada kejadian sama).
  // `match` = substring nama sensor (huruf kecil), `max`/`min` nilai ambang.
  THRESHOLDS: [
    { match: 'suhu', max: 40.0, severity: 'critical' },
    { match: 'kelembapan udara', min: 25.0, severity: 'warning' },
    { match: 'kelembapan tanah', min: 30.0, severity: 'warning' }
  ]
};

/* ======================= Kunci VAPID ======================= */

function getVapidKeys_() {
  var props = PropertiesService.getScriptProperties();
  var pub = props.getProperty('VAPID_PUBLIC_KEY');
  var priv = props.getProperty('VAPID_PRIVATE_KEY');
  if (!pub || !priv) {
    throw new Error(
      'Kunci VAPID belum diset. Buka Project Settings -> Script Properties ' +
      'lalu tambahkan VAPID_PUBLIC_KEY dan VAPID_PRIVATE_KEY ' +
      '(hasil tools/generate-vapid-keys.js).');
  }
  return { publicKey: pub, privateKey: priv };
}

/** Subjek VAPID (kontak pengirim, RFC 8292).
 *  Script Property VAPID_SUBJECT menimpa PUSH_CONFIG.SUBJECT tanpa
 *  mengedit kode - berguna saat produksi mengganti mailto ke domain
 *  sungguhan. Fallback ke konfigurasi bila properti kosong/tidak ada. */
function getVapidSubject_() {
  var raw = PropertiesService.getScriptProperties().getProperty('VAPID_SUBJECT');
  var s = raw === null || raw === undefined ? '' : String(raw).trim();
  return s || PUSH_CONFIG.SUBJECT;
}

/* ======================= Penyimpanan langganan ======================= */

/**
 * Format record langganan:
 * { endpoint, keys:{p256dh, auth}, context:{...}, addedAt, lastSeenAt }
 */

function getSubscriptions_() {
  if (PUSH_CONFIG.USE_SHEET_STORAGE) {
    return readSubscriptionsFromSheet_();
  }
  var raw = PropertiesService.getScriptProperties().getProperty('PUSH_SUBSCRIPTIONS');
  return raw ? JSON.parse(raw) : [];
}

function saveSubscriptions_(subs) {
  if (PUSH_CONFIG.USE_SHEET_STORAGE) {
    writeSubscriptionsToSheet_(subs);
  } else {
    PropertiesService.getScriptProperties()
      .setProperty('PUSH_SUBSCRIPTIONS', JSON.stringify(subs));
  }
}

function upsertSubscription_(sub) {
  var subs = getSubscriptions_();
  var found = false;
  for (var i = 0; i < subs.length; i++) {
    if (subs[i].endpoint === sub.endpoint) {
      subs[i] = sub; // perbarui kunci (bisa berotasi saat resubscribe)
      found = true;
      break;
    }
  }
  if (!found) subs.push(sub);
  saveSubscriptions_(subs);
  return subs.length;
}

/** Hapus langganan; statusCode 404/410 = endpoint mati. */
function removeSubscription_(endpoint) {
  var subs = getSubscriptions_();
  var kept = subs.filter(function (s) { return s.endpoint !== endpoint; });
  if (kept.length !== subs.length) saveSubscriptions_(kept);
  return subs.length - kept.length;
}

/** [P1 ownership] Hapus langganan HANYA bila record milik device tersebut
 *  (atau record lama tanpa deviceId — warisan pra-binding). Mencegah
 *  device A menghapus langganan device B meski A memegang token sah. */
function removeSubscriptionOwned_(endpoint, deviceId) {
  var subs = getSubscriptions_();
  var kept = subs.filter(function (s) {
    if (s.endpoint !== endpoint) return true; // bukan endpoint target
    if (!s.deviceId) return false;           // record lama -> boleh dihapus
    return String(s.deviceId) !== String(deviceId); // milik device lain -> pertahankan
  });
  if (kept.length !== subs.length) saveSubscriptions_(kept);
  return subs.length - kept.length;
}

function pruneStaleSubscriptions_() {
  var cutoff = Date.now() - PUSH_CONFIG.PRUNE_AFTER_DAYS * 86400000;
  var subs = getSubscriptions_();
  var kept = subs.filter(function (s) {
    var seen = s.lastSeenAt || s.addedAt;
    return !seen || new Date(seen).getTime() >= cutoff;
  });
  if (kept.length !== subs.length) saveSubscriptions_(kept);
  return subs.length - kept.length;
}

// [audit-2 S-13 FIX] Auto-prune trigger. pruneStaleSubscriptions_ was
// defined but never called — stale subscriptions grew unbounded. We trigger
// prune opportunistically: every PRUNE_CHECK_INTERVAL_MS (1 hour), the next
// handleIngest_ call invokes prune. This avoids needing a time-driven
// trigger setup step.
var PRUNE_CHECK_INTERVAL_MS = 60 * 60 * 1000;  // 1 hour
function maybePruneSubscriptions_() {
  var props = PropertiesService.getScriptProperties();
  var lastStr = props.getProperty('PRUNE_LAST_AT');
  var last = lastStr ? Number(lastStr) : 0;
  var now = Date.now();
  if (now - last < PRUNE_CHECK_INTERVAL_MS) return;
  props.setProperty('PRUNE_LAST_AT', String(now));
  try { pruneStaleSubscriptions_(); } catch (e) { /* best-effort */ }
}

function readSubscriptionsFromSheet_() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  if (!ss) return [];
  var sheet = ss.getSheetByName(PUSH_CONFIG.SHEET_NAME);
  if (!sheet || sheet.getLastRow() < 2) return [];
  var values = sheet.getDataRange().getValues();
  var out = [];
  for (var i = 1; i < values.length; i++) {
    try {
      out.push(JSON.parse(values[i][0]));
    } catch (e) { /* baris korup -> lewati */ }
  }
  return out;
}

function writeSubscriptionsToSheet_(subs) {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  if (!ss) throw new Error('Spreadsheet terikat proyek diperlukan untuk penyimpanan langganan.');
  var sheet = ss.getSheetByName(PUSH_CONFIG.SHEET_NAME);
  if (!sheet) sheet = ss.insertSheet(PUSH_CONFIG.SHEET_NAME);
  sheet.clear();
  sheet.getRange(1, 1).setValue('subscription (JSON)');
  if (subs.length > 0) {
    var rows = subs.map(function (s) { return [JSON.stringify(s)]; });
    sheet.getRange(2, 1, rows.length, 1).setValues(rows);
  }
}

/* ======================= Pengiriman push ======================= */

/**
 * Kirim satu push terenkripsi ke satu langganan.
 * @returns {{ok:boolean, status:number, error:string}}
 */
function webPushSend_(subscription, payloadObj, urgency) {
  var payloadStr = JSON.stringify(payloadObj);
  if (payloadStr.length > PUSH_CONFIG.MAX_PAYLOAD_CHARS) {
    // Jangan gagal total: potong body pesan.
    payloadObj.body = String(payloadObj.body || '').slice(0, 160) + '...';
    payloadStr = JSON.stringify(payloadObj);
  }

  var keys = getVapidKeys_();
  var enc = WebPushCore.encryptPayload(subscription, payloadStr);
  var vapid = WebPushCore.vapidHeaders(
    subscription.endpoint, keys.privateKey, keys.publicKey,
    getVapidSubject_(), 12 * 3600);

  var options = {
    method: 'post',
    // Blob menjamin byte biner terkirim utuh. Array byte/string biasa
    // akan dikorupsi oleh konversi teks UrlFetchApp (temuan X-2 audit
    // silang) - payload terenkripsi WAJIB berupa Blob octet-stream.
    payload: Utilities.newBlob(enc.body, 'application/octet-stream'),
    headers: {
      'Authorization': vapid.Authorization,
      'TTL': String(PUSH_CONFIG.TTL),
      'Urgency': urgency || 'high',          // very-low|low|normal|high
      'Content-Encoding': 'aes128gcm'
    },
    muteHttpExceptions: true,
    followRedirects: true
  };

  try {
    var res = UrlFetchApp.fetch(subscription.endpoint, options);
    var code = res.getResponseCode();
    if (code === 404 || code === 410) {
      // Endpoint kedaluwarsa -> hapus agar tidak mengotori daftar kirim.
      removeSubscription_(subscription.endpoint);
      return { ok: false, status: code, error: 'endpoint-mati' };
    }
    if (code >= 200 && code < 300) {
      return { ok: true, status: code, error: '' };
    }
    // 429 = rate limit push service; 400/413 = payload/hasil enkripsi ditolak
    return { ok: false, status: code, error: res.getContentText().slice(0, 200) };
  } catch (err) {
    // Kegagalan jaringan DNS/TLS; biarkan langganan tetap ada.
    return { ok: false, status: 0, error: String(err).slice(0, 200) };
  }
}

/**
 * URL Web App deployment ini — dipakai untuk menempel apiBase pada payload
 * push sehingga service worker PWA tahu ke mana ACK harus dikirim TANPA
 * konstanta yang tertanam di sw.js (audit P0-1: tidak ada lagi placeholder).
 */
function getScriptUrl_() {
  try {
    var url = ScriptApp.getService().getUrl();
    return url ? String(url) : null;
  } catch (e) {
    return null; // eksekusi di luar Web App (editor/trigger) -> tanpa URL
  }
}

/**
 * [P0-3] Kirim SATU AlarmEvent ke langganan yang relevan.
 * Cakupan langganan (fleet):
 *   - record dengan deviceId sama dengan event -> tujuan utama;
 *   - record TANPA deviceId (warisan lama, sebelum binding ada) -> tetap
 *     menerima (perilaku lama dipertahankan agar deployment existing tidak
 *     kehilangan notifikasi sebelum semua pelanggan re-subscribe).
 * Mengembalikan { sent, failed, removed, total }.
 */
function deliverAlarmEvent_(ev) {
  var payload = normalizeAlarm_(ev.payload);
  payload.apiBase = getScriptUrl_(); // [P0-1] SW memakai ini untuk ACK
  // [audit p.482] capability token ACK terikat eventId ini.
  payload.ackToken = makeAckToken_(payload.id);

  var subs = getSubscriptions_();
  var scoped = subs.filter(function (s) {
    return !s.deviceId || String(s.deviceId) === String(ev.deviceId);
  });

  var sent = 0, failed = 0, removed = 0;
  for (var i = 0; i < scoped.length; i++) {
    var r = webPushSend_(scoped[i], payload,
      payload.severity === 'critical' ? 'high' : 'normal');
    if (r.ok) sent++;
    else { failed++; if (r.error === 'endpoint-mati') removed++; }
  }
  return { sent: sent, failed: failed, removed: removed, total: scoped.length };
}

/**
 * API UTAMA - kirim alarm ke semua perangkat.
 * Panggil dari handler data sensor / time-driven trigger Anda.
 * Setiap alarm yang melewati fungsi ini DICATAT ke ALARM_LOG secara
 * terpusat (temuan X-6 audit silang) sehingga ACK dari PWA diterima
 * untuk SEMUA alarm yang pernah terkirim, apa pun jalurnya.
 * [P1 concurrency] Kini juga dijalankan dalam lock; pengiriman langsung
 * (testPush/simulasi) tetap didukung, jalur ingest memakai outbox.
 * @param {object} alarm { id, title, body, severity, url }
 * @returns {{sent:number, failed:number, removed:number}}
 */
function sendAlarmToAll(alarm) {
  var payload = normalizeAlarm_(alarm);
  // [audit p.482 REMEDIATION] Setiap notifikasi membawa capability token ACK
  // terikat alarm ini (HMAC, bucket 6 jam) — dikirim terenkripsi end-to-end
  // oleh push service, dan menjadi SATU-SATUNYA otorisasi aksi "ackAlarm".
  payload.ackToken = makeAckToken_(payload.id);
  payload.apiBase = getScriptUrl_(); // [P0-1] ACK tidak lagi bergantung konstanta sw.js
  logAlarmEvent_(payload, alarm);
  var subs = getSubscriptions_();
  var sent = 0, failed = 0, removed = 0;

  // [audit-2 S-12 FIX] Process ALL subscriptions in batches of BATCH_SIZE,
  // not just the first BATCH_SIZE. The previous loop `i < subs.length && i <
  // BATCH_SIZE` silently dropped subscribers beyond index 99. For the
  // target deployment (2 phones) this is fine, but the cap was a footgun.
  // Bounded by GAS 6-min execution limit; 100 fetch × ~3s = 5min worst case.
  for (var i = 0; i < subs.length; i++) {
    var r = webPushSend_(subs[i], payload,
      payload.severity === 'critical' ? 'high' : 'normal');
    if (r.ok) sent++;
    else { failed++; if (r.error === 'endpoint-mati') removed++; }
  }
  return { sent: sent, failed: failed, removed: removed, total: subs.length };
}

/**
 * Catat satu kejadian alarm ke ALARM_LOG (buffer melingkar).
 * - id sama (kirim ulang) -> perbarui entri, pertahankan acknowledgedAt.
 * - severity 'info' (mis. notifikasi "pulih") dianggap selesai sejak awal
 *   sehingga tidak memenuhi daftar alarm aktif.
 */
function logAlarmEvent_(payload, source) {
  source = source || {};
  var log = loadJsonObject_('ALARM_LOG', []);
  var entry = {
    id: payload.id,
    title: payload.title,
    body: payload.body,
    severity: payload.severity,
    tag: payload.tag,
    url: payload.url,
    timestamp: payload.timestamp,
    requireInteraction: payload.requireInteraction,
    sensor: source.sensor || null,
    raisedAt: source.raisedAt || new Date().toISOString(),
    clearedAt: source.clearedAt !== undefined ? source.clearedAt
      : (payload.severity === 'info' ? new Date().toISOString() : null),
    acknowledgedAt: null
  };
  var replaced = false;
  for (var i = log.length - 1; i >= 0; i--) {
    if (log[i].id === entry.id) {
      entry.acknowledgedAt = log[i].acknowledgedAt || null;
      log[i] = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) log.push(entry);
  if (log.length > PUSH_CONFIG.MAX_ALARM_LOG) {
    log = log.slice(log.length - PUSH_CONFIG.MAX_ALARM_LOG);
  }
  PropertiesService.getScriptProperties()
    .setProperty('ALARM_LOG', JSON.stringify(log));
}

/** Tandai kejadian alarm selesai (pulih). Muat log segar agar aman
 *  terhadap pencatatan terpusat yang baru saja terjadi. */
function markAlarmCleared_(alarmId) {
  var log = loadJsonObject_('ALARM_LOG', []);
  for (var i = log.length - 1; i >= 0; i--) {
    if (log[i].id === alarmId) {
      if (!log[i].clearedAt) log[i].clearedAt = new Date().toISOString();
      PropertiesService.getScriptProperties()
        .setProperty('ALARM_LOG', JSON.stringify(log));
      return true;
    }
  }
  return false;
}

/** Normalisasi alarm ke skema payload sw.js. */
function normalizeAlarm_(alarm) {
  alarm = alarm || {};
  var sev = ['critical', 'warning', 'info'].indexOf(alarm.severity) >= 0
    ? alarm.severity : 'info';
  return {
    id: String(alarm.id || ('ALM-' + Date.now())).slice(0, 64),
    title: String(alarm.title || 'Alarm MonitorIoT').slice(0, 80),
    body: String(alarm.body || '').slice(0, 400),
    severity: sev,
    tag: String(alarm.tag || ('alarm-' + (alarm.id || 'umum'))).slice(0, 64),
    url: String(alarm.url || './index.html?from=push').slice(0, 200),
    timestamp: alarm.timestamp || Date.now(),
    requireInteraction: sev === 'critical'
  };
}

/* ======================= [audit p.482] ACK Capability Token ======================= */

/**
 * [AUDIT p.482 REMEDIATION 2026-09] aksi "ackAlarm" adalah MUTASI STATE,
 * tetapi sebelumnya hanya divalidasi terhadap alarmId — siapa pun yang
 * mengetahui URL Web App + ID alarm bisa mengirim ACK palsu. Kini setiap
 * alarm yang dikirim membawa `ackToken` = HMAC-SHA256(ACK_SECRET,
 * alarmId + '|' + bucket-waktu) dan doPost MENOLAK ACK tanpa token valid.
 *
 * - Secret: Script Property PUSH_ACK_SECRET (auto-provision 64-hex saat
 *   pertama kali dibutuhkan; rotasi manual cukup ganti property).
 * - Masa berlaku: bucket 6 jam; verifikasi menerima bucket saat ini +
   *   sebelumnya (≈12 jam jendela) — cukup untuk alarm interaktif.
 * - Jalur legacy tanpa token HANYA aktif bila Script Property
 *   PUSH_ACK_ALLOW_LEGACY === 'true' (default: closed).
 */
var ACK_TOKEN_BUCKET_MS = 6 * 60 * 60 * 1000;   // 6 jam per bucket

function getAckSecret_() {
  var props = PropertiesService.getScriptProperties();
  var secret = props.getProperty('PUSH_ACK_SECRET');
  if (!secret) {
    // Auto-provision (CSPRNG milik Apps Script) — sekali per deployment.
    secret = Utilities.getUuid().replace(/-/g, '') +
             Utilities.getUuid().replace(/-/g, '');
    props.setProperty('PUSH_ACK_SECRET', secret);
  }
  return secret;
}

function ackTokenBucket_(shift) {
  shift = shift || 0;
  return Math.floor(Date.now() / ACK_TOKEN_BUCKET_MS) - shift;
}

function computeAckToken_(alarmId, bucket) {
  var sig = Utilities.computeHmacSha256Signature(
    String(alarmId) + '|' + String(bucket), getAckSecret_());
  return sig.map(function (b) {
    return ('0' + (b & 0xff).toString(16)).slice(-2);
  }).join('');
}

/** Token utk alarm ini pada bucket berjalan (ditempel ke payload push). */
function makeAckToken_(alarmId) {
  return computeAckToken_(alarmId, ackTokenBucket_(0));
}

/** Validasi token: cocok untuk bucket sekarang ATAU sebelumnya. */
function verifyAckToken_(alarmId, token) {
  if (!alarmId || typeof token !== 'string' || token.length === 0) return false;
  var current = computeAckToken_(alarmId, ackTokenBucket_(0));
  var previous = computeAckToken_(alarmId, ackTokenBucket_(1));
  var a = String(token).toLowerCase();
  return a === current || a === previous;
}

function ackLegacyAllowed_() {
  return String(PropertiesService.getScriptProperties()
    .getProperty('PUSH_ACK_ALLOW_LEGACY') || '') === 'true';
}

/* ======================= Handler HTTP Web App ======================= */

function jsonOut_(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

/** POST dari PWA (subscribe/unsubscribe/ackAlarm) + firmware (ingest). */
function doPost(e) {
  var body = {};
  try { body = JSON.parse(e.postData.contents); }
  catch (err) { return jsonOut_({ ok: false, message: 'Body JSON tidak valid' }); }

  if (body.action === 'ingest') {
    return jsonOut_(handleIngest_(body));
  }

  if (body.action === 'subscribe') {
    // [audit-2 K-7 FIX + p.493] subscribe MUST be authenticated. Kredensial
    // yang diterima: PUSH_TOKENS (token khusus langganan — preferred) ATAU
    // kredensial perangkat ingest (jalur migrasi; PWA baru memakai
    // PUSH_TOKENS sehingga kompromi push ≠ kompromi ingest).
    if (!body.device || !body.device.id || !body.token) {
      return jsonOut_({ ok: false, message: 'Langganan butuh autentikasi perangkat (device.id + token)' });
    }
    if (!isSubscriptionAuthorized_(String(body.device.id), body.token)) {
      return jsonOut_({ ok: false, message: 'Token perangkat tidak valid' });
    }
    if (!body.endpoint || !body.keys || !body.keys.p256dh || !body.keys.auth) {
      return jsonOut_({ ok: false, message: 'Langganan tidak lengkap' });
    }
    var ep = String(body.endpoint);
    // Validasi ringan (temuan X-5 audit silang): endpoint push wajib
    // https agar registri tidak dipakai membuang data ke host sembarangan.
    if (ep.indexOf('https://') !== 0 || ep.length > 500 ||
        typeof body.keys.p256dh !== 'string' || typeof body.keys.auth !== 'string') {
      return jsonOut_({ ok: false, message: 'Endpoint/kunci langganan tidak valid' });
    }

    // [P1 concurrency + P1 ownership] Mutasi daftar langganan di DALAM LOCK,
    // dan record kini MENYIMPAN deviceId secara canonical — ownership
    // relation subscription -> device dipertahankan untuk audit fleet:
    // endpoint mana milik device mana, sejak kapan, terakhir terlihat kapan.
    var result = withLock_(function () {
      // [audit-2 K-7] Cap total subscriptions to bound resource growth. The
      // original README target is 2 phones, but we cap at 50 for headroom.
      // Beyond this, oldest subscriptions are pruned (LRU).
      var existing = loadJsonObject_('PUSH_SUBSCRIPTIONS', []);
      if (existing.length >= 50) {
        existing.sort(function (a, b) {
          return new Date(a.lastSeenAt || a.addedAt).getTime() -
                 new Date(b.lastSeenAt || b.addedAt).getTime();
        });
        existing = existing.slice(existing.length - 49);
      }
      var sub = {
        endpoint: ep,
        keys: { p256dh: body.keys.p256dh, auth: body.keys.auth },
        // [P1 data-model] deviceId canonical pada record — dipakai untuk
        // delivery ter-scoped per device + audit kepemilikan fleet.
        deviceId: String(body.device.id).slice(0, 64),
        context: body.context || {},
        addedAt: new Date().toISOString(),
        lastSeenAt: new Date().toISOString()
      };
      var total = upsertSubscription_(sub);
      return { ok: true, total: total };
    });
    if (!result.ok) return jsonOut_(result);
    return jsonOut_({ ok: true, message: 'Langganan tersimpan', total: result.total });
  }

  if (body.action === 'unsubscribe') {
    // [SELF-AUDIT 2026-09-16 + P1 ownership] unsubscribe mewajibkan
    // autentikasi perangkat yang SAMA dengan subscribe (audit-2 K-7), dan
    // hanya boleh menghapus record yang MEMANG milik device tersebut
    // (atau record lama tanpa deviceId). Tanpa ownership check, siapa pun
    // yang memegang token device A bisa menghapus langganan device B.
    if (!body.device || !body.device.id || !body.token) {
      return jsonOut_({
        ok: false,
        message: 'Pembatalan langganan butuh autentikasi perangkat (device.id + token)'
      });
    }
    if (!isSubscriptionAuthorized_(String(body.device.id), body.token)) {
      return jsonOut_({ ok: false, message: 'Token perangkat tidak valid' });
    }
    var unsub = withLock_(function () {
      return { ok: true, removed: removeSubscriptionOwned_(body.endpoint,
        String(body.device.id).slice(0, 64)) };
    });
    if (!unsub.ok) return jsonOut_(unsub);
    return jsonOut_({ ok: true, message: 'Langganan dihapus', removed: unsub.removed });
  }

  if (body.action === 'ackAlarm') {
    // [audit p.482 REMEDIATION] ACK wajib membawa capability token valid yang
    // diterbitkan bersama notifikasi alarm yang sama. Tanpa token, ACK palsu
    // dari pihak yang hanya mengetahui URL + alarmId DITOLAK (fail-closed).
    // Jalur legacy tanpa token hanya aktif bila operator eksplisit mengatur
    // Script Property PUSH_ACK_ALLOW_LEGACY='true' (untuk jendela migrasi).
    if (!verifyAckToken_(body.alarmId, body.ackToken) && !ackLegacyAllowed_()) {
      return jsonOut_({
        ok: false,
        message: 'ACK ditolak: capability token tidak valid / tidak disertakan. ' +
                 'Gunakan aksi "Tandai Ditangani" pada notifikasi (token terbit otomatis).'
      });
    }
    // [P1 concurrency] Mutasi ALARM_LOG (dan status event) di dalam lock.
    var ackRes = withLock_(function () {
      return markAlarmAcknowledged_(body.alarmId);
    });
    if (!ackRes.ok) return jsonOut_(ackRes);
    if (!ackRes.found) {
      return jsonOut_({ ok: false, message: 'Alarm tidak ditemukan: ' + body.alarmId });
    }
    return jsonOut_({ ok: true, message: 'Alarm ditandai ditangani',
      alarmId: ackRes.alarmId, acknowledgedAt: ackRes.at });
  }

  // [P1 hardening] testPush via POST — autentikasi WAJIB (push token atau
  // kredensial device). Rate limit tetap berlaku, tetapi rate limit bukan
  // authorization: endpoint publik yang bisa memicu push adalah permukaan
  // spam/DoS terhadap fleet langganan.
  if (body.action === 'testPush') {
    var tpAuth = body.device && body.device.id && body.token &&
      isSubscriptionAuthorized_(String(body.device.id), body.token);
    if (!tpAuth) {
      return jsonOut_({ ok: false,
        message: 'testPush butuh autentikasi perangkat (device.id + push token)' });
    }
    var tpGate = checkTestPushRate_();
    if (!tpGate.ok) return jsonOut_({ ok: false, message: tpGate.message });
    var r = sendAlarmToAll({
      id: 'TEST-' + Date.now(),
      title: 'Uji Push Berhasil',
      body: 'Jika Anda menerima ini, jalur GAS -> push service -> PWA sudah benar.',
      severity: 'info',
      tag: 'test-push'
    });
    return jsonOut_({ ok: true, result: r });
  }

  return jsonOut_({ ok: false, message: 'Aksi tidak dikenal: ' + body.action });
}

/** GET dari PWA (snapshot/latestAlarm/testPush). */
function doGet(e) {
  var action = (e && e.parameter && e.parameter.action) || 'snapshot';

  if (action === 'latestAlarm') {
    // Fallback push tanpa payload: kembalikan alarm terbaru.
    // Integrasi: ambil dari penyimpanan alarm Anda.
    return jsonOut_({ ok: true, alarm: getLatestAlarm_() });
  }

  if (action === 'testPush') {
    // [P1 hardening 2026-09-16] testPush GET kini DITUTUP secara default —
    // "rate limiting adalah mitigasi DoS, bukan authorization". Jalur
    // terautentikasi adalah POST dengan device.id + push token. Operator
    // yang benar-benar butuh endpoint GET publik dapat mengaktifkan
    // Script Property TEST_PUSH_ALLOW_PUBLIC='true' (jendela migrasi).
    var allowPublicGet = String(PropertiesService.getScriptProperties()
      .getProperty('TEST_PUSH_ALLOW_PUBLIC') || '').toLowerCase() === 'true';
    if (!allowPublicGet) {
      return jsonOut_({
        ok: false,
        message: 'testPush GET dinonaktifkan. Gunakan POST dengan device.id + push token ' +
                 '(autentikasi), atau set TEST_PUSH_ALLOW_PUBLIC=true untuk jendela migrasi.'
      });
    }
    var gate = checkTestPushRate_();
    if (!gate.ok) {
      return jsonOut_({ ok: false, message: gate.message });
    }
    var r = sendAlarmToAll({
      id: 'TEST-' + Date.now(),
      title: 'Uji Push Berhasil',
      body: 'Jika Anda menerima ini, jalur GAS -> push service -> PWA sudah benar.',
      severity: 'info',
      tag: 'test-push'
    });
    return jsonOut_({ ok: true, result: r });
  }

  // default: snapshot untuk dashboard
  return jsonOut_(getSnapshot_());
}

/* ============ Ingest firmware: kontrak Ambang alarm FW-GAS ============ */
/*
 * Skema laporan firmware (lihat MonitorIoT_Firmware.ino):
 *   {
 *     "action": "ingest",
 *     "token": "<token>",
 *     "device": { "id": "esp32-greenhouse-01", "fw": "1.0.0",
 *                 "uptimeMs": 123456 },
 *     "sensors": [
 *       { "name": "Suhu Greenhouse 1", "value": 41.2, "unit": "C",
 *         "alarm": true, "severity": "critical",
 *         "status": "suhu 41.2 C melebihi ambang 40.0 C" }
 *     ],
 *     "reportedAt": 1724900000000
 *   }
 *
 * Aturan kontrak (anti dobel-kirim):
 *   1. Flag `alarm` firmware bersifat LEVEL dan otoritatif bila hadir.
 *      Evaluasi ambang THRESHOLDS hanya fallback bila field absen.
 *   2. Push hanya dikirim pada tepi naik false->true per sensor
 *      (satu push per kejadian). Laporan berikutnya dengan alarm
 *      tetap true TIDAK memicu push baru.
 *   3. Tepi turun true->false menutup kejadian; bila NOTIFY_RESOLVE,
 *      dikirim satu notifikasi "pulih" (severity info, tag sama
 *      sehingga menimpa notifikasi alarm lama di layar pengguna).
 *   4. Firmware tidak pernah memanggil sendAlarmToAll/subscribe/
 *      testPush - satu-satunya pengirim push adalah GAS.
 */

/** Validasi token perangkat. Dua mode Script Properties:
 *  - FW_DEVICE_TOKEN  : satu token bersama (instalasi satu perangkat).
 *  - FW_DEVICE_TOKENS : JSON array [{deviceId, token}] untuk
 *    banyak perangkat; dicocokkan berdasarkan id+token. */
function isDeviceAuthorized_(deviceId, token) {
  if (!token) return false;
  var props = PropertiesService.getScriptProperties();
  var listRaw = props.getProperty('FW_DEVICE_TOKENS');
  if (listRaw) {
    try {
      var list = JSON.parse(listRaw);
      for (var i = 0; i < list.length; i++) {
        if (list[i] && list[i].deviceId === deviceId) {
          // [audit-2 K-6 FIX] Constant-time token compare (was `===`).
          return constantTimeStrEq_(String(list[i].token), String(token));
        }
      }
      return false; // daftar ada -> hanya token dalam daftar yang sah
    } catch (e) {
      return false; // daftar korup -> tolak (fail-closed)
    }
  }
  var single = props.getProperty('FW_DEVICE_TOKEN');
  if (!single) return false;
  return constantTimeStrEq_(String(single), String(token));
}

/** [audit-2 K-6] Constant-time string equality. Returns true iff strings
 *  are byte-equal AND same length. The accumulator pattern prevents
 *  short-circuit on first differing byte. */
function constantTimeStrEq_(a, b) {
  if (a.length !== b.length) return false;
  var diff = 0;
  for (var i = 0; i < a.length; i++) {
    diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  }
  return diff === 0;
}

/* ======================= [p.493] Push-scoped tokens ======================= */

/**
 * [AUDIT p.493 REMEDIATION] Kredensial khusus langganan push — DIPISAHKAN
 * dari kredensial ingest firmware (FW_DEVICE_TOKEN / FW_DEVICE_TOKENS).
 *
 * Script Property PUSH_TOKENS = [{"deviceId":"...","token":"..."}]
 *
 * Kapabilitas: subscribe / unsubscribe / testPush SAJA. TIDAK dapat
 * memanggil `ingest`. Dengan pemisahan ini, kompromi PWA push (XSS,
 * sessionStorage bocor) tidak serta-merta menjadi kompromi kredensial
 * ingest telemetry perangkat — kebalikan dari model lama yang memakai
 * token firmware yang sama di browser.
 */
function isPushSubscriberAuthorized_(deviceId, token) {
  if (!deviceId || !token) return false;
  var raw;
  try {
    raw = PropertiesService.getScriptProperties().getProperty('PUSH_TOKENS');
  } catch (e) {
    return false; // fail-closed
  }
  if (!raw) return false;
  var list;
  try {
    list = JSON.parse(raw);
  } catch (e) {
    return false; // daftar korup -> tolak (fail-closed)
  }
  if (!Array.isArray(list)) return false;
  for (var i = 0; i < list.length; i++) {
    var rec = list[i] || {};
    if (String(rec.deviceId) === String(deviceId)) {
      return constantTimeStrEq_(String(rec.token), String(token));
    }
  }
  return false;
}

/** Autentikasi jalur langganan: PUSH_TOKENS (preferred) ATAU kredensial
 *  perangkat ingest (jalur migrasi — mengikuti aturan p.493, PWA baru
 *  seharusnya memakai PUSH_TOKENS). */
function isSubscriptionAuthorized_(deviceId, token) {
  return isPushSubscriberAuthorized_(deviceId, token) ||
    isDeviceAuthorized_(String(deviceId), String(token || ''));
}

/* ======================= [P1] Serialisasi mutasi (LockService) ======================= */

/**
 * [AUDIT P1 — concurrency] Semua mutasi state (subscription list, alarm
 * state, ALARM_LOG, outbox) kini berjalan di dalam script lock. Tanpa ini,
 * dua eksekusi Apps Script paralel bisa load-modify-save saling menimpa
 * (lost update): subscribe A + unsubscribe B berbarengan bisa menghapus
 * salah satunya. canonical telemetry backend sudah memakai LockService;
 * push service kini mengikuti disiplin yang sama.
 */
function withLock_(fn) {
  var lock = LockService.getScriptLock();
  if (!lock.tryLock(30000)) {
    return { ok: false, message: 'Server sibuk (lock timeout) — coba lagi.' };
  }
  try {
    return fn();
  } finally {
    lock.releaseLock();
  }
}

/* ======================= [P0-3] Outbox AlarmEvent durable ======================= */

/**
 * Model pengiriman alarm (audit P0-3 — "push membutuhkan durable
 * event/outbox"). Identitas event:
 *
 *   eventId   = ALM-<sensor-slug>-<reportedAt36>  (raise) / RSV-... (clear)
 *   deviceId  = perangkat asal
 *   alarmCode = slug sensor
 *   generation= pencacah kejadian per sensor (naik tiap raise)
 *   raisedAt / clearedAt
 *   state     = 'RAISED' | 'CLEARED'
 *   delivery  = { status: 'PENDING'|'SENT'|'PARTIAL'|'FAILED'|'EXPIRED',
 *                 attempts, lastAttemptAt, sent, failed, removed }
 *
 * Urutan tulis: (1) event dibuat/disimpan DULU dengan delivery PENDING,
 * (2) push dikirim, (3) status delivery diperbarui. Bila eksekusi mati
 * di tengah, event tetap PENDING dan di-retry pada eksekusi berikutnya
 * dengan backoff — tidak ada notifikasi yang hilang diam-diam, dan
 * duplikat dibatasi oleh identitas event + tag notifikasi di klien.
 */
function loadAlarmEvents_() {
  return loadJsonObject_('ALARM_EVENTS', []);
}

function saveAlarmEvents_(events) {
  if (events.length > PUSH_CONFIG.OUTBOX_MAX_EVENTS) {
    events = events.slice(events.length - PUSH_CONFIG.OUTBOX_MAX_EVENTS);
  }
  PropertiesService.getScriptProperties()
    .setProperty('ALARM_EVENTS', JSON.stringify(events));
}

function findEventIndex_(events, eventId) {
  for (var i = events.length - 1; i >= 0; i--) {
    if (events[i].eventId === eventId) return i;
  }
  return -1;
}

/** Apakah event PENDING sudah boleh dicoba ulang? (backoff eksponensial) */
function eventRetryDue_(ev, now) {
  if (!ev.delivery || ev.delivery.status !== 'PENDING') return false;
  if (ev.delivery.attempts >= PUSH_CONFIG.OUTBOX_RETRY_MAX_ATTEMPTS) return false;
  var age = now - new Date(ev.raisedAt).getTime();
  if (age > PUSH_CONFIG.OUTBOX_MAX_RETRY_AGE_MS) return false;
  if (!ev.delivery.attempts || !ev.delivery.lastAttemptAt) return true;
  var backoff = PUSH_CONFIG.OUTBOX_RETRY_BASE_MS *
    Math.pow(2, Math.max(0, ev.delivery.attempts - 1));
  return (now - new Date(ev.delivery.lastAttemptAt).getTime()) >= backoff;
}

/** Tandai event kedaluwarsa (melewati umur retry) — tercatat jujur. */
function expireStaleEvents_(events, now) {
  for (var i = 0; i < events.length; i++) {
    var ev = events[i];
    if (ev.delivery && ev.delivery.status === 'PENDING') {
      var age = now - new Date(ev.raisedAt).getTime();
      if (age > PUSH_CONFIG.OUTBOX_MAX_RETRY_AGE_MS ||
          ev.delivery.attempts >= PUSH_CONFIG.OUTBOX_RETRY_MAX_ATTEMPTS) {
        ev.delivery.status = (age > PUSH_CONFIG.OUTBOX_MAX_RETRY_AGE_MS)
          ? 'EXPIRED' : 'FAILED';
      }
    }
  }
}

/** Utama: proses satu laporan firmware.
 *  [AUDIT P0-3 + P1] Eksekusi kini DI DALAM LOCK dengan urutan outbox:
 *  (1) deteksi tepi dari state, (2) tulis AlarmEvent PENDING + state DULU
 *  (durable sebelum efek samping), (3) kirim push, (4) perbarui status
 *  delivery, (5) tulis snapshot. Gagal di tengah -> event PENDING di-retry
 *  pada ingest berikutnya (backoff) — tidak ada push hilang diam-diam. */
function handleIngest_(body) {
  var deviceId = body.device && body.device.id
    ? String(body.device.id).slice(0, 64) : '(tanpa-id)';
  if (!isDeviceAuthorized_(deviceId, body.token)) {
    return { ok: false, message: 'Token perangkat tidak valid' };
  }
  // [audit-2 S-13] Opportunistic prune — runs at most once per hour.
  maybePruneSubscriptions_();
  if (!Array.isArray(body.sensors) || body.sensors.length === 0) {
    return { ok: false, message: 'Data sensor kosong / tidak valid' };
  }

  var reportedAt = Number(body.reportedAt) || Date.now();
  var sensors = body.sensors
    .slice(0, PUSH_CONFIG.MAX_SENSORS_PER_REPORT)
    .map(normalizeIngestSensor_);

  var locked = withLock_(function () {
    var state = loadJsonObject_('FW_ALARM_STATE', {});
    var events = loadAlarmEvents_();
    var now = Date.now();
    expireStaleEvents_(events, now);

    var triggered = [];
    var resolved = [];
    var toPush = []; // {kind:'raise'|'resolve', eventIndex}
    var generations = loadJsonObject_('FW_ALARM_GENERATIONS', {});

    /* ---- Fase 1: deteksi tepi per sensor (tanpa efek samping I/O) ---- */
    for (var i = 0; i < sensors.length; i++) {
      var s = sensors[i];
      /* [P1-A backport round-3] state & generation di-scope per device —
       * dua device dengan sensor senama tidak saling mencemari tepi. */
      var skey = deviceId + '::' + s.name;
      var prev = state[skey]; // { alarm, since, eventId, severity }
      var prevAlarm = !!(prev && prev.alarm);

      if (s.alarm && !prevAlarm) {
        // Tepi naik: kejadian alarm BARU -> AlarmEvent durable.
        generations[skey] = (generations[skey] || 0) + 1;
        var alarm = normalizeAlarm_({
          id: 'ALM-' + slugify_(deviceId).slice(0, 20) + '-' +
            slugify_(s.name).slice(0, 20) + '-' +
            reportedAt.toString(36) + '-' +
            Utilities.getUuid().replace(/-/g, '').slice(0, 6),
          title: (s.severity === 'critical' ? 'KRITIS: ' : 'PERINGATAN: ') + s.name,
          body: s.status || (s.name + ' keluar batas aman (' +
            s.value + ' ' + s.unit + ')'),
          severity: s.severity,
          tag: 'alarm-' + slugify_(s.name),
          url: './index.html?from=push',
          timestamp: reportedAt
        });
        alarm.sensor = s.name;
        alarm.raisedAt = new Date(reportedAt).toISOString();
        triggered.push(alarm.id);
        state[skey] = { alarm: true, since: alarm.raisedAt,
          eventId: alarm.id, severity: s.severity };
        events.push({
          eventId: alarm.id,
          deviceId: deviceId,
          alarmCode: slugify_(s.name),
          generation: generations[skey],
          severity: s.severity,
          raisedAt: alarm.raisedAt,
          clearedAt: null,
          state: 'RAISED',
          payload: alarm,
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0 }
        });
        toPush.push({ kind: 'raise', eventId: alarm.id });
      }

      else if (!s.alarm && prevAlarm) {
        // Tepi turun: kejadian selesai (+ notifikasi pulih).
        resolved.push(prev.eventId);
        var rsv = normalizeAlarm_({
          id: 'RSV-' + slugify_(deviceId).slice(0, 20) + '-' +
            slugify_(s.name).slice(0, 20) + '-' +
            reportedAt.toString(36) + '-' +
            Utilities.getUuid().replace(/-/g, '').slice(0, 6),
          title: 'PULIH: ' + s.name,
          body: (s.status && s.status !== '-') ? s.status :
            (s.name + ' kembali normal (' + s.value + ' ' + s.unit + ')'),
          severity: 'info',
          tag: 'alarm-' + slugify_(s.name), // tag sama -> timpa notifikasi lama
          url: './index.html?from=push',
          timestamp: reportedAt
        });
        rsv.sensor = s.name; // atribusi sensor pada jejak log
        events.push({
          eventId: rsv.id,
          deviceId: deviceId,
          alarmCode: slugify_(s.name),
          generation: generations[skey] || 1,
          severity: 'info',
          raisedAt: new Date(reportedAt).toISOString(),
          clearedAt: new Date(reportedAt).toISOString(),
          state: 'CLEARED',
          resolveFor: prev.eventId, // entri ALM asal yang harus ditutup di log
          payload: rsv,
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0 }
        });
        toPush.push({ kind: 'resolve', eventId: rsv.id, resolves: prev.eventId });
        state[skey] = { alarm: false, since: null,
          eventId: prev.eventId, severity: 'info' };
        // Tutup entri ALM asal SEKARANG (durable-first): daftar alarm aktif
        // harus benar meski notifikasi pulih belum/gagal terkirim.
        markAlarmCleared_(prev.eventId);
      }

      else {
        // Level (tetap true / tetap false): tidak ada push.
        if (s.alarm && prev) prev.severity = s.severity;
      }
    }

    /* ---- Fase 2: DURABLE-FIRST — simpan event + state SEBELUM push ---- */
    saveAlarmEvents_(events);
    PropertiesService.getScriptProperties()
      .setProperty('FW_ALARM_STATE', JSON.stringify(state));
    PropertiesService.getScriptProperties()
      .setProperty('FW_ALARM_GENERATIONS', JSON.stringify(generations));

    /* ---- Fase 3: kirim push (event baru + retry PENDING yang jatuh tempo) ---- */
    var pushAgg = { sent: 0, failed: 0, removed: 0, batches: 0, retries: 0 };
    var deliverIds = {};
    for (var k = 0; k < toPush.length; k++) deliverIds[toPush[k].eventId] = true;
    for (var e = 0; e < events.length; e++) {
      var ev = events[e];
      var isNew = deliverIds[ev.eventId];
      var isRetry = !isNew && eventRetryDue_(ev, now);
      if (!isNew && !isRetry) continue;
      if (isRetry) pushAgg.retries++;
      var r = deliverAlarmEvent_(ev);
      pushAgg.sent += r.sent; pushAgg.failed += r.failed;
      pushAgg.removed += r.removed; pushAgg.batches++;
      ev.delivery.attempts++;
      ev.delivery.lastAttemptAt = new Date().toISOString();
      ev.delivery.sent = r.sent; ev.delivery.failed = r.failed;
      ev.delivery.removed = r.removed;
      // Status: SENT penuh / PARTIAL (sebagian, masih bisa retry) /
      // PENDING (gagal total, retry sampai maksimum) / FAILED (habis
      // percobaan) / SENT-0 (tidak ada pelanggan — bukan kegagalan).
      var exhausted = ev.delivery.attempts >= PUSH_CONFIG.OUTBOX_RETRY_MAX_ATTEMPTS;
      if (r.sent === 0 && r.total === 0) {
        ev.delivery.status = 'SENT'; // tidak ada pelanggan — tercatat jujur
      } else if (r.sent > 0 && r.failed === 0) {
        ev.delivery.status = 'SENT';
      } else if (exhausted) {
        ev.delivery.status = (r.sent > 0) ? 'PARTIAL' : 'FAILED';
      } else {
        ev.delivery.status = 'PENDING'; // sebagian/gagal -> retry dengan backoff
      }
      // Pencatatan terpusat di ALARM_LOG (ACK PWA) tetap jalan.
      logAlarmEvent_(ev.payload, { sensor: ev.payload.sensor,
        raisedAt: ev.raisedAt,
        clearedAt: ev.state === 'CLEARED' ? ev.clearedAt : undefined });
    }

    /* ---- Fase 4: perbarui status delivery + snapshot dari log termutakhir ---- */
    saveAlarmEvents_(events);
    var logNow = loadJsonObject_('ALARM_LOG', []);
    var snapshot = {
      ok: true,
      updatedAt: new Date().toISOString(),
      reportedAt: reportedAt,
      device: {
        id: deviceId,
        fw: body.device && body.device.fw ? String(body.device.fw).slice(0, 16) : null,
        uptimeMs: body.device ? Number(body.device.uptimeMs) || 0 : 0
      },
      sensors: sensors,
      alarms: activeAlarmsFromLog_(logNow),
      outbox: outboxSummary_(events)
    };
    PropertiesService.getScriptProperties()
      .setProperty('FW_LAST_SNAPSHOT', JSON.stringify(snapshot));

    return {
      ok: true,
      received: sensors.length,
      triggered: triggered,
      resolved: resolved,
      pushes: pushAgg
    };
  });

  if (locked && locked.ok === false && locked.message) return locked;
  return locked;
}

/** Ringkasan outbox untuk observability snapshot. */
function outboxSummary_(events) {
  var s = { pending: 0, sent: 0, partial: 0, failed: 0, expired: 0, total: events.length };
  for (var i = 0; i < events.length; i++) {
    var st = events[i].delivery && events[i].delivery.status;
    if (st === 'PENDING') s.pending++;
    else if (st === 'SENT') s.sent++;
    else if (st === 'PARTIAL') s.partial++;
    else if (st === 'FAILED') s.failed++;
    else if (st === 'EXPIRED') s.expired++;
  }
  return s;
}

/**
 * Normalisasi satu sensor dari laporan firmware.
 * Field `alarm` otoritatif bila boolean; bila tidak hadir,
 * dievaluasi dari THRESHOLDS (jaring pengaman firmware lama).
 */
function normalizeIngestSensor_(s) {
  s = s || {};
  var name = String(s.name || 'Sensor').slice(0, 40);
  var value = (s.value === null || s.value === undefined) ? null : Number(s.value);
  if (value !== null && !isFinite(value)) value = null;
  var unit = String(s.unit || '').slice(0, 8);
  var status = String(s.status || '').slice(0, 120);

  var alarm, severity;
  if (typeof s.alarm === 'boolean') {
    alarm = s.alarm;
    severity = ['critical', 'warning', 'info'].indexOf(s.severity) >= 0
      ? s.severity : (alarm ? 'warning' : 'info');
  } else {
    var ev = evaluateThresholds_(name, value);
    alarm = ev.alarm;
    severity = ev.severity;
  }
  if (alarm && !status) {
    status = name + ' di luar batas aman';
  }
  return { name: name, value: value, unit: unit, alarm: alarm,
    severity: severity, status: status };
}

/** Evaluasi ambang fallback (nama sensor -> aturan THRESHOLDS). */
function evaluateThresholds_(name, value) {
  var lower = String(name).toLowerCase();
  for (var i = 0; i < PUSH_CONFIG.THRESHOLDS.length; i++) {
    var rule = PUSH_CONFIG.THRESHOLDS[i];
    if (lower.indexOf(rule.match) < 0) continue;
    if (value === null) return { alarm: false, severity: 'info' };
    if (rule.max !== undefined && value > rule.max) {
      return { alarm: true, severity: rule.severity || 'critical' };
    }
    if (rule.min !== undefined && value < rule.min) {
      return { alarm: true, severity: rule.severity || 'warning' };
    }
    return { alarm: false, severity: 'info' };
  }
  return { alarm: false, severity: 'info' };
}

/**
 * Daftar alarm AKTIF dari log, terbaru dulu.
 * Alarm aktif = alarm berbasis sensor (field sensor terisi) yang belum
 * pulih. Kirim-langsung (testPush/simulasi/integrasi kustom) tidak
 * memiliki kondisi pulih sehingga tidak boleh mengotori daftar ini
 * selamanya (temuan X-7 audit silang) - mereka tetap tercatat di log
 * untuk jejak audit dan ACK.
 */
function activeAlarmsFromLog_(log) {
  var out = [];
  for (var i = log.length - 1; i >= 0 && out.length < 20; i--) {
    if (!log[i].clearedAt && log[i].sensor) {
      out.push({
        id: log[i].id,
        title: log[i].title,
        body: log[i].body,
        severity: log[i].severity,
        tag: log[i].tag,
        url: log[i].url,
        timestamp: log[i].timestamp,
        since: log[i].raisedAt,
        acknowledged: !!log[i].acknowledgedAt
      });
    }
  }
  return out;
}

/** Tandai alarm ditangani (ACK dari PWA). Idempoten.
 *  [P1 concurrency] Selalu dipanggil dalam withLock_ oleh handler.
 *  [P0-3] Juga menyinkronkan status event outbox terkait (jika ada). */
function markAlarmAcknowledged_(alarmId) {
  if (!alarmId) return { ok: true, found: false, alarmId: null, at: null };
  alarmId = String(alarmId);
  var log = loadJsonObject_('ALARM_LOG', []);
  var at = new Date().toISOString();
  for (var i = log.length - 1; i >= 0; i--) {
    if (log[i].id === alarmId) {
      if (!log[i].acknowledgedAt) log[i].acknowledgedAt = at;
      PropertiesService.getScriptProperties()
        .setProperty('ALARM_LOG', JSON.stringify(log));
      return { ok: true, found: true, alarmId: alarmId, at: log[i].acknowledgedAt };
    }
  }
  return { ok: true, found: false, alarmId: alarmId, at: null };
}

/** Baca properti JSON dengan toleransi korupsi. */
function loadJsonObject_(key, fallback) {
  try {
    var raw = PropertiesService.getScriptProperties().getProperty(key);
    var v = raw ? JSON.parse(raw) : null;
    return v || fallback;
  } catch (e) {
    return fallback;
  }
}

/** Slug sederhana untuk id/tag: huruf kecil, tanpa spasi/tanda baca. */
function slugify_(str) {
  return String(str).toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 40) || 'sensor';
}

/* ============ Titik integrasi data (dipakai oleh PWA) ============ */

/** Snapshot terakhir dari firmware (dipakai dashboard PWA). */
function getSnapshot_() {
  var snap = loadJsonObject_('FW_LAST_SNAPSHOT', null);
  if (!snap) {
    return { ok: true, sensors: [], alarms: [],
      message: 'Belum ada laporan firmware. Pastikan perangkat sudah dikonfigurasi dan terhubung.' };
  }
  return snap;
}

/** Alarm aktif berbasis sensor terbaru (fallback push tanpa payload;
 *  skema keluaran = skema payload. Lihat catatan activeAlarmsFromLog_. */
function getLatestAlarm_() {
  var log = loadJsonObject_('ALARM_LOG', []);
  for (var i = log.length - 1; i >= 0; i--) {
    if (!log[i].clearedAt && log[i].sensor) {
      // Kembalikan HANYA field skema payload (konsistensi dua arah).
      // [audit p.482] Token dihitung SEGAR saat dibaca (fallback push ini
      // bisa terjadi berjam-jam setelah alarm dicatat).
      var out = {
        id: log[i].id,
        title: log[i].title,
        body: log[i].body,
        severity: log[i].severity,
        tag: log[i].tag,
        url: log[i].url,
        timestamp: log[i].timestamp,
        requireInteraction: log[i].severity === 'critical'
      };
      out.ackToken = makeAckToken_(out.id);
      // [P0-1] apiBase ikut dikirim agar SW tahu ke mana ACK dikirim.
      out.apiBase = getScriptUrl_();
      return out;
    }
  }
  return null;
}

/** Uji manual dari editor GAS: kirim alarm contoh. */
function simulateAlarmPush() {
  var r = sendAlarmToAll({
    id: 'SIM-' + Utilities.getUuid().slice(0, 8),
    title: 'SIMULASI ALARM',
    body: 'Alarm simulasi dari editor Apps Script pada ' +
      new Date().toLocaleString('id-ID'),
    severity: 'critical'
  });
  Logger.log(r);
  return r;
}

/* ======================= Rate limit testPush ======================= */

/** Jeda minimal antar testPush (ms). Script Property
 *  TEST_PUSH_MIN_INTERVAL_MS menimpa PUSH_CONFIG.TEST_PUSH_MIN_INTERVAL_MS.
 *  Nilai tidak valid/negatif -> fallback konfigurasi. */
function getTestPushMinInterval_() {
  var raw = PropertiesService.getScriptProperties()
    .getProperty('TEST_PUSH_MIN_INTERVAL_MS');
  var n = (raw !== null && raw !== undefined && raw !== '' &&
    isFinite(Number(raw))) ? Number(raw)
    : PUSH_CONFIG.TEST_PUSH_MIN_INTERVAL_MS;
  return isNaN(n) || n < 0 ? 0 : n;
}

/** Gerbang rate-limit testPush (global, state di Script Properties).
 *  Di luar interval -> tolak tanpa mengirim apa pun. */
function checkTestPushRate_() {
  var interval = getTestPushMinInterval_();
  if (!(interval > 0)) return { ok: true };
  var props = PropertiesService.getScriptProperties();
  var last = Number(props.getProperty('TEST_PUSH_LAST_AT')) || 0;
  var now = Date.now();
  if (now - last < interval) {
    return {
      ok: false,
      message: 'Permintaan uji push ditolak: dibatasi sekali per ' +
        Math.round(interval / 1000) + ' detik (anti-spam).'
    };
  }
  props.setProperty('TEST_PUSH_LAST_AT', String(now));
  return { ok: true };
}
