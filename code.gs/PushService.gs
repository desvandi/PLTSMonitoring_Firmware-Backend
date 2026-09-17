/**
 * PushService.gs — [AUDIT ROUND-3 REMEDIATION 2026-09-17]
 * =======================================================================
 * Modul Web Push untuk backend CANONICAL (satu trust boundary).
 *
 *   ESP32 ──HMAC──► canonical GAS (Code.gs)
 *                     ├─ TELEMETRY ─► Sheets (+ emergency edge → push)
 *                     ├─ PUSH_ALARM_INGEST ─► AlarmEvent outbox ─► Web Push
 *                     └─ PUSH_SUBSCRIBE / PUSH_UNSUBSCRIBE / PUSH_ACK
 *                          (browser, push-scoped token — p.493)
 *
 * ROUND-3 P1 REMEDIATION (temuan auditor 2026-09-17, HEAD 9ecc77e7):
 *   [P1-A] Alarm state & generation kini DI-SCOPE per deviceId
 *          (state per-device PUSH_ST_<h16(dev)>, key = alarmCode). Event ID
 *          memuat deviceId + generation + komponen acak → dua device dengan
 *          sensor senama tidak saling mencemari edge detection dan tidak
 *          berpotensi bertabrakan ID.
 *   [P1-B] Delivery state PER-SUBSCRIPTION (delivery.perSub[subId]='SENT').
 *          Retry hanya mengirim ke subscriber yang BELUM terkirim —
 *          partial failure (A sukses, B gagal) tidak lagi menduplikasi
 *          push ke A saat retry B.
 *   [P1-C] Lifecycle persisten: tepi turun kini MENULIS kembali event
 *          RAISED asal (state='CLEARED', clearedAt, resolvedBy) dan event
 *          RSV membawa resolvesEventId — rekonstruksi history tidak lagi
 *          ambigu (RAISED tak berubah + RSV lepas).
 *   [P1-D] Arsitektur lock: critical section PENDEK saja — TIDAK ADA
 *          operasi jaringan (UrlFetchApp) di dalam Script Lock, dan tidak
 *          ada nested tryLock. Delivery round dilindungi claim-token
 *          (TTL 90 dtk) sehingga retry worker tidak mengirim bersamaan
 *          dengan delivery ingest untuk event yang sama.
 *   [CAP]  Kapasitas Script Properties dihormati secara mekanis: SATU
 *          property per event (PUSH_EVT_<id>) dan per langganan
 *          (PUSH_SUB_<subId>) — tidak ada lagi array JSON tunggal yang
 *          menabrak batas 9 KB/value GAS. Total budget 450.000 karakter
 *          (headroom 10% dari batas 500 KB) dijaga dengan eviksi event
 *          terminal tertua; kehilangan akibat kapasitas dihitung jujur
 *          (PUSH_CAPACITY_DROPPED) dan dilaporkan PUSH_STATUS.
 *   [QUOTA] Ekonomi operasi property: jalur tenang (tanpa alarm) membaca
 *          O(1)-beberapa property saja (indeks PENDING), bukan seluruh
 *          outbox; penulisan bersifat INCREMENTAL (event yang berubah
 *          saja), bukan rewrite penuh — mencegah pembakaran kuota harian
 *          PropertiesService pada fleet multi-device.
 *
 * Semantika delivery: AT-LEAST-ONCE per subscriber (bukan exactly-once).
 * Jendela duplikat dibatasi hanya pada subscriber yang sedang in-flight
 * saat eksekusi GAS mati di antara kirim dan merge status (kontrak
 * at-least-once dengan kursor per-subscriber).
 *
 * PASANGAN WAJIB: tempel juga WebPushCore.gs (dari push-alarm/gas/) ke
 * proyek Apps Script yang sama — berisi kripto Web Push (P-256, AES-GCM).
 * Gunakan artefak gas-bundle hasil CI (ter-stamp PUSH_SOURCE_REVISION)
 * agar revision yang dideploy dapat diverifikasi — scripts/verify-gas-deployment.js.
 *
 * SCRIPT PROPERTIES:
 *   VAPID_PUBLIC_KEY / VAPID_PRIVATE_KEY  (hasil generate-vapid-keys.js)
 *   PUSH_TOKENS = [{"deviceId":"...","token":"..."}]   (p.493: kapabilitas
 *       subscribe/unsubscribe SAJA; bisa juga di-set di Config sheet dengan
 *       key PUSH_TOKENS — Script Property menang)
 *   PUSH_ACK_SECRET        (auto-provision 64-hex; rotasi manual)
 *   PUSH_EVT_IDX_<n>       shard index event (urut waktu, ≤100 id/shard)
 *   PUSH_EVT_<eventId>     SATU AlarmEvent per property
 *   PUSH_PEND_IDX_<n>      shard index event PENDING (gerbang murah retry)
 *   PUSH_SUB_IDX           ["<subId>", ...]
 *   PUSH_SUB_<subId>       SATU langganan per property
 *   PUSH_ST_<h16(dev)>     state alarm per-device [P1-A]
 *   PUSH_EMERGENCY_STATE   { "<deviceId>": {state, eventId} }
 *   PUSH_CAPACITY_DROPPED  counter event yang dibuang karena kapasitas
 */

