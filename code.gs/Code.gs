/**
 * PLTS Monitor — Container-Bound Google Apps Script (BACKEND v2)
 * ============================================================================
 * REMEDIATION 2026-08 — Consolidated Directive (Audit #1+#2+#3):
 *   [P0-001] Canonical API contract: PING, TELEMETRY, LATEST, HISTORY, DAILY,
 *            SEQ_STATUS, OTA_*, CALIBRATION_* — one documented surface.
 *   [P0-002] Canonical TelemetryEnvelope accepted AND returned (nested);
 *            Sheets is a persistence ADAPTER only (flat rows in, canonical
 *            envelope out — never the source of semantic truth).
 *   [P0-007] Config defaults aligned to the 48 V / 15S LiFePO4 production
 *            system (was 24V_8S_LIFEPO4 / 21.6 V cutoff — a cross-system
 *            contradiction with the firmware's 45 V low threshold).
 *   [P1-001] Telemetry identity = (device_key, sequence); sequence expected
 *            monotonic per device.
 *   [P1-002] Duplicate handling: same (device_key, sequence) → DUPLICATE ack,
 *            never a second row (idempotent insert).
 *   [P1-003] Out-of-order: late sequences (< expected) are stored flagged
 *            is_late; HISTORY orders by event_time, not arrival.
 *   [P1-004] Gaps: missing sequence ranges are RECORDED (SeqIndex ledger),
 *            never filled with synthetic rows or interpolated energy.
 *   [P1-016] Atomicity: every mutation runs under LockService (device-scoped)
 *            — check-then-insert is a single critical section.
 *   [WAVE-2 2026-08-28] Data honesty (re-audit GAS-2-E/F/G/M):
 *            honest per-channel quality on read (no fabricated VALID),
 *            DAILY buckets in the deployment timezone (Config TIMEZONE,
 *            default Asia/Jakarta), counter-reset detection in DAILY
 *            (segment sums + COUNTER_RESET flag), event_time validation at
 *            ingest (unparseable → server time + timeQuality DEGRADED).
 *            Spec + evidence: docs/remediation-2026-08/16_WAVE2_DATA_HONESTY.md.
 *   [WAVE-3 2026-08-28] Cross-device authorization (re-audit GAS-2-I/J/K/L):
 *            HMAC callers are pinned to their signed deviceId (a body
 *            device_key naming a different device → 400), CALIBRATION_ACK
 *            is bound to the device the command was queued for, OTA_PUBLISH
 *            requires the operator-only ADMIN_TOKEN (fail-closed until set
 *            in Config), calibration factors are range-checked, and LATEST
 *            tie-breaking no longer loses sequence precision at epoch
 *            magnitude. Spec + evidence:
 *            docs/remediation-2026-08/17_WAVE3_AUTHORIZATION.md.
 *   [WAVE-6 2026-08-29] Firmware-audit completion (doc 20):
 *            OTA_MANIFEST now serves a PER-DEVICE manifest hmac to firmware
 *            >= 1.5.4 (key = HMAC-SHA256(AUTH_TOKEN, device_key), derived
 *            identically on-device) — a leaked AUTH_TOKEN alone can no longer
 *            author OTA for the fleet. Devices < 1.5.4 (no fw_version in the
 *            request) keep receiving the fleet-keyed hmac: mixed fleets work,
 *            migration is per-device at update time. OTA_STATUS additionally
 *            accepts event REFUSED (anti-downgrade / non-semver refusal —
 *            previously the operator waited forever with zero feedback).
 *            Spec + evidence: docs/remediation-2026-08/20_WAVE6_FIRMWARE_COMPLETION.md.
 *   [WAVE-7 2026-09] Emergency relay control layer (E-series):
 *            EMERGENCY_COMMAND (operator-only ADMIN_TOKEN, fail-closed) queues
 *            ARM / DISARM / CONFIG for a device; EMERGENCY_PENDING +
 *            EMERGENCY_ACK mirror the proven CALIBRATION_* pending/ack pattern
 *            (device-bound, TTL-expiring, never deleted while pending);
 *            EMERGENCY_EVENT logs device-side trips (sensor / E-stop / boot)
 *            with bounded rotation + optional Telegram alert; EMERGENCY_LOG
 *            returns the recent event history. The TELEMETRY response now
 *            piggybacks `pendingEmergency` so firmware consumes commands on
 *            its existing ingest cadence (zero extra polls). Telemetry rows
 *            gain v1.7.0 columns (i_ac_gen + emergency state) appended at the
 *            end — old sheets migrate in place, old rows read as UNKNOWN.
 *
 * Response envelope (uniform across ALL actions):
 *   { status: 'SUCCESS'|'ERROR', code: number, data?: object|null,
 *     message: string, timestamp: ISO8601 }
 * GAS cannot set arbitrary HTTP status codes; `code` carries the semantic
 * status (200/400/401/404/409/500/503) documented in 02_CANONICAL_API_CONTRACT.md
 * — the PWA checks `status`/`code`, never string-matches messages.
 *
 * Auth (two supported paths, per-device):
 *   1. LEGACY token:  { token } body field vs Config!AUTH_TOKEN (shared secret
 *      — kept ONLY for firmware-generic ≤ v1.4.0 backward compatibility).
 *   2. HMAC (production, WAVE-1 contract v2.1): credentials travel INSIDE the
 *      body envelope — GAS Web Apps CANNOT read HTTP request headers, so any
 *      X-Auth-* header scheme is physically impossible on this platform:
 *        { action:'TELEMETRY',
 *          auth:{ method:'HMAC-SHA256', timestamp, nonce, deviceId, signature },
 *          data:'<raw telemetry JSON string>' }
 *      canonical = 'HMAC-SHA256\n' + action + '\n' + timestamp + '\n' +
 *                  nonce + '\n' + deviceId + '\n' + sha256hex(data)
 *      `data` is a RAW JSON STRING on this path (the exact bytes covered by
 *      the signature) so both sides hash identical bytes — immune to
 *      cross-runtime re-serialization drift (ArduinoJson vs V8 number
 *      formatting). `action` is signed (closes action-confusion replay,
 *      GAS-2-C). Replay window ±300 s + nonce cache 10 min.
 *      Full spec: docs/remediation-2026-08/15_WAVE1_PIPELINE_EXECUTION.md.
 *   [WAVE-3 / GAS-2-J] On the HMAC path the signed deviceId IS the caller's
 *      identity: a body-level device_key naming a DIFFERENT device is
 *      cross-device impersonation and gets a 400. The legacy token path
 *      keeps its documented single shared trust domain (F-G17).
 *   [WAVE-3 / GAS-2-K] OTA_PUBLISH additionally requires `admin_token`
 *      matching Config!ADMIN_TOKEN (operator-only secret, fail-closed while
 *      unset) — device credentials must never be enough to push firmware.
 * ============================================================================
 */

const CONFIG_SHEET = 'Config';
const LOG_SHEET = 'Telemetry';
const DEVICES_SHEET = 'Devices';
const SEQ_LEDGER_SHEET = 'SeqIndex';
const CACHE_TTL_S = 21600; // 6 hours

// [WAVE-7] Emergency relay control layer — command queue + event log.
const EMERGENCY_QUEUE_SHEET = 'EmergencyQueue';
const EMERGENCY_QUEUE_HEADER = [
  'command_id', 'device_key', 'command', 'params_json', 'note',
  'issued_at', 'status', 'applied_at', 'result_msg'
];
const EMERGENCY_EVENTS_SHEET = 'EmergencyEvents';
const EMERGENCY_EVENTS_HEADER = [
  'ts', 'device_key', 'type', 'reason', 'detail', 'state_after', 'source'
];
// Valid EMERGENCY_EVENT types (device-reported). Fail-closed whitelist.
const EMERGENCY_EVENT_TYPES = [
  'TRIP', 'ESTOP', 'BOOT', 'CRASHLOOP', 'ARMED', 'DISARMED',
  'CONFIG_APPLIED', 'REJECTED', 'ESTOP_RELEASED'
];
// Valid EMERGENCY_COMMAND commands (operator-issued).
const EMERGENCY_COMMANDS = ['ARM', 'DISARM', 'CONFIG'];
// Emergency trigger config schema: [field, min, max, default]. The CONFIG
// command body is validated against this table BEFORE queueing — the device
// re-validates on apply (mixed-version fleets must never park garbage in
// LittleFS). Pins are included so wiring can be adjusted without a reflash.
const EMERGENCY_CONFIG_FIELDS = [
  ['vbatLowV',      30,   60,  42.0],
  ['vbatLowHystV',  0.1,  5,   1.0],
  ['vbatHighV',     48,   60,  55.0],
  ['vbatHighHystV', 0.1,  5,   1.0],
  ['iDcOverA',      10,   120, 110.0],
  ['iAcLoadOverA',  5,    40,  28.0],
  ['iAcGenOverA',   5,    40,  28.0],
  ['debounceN',     1,    10,  3],
  ['recoverySec',   0,    3600, 60],
  ['relayPin',      12,   39,  27],
  ['estopPin',      -1,   39,  14],
  ['estopEnabled',  0,    1,   1],
  // v1.7.0 [P1-SC1] (audit remediation) — safety-sensor failure policy.
  // 1 (default) = fail-closed: the current sensors are MANDATORY safety
  //   inputs (they feed I_DC/I_AC_LOAD/I_AC_GEN trip triggers); firmware
  //   rejects ARM and trips SENSOR_LOSS while any safety sensor is
  //   absent/invalid. "Unmonitored IS unsafe" for a safety interlock.
  // 0 = explicit legacy opt-out (bench/commissioning only, unsafe).
  // Mixed-version fleets: field omitted by an older PWA → GAS injects the
  // default 1 here, so the fail-closed direction survives version skew.
  ['sensorFailPolicy', 0,  1,   1]
];

// [P0-007] 48 V / 15S LiFePO4 — matches firmware Core::Config defaults.
const DEFAULT_CONFIG = [
  ['AUTH_TOKEN', 'plts_sec_CHANGE_ME'],
  // [WAVE-3 / GAS-2-K] Operator-only secret gating OTA_PUBLISH. Empty by
  // design: OTA publishing stays DISABLED (fail-closed) until the operator
  // sets a real value in the Config sheet. NEVER burn this into firmware or
  // ship it in PWA config — it is the one credential that outranks devices.
  ['ADMIN_TOKEN', ''],
  ['DEVICE_KEY', 'PLTS_MONITOR_01'],
  ['BATTERY_SYSTEM_TYPE', '48V_15S_LIFEPO4'],
  ['BATTERY_NOMINAL_V', '48.0'],
  ['BATTERY_FULL_V', '54.0'],
  ['BATTERY_LOW_V', '45.0'],
  ['BATTERY_CRITICAL_V', '42.0'],
  ['BATTERY_CAPACITY_AH', '200'],
  ['VOLTAGE_CALIB_FACTOR', '18.857'],
  ['CURRENT_CALIB_FACTOR', '1.00'],
  ['TELEGRAM_BOT_TOKEN', ''],
  ['TELEGRAM_CHAT_ID', ''],
  ['LOW_BATTERY_CUTOFF_V', '45.0'],
  ['LOG_ROTATION_MAX_ROWS', '5000'],
  ['HISTORY_MAX_ROWS', '2000'],
  ['ALLOW_LATE_TELEMETRY', 'true'],
  // [GAS-2-F] DAILY-report bucketing timezone (operator-local calendar day).
  // Deployments that ran setupMasterTemplate before this key existed fall
  // back to 'Asia/Jakarta' via safeConfig_ (documented migration note in 16).
  ['TIMEZONE', 'Asia/Jakarta'],
  // [WAVE-4 / GAS-2-P + GAS-2-V] Alert cooldown + bookkeeping-sheet
  // retention. Deployments that predate these keys get the same defaults
  // via safeConfig_ fallbacks in code — no migration step needed.
  ['LOW_BATTERY_ALERT_COOLDOWN_MIN', '30'],
  ['OTA_MANIFEST_MAX_ROWS', '50'],
  ['OTA_EVENTS_MAX_ROWS', '2000'],
  ['CALIB_HISTORY_MAX_ROWS', '500'],
  // [WAVE-7] Emergency layer tuning. Queue TTL bounds how long an un-ACKed
  // command stays servable (a device offline past the TTL sees it EXPIRED —
  // an ARM command that old is stale by definition, never silently applied).
  ['EMERGENCY_QUEUE_TTL_MIN', '10'],
  ['EMERGENCY_EVENTS_MAX_ROWS', '500'],
  ['EMERGENCY_ALERT_COOLDOWN_MIN', '2'],
];

