/**
 * PushService.gs — [AUDIT P0-2 REMEDIATION 2026-09-16]
 * =======================================================================
 * Modul Web Push untuk backend CANONICAL (satu trust boundary). Menutup
 * temuan auditor: "push alarm masih menggunakan backend contract terpisah
 * dari canonical GAS/HMAC". Target arsitektur akhir kini tersedia di sini:
 *
 *   ESP32 ──HMAC──► canonical GAS (Code.gs)
 *                     ├─ TELEMETRY ─► Sheets (+ emergency edge → push)
 *                     ├─ PUSH_ALARM_INGEST ─► AlarmEvent outbox ─► Web Push
 *                     └─ PUSH_SUBSCRIBE / PUSH_UNSUBSCRIBE / PUSH_ACK
 *                          (browser, push-scoped token — p.493)
 *
 * Kontrak yang SAMA dengan push-alarm/gas/Code.gs (legacy, kini hanya untuk
 * migrasi): outbox durable P0-3, LockService pada semua mutasi, deviceId
 * binding pada record langganan, testPush terautentikasi admin, kredensial
 * push terpisah dari kredensial ingest.
 *
 * PASANGAN WAJIB: tempel juga WebPushCore.gs (dari push-alarm/gas/) ke
 * proyek Apps Script yang sama — berisi kripto Web Push (P-256, AES-GCM).
 *
 * SCRIPT PROPERTIES:
 *   VAPID_PUBLIC_KEY / VAPID_PRIVATE_KEY  (hasil generate-vapid-keys.js)
 *   PUSH_TOKENS = [{"deviceId":"...","token":"..."}]   (p.493: kapabilitas
 *       subscribe/unsubscribe SAJA; bisa juga di-set di Config sheet dengan
 *       key PUSH_TOKENS — Script Property menang)
 *   PUSH_ACK_SECRET        (auto-provision 64-hex; rotasi manual)
 *   PUSH_OUTBOX_*          (opsional — lihat PUSH_CFG di bawah)
 */

const PUSH_CFG = {
  SUBJECT: 'mailto:admin@plts-monitoring.local',
  TTL: 86400,                       // umur pesan di push service (24 jam)
  MAX_PAYLOAD_CHARS: 3000,
  MAX_SUBSCRIPTIONS: 50,            // cap pertumbuhan (LRU di atas ini)
  PRUNE_AFTER_DAYS: 90,
  OUTBOX_MAX_EVENTS: 200,
  OUTBOX_RETRY_MAX_ATTEMPTS: 6,
  OUTBOX_RETRY_BASE_MS: 60000,
  OUTBOX_MAX_RETRY_AGE_MS: 24 * 60 * 60 * 1000,
  ACK_TOKEN_BUCKET_MS: 6 * 60 * 60 * 1000   // 6 jam per bucket
};

/* ======================= Penyimpanan (Script Properties) ======================= */

function pushProps_() {
  return PropertiesService.getScriptProperties();
}

function pushLoadJson_(key, fallback) {
  try {
    const raw = pushProps_().getProperty(key);
    const v = raw ? JSON.parse(raw) : null;
    return v || fallback;
  } catch (e) {
    return fallback;
  }
}

function pushSaveJson_(key, value) {
  pushProps_().setProperty(key, JSON.stringify(value));
}

/* ======================= Kredensial push-scoped (p.493) ======================= */

/**
 * PUSH_TOKENS — kredensial khusus langganan, DIPISAHKAN dari kredensial
 * device canonical (Devices sheet `secret` untuk HMAC ingest). Kapabilitas
 * TERBATAS: PUSH_SUBSCRIBE / PUSH_UNSUBSCRIBE. Sumber: Script Property
 * PUSH_TOKENS (prioritas) atau baris Config sheet key='PUSH_TOKENS'.
 */
function pushTokens_() {
  const rawProp = pushProps_().getProperty('PUSH_TOKENS');
  if (rawProp) {
    try {
      const list = JSON.parse(rawProp);
      if (Array.isArray(list)) return list;
    } catch (e) { /* korup -> lanjut ke Config sheet */ }
  }
  try {
    const cfg = String(getPltsConfig('PUSH_TOKENS') || '');
    const list = cfg ? JSON.parse(cfg) : [];
    return Array.isArray(list) ? list : [];
  } catch (e) {
    return [];
  }
}