// [round-3] var (bukan const) agar harness vm dapat menyetel knob uji
// (OUTBOX_RETRY_BASE_MS=0) melalui properti konteks — GAS tidak membedakan.
var PUSH_CFG = {
  SUBJECT: 'mailto:admin@plts-monitoring.local',
  TTL: 86400,                       // umur pesan di push service (24 jam)
  MAX_PAYLOAD_CHARS: 3000,
  MAX_SUBSCRIPTIONS: 50,            // cap pertumbuhan (LRU di atas ini)
  PRUNE_AFTER_DAYS: 90,
  OUTBOX_MAX_EVENTS: 200,
  OUTBOX_RETRY_MAX_ATTEMPTS: 6,
  OUTBOX_RETRY_BASE_MS: 60000,
  OUTBOX_MAX_RETRY_AGE_MS: 24 * 60 * 60 * 1000,
  ACK_TOKEN_BUCKET_MS: 6 * 60 * 60 * 1000,  // 6 jam per bucket
  // ---- [CAP round-3] batas penyimpanan GAS (dihormati mekanis) ----
  // Batas nyata Google: 9 KB per value, 500 KB total property store.
  // Angka di bawah memakai headroom ~6% / ~10% supaya aproksimasi
  // panjang string (UTF-16 char) tetap aman terhadap UTF-8 multibyte.
  STORAGE_VALUE_LIMIT: 8500,        // karakter per property value
  STORAGE_TOTAL_LIMIT: 450000,      // total karakter semua PUSH_*/VAPID_* property
  IDX_SHARD: 100,                   // id per shard index
  CLAIM_TTL_MS: 90000               // TTL claim delivery round (90 detik)
};

/* Revision sumber — DI-STAMP oleh CI gas-bundle job (mengganti nilai ini
 * dengan GITHUB_SHA saat membangun artefak rilis). Deploy manual dari
 * working copy akan melaporkan 'dev-unstamped' — jujur, bukan pura-pura
 * tahu revisionnya. Verifikasi: scripts/verify-gas-deployment.js. */
const PUSH_SOURCE_REVISION = 'dev-unstamped';

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

/** sha256 hex pendek (16 char) untuk key property yang pendek. */
function pushHash16_(str) {
  const bytes = Utilities.computeDigest(
    Utilities.DigestAlgorithm.SHA_256, String(str), Utilities.Charset.UTF_8);
  let out = '';
  for (let i = 0; i < 8; i++) {
    out += ('0' + (bytes[i] & 0xff).toString(16)).slice(-2);
  }
  return out;
}

/** Total karakter semua property PUSH_x dan VAPID_x (aproksimasi budget). */
function pushStorageBytes_() {
  let total = 0;
  const all = pushProps_().getProperties();
  for (const k in all) {
    if (String(k).indexOf('PUSH_') === 0 || String(k).indexOf('VAPID_') === 0) {
      total += String(k).length + String(all[k]).length;
    }
  }
  return total;
}

/* ---------------------- helper index ter-shard ---------------------- */

function pushReadShardedIdx_(prefix) {
  const ids = [];
  let n = 0;
  while (n < 64) {
    const raw = pushProps_().getProperty(prefix + '_' + n);
    if (!raw) break;
    try {
      const arr = JSON.parse(raw);
      if (!Array.isArray(arr)) break;
      for (let i = 0; i < arr.length; i++) ids.push(String(arr[i]));
    } catch (e) { break; }
    n++;
  }
  return ids;
}

function pushWriteShardedIdx_(prefix, ids) {
  let n = 0;
  while (pushProps_().getProperty(prefix + '_' + n) !== null) {
    pushProps_().deleteProperty(prefix + '_' + n);
    n++;
    if (n > 64) break;
  }
  if (ids.length === 0) return;
  for (let i = 0; i < ids.length; i += PUSH_CFG.IDX_SHARD) {
    pushSaveJson_(prefix + '_' + Math.floor(i / PUSH_CFG.IDX_SHARD),
      ids.slice(i, i + PUSH_CFG.IDX_SHARD));
  }
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

/* ======================= Lock helper [P1-D] =======================
 * SEMUA mutasi store push berada dalam critical section PENDEK yang
 * tidak pernah berisi UrlFetchApp.fetch. Tidak ada pemanggilan
 * tryLock bersarang: fungsi *Unlocked_ hanya dipanggil saat lock
 * sudah dipegang; fungsi publik mengambil lock sendiri. */

function pushWithLock_(fn, waitMs) {
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(waitMs === undefined ? 20000 : waitMs)) {
    return { busy: true };
  }
  try {
    return { busy: false, value: fn() };
  } finally {
    lock.releaseLock();
  }
}

/* ======================= Langganan (device-bound, sharded) ======================= */

function pushSubId_(endpoint) {
  return 'S' + pushHash16_(endpoint);
}

/** Baca seluruh langganan (IDX + N property). */
function pushSubsAll_() {
  const out = [];
  const idx = pushLoadJson_('PUSH_SUB_IDX', []);
  for (let i = 0; i < idx.length; i++) {
    const raw = pushProps_().getProperty('PUSH_SUB_' + idx[i]);
    if (!raw) continue;
    try {
      const sub = JSON.parse(raw);
      if (sub && sub.subId === idx[i]) out.push(sub);
    } catch (e) { /* korup → abaikan record ini */ }
  }
  return out;
}

/**
 * Tulis seluruh store langganan. [CAP] SATU property per langganan;
 * context dipangkas agar nilai per-property jauh di bawah
 * STORAGE_VALUE_LIMIT. Pemanggil WAJIB memegang lock.
 */