// Flat persistence columns (the ADAPTER representation — not the contract).
// event_time = device clock (authoritative ordering); timestamp = server
// ingestion time. is_late flags out-of-order arrivals.
// v1.6.0 columns (soc_source, bms_*) are APPENDED at the end so existing
// deployments keep valid indices — the header migrator extends old sheets
// in place (see getOrCreateSheet_ / migrateTelemetryHeader_).
const TELEMETRY_HEADER = [
  'timestamp', 'device_key', 'sequence', 'event_time', 'is_late',
  'v_bat', 'i_bat_dc', 'p_bat_dc', 'soc', 'soc_quality',
  'i_ac_load', 'p_ac_est', 'temp_celsius', 'humidity',
  'charge_wh', 'discharge_wh', 'charge_ah', 'discharge_ah',
  'ina219_ok', 'time_quality', 'overall_quality',
  'free_heap', 'rssi', 'fw_version',
  // v1.6.0 — multi-protocol BMS provenance (indices 24..30, 0-based)
  'soc_source', 'bms_protocol', 'bms_connected',
  'bms_cell_v_min', 'bms_cell_v_max', 'bms_temp_c', 'bms_fault_flags',
  // v1.7.0 [WAVE-7] — emergency relay state + 2nd ACS712 channel (indices
  // 31..35, 0-based). APPENDED so every existing deployment keeps valid
  // indices; migrateTelemetryHeader_ extends old sheets in place and old
  // rows read back as UNKNOWN / null (honest, never fabricated).
  'i_ac_gen', 'emg_state', 'emg_reason', 'emg_estop', 'emg_trips'
];
const TELEMETRY_HEADER_V1_5_LEN = 24;   // column count before the v1.6.0 extension
const TELEMETRY_HEADER_V1_6_LEN = 31;   // column count before the v1.7.0 extension

// last_nonce/last_ts are VESTIGIAL (schema-compat only, no longer written
// since WAVE-4 / GAS-2-X — replay protection lives in the nonce cache).
const DEVICES_HEADER = ['device_key', 'secret', 'label', 'last_nonce', 'last_ts'];
const SEQ_LEDGER_HEADER = ['device_key', 'expected_next', 'highest_seq', 'dup_count', 'gap_count', 'gaps_json'];

const PROTOCOL_VERSION = '2';

// ----------------------------------------------------------------------------
// HTTP entry points
// ----------------------------------------------------------------------------

function doPost(e) {
  // [WAVE-4 / GAS-2-R] A malformed JSON body is a CLIENT error (400), not a
  // server error (500). Parsing used to sit inside the generic catch, which
  // relabeled every garbage payload as "the backend broke" — a lie about
  // whose fault it is, and noise in any uptime monitoring.
  let body;
  try {
    body = JSON.parse((e && e.postData && e.postData.contents) || '{}');
  } catch (parseErr) {
    return json_(resp_(400, 'Request body is not valid JSON', null));
  }
  if (!body || typeof body !== 'object' || Array.isArray(body)) {
    return json_(resp_(400, 'Request body must be a JSON object', null));
  }
  try {
    const action = String(body.action || '').toUpperCase();

    // ---- Auth: HMAC body envelope (production) or legacy token body ----
    const auth = authenticate_(body, action);

    if (action === 'PING') {
      // [WAVE-4 / GAS-2-S] The handshake may optionally report device
      // REGISTRATION (additive data field; PING/PONG contract unchanged).
      return json_(resp_(auth.ok ? 200 : 401, auth.ok ? 'PONG' : auth.reason,
        auth.ok ? pingHandshakeData_(body, auth) : null));
    }
    if (!auth.ok) return json_(resp_(401, 'Unauthorized: ' + auth.reason, null));

    // [WAVE-3 / GAS-2-J] HMAC callers are pinned to the device named in the
    // signed envelope. A body-level device_key pointing at a DIFFERENT
    // device is cross-device impersonation (reading another device's
    // telemetry, queueing calibration for it, forging its OTA logs) — reject
    // loudly. The legacy token path keeps body.device_key free: its single
    // shared trust domain is documented at requireRegisteredDevice_ (F-G17).
    if (auth.method === 'HMAC' && body.device_key != null &&
        String(body.device_key).trim() !== String(auth.deviceKey).trim()) {
      return json_(resp_(400,
        'Unauthorized: body device_key "' + body.device_key +
        '" does not match the HMAC-authenticated device "' + auth.deviceKey + '"', null));
    }

    // [WAVE-1 / GAS-2-B] HMAC clients send `data` as a RAW JSON STRING — the
    // exact bytes covered by the signature. Unwrap centrally, AFTER signature
    // verification, so every handler below keeps receiving an object.
    // Empty string = action carries no data (e.g. PING/LATEST via HMAC).
    if (typeof body.data === 'string' && body.data.length > 0) {
      try {
        body.data = JSON.parse(body.data);
      } catch (err) {
        return json_(resp_(400, 'data field is not valid JSON', null));
      }
    }

    if (action === 'TELEMETRY') {
      const gate = requireRegisteredDevice_(auth.deviceKey);
      if (gate) return json_(gate);
      return json_(recordTelemetry_(body, auth.deviceKey));
    }
    if (action === 'LATEST') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(latestTelemetry_(dk));
    }
    if (action === 'HISTORY') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(historyTelemetry_(body, dk));
    }
    if (action === 'DAILY') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(dailyReport_(body, dk));
    }
    if (action === 'SEQ_STATUS') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(seqStatus_(dk));
    }
    if (action === 'OTA_MANIFEST') {
      // [WAVE-6 / FW6-9] Resolve the caller's device + firmware version so
      // the manifest hmac can be keyed per-device for fw >= 1.5.4. Legacy
      // callers (no fw_version) transparently keep the fleet-keyed hmac.
      const dk = resolveDeviceKey_(body, auth);
      const payload = bodyPayload_(body);
      const fwv = String(
        (payload && payload.fw_version !== undefined ? payload.fw_version : '') ||
        body.fw_version || '');
      return json_(otaGetManifest_(dk, fwv));
    }
    if (action === 'OTA_PUBLISH') {
      // [WAVE-3 / GAS-2-K] OTA publishing is an OPERATOR action, not a fleet
      // action: a device credential (or the legacy AUTH_TOKEN that firmware
      // holds) must never be enough to push firmware to the whole fleet.
      // Fail-closed while Config!ADMIN_TOKEN is unset.
      const admin = verifyAdminToken_(body.admin_token);
      if (!admin.ok) return json_(resp_(401, 'Unauthorized: ' + admin.reason, null));
      return json_(otaPublishManifest_(body.manifest || {}));
    }
    if (action === 'OTA_STATUS') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      const payload = bodyPayload_(body);
      return json_(otaLogStatus_({
        device_key: dk,
        event: payload.event !== undefined ? payload.event : body.event,
        version: payload.version !== undefined ? payload.version : body.version,
        message: payload.message !== undefined ? payload.message : body.message
      }));
    }
    if (action === 'CALIBRATION_PUBLISH') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      const payload = bodyPayload_(body);
      return json_(calibrationPublish_({
        device_key: dk,
        v_calib: payload.v_calib !== undefined ? payload.v_calib : body.v_calib,
        i_calib_dc: payload.i_calib_dc !== undefined ? payload.i_calib_dc : body.i_calib_dc,
        i_calib_ac: payload.i_calib_ac !== undefined ? payload.i_calib_ac : body.i_calib_ac,
        note: payload.note !== undefined ? payload.note : body.note
      }));
    }
    if (action === 'CALIBRATION_PENDING') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(calibrationPending_(dk));
    }
    if (action === 'CALIBRATION_ACK') {
      // [WAVE-3 / GAS-2-I] The ACK is bound to the device the command was
      // queued for. HMAC clients ride command_id inside the signed `data`
      // string; token clients (firmware-generic ≥ v1.5.1) send it top-level
      // together with device_key.
      const payload = bodyPayload_(body);
      return json_(calibrationAck_(
        payload.command_id !== undefined ? payload.command_id : body.command_id,
        resolveDeviceKey_(body, auth)));
    }

    // ---- [WAVE-7] Emergency relay control layer ----
    if (action === 'EMERGENCY_COMMAND') {
      // Operator-only (same gate as OTA_PUBLISH): a device credential must
      // never be able to ARM/DISARM the fleet's safety relay. Fail-closed
      // while Config!ADMIN_TOKEN is unset.
      const admin = verifyAdminToken_(body.admin_token);
      if (!admin.ok) return json_(resp_(401, 'Unauthorized: ' + admin.reason, null));
      const dk = resolveDeviceKey_(body, auth);
      if (dk && String(body.device_key) != null && String(body.device_key).trim() !== '' &&
          String(body.device_key).trim() !== String(dk).trim()) {
        return json_(resp_(400,
          'Unauthorized: body device_key "' + body.device_key +
          '" does not match the authenticated device "' + dk + '"', null));
      }
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(emergencyCommand_(body, dk));
    }
    if (action === 'EMERGENCY_PENDING') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      return json_(emergencyPending_(dk));
    }
    if (action === 'EMERGENCY_ACK') {
      // [WAVE-3 / GAS-2-I pattern] ACK bound to the device the command was
      // queued for — cross-device ACK is rejected and the row stays unapplied.
      const payload = bodyPayload_(body);
      return json_(emergencyAck_(
        payload.command_id !== undefined ? payload.command_id : body.command_id,
        payload.result !== undefined ? payload.result : body.result,
        payload.message !== undefined ? payload.message : body.message,
        payload.state !== undefined ? payload.state : body.state,
        resolveDeviceKey_(body, auth)));
    }
    if (action === 'EMERGENCY_EVENT') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      // Token clients (firmware-generic) send flat top-level fields; HMAC
      // clients ride them inside the signed `data` string — accept both,
      // exactly like OTA_STATUS.
      const payload = bodyPayload_(body);
      return json_(emergencyEvent_({
        type:     payload.type !== undefined ? payload.type : body.type,
        reason:   payload.reason !== undefined ? payload.reason : body.reason,
        detail:   payload.detail !== undefined ? payload.detail : body.detail,
        state:    payload.state !== undefined ? payload.state : body.state
      }, dk));
    }
    if (action === 'EMERGENCY_LOG') {
      const dk = resolveDeviceKey_(body, auth);
      const gate = requireRegisteredDevice_(dk);
      if (gate) return json_(gate);
      const payload = bodyPayload_(body);
      const limit = Math.min(Number(
        (payload.limit !== undefined ? payload.limit : body.limit)) || 20, 100);
      return json_(emergencyLog_(dk, limit));
    }

    return json_(resp_(400,
      'Unknown action. Supported: PING, TELEMETRY, LATEST, HISTORY, DAILY, ' +
      'SEQ_STATUS, OTA_MANIFEST, OTA_PUBLISH, OTA_STATUS, CALIBRATION_PUBLISH, ' +
      'CALIBRATION_PENDING, CALIBRATION_ACK, EMERGENCY_COMMAND, ' +
      'EMERGENCY_PENDING, EMERGENCY_ACK, EMERGENCY_EVENT, EMERGENCY_LOG.', null));
  } catch (err) {
    return json_(resp_(500, String(err && err.message ? err.message : err), null));
  }
}

function doGet(e) {
  const action = String(((e && e.parameter) || {}).action || 'HEALTH').toUpperCase();
  if (action === 'HEALTH') {
    return json_(resp_(200, 'PLTS Monitor backend online', {
      protocolVersion: PROTOCOL_VERSION,
      backend: 'google-apps-script',
      sheets: {
        telemetry: !!getOrCreateSheet_(LOG_SHEET, TELEMETRY_HEADER),
        devices: !!getOrCreateSheet_(DEVICES_SHEET, DEVICES_HEADER),
        seqLedger: !!getOrCreateSheet_(SEQ_LEDGER_SHEET, SEQ_LEDGER_HEADER)
      }
    }));
  }
  return json_(resp_(400, 'Please use POST for API calls', null));
}

// ----------------------------------------------------------------------------
// Authentication — HMAC (production) + legacy token (compat)
// ----------------------------------------------------------------------------

function authenticate_(body, action) {
  // Path 1: HMAC credentials in the BODY envelope (production firmware
  // GasAdvisor ≥ v1.6.2 — WAVE-1 synchronized contract, fixes GAS-2-B).
  // GAS Web Apps cannot read HTTP request headers; X-Auth-* header schemes
  // are physically impossible on this platform (the comment that used to
  // claim GasAdvisor posts the body-envelope form was fiction — it did not,
  // see re-audit GAS-2-B lapis 2).
  const auth = (body && body.auth) || null;
  if (auth && String(auth.method || '').toUpperCase() === 'HMAC-SHA256') {
    const ok = verifyHmac_(auth, body, action);
    if (!ok.ok) return { ok: false, reason: ok.reason };
    return { ok: true, deviceKey: ok.deviceKey, method: 'HMAC' };
  }
  // Path 2: legacy shared token (firmware-generic ≤ v1.4.0).
  if (verifyToken_(body.token)) {
    return { ok: true, deviceKey: body.device_key || safeConfig_('DEVICE_KEY'), method: 'TOKEN' };
  }
  return { ok: false, reason: 'invalid auth (no valid HMAC envelope or AUTH_TOKEN)' };
}

