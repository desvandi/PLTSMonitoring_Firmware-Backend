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
  const char* fields[12];  // null-terminated list of allowed payload fields
};

static const CommandDef COMMAND_REGISTRY[] = {
  {"config", "update",
    {"batteryCapacityAh","batteryNominalV","fullVoltage","lowVoltage",
     "idleCurrentThreshold","fullChargeCurrentThreshold",
     "fullChargePersistenceSec","telemetryIntervalSec",
     "deviceName","timezone", nullptr}},
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
    {"url","version","size","sha256","signature", nullptr}},
  {"ota", "check",
    {"url","version", nullptr}},
  {"system", "reboot",
    {nullptr}},
  {"system", "factory_reset_prepare",
    {nullptr}},
  {"system", "factory_reset_confirm",
    {"token", nullptr}},
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

bool CommandCanonicalizer::isFieldAllowed(const String& type, const String& field) {
  // Envelope fields are always allowed
  if (field == "type" || field == "action" || field == "requestId" ||
      field == "transactionId" || field == "version" || field == "issuedAt" ||
      field == "expiresAt") {
    return true;
  }
  String t = type; t.toLowerCase();
  for (size_t i = 0; i < COMMAND_REGISTRY_COUNT; i++) {
    if (t == COMMAND_REGISTRY[i].type) {
      for (size_t j = 0; j < 12 && COMMAND_REGISTRY[i].fields[j]; j++) {
        if (field == COMMAND_REGISTRY[i].fields[j]) return true;
      }
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
  
  // Whitelist check on payload fields
  JsonObject payload = doc.as<JsonObject>();
  if (payload) {
    for (JsonPair p : payload) {
      String field = p.key().c_str();
      if (!isFieldAllowed(t, field)) {
        errOut = "unknown field '" + field + "' for type '" + t + "'";
        return String();
      }
    }
  }
  // Emit fields in canonical order
  for (size_t j = 0; j < 12 && def->fields[j]; j++) {
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
      char buf[32];
      snprintf(buf, sizeof(buf), "%.6f", (double)v.as<float>());
      canon += buf;
    } else if (v.is<const char*>()) {
      canon += v.as<const char*>();
    } else {
      canon += "?";
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

  // Extract + validate transactionId (requestId alias)
  String tid = doc["requestId"] | "";
  String tidAlt = doc["transactionId"] | "";
  if (tid.length() == 0 && tidAlt.length() > 0) tid = tidAlt;
  else if (tid.length() > 0 && tidAlt.length() > 0 && tid != tidAlt) {
    r.errorMessage = "requestId and transactionId differ";
    return r;
  }
  if (tid.length() > 0) {
    String err;
    if (!validateTransactionId(tid, err)) {
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