function pushSaveSubsAll_(subs) {
  const keep = [];
  for (let i = 0; i < subs.length; i++) {
    const s = subs[i];
    if (!s || !s.endpoint) continue;
    if (!s.subId) s.subId = pushSubId_(s.endpoint);
    if (JSON.stringify(s.context || {}).length > 400) s.context = { trimmed: true };
    let val = JSON.stringify(s);
    if (val.length > PUSH_CFG.STORAGE_VALUE_LIMIT) s.context = { trimmed: true };
    pushProps_().setProperty('PUSH_SUB_' + s.subId, JSON.stringify(s));
    keep.push(s.subId);
  }
  const oldIdx = pushLoadJson_('PUSH_SUB_IDX', []);
  for (let i = 0; i < oldIdx.length; i++) {
    if (keep.indexOf(oldIdx[i]) < 0) pushProps_().deleteProperty('PUSH_SUB_' + oldIdx[i]);
  }
  pushSaveJson_('PUSH_SUB_IDX', keep);
}

function pushUpsertSubscriptionUnlocked_(sub) {
  const subs = pushSubsAll_();
  let found = false;
  for (let i = 0; i < subs.length; i++) {
    if (subs[i].endpoint === sub.endpoint) { subs[i] = sub; found = true; break; }
  }
  if (!found) subs.push(sub);
  pushSaveSubsAll_(subs);
  return subs.length;
}

/** [P1 ownership] Hanya record milik device (atau record lama tanpa
 *  deviceId) yang boleh dihapus. Versi UNLOCKED — pemanggil memegang lock. */
function pushRemoveSubscriptionUnlocked_(endpoint, deviceId) {
  const subs = pushSubsAll_();
  const kept = subs.filter(function (s) {
    if (s.endpoint !== endpoint) return true;
    if (!s.deviceId) return false;
    return String(s.deviceId) !== String(deviceId);
  });
  if (kept.length !== subs.length) pushSaveSubsAll_(kept);
  return subs.length - kept.length;
}

/** Versi ber-lock (dipakai jalur delivery yang TIDAK memegang lock). */
function pushRemoveSubscriptionOwned_(endpoint, deviceId) {
  return pushWithLock_(function () {
    return pushRemoveSubscriptionUnlocked_(endpoint, deviceId);
  }, 0).value || 0;
}

/* ======================= ACK capability token (p.482) ======================= */

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

/* ======================= Outbox AlarmEvent (P0-3, sharded [CAP]) ======================= */

/** slug pendek untuk event ID (bounded, aman untuk key property). */
function pushSlug_(s, max) {
  return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '')
    .slice(0, max || 16);
}

/**
 * [P1-A] Event ID unik per (device, alarmCode, generation) + komponen acak
 * → dua device dengan sensor senama TIDAK bisa bertabrakan ID walau
 * timestamp identik, dan re-raise setelah clear tetap berbeda ID.
 */
function pushMakeEventId_(prefix, deviceKey, alarmCode, generation) {
  const rand = Utilities.getUuid().replace(/-/g, '').slice(0, 8);
  return prefix + '-' + pushSlug_(deviceKey, 24) + '-' + pushSlug_(alarmCode, 20) +
    '-' + Number(generation || 1).toString(36) + '-' + rand;
}

function pushEventKey_(eventId) {
  return 'PUSH_EVT_' + String(eventId).replace(/[^A-Za-z0-9_-]/g, '').slice(0, 96);
}

/** Baca seluruh outbox (urut waktu — urutan IDX). Operasi O(N); hanya
 *  untuk observability/status — JALUR HOT memakai indeks PENDING. */
function pushEvents_() {
  const out = [];
  const ids = pushReadShardedIdx_('PUSH_EVT_IDX');
  for (let i = 0; i < ids.length; i++) {
    const ev = pushFindEvent_(ids[i]);
    if (ev) out.push(ev);
  }
  return out;
}

function pushFindEvent_(eventId) {
  const raw = pushProps_().getProperty(pushEventKey_(eventId));
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { return null; }
}

/** Indeks PENDING — gerbang murah: jalur tenang membaca ini saja. */
function pushPendingIdx_() {
  return pushReadShardedIdx_('PUSH_PEND_IDX');
}

function pushSetPending_(eventId, isPending) {
  const ids = pushPendingIdx_();
  const at = ids.indexOf(String(eventId));
  if (isPending && at < 0) ids.push(String(eventId));
  if (!isPending && at >= 0) ids.splice(at, 1);
  pushWriteShardedIdx_('PUSH_PEND_IDX', ids);
}

/** JSON event dengan pemangkasan defensif (tidak pernah lewat value limit). */
function pushEventJson_(ev) {
  let s = JSON.stringify(ev);
  if (s.length > PUSH_CFG.STORAGE_VALUE_LIMIT) {
    ev.payload = ev.payload || {};
    ev.payload.body = String(ev.payload.body || '').slice(0, 120);
    s = JSON.stringify(ev);
  }
  return s;
}

/**
 * Tulis SATU event (inkremental [QUOTA]) + sinkron indeks PENDING.
 * Pemanggil WAJIB memegang lock.
 */
function pushWriteEvent_(ev) {
  pushProps_().setProperty(pushEventKey_(ev.eventId), pushEventJson_(ev));
  pushSetPending_(ev.eventId,
    !!(ev.delivery && ev.delivery.status === 'PENDING'));
}

/**
 * Tambah SATU event ke outbox: tulis property + IDX + kapasitas.
 * Eviksi [CAP]: bila jumlah > OUTBOX_MAX_EVENTS atau total bytes >
 * STORAGE_TOTAL_LIMIT → buang event TERMINAL tertua dulu; bila tak ada
 * terminal, buang PENDING tertua dan hitung di PUSH_CAPACITY_DROPPED
 * (kehilangan jujur, dilaporkan PUSH_STATUS — tidak pernah diam-diam).
 * Pemanggil WAJIB memegang lock.
 */