function verifyHmac_(auth, body, action) {
  const deviceKey = String(auth.deviceId || '');
  if (!deviceKey) return { ok: false, reason: 'missing deviceId' };
  if (!auth.timestamp || !auth.nonce || !auth.signature) {
    return { ok: false, reason: 'incomplete HMAC envelope' };
  }
  // Replay window ±300 s on server time.
  const skew = Math.abs(Date.now() / 1000 - Number(auth.timestamp));
  if (skew > 300) return { ok: false, reason: 'timestamp outside replay window' };

  const sheet = getOrCreateSheet_(DEVICES_SHEET, DEVICES_HEADER);
  const rows = sheet.getDataRange().getValues();
  let secret = '';
  for (let i = 1; i < rows.length; i++) {
    if (String(rows[i][0]).trim() === deviceKey) {
      secret = String(rows[i][1]).trim();
      break;
    }
  }
  if (!secret) return { ok: false, reason: 'unknown device' };

  // Nonce replay check (CacheService, per-device, 10 min TTL).
  const cache = CacheService.getScriptCache();
  const nonceKey = 'PLTS_NONCE_' + deviceKey + '_' + auth.nonce;
  if (cache.get(nonceKey)) return { ok: false, reason: 'nonce replayed' };

  // [WAVE-1 / GAS-2-B + GAS-2-C] Signature over the SYNCHRONIZED canonical
  // string — byte-identical to GasAdvisor::_signRequest() (firmware ≥ v1.6.2):
  //   'HMAC-SHA256' \n action \n timestamp \n nonce \n deviceId \n sha256hex(data)
  // `data` must be the RAW JSON STRING received in the body ('' when the
  // action carries no data). Hashing the exact received bytes makes the
  // contract immune to cross-runtime re-serialization drift (ArduinoJson vs
  // V8 number formatting), and signing `action` closes action-confusion
  // replay (a captured TELEMETRY signature cannot be replayed as another
  // action).
  let dataStr = '';
  if (body.data != null) {
    if (typeof body.data !== 'string') {
      return { ok: false, reason: 'HMAC envelope requires data as a raw JSON string' };
    }
    dataStr = body.data;
  }
  const dataDigest = Utilities.computeDigest(
      Utilities.DigestAlgorithm.SHA_256, dataStr, Utilities.Charset.UTF_8)
    .map(function (b) { return ('0' + (b & 0xFF).toString(16)).slice(-2); })
    .join('');
  const canonical = 'HMAC-SHA256\n' + action + '\n' + auth.timestamp + '\n' +
                    auth.nonce + '\n' + deviceKey + '\n' + dataDigest;
  const sig = Utilities.computeHmacSha256Signature(
      canonical, secret, Utilities.Charset.UTF_8)
    .map(function (b) { return ('0' + (b & 0xFF).toString(16)).slice(-2); })
    .join('');
  if (sig !== String(auth.signature).toLowerCase()) {
    return { ok: false, reason: 'signature mismatch' };
  }
  cache.put(nonceKey, '1', 600);
  // [WAVE-4 / GAS-2-X] No sheet write-back of last_nonce/last_ts: that added
  // a sheet WRITE (~0.5–1.5 s) to EVERY authenticated POST with zero
  // functional value — replay protection is the CacheService nonce cache
  // above (10 min TTL > 5 min replay window). The DEVICES columns stay in
  // the schema for compatibility but are intentionally no longer written.
  return { ok: true, deviceKey: deviceKey };
}

// ----------------------------------------------------------------------------
// TELEMETRY — canonical envelope ingest with idempotent insert
// ----------------------------------------------------------------------------

/**
 * Accepts BOTH representations (adapter, P0-002):
 *   A) Canonical nested envelope (production firmware v1.5.0):
 *      { data: { protocolVersion, deviceId, sequence, timestamp, timeQuality,
 *                battery:{voltage:{value,quality},current:{...},power:{...},
 *                         soc:{value,quality}, chargeWh, dischargeWh, ...},
 *                ac:{rmsCurrent:{value},estimatedPower:{value}},
 *                environment:{temperature:{value},humidity:{value}},
 *                health:{freeHeap,rssi,...}, overallQuality } }
 *   B) Legacy flat (firmware-generic v1.4.0):
 *      { data: { v_bat, i_bat_dc, p_bat_dc, i_ac_load, ina219_ok,
 *                temp_celsius, free_heap, rssi, fw_version } }
 * Identity: (device_key, sequence). Duplicate → 409-equivalent DUPLICATE ack.
 */
function recordTelemetry_(body, deviceKey) {
  const data = body.data || {};
  const norm = normalizeEnvelope_(data, deviceKey);

  if (norm.sequence === null || !isFinite(Number(norm.sequence))) {
    return resp_(400, 'TELEMETRY requires a numeric sequence (identity: device_key + sequence)', null);
  }

  // [P1-016 + WAVE-4 / GAS-2-O] This is a GLOBAL script lock — GAS offers
  // no per-device locks, so the WHOLE fleet serializes through ONE critical
  // section (throughput bound: roughly one ingest per 1–3 s; documented in
  // 18_WAVE4_HYGIENE.md). The duplicate-check + appendRow + ledger upsert
  // must stay atomic, but everything SLOW that mutates NO state belongs
  // OUTSIDE the lock: the Telegram low-battery HTTP call used to run inside
  // it, stalling ingest for EVERY device for the duration of an external
  // network round-trip.
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(20000)) {
    return resp_(503, 'Backend busy — retry (lock timeout)', null);
  }
  let out;
  try {
    out = recordTelemetryLocked_(norm, deviceKey);   // → { resp, alert }
  } finally {
    lock.releaseLock();
  }
  // [WAVE-4 / GAS-2-O] Alerts fire AFTER the lock is released — one slow
  // Telegram round-trip must never serialize the whole fleet's ingest.
  if (out && out.alert) {
    maybeSendTelegramAlert_(out.alert, 'PLTS_TG_LOWBATT_' + deviceKey);
  }
  // [WAVE-7] Piggyback the device's pending emergency command on the ingest
  // response — the firmware consumes commands on its EXISTING cadence (zero
  // extra polls). Read-only scan of a small bounded sheet, outside the lock.
  if (out && out.resp && out.resp.data && typeof out.resp.data === 'object') {
    try {
      const pending = emergencyPendingFor_(deviceKey);
      out.resp.data.pendingEmergency = pending ? {
        command_id: pending.command_id, command: pending.command,
        note: pending.note, config: pending.config
      } : null;
    } catch (err) {
      out.resp.data.pendingEmergency = null;   // queue hiccup never breaks ingest
    }
  }
  return out.resp;
}

/** [WAVE-4 / GAS-2-O] Locked section of recordTelemetry_ (see above). */
function recordTelemetryLocked_(norm, deviceKey) {
  const sheet = getOrCreateSheet_(LOG_SHEET, TELEMETRY_HEADER);
    const ledger = getOrCreateSheet_(SEQ_LEDGER_SHEET, SEQ_LEDGER_HEADER);

    const seq = Number(norm.sequence);
    const lrow = findLedgerRow_(ledger, deviceKey);
    const lvals = lrow > 0 ? ledger.getRange(lrow, 1, 1, 6).getValues()[0] : null;
    let expectedNext = lvals ? Number(lvals[1]) : null;
    let highestSeq = lvals ? Number(lvals[2]) : 0;
    let dupCount = lvals ? Number(lvals[3]) : 0;
    let gapCount = lvals ? Number(lvals[4]) : 0;
    let gaps = lvals ? parseGaps_(lvals[5]) : [];

    const allowLate = String(safeConfig_('ALLOW_LATE_TELEMETRY')).toLowerCase() !== 'false';

    let decision = 'ACCEPTED';
    let isLate = false;

    if (expectedNext === null || expectedNext === 0) {
      // First telemetry from this device — accept, set ledger.
      expectedNext = seq + 1;
    } else if (seq === expectedNext - 1 && seenExact_(sheet, deviceKey, seq)) {
      decision = 'DUPLICATE'; dupCount++;
    } else if (seq >= expectedNext) {
      // [P1-004] GAP — record the missing range, never fabricate rows.
      if (seq > expectedNext) {
        gaps.push({ from: expectedNext, to: seq - 1, detectedAt: new Date().toISOString() });
        if (gaps.length > 20) gaps = gaps.slice(gaps.length - 20);
        gapCount += (seq - expectedNext);
      }
      expectedNext = seq + 1;
    } else {
      // seq < expectedNext — duplicate or late arrival; exact existence check.
      if (seenExact_(sheet, deviceKey, seq)) {
        decision = 'DUPLICATE'; dupCount++;
      } else if (allowLate) {
        decision = 'ACCEPTED_LATE'; isLate = true;   // [P1-003]
      } else {
        decision = 'REJECTED_LATE';
      }
    }
    if (seq > highestSeq) highestSeq = seq;

    if (decision === 'DUPLICATE') {
      upsertLedger_(ledger, lrow, deviceKey, expectedNext, highestSeq, dupCount, gapCount, gaps);
      return { resp: resp_(409, 'Duplicate telemetry — (device_key, sequence) already stored', {
        deviceKey: deviceKey, sequence: seq, decision: 'DUPLICATE'
      }), alert: null };
    }
    if (decision === 'REJECTED_LATE') {
      upsertLedger_(ledger, lrow, deviceKey, expectedNext, highestSeq, dupCount, gapCount, gaps);
      return { resp: resp_(400, 'Late telemetry rejected by policy (ALLOW_LATE_TELEMETRY=false)', {
        deviceKey: deviceKey, sequence: seq, decision: 'REJECTED_LATE'
      }), alert: null };
    }

    const now = new Date();
    sheet.appendRow([
      now,                                   // timestamp (server ingestion)
      deviceKey,
      seq,
      norm.eventTime ? new Date(norm.eventTime) : now, // event_time (device clock)
      isLate,
      norm.vBat, norm.iBat, norm.pBat, norm.soc, norm.socQuality,
      norm.iAc, norm.pAc, norm.temp, norm.humidity,
      norm.chargeWh, norm.dischargeWh, norm.chargeAh, norm.dischargeAh,
      norm.ina219Ok, norm.timeQuality, norm.overallQuality,
      norm.freeHeap, norm.rssi, norm.fwVersion,
      // v1.6.0 — BMS provenance columns
      norm.socSource, norm.bmsProtocol, norm.bmsConnected,
      norm.bmsCellVMin, norm.bmsCellVMax, norm.bmsTempC, norm.bmsFaultFlags,
      // v1.7.0 [WAVE-7] — 2nd ACS712 channel + emergency relay state
      norm.iAcGen, norm.emgState, norm.emgReason, norm.emgEstop, norm.emgTrips
    ]);
    rotateLogs_(sheet);
    upsertLedger_(ledger, lrow, deviceKey, expectedNext, highestSeq, dupCount, gapCount, gaps);

    // [WAVE-4 / GAS-2-P + GAS-2-O] Compute the alert MESSAGE here (cheap),
    // let the caller SEND it outside the lock (with per-device cooldown).
    const alert = lowBatteryAlertMessage_(norm, deviceKey);

    return { resp: resp_(200, decision === 'ACCEPTED_LATE' ? 'Late telemetry stored' : 'Telemetry stored', {
      deviceKey: deviceKey, sequence: seq, decision: decision,
      gapsOpen: gapCount
    }), alert: alert };
}

/** Exact (device_key, sequence) existence check over the telemetry sheet. */
function seenExact_(sheet, deviceKey, seq) {
  const last = sheet.getLastRow();
  if (last < 2) return false;
  // [WAVE-4 / GAS-2-T] Scan window covers the FULL retained sheet — the old
  // hardcoded 3.000 let a duplicate age past the window while still stored
  // (rotation 5.000): it then classified as ACCEPTED_LATE and was stored a
  // SECOND time. Bounded by retentionScanRows_ (config-driven).
  const from = Math.max(2, last - retentionScanRows_());
  const vals = sheet.getRange(from, 2, last - from + 1, 2).getValues(); // device_key, sequence
  for (let i = vals.length - 1; i >= 0; i--) {
    if (String(vals[i][0]) === String(deviceKey) && Number(vals[i][1]) === Number(seq)) {
      return true;
    }
  }
  return false;
}

function findLedgerRow_(ledger, deviceKey) {
  const last = ledger.getLastRow();
  if (last < 2) return 0;
  const vals = ledger.getRange(2, 1, last - 1, 1).getValues();
  for (let i = 0; i < vals.length; i++) {
    if (String(vals[i][0]).trim() === String(deviceKey)) return i + 2;
  }
  return 0;
}

function upsertLedger_(ledger, row, deviceKey, expectedNext, highestSeq, dupCount, gapCount, gaps) {
  const rowVals = [[deviceKey, expectedNext, highestSeq, dupCount, gapCount, JSON.stringify(gaps)]];
  if (row > 0) ledger.getRange(row, 1, 1, 6).setValues(rowVals);
  else ledger.appendRow(rowVals[0]);
}

function parseGaps_(json) {
  try { const g = JSON.parse(json || '[]'); return Array.isArray(g) ? g : []; }
  catch (err) { return []; }
}

