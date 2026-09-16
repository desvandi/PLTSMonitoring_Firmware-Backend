// =============================================================================
// Web/ExtraHandlers.cpp — PWA-contract route parity (FW-20 REMEDIATION 2026-08)
// -----------------------------------------------------------------------------
// The PWA api.ts calls endpoints the firmware never registered:
//   /api/log (alias of /api/logs), /api/insights, /api/ota/history,
//   /api/config/device, /api/config/password, /api/config/export,
//   /api/config/import, /api/alarms/{code}/acknowledge
// and /api/reports. All are registered here with the SAME auth/CSRF policy as
// the rest of the surface. Where the device honestly cannot serve data
// (daily reports — that lives in GAS history), the route returns a
// deterministic documented error instead of fabricated data (directive §3.1).
// =============================================================================
#include "ExtraHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Comm/Rs485Console.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Storage/ConfigStore.h"
#include "../Services/AuthManager.h"
#include "../Services/OtaManager.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/LogService.h"
#include "../Services/HealthSupervisor.h"
#include "../Services/CommandCanonicalizer.h"   // [PARITY-4] password + import canonical path
#include "../Services/TransactionJournal.h"     // [PARITY-4] import journal (X-Request-Id)
#include "../Utils/Crypto.h"                    // [PARITY-4] sha256Hex over the raw import body
#include "../Comm/BatteryCommManager.h"
#include "../Comm/BatteryProtocol.h"
#include "../AI/GasAdvisor.h"
#include "../Drivers/RtcDriver.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <cstring>

namespace Web {
namespace ExtraHandlers {

// ---------------------------------------------------------------------------
// /api/log — alias so the PWA contract (/api/log) and the firmware route
// (/api/logs) are the same operation. Canonical: /api/log (P0-001 contract).
// ---------------------------------------------------------------------------
void handleLogAlias() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  // Reuse the canonical LogHandlers implementation via a request forward.
  // LogHandlers::handleGetLogs is not exported; replicate the behavior:
  uint16_t limit = 100;
  if (http.hasArg("limit")) {
    long v = http.arg("limit").toInt();
    if (v > 0 && v <= 500) limit = (uint16_t)v;
  }
  String body = Services::Log.getActivityJson(limit, -1);
  sendSuccess("OK", body);
}

// ---------------------------------------------------------------------------
// /api/insights — proxied through the ESP32 HMAC signer to GAS.
// Fail-closed: when GAS is not configured the route returns a deterministic
// 503 with the reason (NEVER a mock insight — directive §3.1 / P0-006).
// [PARITY-3 2026-09-06] GAS now SERVES action=INSIGHTS (doGet + HMAC query
// envelope — this client was "contract-ready, not claim-ready" since WAVE-7).
// Envelope transform added: GAS wraps every payload in {status, code, data,
// message}; the PWA InsightsEnvelope contract is the INNER data object. The
// old code forwarded the raw GAS body as route data, so the PWA read
// envelope.insights and got undefined forever. Now:
//   - GAS SUCCESS → forward ONLY resp.data (the InsightsEnvelope payload)
//   - GAS ERROR   → honest HTTP error here carrying the GAS message
// ---------------------------------------------------------------------------
void handleInsights() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!AI::advisor.isEnabled()) {
    sendError(503, "AI insights unavailable — GAS_INGEST_URL not configured on device");
    return;
  }
  String body = AI::advisor.fetchInsights();
  if (body.length() == 0) {
    sendError(503, "AI insights unavailable — GAS fetch failed: " +
               AI::advisor.getLastError());
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    sendError(502, "AI insights unavailable — invalid GAS response");
    return;
  }
  const char* status = doc["status"] | "";
  if (strcmp(status, "SUCCESS") != 0) {
    // Honest relay of the GAS verdict (503 = key unset / 404 = no telemetry /
    // 502 = Gemini failure — the message names the real reason).
    const char* msg = doc["message"] | "GAS error";
    int code = doc["code"] | 0;
    if (code < 400 || code > 599) code = 502;
    sendError(code, "AI insights unavailable (GAS): " + String(msg));
    return;
  }
  JsonVariant data = doc["data"];
  if (data.isNull() || !data.is<JsonObject>()) {
    sendError(502, "AI insights unavailable — GAS returned no data object");
    return;
  }
  String inner;
  serializeJson(data, inner);
  sendSuccess("OK", inner);
}