function pushConstantTimeEq_(a, b) {
  a = String(a); b = String(b);
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

function pushSubscriberAuthorized_(deviceId, token) {
  if (!deviceId || !token) return false;
  const list = pushTokens_();
  for (let i = 0; i < list.length; i++) {
    const rec = list[i] || {};
    if (String(rec.deviceId) === String(deviceId)) {
      return pushConstantTimeEq_(rec.token, token);
    }
  }
  return false;
}

/* ======================= Langganan (device-bound) ======================= */

function pushSubscriptions_() {
  return pushLoadJson_('PUSH_SUBSCRIPTIONS', []);
}

function pushSaveSubscriptions_(subs) {
  pushSaveJson_('PUSH_SUBSCRIPTIONS', subs);
}

function pushUpsertSubscription_(sub) {
  const subs = pushSubscriptions_();
  let found = false;
  for (let i = 0; i < subs.length; i++) {
    if (subs[i].endpoint === sub.endpoint) { subs[i] = sub; found = true; break; }
  }
  if (!found) subs.push(sub);
  pushSaveSubscriptions_(subs);
  return subs.length;
}

/** [P1 ownership] Hanya record milik device (atau record lama tanpa
 *  deviceId) yang boleh dihapus. */
function pushRemoveSubscriptionOwned_(endpoint, deviceId) {
  const subs = pushSubscriptions_();
  const kept = subs.filter(function (s) {
    if (s.endpoint !== endpoint) return true;
    if (!s.deviceId) return false;
    return String(s.deviceId) !== String(deviceId);
  });
  if (kept.length !== subs.length) pushSaveSubscriptions_(kept);
  return subs.length - kept.length;
}

/* ======================= ACK capability token (p.482, paritas legacy) ======================= */

function pushAckSecret_() {
  let secret = pushProps_().getProperty('PUSH_ACK_SECRET');
  if (!secret) {
    secret = Utilities.getUuid().replace(/-/g, '') +
             Utilities.getUuid().replace(/-/g, '');
    pushProps_().setProperty('PUSH_ACK_SECRET', secret);
  }
  return secret;
}

function pushAckBucket_(shift) {
  shift = shift || 0;
  return Math.floor(Date.now() / PUSH_CFG.ACK_TOKEN_BUCKET_MS) - shift;
}

function pushComputeAckToken_(alarmId, bucket) {
  const sig = Utilities.computeHmacSha256Signature(
    String(alarmId) + '|' + String(bucket), pushAckSecret_());
  return sig.map(function (b) { return ('0' + (b & 0xff).toString(16)).slice(-2); }).join('');
}

function pushMakeAckToken_(alarmId) {
  return pushComputeAckToken_(alarmId, pushAckBucket_(0));
}

function pushVerifyAckToken_(alarmId, token) {
  if (!alarmId || typeof token !== 'string' || token.length === 0) return false;
  const a = String(token).toLowerCase();
  return a === pushComputeAckToken_(alarmId, pushAckBucket_(0)) ||
         a === pushComputeAckToken_(alarmId, pushAckBucket_(1));
}

/* ======================= Outbox AlarmEvent (P0-3) ======================= */

function pushEvents_() {
  return pushLoadJson_('PUSH_ALARM_EVENTS', []);
}

function pushSaveEvents_(events) {
  if (events.length > PUSH_CFG.OUTBOX_MAX_EVENTS) {
    events = events.slice(events.length - PUSH_CFG.OUTBOX_MAX_EVENTS);
  }
  pushSaveJson_('PUSH_ALARM_EVENTS', events);
}

function pushEventRetryDue_(ev, now) {
  if (!ev.delivery || ev.delivery.status !== 'PENDING') return false;
  if (ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS) return false;
  const age = now - new Date(ev.raisedAt).getTime();
  if (age > PUSH_CFG.OUTBOX_MAX_RETRY_AGE_MS) return false;
  if (!ev.delivery.attempts || !ev.delivery.lastAttemptAt) return true;
  const backoff = PUSH_CFG.OUTBOX_RETRY_BASE_MS *
    Math.pow(2, Math.max(0, ev.delivery.attempts - 1));
  return (now - new Date(ev.delivery.lastAttemptAt).getTime()) >= backoff;
}

function pushExpireStaleEvents_(events, now) {
  for (let i = 0; i < events.length; i++) {
    const ev = events[i];
    if (ev.delivery && ev.delivery.status === 'PENDING') {
      const age = now - new Date(ev.raisedAt).getTime();
      if (age > PUSH_CFG.OUTBOX_MAX_RETRY_AGE_MS) ev.delivery.status = 'EXPIRED';
      else if (ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS) ev.delivery.status = 'FAILED';
    }
  }
}

/** Ringkasan outbox untuk observability. */
function pushOutboxSummary_(events) {
  const s = { pending: 0, sent: 0, partial: 0, failed: 0, expired: 0, total: events.length };
  for (let i = 0; i < events.length; i++) {
    const st = events[i].delivery && events[i].delivery.status;
    if (st === 'PENDING') s.pending++;
    else if (st === 'SENT') s.sent++;
    else if (st === 'PARTIAL') s.partial++;
    else if (st === 'FAILED') s.failed++;
    else if (st === 'EXPIRED') s.expired++;
  }
  return s;
}

/* ======================= Pengiriman ======================= */

function pushVapidKeys_() {
  const pub = pushProps_().getProperty('VAPID_PUBLIC_KEY');
  const priv = pushProps_().getProperty('VAPID_PRIVATE_KEY');
  if (!pub || !priv) {
    throw new Error('VAPID_PUBLIC_KEY / VAPID_PRIVATE_KEY belum diset di Script Properties (hasil generate-vapid-keys.js).');
  }
  return { publicKey: pub, privateKey: priv };
}

function pushScriptUrl_() {
  try {
    const url = ScriptApp.getService().getUrl();
    return url ? String(url) : null;
  } catch (e) {
    return null;
  }
}

/** Kirim satu payload terenkripsi ke satu langganan. */
function pushWebSend_(subscription, payloadObj, urgency) {
  let payloadStr = JSON.stringify(payloadObj);
  if (payloadStr.length > PUSH_CFG.MAX_PAYLOAD_CHARS) {
    payloadObj.body = String(payloadObj.body || '').slice(0, 160) + '...';
    payloadStr = JSON.stringify(payloadObj);
  }
  const keys = pushVapidKeys_();
  const enc = WebPushCore.encryptPayload(subscription, payloadStr);
  const vapid = WebPushCore.vapidHeaders(
    subscription.endpoint, keys.privateKey, keys.publicKey,
    String(pushProps_().getProperty('VAPID_SUBJECT') || PUSH_CFG.SUBJECT), 12 * 3600);
  const options = {
    method: 'post',
    payload: Utilities.newBlob(enc.body, 'application/octet-stream'),
    headers: {
      'Authorization': vapid.Authorization,
      'TTL': String(PUSH_CFG.TTL),
      'Urgency': urgency || 'high',
      'Content-Encoding': 'aes128gcm'
    },
    muteHttpExceptions: true,
    followRedirects: true
  };
  try {
    const res = UrlFetchApp.fetch(subscription.endpoint, options);
    const code = res.getResponseCode();
    if (code === 404 || code === 410) {
      pushRemoveSubscriptionOwned_(subscription.endpoint, subscription.deviceId);
      return { ok: false, status: code, error: 'endpoint-mati' };
    }
    if (code >= 200 && code < 300) return { ok: true, status: code, error: '' };
    return { ok: false, status: code, error: res.getContentText().slice(0, 200) };
  } catch (err) {
    return { ok: false, status: 0, error: String(err).slice(0, 200) };
  }
}

/** Kirim SATU AlarmEvent ke langganan device terkait (+ record lama tanpa
 *  binding). Payload membawa ackToken (p.482) dan apiBase (P0-1). */
function pushDeliverEvent_(ev) {
  const payload = ev.payload;
  payload.ackToken = pushMakeAckToken_(payload.id);
  payload.apiBase = pushScriptUrl_();
  const subs = pushSubscriptions_();
  const scoped = subs.filter(function (s) {
    return !s.deviceId || String(s.deviceId) === String(ev.deviceId);
  });
  let sent = 0, failed = 0, removed = 0;
  for (let i = 0; i < scoped.length; i++) {
    const r = pushWebSend_(scoped[i], payload,
      payload.severity === 'critical' ? 'high' : 'normal');
    if (r.ok) sent++;
    else { failed++; if (r.error === 'endpoint-mati') removed++; }
  }
  return { sent: sent, failed: failed, removed: removed, total: scoped.length };
}

/** Buat event baru (atau lengkapi) + kirim dalam lock. Durable-first. */
function pushAppendAndDeliver_(events, ev) {
  events.push(ev);
  pushSaveEvents_(events);            // (1) durable DULU
  const r = pushDeliverEvent_(ev);    // (2) kirim
  ev.delivery.attempts++;             // (3) status delivery terakhir
  ev.delivery.lastAttemptAt = new Date().toISOString();
  ev.delivery.sent = r.sent; ev.delivery.failed = r.failed; ev.delivery.removed = r.removed;
  const exhausted = ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS;
  if (r.sent === 0 && r.total === 0) ev.delivery.status = 'SENT';
  else if (r.sent > 0 && r.failed === 0) ev.delivery.status = 'SENT';
  else if (exhausted) ev.delivery.status = (r.sent > 0) ? 'PARTIAL' : 'FAILED';
  else ev.delivery.status = 'PENDING';
  pushSaveEvents_(events);
  return r;
}

/** Ulangi pengiriman event PENDING yang jatuh tempo (dipanggil dari ingest
 *  dan time-driven trigger opsional pushRetryPending). */
function pushRetryPending_() {
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(20000)) return { ok: false, retried: 0 };
  try {
    const events = pushEvents_();
    const now = Date.now();
    pushExpireStaleEvents_(events, now);
    let retried = 0;
    for (let i = 0; i < events.length; i++) {
      const ev = events[i];
      if (!pushEventRetryDue_(ev, now)) continue;
      const r = pushDeliverEvent_(ev);
      ev.delivery.attempts++;
      ev.delivery.lastAttemptAt = new Date().toISOString();
      ev.delivery.sent = r.sent; ev.delivery.failed = r.failed;
      ev.delivery.removed = r.removed;
      const exhausted = ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS;
      if (r.sent > 0 && r.failed === 0) ev.delivery.status = 'SENT';
      else if (exhausted) ev.delivery.status = (r.sent > 0) ? 'PARTIAL' : 'FAILED';
      else ev.delivery.status = 'PENDING';
      retried++;
    }
    pushSaveEvents_(events);
    return { ok: true, retried: retried };
  } finally {
    lock.releaseLock();
  }
}