/** Adapter: nested canonical OR legacy flat → normalized row values. */
function normalizeEnvelope_(data, deviceKey) {
  const n = {
    deviceKey: deviceKey,
    sequence: null, eventTime: null, timeQuality: 'UNKNOWN', overallQuality: 'UNKNOWN',
    vBat: '', iBat: '', pBat: '', soc: '', socQuality: 'UNKNOWN',
    iAc: '', pAc: '', temp: '', humidity: '',
    chargeWh: '', dischargeWh: '', chargeAh: '', dischargeAh: '',
    ina219Ok: false, freeHeap: '', rssi: '', fwVersion: '',
    // v1.6.0 — SOC provenance + BMS block (absent = legacy device → UNKNOWN)
    socSource: 'UNKNOWN',
    bmsProtocol: '', bmsConnected: false,
    bmsCellVMin: '', bmsCellVMax: '', bmsTempC: '', bmsFaultFlags: '',
    // v1.7.0 [WAVE-7] — 2nd ACS712 (genset→inverter) + emergency relay state
    iAcGen: '', emgState: '', emgReason: '', emgEstop: false, emgTrips: ''
  };
  if (data.battery || data.protocolVersion) {
    // Canonical nested envelope
    n.sequence = data.sequence != null ? Number(data.sequence) : null;
    // [GAS-2-M] Unparseable device clock must never reach the sheet as
    // "Invalid Date" (it poisons HISTORY/LATEST ordering with NaN keys and
    // lies about when the sample was taken). Honest degradation: fall back
    // to server time for ordering + timeQuality='DEGRADED' so consumers see
    // that the device clock was garbage.
    const et = sanitizeEventTime_(data.timestamp || data.eventTime || null);
    n.eventTime = et.iso;
    n.timeQuality = et.degraded
      ? 'DEGRADED'
      : String((data.timeQuality || 'UNKNOWN')).toUpperCase();
    const bat = data.battery || {};
    n.vBat = measVal_(bat.voltage); n.iBat = measVal_(bat.current); n.pBat = measVal_(bat.power);
    n.soc = bat.soc ? measVal_(bat.soc) : '';
    n.socQuality = bat.soc ? String(bat.soc.quality || 'UNKNOWN').toUpperCase() : 'UNKNOWN';
    n.socSource = String((bat.soc && bat.soc.provenance) || 'UNKNOWN').toUpperCase();
    n.chargeWh = numOrEmpty_(bat.chargeWh); n.dischargeWh = numOrEmpty_(bat.dischargeWh);
    n.chargeAh = numOrEmpty_(bat.chargeAh); n.dischargeAh = numOrEmpty_(bat.dischargeAh);
    const bms = bat.bms || null;
    if (bms) {
      n.bmsProtocol = String(bms.protocol || '');
      n.bmsConnected = bms.connected === true;
      n.bmsCellVMin = numOrEmpty_(bms.cellVoltageMin);
      n.bmsCellVMax = numOrEmpty_(bms.cellVoltageMax);
      n.bmsTempC = numOrEmpty_(bms.temperature);
      n.bmsFaultFlags = bms.faultFlags != null ? numOrEmpty_(bms.faultFlags) : '';
    }
    const ac = data.ac || {};
    n.iAc = measVal_(ac.rmsCurrent);
    n.pAc = ac.estimatedPower ? measVal_(ac.estimatedPower) : '';
    // v1.7.0 [WAVE-7] — 2nd ACS712 channel (genset→inverter feed)
    n.iAcGen = measVal_(ac.gensetRmsCurrent);
    // v1.7.0 [WAVE-7] — emergency relay block (state/reason/estop/trips)
    const emg = data.emergency || null;
    if (emg) {
      n.emgState = String(emg.state || '').toUpperCase();
      n.emgReason = String(emg.reason || '');
      n.emgEstop = (emg.estopLine === true || String(emg.estopLine).toUpperCase() === 'TRUE'
        || String(emg.estopLine).toUpperCase() === 'OPEN');
      n.emgTrips = numOrEmpty_(emg.tripCount);
    }
    const env = data.environment || {};
    n.temp = measVal_(env.temperature); n.humidity = measVal_(env.humidity);
    const health = data.health || {};
    n.ina219Ok = !!(health.sensorHealth && String(health.sensorHealth.ina219).toUpperCase() === 'ONLINE');
    n.freeHeap = numOrEmpty_(health.freeHeap); n.rssi = numOrEmpty_(health.wifiRssi);
    n.fwVersion = String(data.firmwareVersion || '');
    n.overallQuality = String(data.overallQuality || deriveOverallQuality_(data)).toUpperCase();
  } else {
    // Legacy flat (firmware-generic)
    n.sequence = data.sequence != null ? Number(data.sequence) : null;
    // [GAS-2-M] same honest degradation on the flat path
    const etFlat = sanitizeEventTime_(data.event_time || data.eventTime || null);
    n.eventTime = etFlat.iso;
    n.vBat = numOrEmpty_(data.v_bat);
    n.iBat = numOrEmpty_(data.i_bat_dc);
    n.pBat = numOrEmpty_(data.p_bat_dc);
    n.iAc = numOrEmpty_(data.i_ac_load);
    n.temp = numOrEmpty_(data.temp_celsius);
    // v1.7.0 [WAVE-7] — flat path (firmware-generic ≥ 1.6.0)
    n.iAcGen = numOrEmpty_(data.i_ac_gen);
    n.emgState = String(data.emg_state || '').toUpperCase();
    n.emgReason = String(data.emg_reason || '');
    n.emgEstop = (data.emg_estop === true || String(data.emg_estop).toUpperCase() === 'TRUE'
      || String(data.emg_estop).toUpperCase() === 'OPEN');
    n.emgTrips = numOrEmpty_(data.emg_trips);
    n.ina219Ok = (data.ina219_ok === true || data.ina219_ok === 'true');
    n.freeHeap = numOrEmpty_(data.free_heap);
    n.rssi = numOrEmpty_(data.rssi);
    n.fwVersion = String(data.fw_version || '');
    n.timeQuality = etFlat.degraded ? 'DEGRADED' : 'UNKNOWN';
    n.overallQuality = n.ina219Ok ? 'VALID' : 'SUSPECT';
  }
  return n;
}

function measVal_(m) {
  if (m == null) return '';
  if (typeof m === 'number') return Number.isFinite(m) ? m : '';
  const v = m.value;
  if (v == null) return '';
  const num = Number(v);
  return Number.isFinite(num) ? num : '';
}

function deriveOverallQuality_(data) {
  const parts = [];
  const bat = data.battery || {};
  [bat.voltage, bat.current, bat.power].forEach(function (m) {
    if (m && m.quality) parts.push(String(m.quality).toUpperCase());
  });
  if (parts.indexOf('SENSOR_ERROR') >= 0) return 'SENSOR_ERROR';
  if (parts.indexOf('INVALID') >= 0 || parts.indexOf('OUT_OF_RANGE') >= 0) return 'INVALID';
  if (parts.indexOf('STALE') >= 0) return 'STALE';
  if (parts.indexOf('SUSPECT') >= 0) return 'SUSPECT';
  return 'VALID';
}

// ----------------------------------------------------------------------------
// LATEST / HISTORY / DAILY / SEQ_STATUS — canonical read model
// ----------------------------------------------------------------------------

function latestTelemetry_(deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(LOG_SHEET);
  if (!sheet || sheet.getLastRow() < 2) return resp_(404, 'No telemetry data yet', null);
  const last = sheet.getLastRow();
  // [WAVE-4 / GAS-2-U] Scan window is retention-driven (was hardcoded 3.000)
  // — raising LOG_ROTATION_MAX_ROWS must widen the LATEST scan with it, or
  // the true newest sample silently falls outside the window.
  const from = Math.max(2, last - retentionScanRows_());
  const vals = sheet.getRange(from, 1, last - from + 1, TELEMETRY_HEADER.length).getValues();
  // [P1-003 REMEDIATION 2026-08] LATEST = newest by EVENT TIME (then
  // sequence), NOT by arrival — a late-arriving historical row must never
  // displace genuinely newer telemetry as "latest".
  // [WAVE-3 / GAS-2-L] Two EXPLICIT comparisons — never `ev*1e6+seq`:
  // at epoch magnitude (ms ~1.77e12) that product is ~1.77e18, beyond
  // 2^53 (9.007e15), so sequence differences under ~256 vanish in float
  // granularity and the promised sequence tie-break silently stops
  // working. Comparing (ev, seq) lexicographically keeps every integer
  // exact.
  let best = null;
  let bestEv = -Infinity;
  let bestSeq = -Infinity;
  for (let i = 0; i < vals.length; i++) {
    if (String(vals[i][1]) !== String(deviceKey)) continue;
    const ev = vals[i][3] instanceof Date ? vals[i][3].getTime()
             : (vals[i][3] ? Date.parse(String(vals[i][3])) : 0);
    // Present-but-unparseable legacy event_time → skip honestly: a garbage
    // clock must not claim to be the newest sample (pre-wave-2 rows only;
    // ingest has validated event_time since GAS-2-M). Empty → epoch 0
    // keeps legacy eligibility.
    if (!isFinite(ev)) continue;
    const seq = Number(vals[i][2]) || 0;
    if (ev > bestEv || (ev === bestEv && seq >= bestSeq)) {
      bestEv = ev; bestSeq = seq; best = vals[i];
    }
  }
  if (best) return resp_(200, 'Latest telemetry', rowToEnvelope_(best));
  return resp_(404, 'No telemetry for device ' + deviceKey, null);
}

/**
 * HISTORY — { device_key, from?, to?, limit? }
 * Ordered by event_time (NOT arrival — P1-003). Bounded by HISTORY_MAX_ROWS.
 */
function historyTelemetry_(body, deviceKeyOverride) {
  const deviceKey = deviceKeyOverride || body.device_key;
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const limit = Math.min(Number(body.limit) || 500, Number(safeConfig_('HISTORY_MAX_ROWS')) || 2000);
  const from = body.from ? new Date(body.from) : null;
  const to = body.to ? new Date(body.to) : null;

  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(LOG_SHEET);
  if (!sheet || sheet.getLastRow() < 2) return resp_(200, 'No telemetry data yet', { records: [], total: 0 });

  const last = sheet.getLastRow();
  // [WAVE-4 / GAS-2-U] Was hardcoded 5.000: raising LOG_ROTATION_MAX_ROWS
  // above it silently truncated HISTORY despite rows being retained.
  const from2 = Math.max(2, last - retentionScanRows_());
  const vals = sheet.getRange(from2, 1, last - from2 + 1, TELEMETRY_HEADER.length).getValues();
  const records = [];
  for (let i = 0; i < vals.length; i++) {
    if (String(vals[i][1]) !== String(deviceKey)) continue;
    const ev = vals[i][3] instanceof Date ? vals[i][3] : null;
    if (from && (!ev || ev < from)) continue;
    if (to && (!ev || ev > to)) continue;
    records.push(rowToEnvelope_(vals[i]));
  }
  // event_time ordering, stable for ties by sequence
  records.sort(function (a, b) {
    // [GAS-2-M] NaN-safe parse: an unparseable legacy eventTime sorts as 0 —
    // deterministic ordering even if a corrupt row predates ingest validation.
    const ta = safeTimeKey_(a.eventTime);
    const tb = safeTimeKey_(b.eventTime);
    if (ta !== tb) return ta - tb;
    return (a.sequence || 0) - (b.sequence || 0);
  });
  const total = records.length;
  return resp_(200, 'Telemetry history', {
    deviceKey: deviceKey,
    total: total,
    returned: Math.min(total, limit),
    records: records.slice(Math.max(0, total - limit))   // newest `limit` records
  });
}

/**
 * DAILY — { device_key, days? }
 * Honest aggregation: daily energy = cumulative-counter delta between the
 * day's first and last VALID samples. Missing boundary samples → energy null
 * with quality INCOMPLETE (no interpolation across gaps — directive §3.3).
 */