// ---------------------------------------------------------------------------
// /api/reports — DETERMINISTIC HONEST ERROR.
// Daily energy history is a GAS backend responsibility (it aggregates the
// telemetry the device already uploaded). The device holds only lifetime
// counters — it does not fabricate daily breakdowns (directive §3.3: no
// invented data). The PWA must source reports from GAS.
// ---------------------------------------------------------------------------
void handleReports() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  sendError(501,
    "Daily reports are served by the GAS backend (history/daily aggregation), "
    "not by the device. Point the reports view at the GAS history endpoint.");
}

// ---------------------------------------------------------------------------
// /api/ota/history — in-RAM OTA event ring (boot events + OTA attempts).
// Honest limitation: reset clears it (boot event re-populates the latest
// entry). Persistent OTA history lives in the GAS OtaEvents sheet.
// ---------------------------------------------------------------------------
#define OTA_HIST_MAX 12
struct OtaHistEntry {
  uint32_t timestamp;
  char version[16];
  char event[24];
};
static OtaHistEntry s_otaHist[OTA_HIST_MAX] = {};
static uint8_t s_otaHistCount = 0;

static void recordOtaEvent(const char* version, const char* event) {
  uint8_t idx = s_otaHistCount < OTA_HIST_MAX ? s_otaHistCount : OTA_HIST_MAX - 1;
  if (s_otaHistCount == OTA_HIST_MAX) {
    // shift left (drop oldest)
    for (uint8_t i = 0; i < OTA_HIST_MAX - 1; i++) s_otaHist[i] = s_otaHist[i + 1];
    idx = OTA_HIST_MAX - 1;
  } else {
    s_otaHistCount++;
  }
  s_otaHist[idx].timestamp = Drivers::rtc.getUnixTime();
  strncpy(s_otaHist[idx].version, version ? version : Core::FIRMWARE_VERSION, 15);
  s_otaHist[idx].version[15] = '\0';
  strncpy(s_otaHist[idx].event, event, 23);
  s_otaHist[idx].event[23] = '\0';
}

void noteBootEvent() {
  const char* ev = "BOOTED";
  uint8_t reason = (uint8_t)esp_reset_reason();
  if (Core::isWatchdogReset(reason)) ev = "BOOTED_AFTER_WDT";
  else if (Core::isBrownoutReset(reason)) ev = "BOOTED_AFTER_BROWNOUT";
  recordOtaEvent(Core::FIRMWARE_VERSION, ev);
}

void noteOtaEvent(const char* version, const char* event) {
  recordOtaEvent(version, event);
}

void handleOtaHistory() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("entries");
  for (uint8_t i = 0; i < s_otaHistCount; i++) {
    JsonObject e = arr.createNestedObject();
    e["timestamp"] = s_otaHist[i].timestamp;
    e["version"] = s_otaHist[i].version;
    e["event"] = s_otaHist[i].event;
  }
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

