// =============================================================================
// Services/CommandCanonicalizer.cpp
// =============================================================================
#include "CommandCanonicalizer.h"
#include "../Core/Config.h"
#include <cstring>
#include <cctype>
#include <ctime>

namespace Services {

// ---------------------------------------------------------------------------
// Whitelist (fail-closed). Add new commands here.
// ---------------------------------------------------------------------------
struct CommandDef {
  const char* type;
  const char* action;
  // [PARITY-4] 12 → 32 slots: config.update now carries 10 base + 5 BMS +
  // 11 alarm-threshold fields (the PWA AlarmThresholds schema names).
  const char* fields[32];  // null-terminated list of allowed payload fields
};

static const CommandDef COMMAND_REGISTRY[] = {
  {"config", "update",
    {"batteryCapacityAh","batteryNominalV","fullVoltage","lowVoltage",
     "idleCurrentThreshold","fullChargeCurrentThreshold",
     "fullChargePersistenceSec","telemetryIntervalSec",
     "deviceName","timezone",
     // v1.6.0 BMS comm fields — REST handlePostConfig + MqttConfigReceiver
     // both apply these; the whitelist omission made every REST/MQTT config
     // update carrying them fail with "unknown field" (latent parity bug).
     "bmsProtocol","bmsPollIntervalMs","bmsModbusSlaveId",
     "bmsModbusTcpHost","bmsModbusTcpPort",
     // [PARITY-4] two-tier alarm thresholds (PWA AlarmThresholds names).
     "voltageLowWarn","voltageLowCritical","voltageHighWarn",
     "voltageHighCritical","currentHighWarn","currentHighCritical",
     "temperatureHighWarn","temperatureHighCritical","humidityHighWarn",
     "socLowWarn","socLowCritical", nullptr}},
  // [PARITY-4] password change is a durable config mutation — same canonical
  // transaction path (requestId + journal + dedup) as every other mutation.
  {"config", "password",
    {"current","next", nullptr}},
  // [PRODUCTION-GRADE 2026-09 — latent-bug fix] The (config, device) entry
  // was MISSING from the registry while Web::ExtraHandlers::handleDevicePost
  // tags its commands with type="config" action="device" — every device-name
  // / site / timezone update from the PWA failed canonicalization with
  // "unknown (type, action)" and returned 400. Registered now.
  {"config", "device",
    {"deviceName","siteName","timezone", nullptr}},
  {"calibration", "update",
    {"version","voltageLow","voltageNominal","voltageFull",
     "acs712Offset","acs712Sensitivity","sht31TempOffset","sht31HumOffset",
     "source", nullptr}},
  {"calibration", "point",
    {"which","reference","raw", nullptr}},
  {"calibration", "acs712_zero",
    {"capture","offset", nullptr}},
  {"alarm", "acknowledge",
    {"code", nullptr}},
  {"alarm", "acknowledgeAll",
    {nullptr}},
  {"ota", "start",
    {"url","version","size","sha256","signature","target", nullptr}},
  {"ota", "check",
    {"url","version", nullptr}},
  {"system", "reboot",
    {nullptr}},
  {"system", "factory_reset_prepare",
    {nullptr}},
  {"system", "factory_reset_confirm",
    {"token", nullptr}},
  // [v1.8.0] 8-channel relay commands (PLTS_ENABLE_RELAYS)
  {"relay", "on",
    {"channel","source", nullptr}},
  {"relay", "off",
    {"channel","source", nullptr}},
  {"relay", "pulse",
    {"channel","durationMs","source", nullptr}},
  {"relay", "all_off",
    {nullptr}},
  // [audit p.434] Schema registered for CANONICAL VALIDATION only — relay
  // config (maxOnTime/minOnTime/interlock/...) has NO runtime mutation
  // ingress: MQTT rejects "config" before queueing and applyCommand("config")
  // fails closed. Kept so a future provisioning ingress can adopt the exact
  // same field whitelist instead of inventing a second schema.
  {"relay", "config",
    {"channel","name","maxOnTimeSec","minOnTimeSec","minOffTimeSec",
     "minSwitchIntervalSec","enabled","interlockGroup", nullptr}},
  {"relay", "acknowledge",
    {"channel", nullptr}},
  {"relay", "clear",
    {"channel", nullptr}},
};

static const size_t COMMAND_REGISTRY_COUNT =
  sizeof(COMMAND_REGISTRY) / sizeof(COMMAND_REGISTRY[0]);

bool CommandCanonicalizer::isKnownCommandType(const String& type, const String& action) {
  String t = type; t.toLowerCase();
  String a = action; a.toLowerCase();
  for (size_t i = 0; i < COMMAND_REGISTRY_COUNT; i++) {
    if (t == COMMAND_REGISTRY[i].type && a == COMMAND_REGISTRY[i].action) return true;
  }
  return false;
}

// [CORE-01] Field whitelist scoped to the EXACT (type, action) tuple.
// Previously this matched on type alone, so relay.on accepted durationMs
// (allowed only for relay.pulse) WITHOUT hashing it — two semantically
// different payloads produced the same commandHash (audit p.309-313).
bool CommandCanonicalizer::isFieldAllowed(const String& type, const String& action,
                                          const String& field) {
  // Envelope fields are always allowed
  if (field == "type" || field == "action" || field == "requestId" ||
      field == "transactionId" || field == "version" || field == "issuedAt" ||
      field == "expiresAt") {
    return true;
  }
  String t = type; t.toLowerCase();
  String a = action; a.toLowerCase();
  for (size_t i = 0; i < COMMAND_REGISTRY_COUNT; i++) {
    if (t == COMMAND_REGISTRY[i].type && a == COMMAND_REGISTRY[i].action) {
      for (size_t j = 0; j < 32 && COMMAND_REGISTRY[i].fields[j]; j++) {
        if (field == COMMAND_REGISTRY[i].fields[j]) return true;
      }
      return false;  // exact tuple found — field not in it → reject
    }
  }
  return false;
}

bool CommandCanonicalizer::validateTransactionId(const String& tid, String& errOut) {
  if (tid.length() < MIN_TRANSACTION_ID_LEN || tid.length() > MAX_TRANSACTION_ID_LEN) {
    errOut = "transactionId length out of range";
    return false;
  }
  for (size_t i = 0; i < tid.length(); i++) {
    char c = tid[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) { errOut = "transactionId contains invalid character"; return false; }
  }
  return true;
}

bool CommandCanonicalizer::validateProtocolVersion(int version, String& errOut) {
  if (version != CANONICAL_COMMAND_VERSION) {
    errOut = "unsupported protocol version";
    return false;
  }
  return true;
}

// [P2-1 REMEDIATION 2026-09] — shared freshness gate (REST + MQTT parity).
// expiresAt is unix-seconds, optional per envelope. Rejection requires BOTH
// a non-zero expiresAt AND a usable device clock: a device without RTC sync
// cannot evaluate freshness and must fail-open on THIS check only (the
// journal + HMAC auth remain in force); this mirrors the pre-existing
// MqttConfigReceiver semantics so both ingresses behave identically.
bool CommandCanonicalizer::isCommandExpired(JsonDocument& doc, String& errOut) {
  if (!doc.containsKey("expiresAt")) return false;
  uint32_t expiresAt = doc["expiresAt"] | 0U;
  if (expiresAt == 0) return false;
  uint32_t now = (uint32_t)::time(nullptr);
  if (now == 0) return false;   // no clock — cannot enforce freshness
  if (expiresAt < now) {
    errOut = "command expired (issuedAt/expiresAt in the past)";
    return true;
  }
  return false;
}

// [CORE-02] Mandatory mutation envelope — audit p.314-318, p.330.
// version + transactionId + issuedAt + expiresAt must ALL be
// present. Freshness evaluation may be impossible (no clock), but the
// envelope claim is mandatory so replay protection is never silently
// downgraded to journal-retention-only.
// [audit p.418] requestId is the TRANSPORT identity and is optional; it MAY
// differ from transactionId (the logical/dedup identity). A retry presents
// a fresh requestId with the SAME transactionId so attempts stay
// distinguishable in the audit trail. When requestId is absent it falls
// back to transactionId (v2 single-id clients remain fully compatible).
bool CommandCanonicalizer::validateCommandEnvelope(JsonDocument& doc, String& errOut) {
  // version: mandatory + valid
  if (!doc.containsKey("version")) {
    errOut = "missing version (command envelope v1 required)";
    return false;
  }
  {
    int v = doc["version"] | 0;
    String err;
    if (!validateProtocolVersion(v, err)) { errOut = err; return false; }
  }
  // transactionId: MANDATORY logical mutation identity (journal dedup key)
  String tid = doc["transactionId"] | "";
  if (tid.length() == 0) {
    // v2 compatibility: clients that only send requestId use it as both
    // identities (single-logical-command semantics).
    tid = doc["requestId"] | "";
  }
  if (tid.length() == 0) {
    errOut = "missing transactionId/requestId (required for mutation)";
    return false;
  }
  {
    String err;
    if (!validateTransactionId(tid, err)) { errOut = err; return false; }
  }
  // requestId: OPTIONAL transport identity — MAY differ from transactionId.
  // If present it must be well-formed (same charset/length rules).
  {
    String rid = doc["requestId"] | "";
    if (rid.length() > 0) {
      String err;
      if (!validateTransactionId(rid, err)) { errOut = err; return false; }
    }
  }
  // issuedAt: mandatory, plausible (> 2020-01-01)
  if (!doc.containsKey("issuedAt")) {
    errOut = "missing issuedAt (command envelope requires freshness claim)";
    return false;
  }
  uint32_t issuedAt = doc["issuedAt"] | 0U;
  if (issuedAt == 0) {
    errOut = "issuedAt must be a non-zero unix timestamp";
    return false;
  }
  // expiresAt: mandatory, non-zero, after issuedAt
  if (!doc.containsKey("expiresAt")) {
    errOut = "missing expiresAt (command envelope requires freshness claim)";
    return false;
  }
  uint32_t expiresAt = doc["expiresAt"] | 0U;
  if (expiresAt == 0) {
    errOut = "expiresAt must be non-zero (use issuedAt + TTL)";
    return false;
  }
  if (expiresAt <= issuedAt) {
    errOut = "expiresAt must be > issuedAt";
    return false;
  }
  return true;
}

static String lower(const String& s) {
  String o = s; o.toLowerCase(); return o;
}

// Build deterministic canonical string:
//   v{version}|{type}|{action}|field1=value|field2=value|...
// Field order is FIXED per type (per registry declaration order).
String CommandCanonicalizer::buildCanonicalString(JsonDocument& doc,
                                                   const String& type,
                                                   const String& action,
                                                   String& errOut) {
  String canon = "v" + String(CANONICAL_COMMAND_VERSION);
  canon += "|" + lower(type);
  canon += "|" + lower(action);
  // Find the registry entry to get the canonical field order
  String t = lower(type);
  String a = lower(action);
  const CommandDef* def = nullptr;
  for (size_t i = 0; i < COMMAND_REGISTRY_COUNT; i++) {
    if (t == COMMAND_REGISTRY[i].type && a == COMMAND_REGISTRY[i].action) {
      def = &COMMAND_REGISTRY[i]; break;
    }
  }
  if (!def) {
    errOut = "unknown (type, action)";
    return String();
  }
  
  // Whitelist check on payload fields — [CORE-01] scoped to exact (type, action)
  JsonObject payload = doc.as<JsonObject>();
  if (payload) {
    for (JsonPair p : payload) {
      String field = p.key().c_str();
      if (!isFieldAllowed(t, a, field)) {
        errOut = "unknown field '" + field + "' for type '" + t + "'";
        if (a.length() > 0) errOut += " action '" + a + "'";
        return String();
      }
    }
  }
  // Emit fields in canonical order
  for (size_t j = 0; j < 32 && def->fields[j]; j++) {
    const char* fieldName = def->fields[j];
    if (!payload.containsKey(fieldName)) continue;
    JsonVariant v = payload[fieldName];
    canon += "|";
    canon += fieldName;
    canon += "=";
    if (v.is<bool>()) {
      canon += v.as<bool>() ? "true" : "false";
    } else if (v.is<int>() || v.is<long>()) {
      canon += String((long)v.as<long>());
    } else if (v.is<float>() || v.is<double>()) {
      // [audit p.338-339] Numeric hardening: reject NaN/±Inf, normalize -0.0
      double d = (double)v.as<double>();
      if (!isfinite(d)) {
        errOut = String(fieldName) + ": NaN/Infinity not allowed";
        return String();
      }
      if (d == 0.0) d = 0.0;  // collapse -0.0 → +0.0
      char buf[32];
      snprintf(buf, sizeof(buf), "%.6f", d);
      canon += buf;
    } else if (v.is<const char*>()) {
      // [audit p.334] Unambiguous canonical grammar — escape the reserved
      // delimiters so a value containing '|' or '=' cannot forge a different
      // canonical string (collision via crafted strings).
      const char* s = v.as<const char*>();
      for (const char* pc = s; *pc; pc++) {
        if (*pc == '|' || *pc == '=' || *pc == '\\') canon += '\\';
        canon += *pc;
      }
    } else {
      errOut = String(fieldName) + ": unsupported JSON value type";
      return String();
    }
  }
  return canon;
}

CanonicalResult CommandCanonicalizer::canonicalizeAndHash(JsonDocument& doc) {
  CanonicalResult r;
  r.ok = false;

  // Extract type + action
  String type   = doc["type"]   | "";
  String action = doc["action"] | "";
  if (type.length() == 0 || action.length() == 0) {
    r.errorMessage = "missing type or action";
    return r;
  }
  if (!isKnownCommandType(type, action)) {
    r.errorMessage = "unknown (type, action) — rejected";
    return r;
  }

  // Validate protocol version if present
  if (doc.containsKey("version")) {
    int v = doc["version"] | 0;
    String err;
    if (!validateProtocolVersion(v, err)) {
      r.errorMessage = err;
      return r;
    }
  }

  // Extract + validate transactionId (logical identity) and requestId
  // (transport identity — audit p.418). transactionId is the dedup key and
  // MAY be aliased by requestId for v2 single-id clients; requestId MAY
  // differ from transactionId (v3 retry-traceable clients).
  String tid = doc["transactionId"] | "";
  String rid = doc["requestId"] | "";
  if (tid.length() == 0) tid = rid;            // v2 alias fallback
  if (rid.length() == 0) rid = tid;            // v2/no-requestId fallback
  if (tid.length() > 0) {
    String err;
    if (!validateTransactionId(tid, err)) {
      r.errorMessage = err;
      return r;
    }
  }
  // Transport identity, when present, must itself be well-formed.
  if (rid.length() > 0 && rid != tid) {
    String err;
    if (!validateTransactionId(rid, err)) {
      r.errorMessage = err;
      return r;
    }
  }

  String err;
  String canon = buildCanonicalString(doc, type, action, err);
  if (canon.length() == 0) {
    r.errorMessage = err;
    return r;
  }
  r.transactionId = tid;
  r.requestId = rid;   // [audit p.418] survives the queue for the audit trail
  r.canonicalString = canon;
  r.commandHash = Utils::sha256Hex(canon);
  r.ok = true;
  return r;
}

DecisionResult CommandCanonicalizer::decideTransaction(const String& tid,
                                                         const String& hash) {
  DecisionResult r;
  r.decision = TransactionDecision::New;
  if (tid.length() == 0) return r;  // no journal integration (backward compat)
  String prevAck;
  r.decision = journal.decide(tid, hash, prevAck);
  r.previousAckJson = prevAck;
  return r;
}

} // namespace Services