function dailyReport_(body, deviceKeyOverride) {
  const deviceKey = deviceKeyOverride || body.device_key;
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const days = Math.min(Number(body.days) || 7, 31);

  const hist = historyTelemetry_({ device_key: deviceKey, limit: 5000 });
  if (!hist || hist.status !== 'SUCCESS' || !hist.data || !hist.data.records.length) {
    return resp_(404, 'No telemetry for daily aggregation', null);
  }
  const recs = hist.data.records;

  // [GAS-2-F] Bucket by the DEPLOYMENT timezone (Config TIMEZONE, default
  // Asia/Jakarta) — NOT by UTC slice. For a Jakarta operator a "day" runs
  // 00:00–24:00 WIB; the old UTC slicing cut every day at 07:00 WIB,
  // shifting evening energy into the next day and 00:00–07:00 into the
  // previous one.
  const tz = safeConfig_('TIMEZONE') || 'Asia/Jakarta';
  const byDay = {};
  recs.forEach(function (r) {
    if (!r.eventTime) return;
    const d = dayKeyInTz_(r.eventTime, tz);
    if (!d) return;   // unparseable legacy row — skip honestly, never guess
    (byDay[d] = byDay[d] || []).push(r);
  });

  const out = [];
  const daysSorted = Object.keys(byDay).sort().slice(-days);
  daysSorted.forEach(function (d) {
    const day = byDay[d];
    // [GAS-2-G] Daily energy = sum of POSITIVE deltas between consecutive
    // samples (recs arrive ordered by event_time). A NEGATIVE delta means
    // the device's cumulative counter RESET mid-day (reboot / NVS wipe) —
    // the old last-minus-first math reported that as NEGATIVE energy
    // (e.g. -4900 Wh). Honest behavior: skip the reset jump (accumulation
    // before the reset is real measured energy), keep summing the segments,
    // and flag the day energyQuality='COUNTER_RESET' so the operator sees
    // that a reset happened. No interpolation, no fabrication.
    const chargeWh = dailyEnergyFromSamples_(day, 'chargeWh');
    const dischargeWh = dailyEnergyFromSamples_(day, 'dischargeWh');
    const chargeAh = dailyEnergyFromSamples_(day, 'chargeAh');
    const dischargeAh = dailyEnergyFromSamples_(day, 'dischargeAh');
    const hasCounters = chargeWh.hasCounters || dischargeWh.hasCounters ||
                        chargeAh.hasCounters || dischargeAh.hasCounters;
    const resets = chargeWh.resets + dischargeWh.resets +
                   chargeAh.resets + dischargeAh.resets;
    const expected = Math.round((24 * 3600) / 3600); // 1 sample/hour default cadence
    const completeness = Math.min(1, day.length / expected);
    out.push({
      date: d,
      chargeWh: chargeWh.total,
      dischargeWh: dischargeWh.total,
      chargeAh: chargeAh.total,
      dischargeAh: dischargeAh.total,
      socMin: minOf_(day.map(function (r) { return r.battery && r.battery.soc && r.battery.soc.value; })),
      socMax: maxOf_(day.map(function (r) { return r.battery && r.battery.soc && r.battery.soc.value; })),
      samples: day.length,
      completeness: round2_(completeness),
      energyQuality: !hasCounters ? 'NO_DATA'
        : (resets > 0 ? 'COUNTER_RESET'
        : (completeness >= 0.8 ? 'VALID' : 'PARTIAL'))
    });
  });
  return resp_(200, 'Daily report', { deviceKey: deviceKey, days: out });
}

/** [P1-004] Gap diagnostics for a device. */
function seqStatus_(deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const ledger = getOrCreateSheet_(SEQ_LEDGER_SHEET, SEQ_LEDGER_HEADER);
  const row = findLedgerRow_(ledger, deviceKey);
  if (!row) return resp_(404, 'No sequence ledger for device', null);
  const v = ledger.getRange(row, 1, 1, 6).getValues()[0];
  return resp_(200, 'Sequence ledger', {
    deviceKey: String(v[0]),
    expectedNext: Number(v[1]),
    highestSeq: Number(v[2]),
    duplicateCount: Number(v[3]),
    gapCount: Number(v[4]),
    gaps: parseGaps_(v[5])
  });
}

/** Sheet row → canonical nested envelope (the ADAPTER's output side). */
function rowToEnvelope_(row) {
  const num = function (v) {
    if (v === '' || v == null) return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
  };
  const quality = function (v) { return v ? String(v) : 'UNKNOWN'; };
  // [GAS-2-E] Per-channel quality is NOT persisted per-channel in the sheet
  // — fabricating 'VALID' was a contractual lie (re-audit GAS-2-E). Honest
  // inference via channelQuality_(): absent value → NOT_AVAILABLE; overall
  // VALID → the channel's semantic label (sound: at ingest, overall VALID
  // is derived from ALL channels being VALID); anything else → UNVERIFIED
  // (something was wrong; the sheet does not record WHICH channel, so any
  // stronger claim would be a guess dressed as a measurement).
  const overall = row[20];   // overall_quality column (empty on legacy rows)
  const cq = function (v, okLabel) { return channelQuality_(v, overall, okLabel); };
  return {
    protocolVersion: PROTOCOL_VERSION,
    deviceId: String(row[1]),
    sequence: num(row[2]),
    eventTime: row[3] instanceof Date ? row[3].toISOString() : (row[3] ? String(row[3]) : null),
    ingestionTime: row[0] instanceof Date ? row[0].toISOString() : (row[0] ? String(row[0]) : null),
    isLate: row[4] === true,
    timeQuality: quality(row[19]),
    battery: {
      voltage: { value: num(row[5]), unit: 'V', quality: cq(num(row[5]), 'VALID') },
      current: { value: num(row[6]), unit: 'A', quality: cq(num(row[6]), 'VALID') },
      power: { value: num(row[7]), unit: 'W', quality: cq(num(row[7]), 'DERIVED') },
      soc: {
        value: num(row[8]), unit: '%', quality: quality(row[9]),
        // v1.6.0 — provenance; pre-1.6 rows have no column → UNKNOWN (honest).
        provenance: row.length > 24 && row[24] ? String(row[24]).toUpperCase() : 'UNKNOWN'
      },
      direction: num(row[6]) != null ? (num(row[6]) > 0.5 ? 'CHARGING' : num(row[6]) < -0.5 ? 'DISCHARGING' : 'IDLE') : 'UNKNOWN',
      // v1.6.0 — BMS block reconstructed when v1.6 columns are present.
      bms: row.length > 25 ? {
        connected: row[26] === true || row[26] === 'TRUE',
        protocol: row[25] ? String(row[25]) : 'NONE',
        cellVoltageMin: num(row[27]),
        cellVoltageMax: num(row[28]),
        temperature: num(row[29]),
        faultFlags: num(row[30])
      } : undefined
    },
    ac: {
      rmsCurrent: { value: num(row[10]), unit: 'A', quality: cq(num(row[10]), 'VALID') },
      estimatedPower: { value: num(row[11]), unit: 'W', quality: cq(num(row[11]), 'ESTIMATED') },
      // v1.7.0 [WAVE-7] — 2nd ACS712 (genset→inverter). Absent on pre-1.7
      // rows → null (honest, never 0).
      gensetRmsCurrent: row.length > 31
        ? { value: num(row[31]), unit: 'A', quality: cq(num(row[31]), 'VALID') }
        : undefined
    },
    environment: {
      temperature: { value: num(row[12]), unit: '°C', quality: cq(num(row[12]), 'VALID') },
      humidity: { value: num(row[13]), unit: '%', quality: cq(num(row[13]), 'VALID') }
    },
    energy: {
      chargeWh: num(row[14]), dischargeWh: num(row[15]),
      chargeAh: num(row[16]), dischargeAh: num(row[17]),
      quality: (num(row[14]) == null && num(row[15]) == null &&
                num(row[16]) == null && num(row[17]) == null)
        ? 'NOT_AVAILABLE'
        : cq(num(row[14]), 'VALID')
    },
    health: {
      freeHeap: num(row[21]), rssi: num(row[22]),
      firmwareVersion: String(row[23] || ''),
      ina219Online: row[18] === true || row[18] === 'TRUE'
    },
    // v1.7.0 [WAVE-7] — emergency relay state. Pre-1.7 rows (no columns)
    // report state UNKNOWN — the PWA renders an honest "tidak diketahui",
    // never a fabricated RUN.
    emergency: row.length > 32 ? {
      state: row[32] ? String(row[32]).toUpperCase() : 'UNKNOWN',
      reason: row[33] ? String(row[33]) : null,
      estopLineOpen: row[34] === true || String(row[34]).toUpperCase() === 'TRUE',
      tripCount: num(row[35])
    } : undefined,
    overallQuality: quality(row[20])
  };
}

// ----------------------------------------------------------------------------
// Config accessor (Cached)
// ----------------------------------------------------------------------------

function getPltsConfig(key) {
  const cache = CacheService.getScriptCache();
  const cacheKey = 'PLTS_CFG_' + key;
  const cached = cache.get(cacheKey);
  if (cached !== null) return cached;

  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const sheet = ss.getSheetByName(CONFIG_SHEET);
  if (!sheet) throw new Error('Config sheet is missing. Run setupMasterTemplate() once.');

  const data = sheet.getDataRange().getValues();
  for (let i = 1; i < data.length; i++) {
    if (String(data[i][0]).trim() === key) {
      const value = String(data[i][1]);
      // [WAVE-4 / GAS-2-Q] Credentials cache for MINUTES, not hours: a
      // rotated/revoked AUTH_TOKEN used to stay accepted for up to 6 h
      // because the old value sat in the cache. 300 s bounds the exposure
      // window while keeping the hot path off the Config sheet.
      // invalidatePltsCache() still forces an immediate drop.
      cache.put(cacheKey, value, credentialCacheTtlS_(key));
      return value;
    }
  }
  throw new Error('PLTS Config Key Not Found: ' + key);
}

/** [WAVE-4 / GAS-2-Q] Secrets rotate fast; knobs don't. See getPltsConfig. */
function credentialCacheTtlS_(key) {
  return (key === 'AUTH_TOKEN' || key === 'ADMIN_TOKEN') ? 300 : CACHE_TTL_S;
}

function invalidatePltsCache() {
  const cache = CacheService.getScriptCache();
  DEFAULT_CONFIG.forEach(function (row) { cache.remove('PLTS_CFG_' + row[0]); });
}

/**
 * [WAVE-3 / GAS-2-J] Resolve the acting device's key.
 * HMAC path: the SIGNED deviceId is the only admissible identity — any
 *   device-level claim elsewhere is overridden (impersonation is already
 *   rejected at body level by the doPost gate; here it simply cannot win).
 * Token path: device_key may ride top-level or inside `data` (single
 *   documented trust domain, F-G17); falls back to Config!DEVICE_KEY.
 * Returns '' when nothing resolves — callers gate on it honestly.
 */
function resolveDeviceKey_(body, auth) {
  if (auth.method === 'HMAC') return auth.deviceKey;
  const payload = bodyPayload_(body);
  return payload.device_key || body.device_key || auth.deviceKey || '';
}

/** Payload object after the WAVE-1 unwrap ({} when data is absent). */
function bodyPayload_(body) {
  return (body && body.data && typeof body.data === 'object') ? body.data : {};
}

function verifyToken_(token) {
  if (!token) return false;
  try {
    // [AUDIT 2026-08-28 F-G16] Constant-time comparison (panjang diabaikan:
    // bocor panjang token — praktik standar, sama seperti pembanding CSRF di
    // PWA). `===` short-circuit pada byte berbeda pertama → sinyal timing.
    const expected = String(getPltsConfig('AUTH_TOKEN'));
    const given = String(token);
    if (!expected) return false;
    if (given.length !== expected.length) return false;
    let diff = 0;
    for (let i = 0; i < expected.length; i++) {
      diff |= given.charCodeAt(i) ^ expected.charCodeAt(i);
    }
    return diff === 0;
  } catch (err) { return false; }
}

// [AUDIT 2026-08-28 F-G17] Device-registration gate (fail-closed untuk fleet).
// Saat sheet DEVICES berisi ≥1 perangkat, SEMUA aksi ber-scope perangkat wajib
// menunjuk device_key yang terdaftar. Ini menutup:
//   - polusi data: device_id salah-ketik menulis baris yatim ke sheet Telemetry
//   - pembacaan untuk device_key yang tak pernah di-provision
// Saat sheet KOSONG (deployment legacy satu-perangkat, firmware-generic
// ≤ v1.4.0), gerbang TERBUKA — domain kepercayaan tunggal AUTH_TOKEN adalah
// model legacy yang terdokumentasi.
// CATATAN JUJUR: AUTH_TOKEN legacy dibagi per-deployment; siapa pun yang
// memegangnya dapat membaca/menulis SEMUA perangkat terdaftar (domain
// kepercayaan tunggal). Pengikatan per-perangkat = jalur HMAC (secret
// per deviceKey di sheet DEVICES).
function requireRegisteredDevice_(deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const sheet = getOrCreateSheet_(DEVICES_SHEET, DEVICES_HEADER);
  const last = sheet.getLastRow();
  if (last < 2) return null;   // mode legacy: belum ada perangkat terdaftar
  const keys = sheet.getRange(2, 1, last - 1, 1).getValues();
  for (let i = 0; i < keys.length; i++) {
    if (String(keys[i][0]).trim() === String(deviceKey).trim()) return null;
  }
  return resp_(400, 'Unknown device_key "' + deviceKey + '" — daftarkan di sheet DEVICES terlebih dahulu', null);
}

/**
 * [WAVE-4 / GAS-2-S] Optional registration check during the PING handshake.
 * The /setup handshake used to report "Sukses" even when the entered
 * device_key was not (yet) registered — the operator learned the truth only
 * later, from a 400 on the first TELEMETRY/LATEST call. Additive and
 * backward compatible: no device_key supplied (and not HMAC) →
 * { device_registered: null } ("not checked"); supplied → honest membership
 * report against the DEVICES sheet. An EMPTY DEVICES sheet is legacy
 * single-device mode (requireRegisteredDevice_ semantics) — reported as
 * legacy_mode: true, registered: true, not as a failure.
 */