// ---------------------------------------------------------------------------
// /api/config/device — GET/POST device identity (name, site, timezone).
// POST carries requestId → canonical command pipeline (journal + dedupe).
// ---------------------------------------------------------------------------
void handleDeviceGet() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  StaticJsonDocument<256> doc;
  doc["deviceName"] = Core::deviceName;
  doc["siteName"] = Core::siteName;
  doc["timezone"] = Core::cfgTimezone;
  doc["deviceId"] = Core::deviceId;
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void handleDevicePost() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
  if (!requireCsrf()) return;
  if (!requireBody(2048)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  doc["type"] = "config";
  doc["action"] = "device";
  // [CORE-02] Envelope gate
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(doc, envErr)) {
      sendError(400, envErr);
      return;
    }
  }
  // [P2-1 REMEDIATION 2026-09] Freshness gate (REST/MQTT parity — see
  // ConfigHandlers.cpp for the full rationale).
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr)) {
      sendError(400, expiryErr);
      return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  // [p.473] Journal lock unavailable — REJECT (fail-closed): executing
  // without a dedup check could double-apply the mutation on retry.
  if (d.decision == Services::TransactionDecision::Unavailable) {
    sendError(503, "transaction journal lock unavailable — command NOT executed (fail-closed), retry");
    return;
  }
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command"); return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }
  // [audit p.491] Pre-commit snapshot — a failed NVS write rolls the device
  // identity fields back instead of reporting success for a change that a
  // reboot would silently revert.
  char oldDeviceName[64];
  strncpy(oldDeviceName, Core::deviceName, sizeof(oldDeviceName) - 1);
  oldDeviceName[sizeof(oldDeviceName) - 1] = '\0';
  char oldSiteName[64];
  strncpy(oldSiteName, Core::siteName, sizeof(oldSiteName) - 1);
  oldSiteName[sizeof(oldSiteName) - 1] = '\0';
  char oldTimezone[40];
  strncpy(oldTimezone, Core::cfgTimezone, sizeof(oldTimezone) - 1);
  oldTimezone[sizeof(oldTimezone) - 1] = '\0';

  bool changed = false;
  if (doc.containsKey("deviceName")) {
    const char* n = doc["deviceName"];
    if (n && strlen(n) > 0) { strncpy(Core::deviceName, n, 63); Core::deviceName[63] = '\0'; changed = true; }
  }
  if (doc.containsKey("siteName")) {
    const char* n = doc["siteName"];
    if (n) { strncpy(Core::siteName, n, 63); Core::siteName[63] = '\0'; changed = true; }
  }
  if (doc.containsKey("timezone")) {
    const char* t = doc["timezone"];
    if (t && strlen(t) > 0) { strncpy(Core::cfgTimezone, t, 39); Core::cfgTimezone[39] = '\0'; changed = true; }
  }
  if (changed && !Storage::config.saveDeviceConfig()) {
    // [audit p.491] FAIL-CLOSED: roll RAM back to the durable state and
    // surface the persistence failure — never "Device config updated".
    strncpy(Core::deviceName, oldDeviceName, sizeof(Core::deviceName) - 1);
    Core::deviceName[sizeof(Core::deviceName) - 1] = '\0';
    strncpy(Core::siteName, oldSiteName, sizeof(Core::siteName) - 1);
    Core::siteName[sizeof(Core::siteName) - 1] = '\0';
    strncpy(Core::cfgTimezone, oldTimezone, sizeof(Core::cfgTimezone) - 1);
    Core::cfgTimezone[sizeof(Core::cfgTimezone) - 1] = '\0';
    sendError(500, "device config NOT saved — NVS persistence FAILED; RAM rolled back to previous values");
    return;
  }
  String ack = "{\"success\":true,\"message\":\"Device config updated\",\"data\":{\"updated\":" +
               String(changed ? "true" : "false") + "}}";
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);
  sendSuccess(changed ? "Device config updated" : "No changes", "{\"updated\":" +
              String(changed ? "true" : "false") + "}");
}

