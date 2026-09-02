/**
 * Code.gs (= PushService) - Modul pengirim Web Push (VAPID) untuk GAS.
 * =======================================================================
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
 *     Opsional (Script Properties tambahan):
 *       VAPID_SUBJECT             = kontak pengirim (mailto: atau https:)
 *                                    menimpa PUSH_CONFIG.SUBJECT.
 *       TEST_PUSH_MIN_INTERVAL_MS = jeda minimal antar testPush publik
 *                                    (default 60000; 0 = tanpa batas).
 *  3. Deploy Web App: Execute as "Me", Access "Anyone".
 *  4. Salin URL deployment ke js/config.js PWA (API_BASE) dan sw.js.
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
 * API UTAMA - kirim alarm ke semua perangkat.
 * Panggil dari handler data sensor / time-driven trigger Anda.
 * Setiap alarm yang melewati fungsi ini DICATAT ke ALARM_LOG secara
 * terpusat (temuan X-6 audit silang) sehingga ACK dari PWA diterima
 * untuk SEMUA alarm yang pernah terkirim, apa pun jalurnya.
 * @param {object} alarm { id, title, body, severity, url }
 * @returns {{sent:number, failed:number, removed:number}}
 */
function sendAlarmToAll(alarm) {
  var payload = normalizeAlarm_(alarm);
  logAlarmEvent_(payload, alarm);
  var subs = getSubscriptions_();
  var sent = 0, failed = 0, removed = 0;

  for (var i = 0; i < subs.length && i < PUSH_CONFIG.BATCH_SIZE; i++) {
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
    var sub = {
      endpoint: ep,
      keys: { p256dh: body.keys.p256dh, auth: body.keys.auth },
      context: body.context || {},
      addedAt: new Date().toISOString(),
      lastSeenAt: new Date().toISOString()
    };
    var total = upsertSubscription_(sub);
    return jsonOut_({ ok: true, message: 'Langganan tersimpan', total: total });
  }

  if (body.action === 'unsubscribe') {
    var removedN = removeSubscription_(body.endpoint);
    return jsonOut_({ ok: true, message: 'Langganan dihapus', removed: removedN });
  }

  if (body.action === 'ackAlarm') {
    var ackRes = markAlarmAcknowledged_(body.alarmId);
    if (!ackRes.found) {
      return jsonOut_({ ok: false, message: 'Alarm tidak ditemukan: ' + body.alarmId });
    }
    return jsonOut_({ ok: true, message: 'Alarm ditandai ditangani',
      alarmId: ackRes.alarmId, acknowledgedAt: ackRes.at });
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
    // Endpoint ini terbuka (deploy "Anyone") sehingga bisa dipakai spam
    // push. Rate-limit global memutus vektor banjir tanpa merusak tombol
    // "Uji Push" PWA (hardening produksi; konfigurasi di PUSH_CONFIG).
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
        if (list[i] && list[i].deviceId === deviceId && list[i].token === token) {
          return true;
        }
      }
      return false; // daftar ada -> hanya token dalam daftar yang sah
    } catch (e) {
      return false; // daftar korup -> tolak (fail-closed)
    }
  }
  var single = props.getProperty('FW_DEVICE_TOKEN');
  return !!single && single === token;
}

/** Utama: proses satu laporan firmware.
 *  Tiga fase agar ALARM_LOG bebas lost-update:
 *  (1) deteksi tepi dari state, (2) kirim push (pencatatan terpusat
 *  di sendAlarmToAll), (3) tandai pulih dengan log termutakhir. */