function pushAppendEvent_(ev) {
  pushWriteEvent_(ev);
  const ids = pushReadShardedIdx_('PUSH_EVT_IDX');
  ids.push(String(ev.eventId));
  while (ids.length > PUSH_CFG.OUTBOX_MAX_EVENTS ||
         (pushStorageBytes_() > PUSH_CFG.STORAGE_TOTAL_LIMIT && ids.length > 0)) {
    let victimId = null;
    for (let i = 0; i < ids.length; i++) {
      const cand = pushFindEvent_(ids[i]);
      if (cand && cand.delivery) {
        const st = cand.delivery.status;
        if (st === 'SENT' || st === 'EXPIRED' || st === 'FAILED') { victimId = ids[i]; break; }
      }
    }
    let capacityLoss = false;
    if (!victimId) { victimId = ids[0]; capacityLoss = true; }
    pushProps_().deleteProperty(pushEventKey_(victimId));
    pushSetPending_(victimId, false);
    ids.splice(ids.indexOf(victimId), 1);
    if (capacityLoss) {
      const prev = Number(pushProps_().getProperty('PUSH_CAPACITY_DROPPED') || 0);
      pushProps_().setProperty('PUSH_CAPACITY_DROPPED', String(prev + 1));
    }
  }
  pushWriteShardedIdx_('PUSH_EVT_IDX', ids);
}

/** Hapus SATU event (dipakai jalur maintenance/ACK tidak — hanya internal). */
function pushRemoveEvent_(eventId) {
  pushProps_().deleteProperty(pushEventKey_(eventId));
  pushSetPending_(eventId, false);
  const ids = pushReadShardedIdx_('PUSH_EVT_IDX');
  const at = ids.indexOf(String(eventId));
  if (at >= 0) { ids.splice(at, 1); pushWriteShardedIdx_('PUSH_EVT_IDX', ids); }
}

function pushEventRetryDue_(ev, now) {
  if (!ev.delivery || ev.delivery.status !== 'PENDING') return false;
  if (ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS) return false;
  const age = now - new Date(ev.raisedAt).getTime();
  if (age > PUSH_CFG.OUTBOX_MAX_RETRY_AGE_MS) return false;
  // [P1-D] claim aktif = sedang ada delivery round lain → jangan sentuh.
  if (ev.delivery.claim) {
    const claimedAt = new Date(ev.delivery.claimedAt || 0).getTime();
    if (now - claimedAt < PUSH_CFG.CLAIM_TTL_MS) return false;
  }
  if (!ev.delivery.attempts || !ev.delivery.lastAttemptAt) return true;
  const backoff = PUSH_CFG.OUTBOX_RETRY_BASE_MS *
    Math.pow(2, Math.max(0, ev.delivery.attempts - 1));
  return (now - new Date(ev.delivery.lastAttemptAt).getTime()) >= backoff;
}

function pushExpireStaleEvent_(ev, now) {
  if (!ev.delivery || ev.delivery.status !== 'PENDING') return false;
  const age = now - new Date(ev.raisedAt).getTime();
  if (age > PUSH_CFG.OUTBOX_MAX_RETRY_AGE_MS) { ev.delivery.status = 'EXPIRED'; return true; }
  if (ev.delivery.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS) {
    ev.delivery.status = 'FAILED'; return true;
  }
  return false;
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

/* ======================= State alarm per-device [P1-A] ======================= */

function pushDeviceStateKey_(deviceKey) {
  return 'PUSH_ST_' + pushHash16_(String(deviceKey || 'unknown'));
}

/** State device: {"<alarmCode>": {alarm, since, eventId, severity, gen}} */
function pushDeviceState_(deviceKey) {
  return pushLoadJson_(pushDeviceStateKey_(deviceKey), {});
}

function pushSaveDeviceState_(deviceKey, state) {
  pushSaveJson_(pushDeviceStateKey_(deviceKey), state);
}

/** Migrasi sekali-jalan: property pra-round3 (state global & array tunggal)
 *  TIDAK dibaca lagi — kanonikal belum pernah dideploy ke production, jadi
 *  kunci lama dibersihkan jujur untuk mencegah kebangkitan kontaminasi
 *  lintas-device. Laporan status menandai kejadian ini. */
function pushMigrateLegacyKeys_() {
  const legacyKeys = ['PUSH_ALARM_STATE', 'PUSH_ALARM_GENERATIONS',
    'PUSH_ALARM_EVENTS', 'PUSH_SUBSCRIPTIONS'];
  let cleared = 0;
  for (let i = 0; i < legacyKeys.length; i++) {
    if (pushProps_().getProperty(legacyKeys[i]) !== null) {
      pushProps_().deleteProperty(legacyKeys[i]);
      cleared++;
    }
  }
  if (cleared > 0) {
    pushProps_().setProperty('PUSH_LEGACY_MIGRATED_AT', new Date().toISOString());
  }
}

/* ======================= Pengiriman [P1-B/P1-D] ======================= */

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

/** Kirim satu payload terenkripsi ke satu langganan. TIDAK memegang lock
 *  kecuali untuk pembersihan endpoint mati (critical section sendiri). */
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
      return { ok: false, status: code, error: 'endpoint-mati', subId: subscription.subId };
    }
    if (code >= 200 && code < 300) return { ok: true, status: code, error: '', subId: subscription.subId };
    return { ok: false, status: code, error: res.getContentText().slice(0, 200), subId: subscription.subId };
  } catch (err) {
    return { ok: false, status: 0, error: String(err).slice(0, 200), subId: subscription.subId };
  }
}