// ---------------------------------------------------------------------------
// /api/config/password — change operator password (requires current password).
// Re-provisions the PBKDF2 hash + salt; persisted atomically.
// ---------------------------------------------------------------------------
void handlePasswordPost() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
  if (!requireCsrf()) return;
  if (!requireBody(2048)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  // [PARITY-4 2026-09-06] Password change joins the canonical transaction
  // path (audit P1: durable config mutations must carry requestId + journal
  // dedup like every other mutation). Body keys: current, next, requestId.
  doc["type"] = "config";
  doc["action"] = "password";
  // [CORE-02] Envelope gate
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(doc, envErr)) {
      sendError(400, envErr); return;
    }
  }
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr)) {
      sendError(400, expiryErr); return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  // [p.473] Journal lock unavailable — REJECT (fail-closed): executing
  // without a dedup check could double-apply the mutation on retry.
  if (d.decision == Services::TransactionDecision::Unavailable) {
    sendError(503, "transaction journal lock unavailable — command NOT executed (fail-closed), retry");
    return;
  }
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command"); return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }
  const char* current = doc["current"] | "";
  const char* next = doc["next"] | "";
  if (strlen(current) == 0 || strlen(next) == 0) {
    sendError(400, "Missing current or next password"); return;
  }
  if (strlen(next) < 8) {
    sendError(400, "New password must be at least 8 characters"); return;
  }
  // Verify CURRENT password against the stored hash (same PBKDF2 path as login)
  uint8_t hash[32];
  if (!Utils::pbkdf2HmacSha256(current, strlen(current),
                                Core::salt, Core::SALT_LEN,
                                Core::iterations, hash)) {
    sendError(500, "Hash computation failed"); return;
  }
  char hashHex[65];
  Utils::bytesToHex(hash, 32, hashHex);
  memset(hash, 0, sizeof(hash));
  if (!Utils::constantTimeMemEquals((const volatile uint8_t*)hashHex,
                                     (const volatile uint8_t*)Core::passHashHex, 64)) {
    Services::Log.append(Core::LogType::AuthFail, "Password change: wrong current password", 0);
    sendError(401, "Current password incorrect"); return;
  }
  // New salt + hash (re-provision)
  uint8_t newSalt[Core::SALT_LEN];
  Utils::generateRandomBytes(newSalt, Core::SALT_LEN);
  if (!Utils::pbkdf2HmacSha256(next, strlen(next),
                                newSalt, Core::SALT_LEN,
                                Core::iterations, hash)) {
    sendError(500, "Hash computation failed"); return;
  }
  Utils::bytesToHex(hash, 32, Core::passHashHex);
  Core::passHashHex[64] = '\0';
  memcpy(Core::salt, newSalt, Core::SALT_LEN);
  memset(hash, 0, sizeof(hash));
  memset(newSalt, 0, sizeof(newSalt));
  // [AUDIT 2026-09 ROUND 5 / p.444] First successful password change closes
  // the commissioning boundary: the UART-revealed default is no longer in
  // force, so the DEFAULT_CREDENTIALS_ACTIVE alarm clears on the next
  // evaluation and /api/security reports the boundary as closed.
  bool wasDefault = !Core::credentialsProvisioned;
  Core::credentialsProvisioned = true;
  Storage::config.saveUserConfig();
  Services::Log.append(Core::LogType::ConfigurationChanged,
                       wasDefault ? "Operator password changed — commissioning boundary CLOSED (default credential no longer active)"
                                  : "Operator password changed", 0);
  // [PARITY-4] journal AFTER the mutation succeeds (same 2-phase ordering as
  // ConfigHandlers — failed attempts stay retryable, successes are idempotent).
  String ack = "{\"success\":true,\"message\":\"Password changed\",\"data\":{\"changed\":true}}";
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);
  sendSuccess("Password changed", "{}");
}

// ---------------------------------------------------------------------------
// /api/config/export + /api/config/import — full backup/restore.
// ---------------------------------------------------------------------------
void handleExport() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  String json = Storage::config.exportAll();
  if (json.length() == 0) { sendError(500, "Export failed"); return; }
  sendSuccess("OK", json);
}