function pingHandshakeData_(body, auth) {
  let dk = null;
  if (auth.method === 'HMAC') dk = auth.deviceKey;
  else dk = body.device_key || bodyPayload_(body).device_key || null;
  if (!dk) return { device_key: null, device_registered: null };
  const sheet = getOrCreateSheet_(DEVICES_SHEET, DEVICES_HEADER);
  const last = sheet.getLastRow();
  if (last < 2) {
    return { device_key: String(dk), device_registered: true, legacy_mode: true };
  }
  const keys = sheet.getRange(2, 1, last - 1, 1).getValues();
  for (let i = 0; i < keys.length; i++) {
    if (String(keys[i][0]).trim() === String(dk).trim()) {
      return { device_key: String(dk), device_registered: true };
    }
  }
  return { device_key: String(dk), device_registered: false };
}

/**
 * [WAVE-4 / GAS-2-P + GAS-2-O] Returns the alert MESSAGE (or null) WITHOUT
 * sending — the caller sends it outside the ingest lock. A battery that
 * stays below cutoff used to alert on EVERY sample (~5,760/day at a 15 s
 * cadence); the message now names the device (fleet honesty) and
 * maybeSendTelegramAlert_ enforces a per-device cooldown.
 */
function lowBatteryAlertMessage_(norm, deviceKey) {
  const lowCutoff = parseFloat(safeConfig_('LOW_BATTERY_CUTOFF_V')) || 0;
  if (lowCutoff > 0 && norm.vBat !== '' && Number(norm.vBat) < lowCutoff) {
    return 'LOW BATTERY [' + deviceKey + ']: ' + norm.vBat + ' V < cutoff ' + lowCutoff + ' V';
  }
  return null;
}

// ----------------------------------------------------------------------------
// Log rotation
// ----------------------------------------------------------------------------

function rotateLogs_(sheet) {
  const maxRows = parseInt(safeConfig_('LOG_ROTATION_MAX_ROWS'), 10) || 5000;
  const dataRows = sheet.getLastRow() - 1;
  if (dataRows > maxRows) {
    const toDelete = Math.min(1000, dataRows - maxRows);
    sheet.deleteRows(2, toDelete);
  }
}

/**
 * [WAVE-4 / GAS-2-T + GAS-2-U] Scan window for retention-bounded reads
 * (duplicate check, LATEST, HISTORY). Must COVER the full retained sheet:
 * rotateLogs_ keeps Telemetry at ≤ LOG_ROTATION_MAX_ROWS rows, and the old
 * hardcoded windows (3.000 dup/LATEST, 5.000 HISTORY) silently under-scanned
 * whenever an operator RAISED the rotation — old duplicates then passed as
 * ACCEPTED_LATE (double-stored rows) and old samples fell out of reads with
 * no signal. +100 margin; floor 3.000 for legacy deployments without the key.
 */
function retentionScanRows_() {
  const rotation = parseInt(safeConfig_('LOG_ROTATION_MAX_ROWS'), 10) || 5000;
  return Math.max(3000, rotation + 100);
}

// [WAVE-4 / GAS-2-V] Retention for the append-only bookkeeping sheets.
// Telemetry used to be the only rotated sheet — Ota/OtaEvents/Calibration
// grew without bound until reads (and the sheet itself) degraded. Caps are
// Config-tunable; safeConfig_ gives old deployments the defaults with no
// migration step.

/** Keep the newest N OTA manifests (the ACTIVE manifest is the LAST row). */
function rotateOtaManifests_() {
  const cap = parseInt(safeConfig_('OTA_MANIFEST_MAX_ROWS'), 10) || 50;
  const sheet = otaSheet_();
  const dataRows = sheet.getLastRow() - 1;
  if (dataRows > cap) sheet.deleteRows(2, dataRows - cap);   // oldest first; newest (active) always survives
}

/** Keep the newest N OTA events (pure log — oldest rows disposable). */
function rotateOtaEvents_() {
  const cap = parseInt(safeConfig_('OTA_EVENTS_MAX_ROWS'), 10) || 2000;
  const sheet = otaEventsSheet_();
  const dataRows = sheet.getLastRow() - 1;
  if (dataRows > cap) sheet.deleteRows(2, dataRows - cap);
}

/**
 * Bound Calibration history WITHOUT ever deleting a PENDING command: a
 * pending row is a device's lifeline — its ACK is still owed. Only APPLIED
 * rows beyond the cap are deleted, oldest first, so a fleet of devices
 * that never ACK keeps its commands regardless of sheet size.
 */
function rotateCalibrationHistory_() {
  const cap = parseInt(safeConfig_('CALIB_HISTORY_MAX_ROWS'), 10) || 500;
  const sheet = calibSheet_();
  const last = sheet.getLastRow();
  if (last < 2) return;
  const rows = sheet.getDataRange().getValues();
  const isApplied = function (v) { return v === true || String(v).toLowerCase() === 'true'; };
  let appliedCount = 0;
  for (let i = 1; i < rows.length; i++) if (isApplied(rows[i][5])) appliedCount++;
  let excess = appliedCount - cap;
  if (excess <= 0) return;
  let deleted = 0;
  for (let i = 1; i < rows.length && excess > 0; i++) {   // oldest first
    if (!isApplied(rows[i][5])) continue;                  // pending — never delete
    sheet.deleteRow(i + 1 - deleted);                      // index shift from prior deletes
    deleted++;
    excess--;
  }
}

/**
 * [WAVE-4 / GAS-2-P] Optional per-topic cooldown via CacheService: while
 * the cooldown key is alive, further alerts for the SAME topic are
 * suppressed (default 30 min, LOW_BATTERY_ALERT_COOLDOWN_MIN). The cooldown
 * counts SEND ATTEMPTS — a failing/misconfigured bot must not degrade into
 * a per-sample error loop either. Telegram alerting stays optional and
 * best-effort; fetch failures remain silent.
 */
function maybeSendTelegramAlert_(message, cooldownKey) {
  const token = safeConfig_('TELEGRAM_BOT_TOKEN');
  const chat = safeConfig_('TELEGRAM_CHAT_ID');
  if (!token || !chat) return;
  let cache = null;
  if (cooldownKey) {
    const cooldownMin = parseInt(safeConfig_('LOW_BATTERY_ALERT_COOLDOWN_MIN'), 10) || 30;
    cache = CacheService.getScriptCache();
    if (cache.get(cooldownKey)) return;
    cache.put(cooldownKey, '1', Math.max(1, cooldownMin) * 60);
  }
  try {
    UrlFetchApp.fetch('https://api.telegram.org/bot' + token + '/sendMessage', {
      method: 'post',
      payload: { chat_id: chat, text: '[PLTS Monitor] ' + message },
      muteHttpExceptions: true
    });
  } catch (err) {
    // Silent — Telegram alerting is optional.
  }
}

// ----------------------------------------------------------------------------
// Signed OTA — publish + manifest read (unchanged contract)
// ----------------------------------------------------------------------------

const OTA_SHEET = 'Ota';
const OTA_HEADER = ['version', 'url', 'sha256', 'hmac', 'size', 'published_at'];
// [WAVE-6 / FW6-9] First firmware version that verifies the manifest hmac
// with the per-device derived key HMAC-SHA256(AUTH_TOKEN, device_key).
const OTA_PER_DEVICE_KEY_MIN_FW = [1, 5, 4];

function otaSheet_() {
  return getOrCreateSheet_(OTA_SHEET, OTA_HEADER);
}

/** Numeric semver compare of X.Y.Z strings; null when unparseable. */
function semverAtLeast_(version, min) {
  const parts = String(version || '').split('.');
  if (parts.length < 3) return null;
  const nums = [];
  for (let i = 0; i < 3; i++) {
    const n = parseInt(parts[i], 10);
    if (!isFinite(n)) return null;
    nums.push(n);
  }
  for (let i = 0; i < 3; i++) {
    if (nums[i] !== min[i]) return nums[i] > min[i];
  }
  return true;   // equal
}

function toHex_(bytes) {
  return bytes.map(function (b) { return ('0' + (b & 0xFF).toString(16)).slice(-2); })
              .join('');
}

/**
 * [WAVE-6 / FW6-9] Per-device manifest hmac.
 *   deviceKey K = hex( HMAC-SHA256(message = device_key, key = AUTH_TOKEN) )
 *   manifest hmac = hex( HMAC-SHA256(message = 'version|url|sha256', key = K) )
 * The device derives the SAME K from the two secrets it already holds
 * (auth_token + device_key) and verifies the served hmac against it.
 * Fleet-wide forgery from a lone leaked AUTH_TOKEN is thereby impossible for
 * any device on fw >= 1.5.4. AUTH_TOKEN stays server-side only here.
 */
function otaPerDeviceHmac_(version, url, sha256, deviceKey) {
  const authToken = String(getPltsConfig('AUTH_TOKEN') || '');
  if (!authToken || !deviceKey) return '';
  const derived = toHex_(Utilities.computeHmacSha256Signature(
      String(deviceKey), authToken, Utilities.Charset.UTF_8));
  const message = version + '|' + url + '|' + sha256;
  return toHex_(Utilities.computeHmacSha256Signature(
      message, derived, Utilities.Charset.UTF_8));
}

function otaGetManifest_(deviceKey, fwVersion) {
  const sheet = otaSheet_();
  const rows = sheet.getDataRange().getValues();
  if (rows.length < 2) {
    return resp_(404, 'No OTA manifest published yet', null);
  }
  const row = rows[rows.length - 1];
  const version = String(row[0] || '');
  const url     = String(row[1] || '');
  const sha256  = String(row[2] || '');
  let   hmac    = String(row[3] || '');
  // [WAVE-6 / FW6-9] Per-device key for capable callers. Fail-closed on
  // ambiguity: unparseable fw_version falls back to the fleet hmac (the
  // device's own anti-downgrade + HMAC check decides — an old image simply
  // refuses the update, nothing is bricked).
  const capable = semverAtLeast_(fwVersion, OTA_PER_DEVICE_KEY_MIN_FW) === true;
  const gate = capable ? requireRegisteredDevice_(deviceKey) : null;
  if (gate) return gate;   // per-device hmac without a registered device = no manifest
  if (capable) {
    hmac = otaPerDeviceHmac_(version, url, sha256, deviceKey);
    if (!hmac) {
      return resp_(500, 'Per-device OTA hmac unavailable (AUTH_TOKEN not configured)', null);
    }
  }
  return resp_(200, 'OTA manifest', {
    version: version,
    url: url,
    sha256: sha256,
    hmac: hmac,
    size: Number(row[4] || 0),
    published_at: row[5] instanceof Date ? row[5].toISOString() : String(row[5] || '')
  });
}

function otaPublishManifest_(manifest) {
  const required = ['version', 'url', 'sha256', 'hmac'];
  for (const key of required) {
    if (!manifest[key] || typeof manifest[key] !== 'string') {
      return resp_(400, 'Missing manifest field: ' + key, null);
    }
  }
  if (!String(manifest.url).startsWith('https://')) {
    return resp_(400, 'OTA manifest URL must be HTTPS', null);
  }
  const sheet = otaSheet_();
  sheet.appendRow([
    String(manifest.version),
    String(manifest.url),
    String(manifest.sha256).toLowerCase(),
    String(manifest.hmac).toLowerCase(),
    Number(manifest.size || 0),
    new Date()
  ]);
  rotateOtaManifests_();   // [WAVE-4 / GAS-2-V] bound growth; newest (active) row always survives
  return resp_(200, 'OTA manifest published', {
    version: String(manifest.version),
    published_at: new Date().toISOString()
  });
}

const OTA_EVENTS_SHEET = 'OtaEvents';
const OTA_EVENTS_HEADER = ['timestamp', 'device_key', 'event', 'version', 'message'];

function otaEventsSheet_() {
  return getOrCreateSheet_(OTA_EVENTS_SHEET, OTA_EVENTS_HEADER);
}

function otaLogStatus_(payload) {
  const event = String(payload.event || '').toUpperCase();
  // [WAVE-6 / FW6-1] REFUSED joins the set: anti-downgrade / non-semver
  // refusals previously produced zero server-side signal — the operator
  // pushed a manifest and waited forever.
  const validEvents = ['ACTIVATED', 'ROLLBACK', 'BOOT_FAILED', 'DOWNLOAD_FAILED', 'REFUSED'];
  if (validEvents.indexOf(event) === -1) {
    return resp_(400, 'Invalid OTA event: ' + event, null);
  }
  const sheet = otaEventsSheet_();
  sheet.appendRow([
    new Date(),
    String(payload.device_key || safeConfig_('DEVICE_KEY')),
    event,
    String(payload.version || ''),
    String(payload.message || '')
  ]);
  rotateOtaEvents_();   // [WAVE-4 / GAS-2-V] bound growth
  return resp_(200, 'OTA event logged', null);
}

// ----------------------------------------------------------------------------
// Auto Calibration — PWA publish → ESP32 poll → ESP32 ACK (unchanged contract)
// ----------------------------------------------------------------------------

const CALIB_SHEET = 'Calibration';
const CALIB_HEADER = [
  'timestamp', 'device_key',
  'v_calib', 'i_calib_dc', 'i_calib_ac',
  'applied', 'applied_at', 'command_id', 'note'
];

function calibSheet_() {
  return getOrCreateSheet_(CALIB_SHEET, CALIB_HEADER);
}