/**
 * Kirim SATU AlarmEvent HANYA ke subscriber scoped-device yang BELUM
 * tercatat SENT di delivery.perSub [P1-B]. Fungsi ini TIDAK memegang
 * lock [P1-D] — membaca store langganan secara advisory, mengirim di
 * luar critical section, lalu hasil per-subscriber dikembalikan untuk
 * di-merge oleh pushRecordDelivery_ (critical section pendek).
 */
function pushDeliverEvent_(ev, subsAllOverride) {
  const payload = ev.payload;
  payload.ackToken = pushMakeAckToken_(payload.id);
  payload.apiBase = pushScriptUrl_();
  const subs = subsAllOverride || pushSubsAll_();
  const already = (ev.delivery && ev.delivery.perSub) || {};
  const scoped = subs.filter(function (s) {
    if (s.deviceId && String(s.deviceId) !== String(ev.deviceId)) return false;
    return already[s.subId] !== 'SENT';    // [P1-B] skip yang sudah terkirim
  });
  let sent = 0, failed = 0, removed = 0;
  const perSub = {};
  for (let i = 0; i < scoped.length; i++) {
    const r = pushWebSend_(scoped[i], payload,
      payload.severity === 'critical' ? 'high' : 'normal');
    if (r.ok) { sent++; perSub[scoped[i].subId] = 'SENT'; }
    else {
      failed++;
      if (r.error === 'endpoint-mati') removed++;
    }
  }
  return { sent: sent, failed: failed, removed: removed,
    total: scoped.length, perSub: perSub };
}

/** Turunkan status aggregate dari per-sub marker vs langganan aktif. */
function pushDeriveDeliveryStatus_(ev, subsAll) {
  const d = ev.delivery || {};
  const already = d.perSub || {};
  let active = 0, delivered = 0;
  for (let i = 0; i < subsAll.length; i++) {
    const s = subsAll[i];
    if (s.deviceId && String(s.deviceId) !== String(ev.deviceId)) continue;
    active++;
    if (already[s.subId] === 'SENT') delivered++;
  }
  if (active === 0) return 'SENT';       // tidak ada yang perlu diberi tahu
  if (delivered === active) return 'SENT';
  if (d.attempts >= PUSH_CFG.OUTBOX_RETRY_MAX_ATTEMPTS) {
    return delivered > 0 ? 'PARTIAL' : 'FAILED';
  }
  return 'PENDING';
}

/**
 * [P1-B/P1-D] Merge hasil delivery round ke store — critical section PENDEK
 * (tanpa jaringan). Re-read by eventId supaya mutasi lain (ingest device
 * lain, subscribe, ACK) yang terjadi selama pengiriman TIDAK tertimpa
 * (tidak ada lost update). Claim dibebaskan → retry worker bisa melanjutkan.
 */
function pushRecordDelivery_(eventId, result, subsAllOverride) {
  return pushWithLock_(function () {
    const ev = pushFindEvent_(eventId);
    if (!ev) return { ok: false, reason: 'evicted' };
    ev.delivery = ev.delivery || { perSub: {} };
    ev.delivery.perSub = ev.delivery.perSub || {};
    const perSub = (result && result.perSub) || {};
    for (const sid in perSub) {
      if (perSub[sid] === 'SENT') ev.delivery.perSub[sid] = 'SENT';
    }
    // prune marker untuk langganan yang sudah tidak ada
    const subsAll = subsAllOverride || pushSubsAll_();
    const live = {};
    for (let i = 0; i < subsAll.length; i++) live[subsAll[i].subId] = true;
    for (const sid in ev.delivery.perSub) {
      if (!live[sid]) delete ev.delivery.perSub[sid];
    }
    ev.delivery.attempts = (ev.delivery.attempts || 0) + 1;
    ev.delivery.lastAttemptAt = new Date().toISOString();
    ev.delivery.sent = result.sent;
    ev.delivery.failed = result.failed;
    ev.delivery.removed = result.removed;
    ev.delivery.claim = null;
    ev.delivery.claimedAt = null;
    ev.delivery.status = pushDeriveDeliveryStatus_(ev, subsAll);
    pushWriteEvent_(ev);                 // tulis INKREMENTAL [QUOTA]
    return { ok: true, status: ev.delivery.status };
  }, 20000);
}

/** [P1-D] Wrapper publik retry: claim → kirim (tanpa lock) → merge.
 *  TIDAK dipanggil dari dalam lock ingest — nested lock hilang. Jalur
 *  tenang berhenti di gerbang indeks PENDING (O(shard) baca). */