void handleImport() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
  if (!requireCsrf()) return;
  if (!requireBody(Core::HTTP_MAX_BODY_SIZE * 2)) return;
  String raw = http.arg("plain");
  if (raw.length() == 0) { sendError(400, "Empty body"); return; }
  // [PARITY-4 2026-09-06] Config import joins the transaction-identity policy
  // (audit P1: a large persistent state mutation must not be a journal-less
  // special case). The requestId rides the X-Request-Id HEADER, not the body:
  // the body is the CRC32-verified backup payload — injecting a key would
  // break Utils::verifyCRC. The journal hash is sha256 of the raw body, so
  // dedup semantics cover the full backup content (a different backup with
  // the same requestId = CONFLICT, a retry of the same bytes = replayed ACK).
  // Missing header keeps today's behavior (apply without journaling).
  String reqId = http.header("X-Request-Id");
  reqId.trim();
  if (reqId.length() > 0) {
    String tidErr;
    if (!Services::CommandCanonicalizer::validateTransactionId(reqId, tidErr)) {
      sendError(400, tidErr); return;
    }
    String bodyHash = Utils::sha256Hex(raw);
    String prevAck;
    Services::TransactionDecision d = Services::journal.decide(reqId, bodyHash, prevAck);
    // [p.473] Journal lock unavailable — REJECT (fail-closed): importing
    // without a dedup check could re-apply the same large mutation on retry.
    if (d == Services::TransactionDecision::Unavailable) {
      sendError(503, "transaction journal lock unavailable — import NOT executed (fail-closed), retry");
      return;
    }
    if (d == Services::TransactionDecision::Conflict) {
      sendError(409, "requestId reuse with different import payload"); return;
    }
    if (d == Services::TransactionDecision::Duplicate) {
      sendSecurityHeaders();
      http.send(200, "application/json; charset=utf-8", prevAck);
      return;
    }
    bool okImp = Storage::config.importAll(raw);
    String ack;
    if (okImp) {
      Services::Log.append(Core::LogType::ConfigurationChanged, "Configuration imported", 0);
      ack = "{\"success\":true,\"message\":\"Configuration imported — reboot required\",\"data\":{\"imported\":true}}";
    } else {
      ack = "{\"success\":false,\"message\":\"Import failed — invalid or incompatible backup\"}";
    }
    // Journal both outcomes: an import attempt with this requestId must not
    // re-apply the same large mutation on retry (replay returns the verdict).
    Services::journal.storeTransaction(reqId, bodyHash, ack);
    if (!okImp) { sendError(400, "Import failed — invalid or incompatible backup"); return; }
    sendSuccess("Configuration imported — reboot required", "{}");
    return;
  }
  bool ok = Storage::config.importAll(raw);
  if (!ok) { sendError(400, "Import failed — invalid or incompatible backup"); return; }
  Services::Log.append(Core::LogType::ConfigurationChanged, "Configuration imported", 0);
  sendSuccess("Configuration imported — reboot required", "{}");
}

// ---------------------------------------------------------------------------
// [audit-2 R-2] The non-canonical POST /api/alarms/acknowledge handler
// (handleAlarmAckGeneric) and the unused handleAlarmAck stub have been
// REMOVED. Canonical alarm ACK is POST /api/alarms/{code}/acknowledge,
// registered by AlarmHandlers::registerRoutes() as a sub-path catch-all at
// "/api/alarms/". One endpoint, one contract (P1-3).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// /api/bms — v1.6.0 multi-protocol BMS/inverter comm diagnostics.
// Fail-closed honesty: when the layer is compiled out (PLTS_ENABLE_BMS_COMM=0)
// the route returns a deterministic 503, never fabricated "connected" data.
// ---------------------------------------------------------------------------
void handleBmsStatus() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
#if PLTS_ENABLE_BMS_COMM
  Comm::BmsData d = Comm::batteryComm.getData();
  StaticJsonDocument<1536> doc;
  doc["enabled"]       = true;
  doc["state"]         = Comm::batteryComm.stateStr();
  doc["protocol"]      = Comm::batteryComm.activeProtocolStr();
  doc["configured"]    = Core::cfgBmsProtocol;
  doc["pollIntervalMs"]= Core::cfgBmsPollIntervalMs;
  doc["probeCycles"]   = Comm::batteryComm.getProbeCycleCount();
  doc["lockedMs"]      = Comm::batteryComm.getLockMs();
  doc["socAuthoritative"] = Comm::batteryComm.socAuthoritative();
  doc["mismatchActive"]   = Comm::batteryComm.isMismatchActive();
  JsonObject data = doc.createNestedObject("data");
  data["lastUpdateMs"] = d.lastUpdateMs;
  data["frameCount"]   = d.frameCount;
  data["errorCount"]   = d.errorCount;
  if (Core::isValidFloat(d.soc))      data["soc"].set(d.soc);
  else                                data["soc"].set(nullptr);
  if (Core::isValidFloat(d.soh))      data["soh"].set(d.soh);
  else                                data["soh"].set(nullptr);
  if (Core::isValidFloat(d.voltage))  data["voltage"].set(d.voltage);
  else                                data["voltage"].set(nullptr);
  if (Core::isValidFloat(d.current))  data["current"].set(d.current);
  else                                data["current"].set(nullptr);
  if (Core::isValidFloat(d.temperature)) data["temperature"].set(d.temperature);
  else                                   data["temperature"].set(nullptr);
  if (Core::isValidFloat(d.cellVoltageMin)) data["cellVoltageMin"].set(d.cellVoltageMin);
  else                                      data["cellVoltageMin"].set(nullptr);
  if (Core::isValidFloat(d.cellVoltageMax)) data["cellVoltageMax"].set(d.cellVoltageMax);
  else                                      data["cellVoltageMax"].set(nullptr);
  data["cellCount"]         = d.cellCount;
  if (Core::isValidFloat(d.chargeCurrentLimit)) data["chargeCurrentLimit"].set(d.chargeCurrentLimit);
  else                                          data["chargeCurrentLimit"].set(nullptr);
  if (Core::isValidFloat(d.dischargeCurrentLimit)) data["dischargeCurrentLimit"].set(d.dischargeCurrentLimit);
  else                                             data["dischargeCurrentLimit"].set(nullptr);
  data["cycleCount"]        = d.cycleCount;
  data["faultFlags"]        = d.faultFlags;
  data["moduleCount"]       = d.moduleCount;
  if (Core::isValidFloat(Comm::batteryComm.getLastMismatchA())) data["currentMismatchA"].set(Comm::batteryComm.getLastMismatchA());
  else                                                    data["currentMismatchA"].set(nullptr);
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
#else
  sendError(503, "BMS communication layer compiled out (PLTS_ENABLE_BMS_COMM=0)");