/* ======================= Handler aksi browser (p.493) ======================= */

/**
 * PUSH_SUBSCRIBE / PUSH_UNSUBSCRIBE / PUSH_ACK — dipanggil PWA standalone.
 * Autentikasi: push token (PUSH_TOKENS) — BUKAN kredensial device HMAC.
 */
function pushHandleBrowserAction_(action, body) {
  if (action === 'PUSH_SUBSCRIBE') {
    if (!body.device || !body.device.id || !body.token) {
      return resp_(401, 'PUSH_SUBSCRIBE requires device.id + push token (p.493)', null);
    }
    if (!pushSubscriberAuthorized_(String(body.device.id), body.token)) {
      return resp_(401, 'Invalid push token', null);
    }
    if (!body.endpoint || !body.keys || !body.keys.p256dh || !body.keys.auth) {
      return resp_(400, 'Incomplete subscription payload', null);
    }
    const ep = String(body.endpoint);
    if (ep.indexOf('https://') !== 0 || ep.length > 500) {
      return resp_(400, 'Subscription endpoint must be https and <=500 chars', null);
    }
    const lock = LockService.getScriptLock();
    if (!lock.tryLock(20000)) return resp_(503, 'Backend busy — retry', null);
    try {
      let subs = pushSubscriptions_();
      if (subs.length >= PUSH_CFG.MAX_SUBSCRIPTIONS) {
        subs.sort(function (a, b) {
          return new Date(a.lastSeenAt || a.addedAt).getTime() -
                 new Date(b.lastSeenAt || b.addedAt).getTime();
        });
        subs = subs.slice(subs.length - (PUSH_CFG.MAX_SUBSCRIPTIONS - 1));
        pushSaveSubscriptions_(subs);
      }
      const total = pushUpsertSubscription_({
        endpoint: ep,
        keys: { p256dh: body.keys.p256dh, auth: body.keys.auth },
        deviceId: String(body.device.id).slice(0, 64),
        context: body.context || {},
        addedAt: new Date().toISOString(),
        lastSeenAt: new Date().toISOString()
      });
      return resp_(200, 'Subscription stored', { total: total });
    } finally {
      lock.releaseLock();
    }
  }

  if (action === 'PUSH_UNSUBSCRIBE') {
    if (!body.device || !body.device.id || !body.token) {
      return resp_(401, 'PUSH_UNSUBSCRIBE requires device.id + push token', null);
    }
    if (!pushSubscriberAuthorized_(String(body.device.id), body.token)) {
      return resp_(401, 'Invalid push token', null);
    }
    const lock = LockService.getScriptLock();
    if (!lock.tryLock(20000)) return resp_(503, 'Backend busy — retry', null);
    try {
      const removed = pushRemoveSubscriptionOwned_(body.endpoint,
        String(body.device.id).slice(0, 64));
      return resp_(200, 'Subscription removed', { removed: removed });
    } finally {
      lock.releaseLock();
    }
  }

  if (action === 'PUSH_ACK') {
    if (!pushVerifyAckToken_(body.alarmId, body.ackToken)) {
      return resp_(401, 'ACK rejected: invalid/missing capability token (p.482)', null);
    }
    const lock = LockService.getScriptLock();
    if (!lock.tryLock(20000)) return resp_(503, 'Backend busy — retry', null);
    try {
      const events = pushEvents_();
      for (let i = events.length - 1; i >= 0; i--) {
        if (events[i].eventId === String(body.alarmId)) {
          if (!events[i].acknowledgedAt) {
            events[i].acknowledgedAt = new Date().toISOString();
            pushSaveEvents_(events);
          }
          return resp_(200, 'Alarm acknowledged',
            { alarmId: events[i].eventId, acknowledgedAt: events[i].acknowledgedAt });
        }
      }
      return resp_(404, 'Alarm event not found: ' + body.alarmId, null);
    } finally {
      lock.releaseLock();
    }
  }

  return resp_(400, 'Unknown push action: ' + action, null);
}