function pushRetryPending_() {
  const pendingIds = pushPendingIdx_();          // gerbang murah [QUOTA]
  if (pendingIds.length === 0) return { ok: true, retried: 0 };

  const claimed = pushWithLock_(function () {
    const now = Date.now();
    const due = [];
    for (let i = 0; i < pendingIds.length; i++) {
      const ev = pushFindEvent_(pendingIds[i]);
      if (!ev) { pushSetPending_(pendingIds[i], false); continue; }
      if (pushExpireStaleEvent_(ev, now)) { pushWriteEvent_(ev); continue; }
      if (!pushEventRetryDue_(ev, now)) continue;
      ev.delivery.claim = Utilities.getUuid().slice(0, 8);
      ev.delivery.claimedAt = new Date().toISOString();
      pushWriteEvent_(ev);                       // claim persist dulu
      due.push(ev);
    }
    return due;
  }, 20000);
  if (claimed.busy) return { ok: false, retried: 0 };

  const subsAll = pushSubsAll_();                // snapshot sekali per ronde
  let retried = 0;
  for (let i = 0; i < claimed.value.length; i++) {
    const ev = claimed.value[i];
    const r = pushDeliverEvent_(ev, subsAll);
    pushRecordDelivery_(ev.eventId, r, subsAll);
    retried++;
  }
  return { ok: true, retried: retried };
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
    const r = pushWithLock_(function () {
      let subs = pushSubsAll_();
      if (subs.length >= PUSH_CFG.MAX_SUBSCRIPTIONS) {
        subs.sort(function (a, b) {
          return new Date(a.lastSeenAt || a.addedAt).getTime() -
                 new Date(b.lastSeenAt || b.addedAt).getTime();
        });
        subs = subs.slice(subs.length - (PUSH_CFG.MAX_SUBSCRIPTIONS - 1));
        pushSaveSubsAll_(subs);
      }
      return pushUpsertSubscriptionUnlocked_({
        endpoint: ep,
        subId: pushSubId_(ep),
        keys: { p256dh: body.keys.p256dh, auth: body.keys.auth },
        deviceId: String(body.device.id).slice(0, 64),
        context: body.context || {},
        addedAt: new Date().toISOString(),
        lastSeenAt: new Date().toISOString()
      });
    }, 20000);
    if (r.busy) return resp_(503, 'Backend busy — retry', null);
    return resp_(200, 'Subscription stored', { total: r.value });
  }

  if (action === 'PUSH_UNSUBSCRIBE') {
    if (!body.device || !body.device.id || !body.token) {
      return resp_(401, 'PUSH_UNSUBSCRIBE requires device.id + push token', null);
    }
    if (!pushSubscriberAuthorized_(String(body.device.id), body.token)) {
      return resp_(401, 'Invalid push token', null);
    }
    const r = pushWithLock_(function () {
      return pushRemoveSubscriptionUnlocked_(body.endpoint,
        String(body.device.id).slice(0, 64));
    }, 20000);
    if (r.busy) return resp_(503, 'Backend busy — retry', null);
    return resp_(200, 'Subscription removed', { removed: r.value });
  }

  if (action === 'PUSH_ACK') {
    if (!pushVerifyAckToken_(body.alarmId, body.ackToken)) {
      return resp_(401, 'ACK rejected: invalid/missing capability token (p.482)', null);
    }
    const r = pushWithLock_(function () {
      const ev = pushFindEvent_(String(body.alarmId));
      if (!ev) return resp_(404, 'Alarm event not found: ' + body.alarmId, null);
      if (!ev.acknowledgedAt) {
        ev.acknowledgedAt = new Date().toISOString();
        pushWriteEvent_(ev);
      }
      return resp_(200, 'Alarm acknowledged',
        { alarmId: ev.eventId, acknowledgedAt: ev.acknowledgedAt });
    }, 20000);
    if (r.busy) return resp_(503, 'Backend busy — retry', null);
    return r.value;
  }

  return resp_(400, 'Unknown push action: ' + action, null);
}

/* ======================= PUSH_ALARM_INGEST (jalur firmware) ======================= */

/**
 * Kontrak laporan sensor (identik legacy — skema "Ambang alarm FW-GAS"):
 *   { sensors: [{ name, value, unit, alarm?, severity?, status? }], reportedAt }
 * Autentikasi: envelope canonical (HMAC device ATAU legacy token) — SATU
 * trust boundary dengan telemetry. Edge detection + outbox + delivery.
 *
 * [P1-D] Urutan tiga fase (lock hanya di fase pendek):
 *   FASE 1 (locked)   : deteksi tepi per-device [P1-A], tulis event PENDING
 *                       + claim, tulis state, tulis lifecycle [P1-C].
 *   FASE 2 (unlocked) : Web Push network I/O — TIDAK di dalam lock.
 *   FASE 3 (locked)   : merge per-subscriber delivery [P1-B] per event.
 */