#endif
}

// ---------------------------------------------------------------------------
void registerRoutes() {
  // Canonical PWA-contract route names (P0-001)
  http.on("/api/log", HTTP_GET, handleLogAlias);              // alias of /api/logs
  http.on("/api/insights", HTTP_GET, handleInsights);
  http.on("/api/reports", HTTP_POST, handleReports);
  http.on("/api/ota/history", HTTP_GET, handleOtaHistory);
  http.on("/api/config/device", HTTP_GET, handleDeviceGet);
  http.on("/api/config/device", HTTP_POST, handleDevicePost);
  http.on("/api/config/password", HTTP_POST, handlePasswordPost);
  http.on("/api/config/export", HTTP_GET, handleExport);
  http.on("/api/config/import", HTTP_POST, handleImport);
  // v1.6.0 — multi-protocol BMS/inverter comm diagnostics
  http.on("/api/bms", HTTP_GET, handleBmsStatus);
#if PLTS_ENABLE_RS485_CONSOLE
  // v1.7.0 — passive RS485 vendor-frame capture (bench; protocol=rs485_console)
  http.on("/api/rs485/frames", HTTP_GET, handleRs485Frames);
#endif
  // Per-alarm ACK: P1-3 canonical contract is POST /api/alarms/{alarmId}/acknowledge
  // which is registered by AlarmHandlers::registerRoutes() as a sub-path catch-all
  // at "/api/alarms/". The previous non-canonical POST /api/alarms/acknowledge
  // (with body {code}) is REMOVED — one endpoint, one contract.
}

// ---------------------------------------------------------------------------
// v1.7.0 — GET /api/rs485/frames: passive vendor-frame capture (bench tool).
// Raw hex only — NO interpretation (the PylontechRs485 parser stays RESERVED
// until real vendor frames are captured and documented). Fail-closed: when
// the console is compiled out or not active, the route says so honestly.
// ---------------------------------------------------------------------------
void handleRs485Frames() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
#if PLTS_ENABLE_RS485_CONSOLE
  if (!Comm::rs485Console.isActive()) {
    sendError(503, "RS485 console not active — set bmsProtocol=rs485_console (bench mode)");
    return;
  }
  sendSuccess("RS485 console frames (raw, uninterpreted)", Comm::rs485Console.framesJson());
#else
  sendError(503, "RS485 console compiled out (PLTS_ENABLE_RS485_CONSOLE=0)");
#endif
}

} // namespace ExtraHandlers
} // namespace Web