/* ======================= PUSH_ALARM_INGEST (jalur firmware) ======================= */

/**
 * Kontrak laporan sensor (identik legacy — skema "Ambang alarm FW-GAS"):
 *   { sensors: [{ name, value, unit, alarm?, severity?, status? }], reportedAt }
 * Autentikasi: envelope canonical (HMAC device ATAU legacy token) — SATU
 * trust boundary dengan telemetry. Edge detection + outbox + delivery.
 */
function pushAlarmIngest_(payload, deviceKey) {
  payload = payload || {};
  if (!Array.isArray(payload.sensors) || payload.sensors.length === 0) {
    return resp_(400, 'PUSH_ALARM_INGEST requires a non-empty sensors[] array', null);
  }
  const reportedAt = Number(payload.reportedAt) || Date.now();
  const sensors = payload.sensors.slice(0, 24);

  const lock = LockService.getScriptLock();
  if (!lock.tryLock(20000)) return resp_(503, 'Backend busy — retry', null);
  try {
    const state = pushLoadJson_('PUSH_ALARM_STATE', {});
    const generations = pushLoadJson_('PUSH_ALARM_GENERATIONS', {});
    const events = pushEvents_();
    const now = Date.now();
    pushExpireStaleEvents_(events, now);

    const triggered = [];
    const resolved = [];

    for (let i = 0; i < sensors.length; i++) {
      const s = sensors[i] || {};
      const name = String(s.name || 'Sensor').slice(0, 40);
      const alarm = s.alarm === true;
      const severity = ['critical', 'warning', 'info'].indexOf(s.severity) >= 0
        ? s.severity : (alarm ? 'warning' : 'info');
      const prev = state[name];
      const prevAlarm = !!(prev && prev.alarm);

      if (alarm && !prevAlarm) {
        generations[name] = (generations[name] || 0) + 1;
        const id = 'ALM-' + name.toLowerCase().replace(/[^a-z0-9]+/g, '-') + '-' + reportedAt.toString(36);
        const ev = {
          eventId: id,
          deviceId: deviceKey,
          alarmCode: name.toLowerCase().replace(/[^a-z0-9]+/g, '-').slice(0, 40),
          generation: generations[name],
          severity: severity,
          raisedAt: new Date(reportedAt).toISOString(),
          clearedAt: null,
          state: 'RAISED',
          acknowledgedAt: null,
          payload: {
            id: id,
            title: (severity === 'critical' ? 'KRITIS: ' : 'PERINGATAN: ') + name,
            body: String(s.status || (name + ' di luar batas aman')).slice(0, 400),
            severity: severity,
            tag: 'alarm-' + name.toLowerCase().replace(/[^a-z0-9]+/g, '-'),
            url: './index.html?from=push',
            timestamp: reportedAt,
            requireInteraction: severity === 'critical'
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0 }
        };
        triggered.push(id);
        state[name] = { alarm: true, since: ev.raisedAt, eventId: id, severity: severity };
        pushAppendAndDeliver_(events, ev);
      } else if (!alarm && prevAlarm) {
        const id = 'RSV-' + name.toLowerCase().replace(/[^a-z0-9]+/g, '-') + '-' + reportedAt.toString(36);
        resolved.push(prev.eventId);
        const ev = {
          eventId: id,
          deviceId: deviceKey,
          alarmCode: name.toLowerCase().replace(/[^a-z0-9]+/g, '-').slice(0, 40),
          generation: generations[name] || 1,
          severity: 'info',
          raisedAt: new Date(reportedAt).toISOString(),
          clearedAt: new Date(reportedAt).toISOString(),
          state: 'CLEARED',
          acknowledgedAt: null,
          payload: {
            id: id,
            title: 'PULIH: ' + name,
            body: String(s.status || (name + ' kembali normal')).slice(0, 400),
            severity: 'info',
            tag: 'alarm-' + name.toLowerCase().replace(/[^a-z0-9]+/g, '-'),
            url: './index.html?from=push',
            timestamp: reportedAt,
            requireInteraction: false
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0 }
        };
        state[name] = { alarm: false, since: null, eventId: prev.eventId, severity: 'info' };
        pushAppendAndDeliver_(events, ev);
      } else if (alarm && prev) {
        prev.severity = severity;
      }
    }

    pushSaveJson_('PUSH_ALARM_STATE', state);
    pushSaveJson_('PUSH_ALARM_GENERATIONS', generations);
    pushSaveEvents_(events);
    pushRetryPending_();   // kesempatan retry event lama

    return resp_(200, 'Alarm ingest processed', {
      received: sensors.length,
      triggered: triggered,
      resolved: resolved,
      outbox: pushOutboxSummary_(pushEvents_())
    });
  } finally {
    lock.releaseLock();
  }
}