function pushAlarmIngest_(payload, deviceKey) {
  payload = payload || {};
  if (!Array.isArray(payload.sensors) || payload.sensors.length === 0) {
    return resp_(400, 'PUSH_ALARM_INGEST requires a non-empty sensors[] array', null);
  }
  const reportedAt = Number(payload.reportedAt) || Date.now();
  const sensors = payload.sensors.slice(0, 24);
  pushMigrateLegacyKeys_();

  const phase1 = pushWithLock_(function () {
    const state = pushDeviceState_(deviceKey);          // [P1-A] per-device
    const now = Date.now();
    const claimNow = new Date().toISOString();

    const triggered = [];
    const resolved = [];
    const newEvents = [];

    for (let i = 0; i < sensors.length; i++) {
      const s = sensors[i] || {};
      const name = String(s.name || 'Sensor').slice(0, 40);
      const code = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').slice(0, 40);
      const alarm = s.alarm === true;
      const severity = ['critical', 'warning', 'info'].indexOf(s.severity) >= 0
        ? s.severity : (alarm ? 'warning' : 'info');
      const prev = state[code];                          // [P1-A] device-scope
      const prevAlarm = !!(prev && prev.alarm);

      if (alarm && !prevAlarm) {
        const gen = ((prev && prev.gen) || 0) + 1;
        const id = pushMakeEventId_('ALM', deviceKey, code, gen);
        const ev = {
          eventId: id,
          deviceId: deviceKey,
          alarmCode: code,
          generation: gen,
          severity: severity,
          raisedAt: new Date(reportedAt).toISOString(),
          clearedAt: null,
          state: 'RAISED',
          acknowledgedAt: null,
          resolvesEventId: null,
          resolvedBy: null,
          payload: {
            id: id,
            title: (severity === 'critical' ? 'KRITIS: ' : 'PERINGATAN: ') + name,
            body: String(s.status || (name + ' di luar batas aman')).slice(0, 400),
            severity: severity,
            tag: 'alarm-' + code,
            url: './index.html?from=push',
            timestamp: reportedAt,
            requireInteraction: severity === 'critical'
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0, perSub: {},
            claim: Utilities.getUuid().slice(0, 8),       // [P1-D]
            claimedAt: claimNow }
        };
        triggered.push(id);
        state[code] = { alarm: true, since: ev.raisedAt, eventId: id,
          severity: severity, gen: gen };
        pushAppendEvent_(ev);                            // durable DULU
        newEvents.push(ev);
      } else if (!alarm && prevAlarm) {
        const id = pushMakeEventId_('RSV', deviceKey, code, prev.gen || 1);
        resolved.push(prev.eventId);
        const ev = {
          eventId: id,
          deviceId: deviceKey,
          alarmCode: code,
          generation: prev.gen || 1,
          severity: 'info',
          raisedAt: new Date(reportedAt).toISOString(),
          clearedAt: new Date(reportedAt).toISOString(),
          state: 'CLEARED',
          acknowledgedAt: null,
          resolvesEventId: prev.eventId,                  // [P1-C]
          resolvedBy: null,
          payload: {
            id: id,
            title: 'PULIH: ' + name,
            body: String(s.status || (name + ' kembali normal')).slice(0, 400),
            severity: 'info',
            tag: 'alarm-' + code,
            url: './index.html?from=push',
            timestamp: reportedAt,
            requireInteraction: false
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0, perSub: {},
            claim: Utilities.getUuid().slice(0, 8),
            claimedAt: claimNow }
        };
        // [P1-C] TULIS KEMBALI event RAISED asal — lifecycle satu alarm
        // kini utuh di persistence, bukan hanya di event terpisah.
        const orig = pushFindEvent_(prev.eventId);
        if (orig) {
          orig.state = 'CLEARED';
          orig.clearedAt = ev.clearedAt;
          orig.resolvedBy = id;
          pushWriteEvent_(orig);
        }
        state[code] = { alarm: false, since: null, eventId: prev.eventId,
          severity: 'info', gen: prev.gen || 1 };
        pushAppendEvent_(ev);
        newEvents.push(ev);
      } else if (alarm && prev) {
        prev.severity = severity;
      }
    }

    pushSaveDeviceState_(deviceKey, state);
    return { triggered: triggered, resolved: resolved, newEvents: newEvents };
  }, 20000);
  if (phase1.busy) return resp_(503, 'Backend busy — retry', null);

  // FASE 2 [P1-D]: network I/O DI LUAR lock. Snapshot langganan dibaca
  // SEKALI untuk seluruh ronde (ekonomi property [QUOTA]).
  const subsAll = pushSubsAll_();
  for (let i = 0; i < phase1.value.newEvents.length; i++) {
    const ev = phase1.value.newEvents[i];
    const r = pushDeliverEvent_(ev, subsAll);
    pushRecordDelivery_(ev.eventId, r, subsAll);   // FASE 3: merge pendek
  }

  // kesempatan retry event lama (mengelola lock sendiri — tanpa nesting);
  // gerbang PENDING membuat jalur tenang nyaris gratis [QUOTA].
  pushRetryPending_();

  return resp_(200, 'Alarm ingest processed', {
    received: sensors.length,
    triggered: phase1.value.triggered,
    resolved: phase1.value.resolved,
    outbox: { pending: pushPendingIdx_().length }
  });
}

/* ======================= Hook telemetry: emergency edge ======================= */

/**
 * Dipanggil dari recordTelemetry_ (DI LUAR lock telemetry — fungsi ini
 * memakai critical section push sendiri yang pendek). Emergency TRIP/SAFE
 * menghasilkan AlarmEvent dengan prioritas critical — keselamatan tetap
 * local-first di firmware; push hanyalah jalur notifikasi operator,
 * kegagalannya tidak pernah menggagalkan telemetry.
 */