function calibrationPublish_(body) {
  const deviceKey = body.device_key;
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const v = Number(body.v_calib);
  const dc = Number(body.i_calib_dc);
  const ac = Number(body.i_calib_ac);
  if (!Number.isFinite(v) || !Number.isFinite(dc) || !Number.isFinite(ac)) {
    return resp_(400, 'Non-numeric v_calib / i_calib_dc / i_calib_ac', null);
  }
  // [WAVE-3 / GAS-2-K] Sane factor ranges. Number.isFinite alone happily
  // accepted 0, negatives and 1e9 — a zero/negative/huge multiplier scales
  // every measurement into garbage while the PWA presents it as real data
  // (v=0 → all voltages read 0; v=1e9 → permanent saturation). Ranges per
  // the re-audit recommendation (14_CODE_GS_V2_REAUDIT.md §GAS-2-K) and
  // bracket the production defaults (voltage 18.857, current 1.00) with
  // wide margin for legitimate re-calibration.
  const sane = [
    ['v_calib', v, 0.1, 100],
    ['i_calib_dc', dc, 0.1, 50],
    ['i_calib_ac', ac, 0.1, 50]
  ];
  for (let i = 0; i < sane.length; i++) {
    const name = sane[i][0], val = sane[i][1], min = sane[i][2], max = sane[i][3];
    if (val < min || val > max) {
      return resp_(400, name + '=' + val + ' is outside the sane range [' +
        min + ', ' + max + '] — refusing to queue a miscalibration', null);
    }
  }
  const sheet = calibSheet_();
  const commandId = Utilities.getUuid();
  sheet.appendRow([
    new Date(), String(deviceKey),
    v, dc, ac,
    false, '',
    commandId,
    String(body.note || '')
  ]);
  rotateCalibrationHistory_();   // [WAVE-4 / GAS-2-V] bound APPLIED history; pending rows never deleted
  return resp_(200, 'Calibration queued', { command_id: commandId });
}

function calibrationPending_(deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const sheet = calibSheet_();
  const rows = sheet.getDataRange().getValues();
  for (let i = rows.length - 1; i >= 1; i--) {
    if (String(rows[i][1]) === String(deviceKey) &&
        rows[i][5] !== true &&
        String(rows[i][5]).toLowerCase() !== 'true') {
      return resp_(200, 'Pending calibration', {
        command_id: String(rows[i][7]),
        v_calib: Number(rows[i][2]),
        i_calib_dc: Number(rows[i][3]),
        i_calib_ac: Number(rows[i][4])
      });
    }
  }
  return resp_(200, 'No pending calibration', null);
}

/**
 * [WAVE-3 / GAS-2-I] Mark a calibration command applied — BOUND to its device.
 * Before Wave 3 ANY authenticated caller could ACK ANY command_id: device A
 * could swallow device B's command (row marked applied while B never
 * applied it — B then polls "no pending" forever and the command is lost
 * silently). Now the acknowledging device must be the one the command was
 * queued for (Calibration sheet column 2, written at publish time).
 */
function calibrationAck_(commandId, deviceKey) {
  if (!commandId) return resp_(400, 'Missing command_id', null);
  if (!deviceKey) {
    return resp_(400,
      'Missing device_key — CALIBRATION_ACK must name the device the command was queued for', null);
  }
  const sheet = calibSheet_();
  const rows = sheet.getDataRange().getValues();
  for (let i = 1; i < rows.length; i++) {
    if (String(rows[i][7]) === String(commandId)) {
      const owner = String(rows[i][1]).trim();
      if (owner !== String(deviceKey).trim()) {
        return resp_(400, 'command_id belongs to device "' + owner +
          '" — cross-device ACK rejected (authenticated as "' + deviceKey + '")', null);
      }
      sheet.getRange(i + 1, 6).setValue(true);
      sheet.getRange(i + 1, 7).setValue(new Date());
      rotateCalibrationHistory_();   // [WAVE-4 / GAS-2-V] ACKs grow the applied history too
      return resp_(200, 'Calibration marked applied', null);
    }
  }
  return resp_(404, 'command_id not found', null);
}

// ----------------------------------------------------------------------------
// [WAVE-7] Emergency relay control layer
// ----------------------------------------------------------------------------
// Command flow (operator → device):
//   PWA --EMERGENCY_COMMAND(admin_token)--> queue row (PENDING)
//   device <--EMERGENCY_PENDING / TELEMETRY piggyback-- oldest PENDING row
//   device executes (fail-closed local gating) --EMERGENCY_ACK--> row APPLIED
// Device-side events (trip / e-stop / boot / arm) → EMERGENCY_EVENT →
//   bounded EmergencyEvents sheet + optional Telegram alert (cooldown).
//
// Safety semantics (must match firmware E-layer):
//   Relay ENERGIZED  = system RUN  (GPIO LOW on an active-LOW module)
//   Relay DE-ENERGIZED = system ISOLATED (GPIO HIGH / Hi-Z at boot — fail-safe)
//   ARM = operator requests RUN; DISARM = operator requests ISOLATED.
//   The DEVICE is the safety authority: it re-validates every command and
//   may REJECT an ARM while a trigger condition is still active (hysteresis
//   + recovery window). GAS never fabricates device state.
// ----------------------------------------------------------------------------

function emergencySheet_() {
  return getOrCreateSheet_(EMERGENCY_QUEUE_SHEET, EMERGENCY_QUEUE_HEADER);
}

function emergencyEventsSheet_() {
  return getOrCreateSheet_(EMERGENCY_EVENTS_SHEET, EMERGENCY_EVENTS_HEADER);
}

/**
 * Validate an emergency CONFIG payload against EMERGENCY_CONFIG_FIELDS.
 * Returns { ok, config, error }: config carries ONLY whitelisted fields with
 * server-side defaults for omitted ones (the device re-validates on apply).
 */
function validateEmergencyConfig_(raw) {
  const cfg = {};
  for (let i = 0; i < EMERGENCY_CONFIG_FIELDS.length; i++) {
    const name = EMERGENCY_CONFIG_FIELDS[i][0];
    const min = EMERGENCY_CONFIG_FIELDS[i][1];
    const max = EMERGENCY_CONFIG_FIELDS[i][2];
    const dflt = EMERGENCY_CONFIG_FIELDS[i][3];
    if (raw == null || raw[name] === undefined) { cfg[name] = dflt; continue; }
    const v = Number(raw[name]);
    if (!Number.isFinite(v) || v < min || v > max) {
      return { ok: false, config: null,
        error: name + '=' + raw[name] + ' outside [' + min + ', ' + max + ']' };
    }
    cfg[name] = v;
  }
  return { ok: true, config: cfg, error: null };
}

/** TTL-expire stale PENDING/DELIVERED rows (issued_at older than TTL). */
function expireStaleEmergencyCommands_() {
  const ttlMin = parseInt(safeConfig_('EMERGENCY_QUEUE_TTL_MIN'), 10) || 10;
  const sheet = emergencySheet_();
  const last = sheet.getLastRow();
  if (last < 2) return;
  const rows = sheet.getRange(2, 1, last - 1, EMERGENCY_QUEUE_HEADER.length).getValues();
  const now = Date.now();
  for (let i = 0; i < rows.length; i++) {
    const status = String(rows[i][6] || '').toUpperCase();
    if (status !== 'PENDING' && status !== 'DELIVERED') continue;
    const issued = rows[i][5] instanceof Date ? rows[i][5].getTime()
      : Date.parse(String(rows[i][5] || ''));
    if (!isFinite(issued)) continue;
    if (now - issued > ttlMin * 60000) {
      sheet.getRange(i + 2, 7).setValue('EXPIRED');
      sheet.getRange(i + 2, 8).setValue(new Date());
      sheet.getRange(i + 2, 9).setValue('TTL ' + ttlMin + ' min elapsed — command never ACKed');
    }
  }
}

/**
 * EMERGENCY_COMMAND — queue ARM / DISARM / CONFIG for a device.
 * Operator-only (verified by the caller). Idempotent-ish: an identical
 * still-servable command for the same device returns the EXISTING row instead
 * of stacking a second one (operator double-tap must not double-apply).
 */
function emergencyCommand_(body, deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const command = String(body.command || '').toUpperCase();
  if (EMERGENCY_COMMANDS.indexOf(command) < 0) {
    return resp_(400, 'command must be one of ' + EMERGENCY_COMMANDS.join('/'), null);
  }
  const note = String(body.note || '').slice(0, 200);
  let paramsJson = '';
  if (command === 'CONFIG') {
    const v = validateEmergencyConfig_(body.config || {});
    if (!v.ok) return resp_(400, 'Invalid emergency config: ' + v.error, null);
    paramsJson = JSON.stringify(v.config);
  }
  expireStaleEmergencyCommands_();
  const sheet = emergencySheet_();
  const last = sheet.getLastRow();
  if (last >= 2) {
    const rows = sheet.getRange(2, 1, last - 1, EMERGENCY_QUEUE_HEADER.length).getValues();
    for (let i = rows.length - 1; i >= 0; i--) {
      if (String(rows[i][1]) !== String(deviceKey)) continue;
      const status = String(rows[i][6] || '').toUpperCase();
      if (status !== 'PENDING' && status !== 'DELIVERED') continue;
      if (String(rows[i][2]).toUpperCase() !== command) continue;
      if (String(rows[i][3]) !== paramsJson) continue;
      return resp_(200, 'Emergency command already queued', {
        command_id: String(rows[i][0]), command: command, status: status,
        duplicate: true
      });
    }
  }
  const commandId = Utilities.getUuid();
  sheet.appendRow([
    commandId, String(deviceKey), command, paramsJson, note,
    new Date(), 'PENDING', '', ''
  ]);
  maybeSendTelegramAlert_(
    'EMERGENCY ' + command + ' queued [' + deviceKey + ']' +
    (note ? ' — ' + note : ''),
    'PLTS_TG_EMG_CMD_' + deviceKey);
  return resp_(200, 'Emergency command queued', {
    command_id: commandId, command: command, status: 'PENDING'
  });
}

/**
 * EMERGENCY_PENDING — device polls for its oldest servable command.
 * The row is marked DELIVERED (audit trail) but stays servable until the
 * device ACKs or the TTL expires — a lost response never loses the command.
 */
function emergencyPending_(deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const pending = emergencyPendingFor_(deviceKey);
  if (!pending) return resp_(200, 'No pending emergency command', null);
  const sheet = emergencySheet_();
  sheet.getRange(pending.row, 7).setValue('DELIVERED');
  return resp_(200, 'Pending emergency command', {
    command_id: pending.command_id,
    command: pending.command,
    note: pending.note,
    config: pending.config
  });
}

/** Shared helper: oldest servable (PENDING/DELIVERED) row for a device. */
function emergencyPendingFor_(deviceKey) {
  if (!deviceKey) return null;
  expireStaleEmergencyCommands_();
  const sheet = emergencySheet_();
  const last = sheet.getLastRow();
  if (last < 2) return null;
  const rows = sheet.getRange(2, 1, last - 1, EMERGENCY_QUEUE_HEADER.length).getValues();
  for (let i = 0; i < rows.length; i++) {
    if (String(rows[i][1]) !== String(deviceKey)) continue;
    const status = String(rows[i][6] || '').toUpperCase();
    if (status !== 'PENDING' && status !== 'DELIVERED') continue;
    let config = null;
    if (String(rows[i][3])) {
      try { config = JSON.parse(String(rows[i][3])); } catch (err) { config = null; }
    }
    return {
      row: i + 2,
      command_id: String(rows[i][0]),
      command: String(rows[i][2]).toUpperCase(),
      note: String(rows[i][4] || ''),
      config: config
    };
  }
  return null;
}

/**
 * EMERGENCY_ACK — device reports execution result, BOUND to the device the
 * command was queued for (GAS-2-I pattern). result: 'APPLIED' | 'REJECTED'.
 * Idempotent: re-ACK of an APPLIED row returns 200 without side effects
 * (in-flight retries are safe).
 */