function handleIngest_(body) {
  var deviceId = body.device && body.device.id
    ? String(body.device.id).slice(0, 64) : '(tanpa-id)';
  if (!isDeviceAuthorized_(deviceId, body.token)) {
    return { ok: false, message: 'Token perangkat tidak valid' };
  }
  if (!Array.isArray(body.sensors) || body.sensors.length === 0) {
    return { ok: false, message: 'Data sensor kosong / tidak valid' };
  }

  var reportedAt = Number(body.reportedAt) || Date.now();
  var sensors = body.sensors
    .slice(0, PUSH_CONFIG.MAX_SENSORS_PER_REPORT)
    .map(normalizeIngestSensor_);

  var state = loadJsonObject_('FW_ALARM_STATE', {});
  var triggered = [];
  var resolved = [];
  var toPush = []; // {kind:'raise'|'resolve', alarm, eventId?}
  var pushAgg = { sent: 0, failed: 0, removed: 0, batches: 0 };

  /* ---- Fase 1: deteksi tepi per sensor (tanpa efek samping I/O) ---- */
  for (var i = 0; i < sensors.length; i++) {
    var s = sensors[i];
    var prev = state[s.name]; // { alarm, since, eventId, severity }
    var prevAlarm = !!(prev && prev.alarm);

    if (s.alarm && !prevAlarm) {
      // Tepi naik: kejadian alarm BARU (push satu kali).
      var alarm = normalizeAlarm_({
        id: 'ALM-' + slugify_(s.name) + '-' + reportedAt.toString(36),
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
      state[s.name] = { alarm: true, since: alarm.raisedAt,
        eventId: alarm.id, severity: s.severity };
      toPush.push({ kind: 'raise', alarm: alarm });
    }

    else if (!s.alarm && prevAlarm) {
      // Tepi turun: kejadian selesai (+ notifikasi pulih).
      resolved.push(prev.eventId);
      var rsv = normalizeAlarm_({
        id: 'RSV-' + slugify_(s.name) + '-' + reportedAt.toString(36),
        title: 'PULIH: ' + s.name,
        body: (s.status && s.status !== '-') ? s.status :
          (s.name + ' kembali normal (' + s.value + ' ' + s.unit + ')'),
        severity: 'info',
        tag: 'alarm-' + slugify_(s.name), // tag sama -> timpa notifikasi lama
        url: './index.html?from=push',
        timestamp: reportedAt
      });
      rsv.sensor = s.name; // atribusi sensor pada jejak log
      toPush.push({ kind: 'resolve', eventId: prev.eventId, alarm: rsv });
      state[s.name] = { alarm: false, since: null,
        eventId: prev.eventId, severity: 'info' };
    }

    else {
      // Level (tetap true / tetap false): tidak ada push.
      if (s.alarm && prev) prev.severity = s.severity;
    }
  }

  /* ---- Fase 2: kirim push + pencatatan terpusat ---- */
  for (var k = 0; k < toPush.length; k++) {
    var item = toPush[k];
    var r = sendAlarmToAll(item.alarm);
    pushAgg.sent += r.sent; pushAgg.failed += r.failed;
    pushAgg.removed += r.removed; pushAgg.batches++;
    if (item.kind === 'resolve') {
      markAlarmCleared_(item.eventId); // log segar, aman
    }
  }

  /* ---- Fase 3: simpan state + snapshot dari log termutakhir ---- */
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
    alarms: activeAlarmsFromLog_(logNow)
  };
  PropertiesService.getScriptProperties()
    .setProperty('FW_LAST_SNAPSHOT', JSON.stringify(snapshot));
  PropertiesService.getScriptProperties()
    .setProperty('FW_ALARM_STATE', JSON.stringify(state));

  return {
    ok: true,
    received: sensors.length,
    triggered: triggered,
    resolved: resolved,
    pushes: pushAgg
  };
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

/** Tandai alarm ditangani (ACK dari PWA). Idempoten. */
function markAlarmAcknowledged_(alarmId) {
  if (!alarmId) return { found: false, alarmId: null };
  alarmId = String(alarmId);
  var log = loadJsonObject_('ALARM_LOG', []);
  var at = new Date().toISOString();
  for (var i = log.length - 1; i >= 0; i--) {
    if (log[i].id === alarmId) {
      if (!log[i].acknowledgedAt) log[i].acknowledgedAt = at;
      PropertiesService.getScriptProperties()
        .setProperty('ALARM_LOG', JSON.stringify(log));
      return { found: true, alarmId: alarmId, at: log[i].acknowledgedAt };
    }
  }
  return { found: false, alarmId: alarmId };
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
      return {
        id: log[i].id,
        title: log[i].title,
        body: log[i].body,
        severity: log[i].severity,
        tag: log[i].tag,
        url: log[i].url,
        timestamp: log[i].timestamp,
        requireInteraction: log[i].severity === 'critical'
      };
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