/* ======================= Hook telemetry: emergency edge ======================= */

/**
 * Dipanggil dari recordTelemetry_ (DI LUAR lock telemetry — fungsi ini
 * memegang lock push sendiri). Emergency TRIP/SAFE menghasilkan AlarmEvent
 * dengan prioritas critical — keselamatan tetap local-first di firmware;
 * push hanyalah jalur notifikasi operator, kegagalannya tidak pernah
 * menggagalkan telemetry.
 */
function pushEvaluateEmergency_(norm, deviceKey) {
  try {
    const st = String(norm.emgState || '').toUpperCase();
    if (!st) return;                    // firmware tanpa blok emergency -> skip
    const state = pushLoadJson_('PUSH_EMERGENCY_STATE', {});
    const prev = String(state[deviceKey] || '').toUpperCase();
    if (prev === st) return;            // level, bukan tepi
    state[deviceKey] = st;
    pushSaveJson_('PUSH_EMERGENCY_STATE', state);

    const isTrip = (st === 'TRIPPED' || st === 'TRIP' || norm.emgEstop === true);
    const wasSafe = (prev === '' || prev === 'SAFE' || prev === 'DISARMED');
    const isSafe = (st === 'SAFE' || st === 'DISARMED');

    if (isTrip && wasSafe) {
      const id = 'EMG-' + deviceKey + '-' + Date.now().toString(36);
      pushAppendAndDeliver_(pushEvents_(), {
        eventId: id,
        deviceId: deviceKey,
        alarmCode: 'emergency',
        generation: 1,
        severity: 'critical',
        raisedAt: new Date().toISOString(),
        clearedAt: null,
        state: 'RAISED',
        acknowledgedAt: null,
        payload: {
          id: id,
          title: 'KRITIS: EMERGENCY TRIP ' + deviceKey,
          body: 'Relay emergency ter-trip' +
            (norm.emgReason ? ' — ' + String(norm.emgReason).slice(0, 200) : '') +
            '. Keselamatan lokal tetap aktif; ini notifikasi operator.',
          severity: 'critical',
          tag: 'emergency-' + deviceKey,
          url: './index.html?from=push',
          timestamp: Date.now(),
          requireInteraction: true
        },
        delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
          sent: 0, failed: 0, removed: 0 }
      });
    } else if (isSafe && !wasSafe) {
      const id = 'EMG-RSV-' + deviceKey + '-' + Date.now().toString(36);
      pushAppendAndDeliver_(pushEvents_(), {
        eventId: id,
        deviceId: deviceKey,
        alarmCode: 'emergency',
        generation: 1,
        severity: 'info',
        raisedAt: new Date().toISOString(),
        clearedAt: new Date().toISOString(),
        state: 'CLEARED',
        acknowledgedAt: null,
        payload: {
          id: id,
          title: 'PULIH: Emergency ' + deviceKey,
          body: 'Status emergency kembali ' + st + '.',
          severity: 'info',
          tag: 'emergency-' + deviceKey,
          url: './index.html?from=push',
          timestamp: Date.now(),
          requireInteraction: false
        },
        delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
          sent: 0, failed: 0, removed: 0 }
      });
    }
  } catch (err) {
    // Push TIDAK PERNAH boleh menggagalkan jalur telemetry — catat saja.
    console.error('[PushService] emergency edge failed:', err);
  }
}