function pushEvaluateEmergency_(norm, deviceKey) {
  try {
    const st = String(norm.emgState || '').toUpperCase();
    if (!st) return;                    // firmware tanpa blok emergency -> skip
    let deliverId = null;
    const r = pushWithLock_(function () {
      const estate = pushLoadJson_('PUSH_EMERGENCY_STATE', {});
      const prev = estate[deviceKey] || {};
      const prevSt = String(prev.state || '').toUpperCase();
      if (prevSt === st) return null;   // level, bukan tepi

      const isTrip = (st === 'TRIPPED' || st === 'TRIP' || norm.emgEstop === true);
      const wasSafe = (prevSt === '' || prevSt === 'SAFE' || prevSt === 'DISARMED');
      const isSafe = (st === 'SAFE' || st === 'DISARMED');
      const now = new Date().toISOString();

      if (isTrip && wasSafe) {
        const id = pushMakeEventId_('EMG', deviceKey, 'emergency', 1);
        pushAppendEvent_({
          eventId: id,
          deviceId: deviceKey,
          alarmCode: 'emergency',
          generation: 1,
          severity: 'critical',
          raisedAt: now,
          clearedAt: null,
          state: 'RAISED',
          acknowledgedAt: null,
          resolvesEventId: null,
          resolvedBy: null,
          payload: {
            id: id,
            title: 'KRITIS: EMERGENCY TRIP ' + deviceKey,
            body: 'Relay emergency ter-trip' +
              (norm.emgReason ? ' — ' + String(norm.emgReason).slice(0, 200) : '') +
              '. Keselamatan lokal tetap aktif; ini notifikasi operator.',
            severity: 'critical',
            tag: 'emergency-' + pushSlug_(deviceKey, 12),
            url: './index.html?from=push',
            timestamp: Date.now(),
            requireInteraction: true
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0, perSub: {},
            claim: Utilities.getUuid().slice(0, 8), claimedAt: now }
        });
        estate[deviceKey] = { state: st, eventId: id };
        pushSaveJson_('PUSH_EMERGENCY_STATE', estate);
        return id;
      }

      if (isSafe && !wasSafe) {
        const id = pushMakeEventId_('EMG', deviceKey, 'emergency', 2);
        // [P1-C] tutup lifecycle event EMG RAISED asal bila ada
        const raisedId = prev.eventId || null;
        if (raisedId) {
          const orig = pushFindEvent_(raisedId);
          if (orig) {
            orig.state = 'CLEARED';
            orig.clearedAt = now;
            orig.resolvedBy = id;
            pushWriteEvent_(orig);
          }
        }
        pushAppendEvent_({
          eventId: id,
          deviceId: deviceKey,
          alarmCode: 'emergency',
          generation: 2,
          severity: 'info',
          raisedAt: now,
          clearedAt: now,
          state: 'CLEARED',
          acknowledgedAt: null,
          resolvesEventId: raisedId,
          resolvedBy: null,
          payload: {
            id: id,
            title: 'PULIH: Emergency ' + deviceKey,
            body: 'Status emergency kembali ' + st + '.',
            severity: 'info',
            tag: 'emergency-' + pushSlug_(deviceKey, 12),
            url: './index.html?from=push',
            timestamp: Date.now(),
            requireInteraction: false
          },
          delivery: { status: 'PENDING', attempts: 0, lastAttemptAt: null,
            sent: 0, failed: 0, removed: 0, perSub: {},
            claim: Utilities.getUuid().slice(0, 8), claimedAt: now }
        });
        estate[deviceKey] = { state: st, eventId: raisedId };
        pushSaveJson_('PUSH_EMERGENCY_STATE', estate);
        return id;
      }

      estate[deviceKey] = { state: st, eventId: prev.eventId || null };
      pushSaveJson_('PUSH_EMERGENCY_STATE', estate);
      return null;
    }, 20000);
    if (r.busy || !r.value) return;
    deliverId = r.value;

    // [P1-D] delivery tepat SATU event yang baru dibuat — di luar lock.
    const ev = pushFindEvent_(deliverId);
    if (!ev) return;
    const subsAll = pushSubsAll_();
    const dr = pushDeliverEvent_(ev, subsAll);
    pushRecordDelivery_(deliverId, dr, subsAll);
  } catch (err) {
    // Push TIDAK PERNAH boleh menggagalkan jalur telemetry — catat saja.
    console.error('[PushService] emergency edge failed:', err);
  }
}

/* ======================= PUSH_TEST (operator) ======================= */

/** Dipanggil dispatcher setelah verifyAdminToken_ LULUS — testPush di
 *  canonical selalu terautentikasi admin (tidak ada jalur publik). */
function pushTest_(payload) {
  const id = pushMakeEventId_('TEST',
    String((payload && payload.deviceKey) || 'operator-test'), 'test', 1);
  const now = new Date().toISOString();
  const phase1 = pushWithLock_(function () {
    pushAppendEvent_({
      eventId: id,
      deviceId: String((payload && payload.deviceKey) || 'operator-test'),
      alarmCode: 'test',
      generation: 1,
      severity: 'info',
      raisedAt: now,
      clearedAt: now,
      state: 'CLEARED',
      acknowledgedAt: null,
      resolvesEventId: null,
      resolvedBy: null,
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
        sent: 0, failed: 0, removed: 0, perSub: {},
        claim: Utilities.getUuid().slice(0, 8), claimedAt: now }
    });
    return true;
  }, 20000);
  if (phase1.busy) return resp_(503, 'Backend busy — retry', null);

  const subsAll = pushSubsAll_();
  const ev = pushFindEvent_(id);
  if (ev) {
    const r = pushDeliverEvent_(ev, subsAll);     // di luar lock
    pushRecordDelivery_(id, r, subsAll);
  }
  return resp_(200, 'Test push dispatched', { eventId: id });
}

/* ======================= Observability ======================= */

/** PUSH_STATUS — ringkasan langganan + outbox + KAPASITAS + revision sumber
 *  (butuh autentikasi device/admin; dipanggil dispatcher di dalam wilayah
 *  terautentikasi). [round-3] storage.bytesUsed & sourceRevision ditambahkan
 *  untuk menutup temuan capacity-proof dan deployed-source-binding. */
function pushStatus_() {
  const subs = pushSubsAll_();
  const events = pushEvents_();
  return resp_(200, 'Push status', {
    subscriptions: subs.length,
    boundDevices: subs.filter(function (s) { return !!s.deviceId; }).length,
    vapidConfigured: !!(pushProps_().getProperty('VAPID_PUBLIC_KEY') &&
                        pushProps_().getProperty('VAPID_PRIVATE_KEY')),
    outbox: pushOutboxSummary_(events),
    storage: {
      bytesUsed: pushStorageBytes_(),
      valueLimit: PUSH_CFG.STORAGE_VALUE_LIMIT,
      totalLimit: PUSH_CFG.STORAGE_TOTAL_LIMIT,
      capacityDropped: Number(pushProps_().getProperty('PUSH_CAPACITY_DROPPED') || 0),
      legacyMigratedAt: pushProps_().getProperty('PUSH_LEGACY_MIGRATED_AT') || null
    },
    sourceRevision: PUSH_SOURCE_REVISION,
    deliverySemantics: 'at-least-once per subscriber (per-sub cursor)'
  });
}