function emergencyAck_(commandId, result, message, state, deviceKey) {
  if (!commandId) return resp_(400, 'Missing command_id', null);
  if (!deviceKey) {
    return resp_(400,
      'Missing device_key — EMERGENCY_ACK must name the device the command was queued for', null);
  }
  const res = String(result || '').toUpperCase();
  if (res !== 'APPLIED' && res !== 'REJECTED') {
    return resp_(400, "result must be 'APPLIED' or 'REJECTED'", null);
  }
  const sheet = emergencySheet_();
  const last = sheet.getLastRow();
  if (last < 2) return resp_(404, 'command_id not found', null);
  const rows = sheet.getRange(2, 1, last - 1, EMERGENCY_QUEUE_HEADER.length).getValues();
  for (let i = rows.length - 1; i >= 0; i--) {
    if (String(rows[i][0]) !== String(commandId)) continue;
    const owner = String(rows[i][1]).trim();
    if (owner !== String(deviceKey).trim()) {
      return resp_(400, 'command_id belongs to device "' + owner +
        '" — cross-device ACK rejected (authenticated as "' + deviceKey + '")', null);
    }
    const status = String(rows[i][6] || '').toUpperCase();
    if (status === 'APPLIED' || status === 'REJECTED' || status === 'EXPIRED') {
      return resp_(200, 'Command already settled: ' + status, { settled: status });
    }
    sheet.getRange(i + 2, 7).setValue(res);
    sheet.getRange(i + 2, 8).setValue(new Date());
    sheet.getRange(i + 2, 9).setValue(String(message || '').slice(0, 200));
    const type = res === 'APPLIED'
      ? (String(rows[i][2]).toUpperCase() === 'ARM' ? 'ARMED'
        : String(rows[i][2]).toUpperCase() === 'CONFIG' ? 'CONFIG_APPLIED' : 'DISARMED')
      : 'REJECTED';
    emergencyAppendEvent_(deviceKey, type,
      String(rows[i][2]).toUpperCase() + ' (operator)',
      String(message || '').slice(0, 200),
      String(state || 'UNKNOWN'), 'operator');
    maybeSendTelegramAlert_(
      'EMERGENCY ' + res + ' [' + deviceKey + '] cmd=' + String(rows[i][2]).toUpperCase() +
      (message ? ' — ' + String(message).slice(0, 120) : ''),
      'PLTS_TG_EMG_ACK_' + deviceKey);
    return resp_(200, 'Emergency command ' + res.toLowerCase(), { result: res });
  }
  return resp_(404, 'command_id not found', null);
}

/**
 * EMERGENCY_EVENT — device-side state change (trip, e-stop, boot, crash-loop).
 * Fail-closed type whitelist + bounded event log + Telegram alert with its own
 * cooldown (an oscillating sensor must not spam the operator every 15 s).
 */
function emergencyEvent_(payload, deviceKey) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const type = String(payload.type || '').toUpperCase();
  if (EMERGENCY_EVENT_TYPES.indexOf(type) < 0) {
    return resp_(400, 'type must be one of ' + EMERGENCY_EVENT_TYPES.join('/'), null);
  }
  emergencyAppendEvent_(
    deviceKey, type,
    String(payload.reason || '').slice(0, 120),
    String(payload.detail || '').slice(0, 200),
    String(payload.state || 'UNKNOWN'), 'device');
  maybeSendTelegramAlert_(
    'EMERGENCY ' + type + ' [' + deviceKey + ']' +
    (payload.reason ? ' — ' + String(payload.reason).slice(0, 120) : ''),
    'PLTS_TG_EMG_EVT_' + deviceKey + '_' + type);
  return resp_(200, 'Emergency event logged', { type: type });
}

function emergencyAppendEvent_(deviceKey, type, reason, detail, stateAfter, source) {
  const sheet = emergencyEventsSheet_();
  sheet.appendRow([
    new Date(), String(deviceKey), String(type), String(reason || ''),
    String(detail || ''), String(stateAfter || 'UNKNOWN'), String(source || 'device')
  ]);
  const cap = parseInt(safeConfig_('EMERGENCY_EVENTS_MAX_ROWS'), 10) || 500;
  const dataRows = sheet.getLastRow() - 1;
  if (dataRows > cap) sheet.deleteRows(2, Math.min(1000, dataRows - cap));
}

/** EMERGENCY_LOG — newest-first event history for a device (operator UX). */
function emergencyLog_(deviceKey, limit) {
  if (!deviceKey) return resp_(400, 'Missing device_key', null);
  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(EMERGENCY_EVENTS_SHEET);
  if (!sheet || sheet.getLastRow() < 2) {
    return resp_(200, 'No emergency events yet', { events: [] });
  }
  const rows = sheet.getDataRange().getValues();
  const events = [];
  for (let i = rows.length - 1; i >= 1 && events.length < limit; i--) {
    if (String(rows[i][1]) !== String(deviceKey)) continue;
    events.push({
      ts: rows[i][0] instanceof Date ? rows[i][0].toISOString() : String(rows[i][0]),
      type: String(rows[i][2]),
      reason: String(rows[i][3] || ''),
      detail: String(rows[i][4] || ''),
      stateAfter: String(rows[i][5] || 'UNKNOWN'),
      source: String(rows[i][6] || 'device')
    });
  }
  return resp_(200, 'Emergency events', { deviceKey: deviceKey, events: events });
}

// ----------------------------------------------------------------------------
// Master template installer — run ONCE from the script editor on first setup.
// ----------------------------------------------------------------------------

function setupMasterTemplate() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();

  let sheet = ss.getSheetByName(CONFIG_SHEET);
  if (!sheet) sheet = ss.insertSheet(CONFIG_SHEET);
  sheet.clear();
  sheet.getRange(1, 1, 1, 2).setValues([['PARAMETER', 'VALUE']]).setFontWeight('bold');
  sheet.getRange(2, 1, DEFAULT_CONFIG.length, 2).setValues(DEFAULT_CONFIG);
  sheet.setFrozenRows(1);
  sheet.setColumnWidth(1, 240);
  sheet.setColumnWidth(2, 320);

  getOrCreateSheet_(LOG_SHEET, TELEMETRY_HEADER);
  getOrCreateSheet_(DEVICES_SHEET, DEVICES_HEADER);
  getOrCreateSheet_(SEQ_LEDGER_SHEET, SEQ_LEDGER_HEADER);
  otaSheet_();
  otaEventsSheet_();
  calibSheet_();

  invalidatePltsCache();
  SpreadsheetApp.getUi().alert(
    'Master Template siap. Ganti nilai AUTH_TOKEN dan parameter lain di tab "Config". ' +
    'Daftarkan per-device secret di tab "Devices" untuk autentikasi HMAC.'
  );
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

function getOrCreateSheet_(name, header) {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  let sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
    sheet.appendRow(header);
    sheet.setFrozenRows(1);
    return sheet;
  }
  // v1.6.0 header migration: existing sheets with the pre-1.6 header get the
  // new columns APPENDED (old indices unchanged — rows keep their meaning).
  if (name === LOG_SHEET) migrateTelemetryHeader_(sheet, header);
  return sheet;
}

/** Extend a pre-1.6 Telemetry sheet header with the v1.6.0 BMS columns. */
function migrateTelemetryHeader_(sheet, header) {
  try {
    const lastCol = sheet.getLastColumn();
    if (lastCol >= header.length) return;                 // already migrated
    if (lastCol < TELEMETRY_HEADER_V1_5_LEN) return;      // unexpected shape — do not touch
    sheet.getRange(1, lastCol + 1, 1, header.length - lastCol)
        .setValues([header.slice(lastCol)]);
  } catch (err) {
    // Migration is best-effort and logged, never fatal for ingestion.
    try { console.warn('Telemetry header migration failed: ' + err); } catch (e2) {}
  }
}

/**
 * [WAVE-3 / GAS-2-K] Operator gate for OTA_PUBLISH. ADMIN_TOKEN is a
 * Config-sheet secret held ONLY by the operator (never burned into firmware,
 * never shipped in PWA config) — standard authentication (a device secret or
 * the legacy AUTH_TOKEN) must never be enough to push firmware to the fleet.
 * Fail-closed: while Config!ADMIN_TOKEN is unset/empty, OTA publishing is
 * DISABLED entirely (an honest refusal, not a silent fallback to AUTH_TOKEN).
 */
function verifyAdminToken_(given) {
  const expected = String(safeConfig_('ADMIN_TOKEN') || '').trim();
  if (!expected) {
    return { ok: false,
      reason: 'OTA publishing is disabled — set ADMIN_TOKEN in the Config sheet (operator-only secret)' };
  }
  if (!given) {
    return { ok: false, reason: 'OTA_PUBLISH requires admin_token (Config!ADMIN_TOKEN)' };
  }
  const g = String(given);
  // Constant-time comparison — same pattern as verifyToken_ (F-G16).
  if (g.length !== expected.length) return { ok: false, reason: 'invalid admin_token' };
  let diff = 0;
  for (let i = 0; i < expected.length; i++) {
    diff |= g.charCodeAt(i) ^ expected.charCodeAt(i);
  }
  return diff === 0 ? { ok: true } : { ok: false, reason: 'invalid admin_token' };
}

function safeConfig_(key) {
  try { return getPltsConfig(key); } catch (err) { return ''; }
}

function numOrEmpty_(value) {
  if (value === null || value === undefined || value === '') return '';
  const n = Number(value);
  return Number.isFinite(n) ? n : '';
}

/**
 * [GAS-2-M] Validate a device-clock reading. An unparseable event_time
 * must never be persisted — "Invalid Date" poisons LATEST/HISTORY ordering
 * (NaN keys) and lies about when the sample was taken. Returns
 * { iso, degraded }: iso = normalized ISO string or null; degraded = true
 * means "a value was present but unparseable" (caller flags DEGRADED).
 * Absent value (null/'') is NOT degraded — it is simply missing (honest).
 */
function sanitizeEventTime_(raw) {
  if (raw == null || raw === '') return { iso: null, degraded: false };
  if (raw instanceof Date) {
    return isFinite(raw.getTime())
      ? { iso: raw.toISOString(), degraded: false }
      : { iso: null, degraded: true };
  }
  const t = Date.parse(String(raw));
  return isFinite(t)
    ? { iso: new Date(t).toISOString(), degraded: false }
    : { iso: null, degraded: true };
}

/** [GAS-2-M] NaN-safe time key for comparators (null/unparseable → 0). */
function safeTimeKey_(iso) {
  if (!iso) return 0;
  const t = Date.parse(String(iso));
  return isFinite(t) ? t : 0;
}

/** [GAS-2-F] UTC instant → local calendar date key (yyyy-MM-dd) in tz.
 *  Returns null when the input is unparseable or tz is invalid — the
 *  caller skips the record honestly rather than guessing a bucket. */
function dayKeyInTz_(eventTime, tz) {
  try {
    const d = eventTime instanceof Date ? eventTime : new Date(eventTime);
    if (!d || !isFinite(d.getTime())) return null;
    return Utilities.formatDate(d, tz, 'yyyy-MM-dd');
  } catch (err) {
    return null;
  }
}

/**
 * [GAS-2-E] Honest per-channel quality inference on READ. The sheet only
 * persists soc/time/overall quality — per-channel quality is NOT stored,
 * so it must not be fabricated:
 *   value == null                 → 'NOT_AVAILABLE' (nothing exists —
 *                                   labeling it VALID was the audit's
 *                                   definition of a contractual lie)
 *   value present + overall VALID → okLabel ('VALID'/'DERIVED'/'ESTIMATED')
 *                                   — sound: at ingest, overall VALID is
 *                                   derived from ALL channels being VALID
 *                                   (deriveOverallQuality_)
 *   value present + overall anything else (SENSOR_ERROR/INVALID/STALE/
 *   SUSPECT/UNKNOWN/empty legacy)  → 'UNVERIFIED' — something was wrong;
 *                                   the sheet does not record WHICH channel,
 *                                   so any stronger claim would be a guess.
 */
function channelQuality_(value, overall, okLabel) {
  if (value == null) return 'NOT_AVAILABLE';
  const o = String(overall || '').toUpperCase();
  return o === 'VALID' ? okLabel : 'UNVERIFIED';
}

/**
 * [GAS-2-G] Daily energy from consecutive samples: sum POSITIVE deltas,
 * treat a NEGATIVE delta as a counter reset (skip the jump, count it).
 * `day` must be ordered by event_time (caller guarantee from HISTORY).
 * Returns { total, resets, hasCounters }: total=null when the day carries
 * no counter value at all; a single-sample day yields total=0 (identical
 * to the old first-minus-last behavior, flagged PARTIAL by completeness).
 */
function dailyEnergyFromSamples_(day, field) {
  let total = 0, resets = 0, anyCounter = false;
  for (let i = 0; i < day.length; i++) {
    const v = day[i].energy ? day[i].energy[field] : null;
    if (v != null) anyCounter = true;
    if (i === 0) continue;
    const a = day[i - 1].energy ? day[i - 1].energy[field] : null;
    if (a == null || v == null) continue;   // missing boundary — skip pair
    const delta = v - a;
    if (delta < 0) { resets++; continue; }  // counter reset between samples
    total += delta;
  }
  if (!anyCounter) return { total: null, resets: 0, hasCounters: false };
  return { total: round2_(total), resets: resets, hasCounters: true };
}

function round2_(v) { return Math.round(v * 100) / 100; }

function minOf_(arr) {
  const nums = arr.filter(function (v) { return v != null && Number.isFinite(Number(v)); })
                 .map(Number);
  return nums.length ? Math.min.apply(null, nums) : null;
}
function maxOf_(arr) {
  const nums = arr.filter(function (v) { return v != null && Number.isFinite(Number(v)); })
                 .map(Number);
  return nums.length ? Math.max.apply(null, nums) : null;
}

/** Uniform response envelope. */
function resp_(code, message, data) {
  return {
    status: code >= 200 && code < 300 ? 'SUCCESS' : 'ERROR',
    code: code,
    data: data === undefined ? null : data,
    message: message,
    timestamp: new Date().toISOString()
  };
}

function json_(payload) {
  return ContentService
    .createTextOutput(JSON.stringify(payload))
    .setMimeType(ContentService.MimeType.JSON);
}