/* ======================= PUSH_TEST (operator) ======================= */

/** Dipanggil dispatcher setelah verifyAdminToken_ LULUS — testPush di
 *  canonical selalu terautentikasi admin (tidak ada jalur publik). */
function pushTest_(payload) {
  const id = 'TEST-' + Date.now();
  pushAppendAndDeliver_(pushEvents_(), {
    eventId: id,
    deviceId: String((payload && payload.deviceKey) || 'operator-test'),
    alarmCode: 'test',
    generation: 1,
    severity: 'info',
    raisedAt: new Date().toISOString(),
    clearedAt: new Date().toISOString(),
    state: 'CLEARED',
    acknowledgedAt: null,
    payload: {
      id: id,
      title: 'Uji Push Berhasil (canonical)',
      body: 'Jalur canonical GAS -> PushService -> Web Push berfungsi.',
      severity: 'info',
      tag: 'test-push',
      url: './index.html?from=push',
      timestamp: Date.now(),
      requireInteraction: false
    },
    delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
      sent: 0, failed: 0, removed: 0 }
  });
  return resp_(200, 'Test push dispatched', { eventId: id });
}

/* ======================= Observability ======================= */

/** PUSH_STATUS — ringkasan langganan + outbox (butuh autentikasi device/
 *  admin; dipanggil dispatcher di dalam wilayah terautentikasi). */
function pushStatus_() {
  const subs = pushSubscriptions_();
  return resp_(200, 'Push status', {
    subscriptions: subs.length,
    boundDevices: subs.filter(function (s) { return !!s.deviceId; }).length,
    vapidConfigured: !!(pushProps_().getProperty('VAPID_PUBLIC_KEY') &&
                        pushProps_().getProperty('VAPID_PRIVATE_KEY')),
    outbox: pushOutboxSummary_(pushEvents_())
  });
}
