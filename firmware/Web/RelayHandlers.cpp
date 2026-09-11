// =============================================================================
// Web/RelayHandlers.cpp — REST endpoints for 8-channel relay control
// -----------------------------------------------------------------------------
// [v1.8.0] All relay mutations go through the canonical command pipeline:
//   auth → CSRF → envelope → freshness → canonicalize → journal → QUEUE
//   → relayTask (single physical mutation authority) → final result
// NO BYPASS: no direct GPIO writes from REST.
//
// [PRODUCTION-GRADE 2026-09] — audit p.34-39, p.74-77 remediation:
//   - Per-channel AND all_off both enqueue via RelayController::queueCommand
//     (all_off no longer executes in the HTTP task context — RG-RELAY-01/07).
//   - HTTP 200 + result=QUEUED is an ASYNCHRONOUS SUBMISSION ack; the final
//     outcome is retrievable via GET /api/relays/transactions/{transactionId}
//     (RG-RELAY-02/04/09).
//   - journal.storeTransaction() failures now surface as HTTP 503 — a
//     durability failure never produces a success ACK (TXN-02).
//   - The full envelope (version/transactionId/issuedAt/expiresAt) is
//     mandatory for mutations (CORE-02).
// =============================================================================
#include "RelayHandlers.h"
#if PLTS_ENABLE_RELAYS
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Services/AuthManager.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/RelayController.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>

namespace Web {
namespace RelayHandlers {

// Helper: extract channel from URI path /api/relays/{channel}/...
static int8_t extractChannelFromPath(const String& uri) {
  // Find "/relays/" then read the channel number
  int idx = uri.indexOf("/relays/");
  if (idx < 0) return -1;
  int start = idx + 8;  // skip "/relays/"
  int end = uri.indexOf('/', start);
  if (end < 0) end = uri.length();
  String chStr = uri.substring(start, end);
  // Validate: must be 0-7
  if (chStr.length() == 0 || chStr.length() > 1) return -1;
  if (chStr[0] < '0' || chStr[0] > '7') return -1;
  return chStr[0] - '0';
}

// GET /api/relays — list all channels
static void handleGetRelays() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }

  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.createNestedArray("channels");
  Services::relaysController.serializeStatus(arr);
  doc["available"] = Services::relaysController.isAvailable();
  doc["channelCount"] = Services::relaysController.getChannelCount();

  String out;
  serializeJson(doc, out);
  sendSuccess("OK", out);
}

// GET /api/relays/transactions/{transactionId}
// [RG-RELAY-09] Final-outcome reconciliation endpoint. PWA polls this after
// a QUEUED ack to learn EXECUTED / BLOCKED / REJECTED / FAILED / UNKNOWN.
static void handleGetTransaction() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }

  String uri = http.uri();
  int idx = uri.indexOf("/transactions/");
  if (idx < 0) { sendError(400, "missing transactionId"); return; }
  String tid = uri.substring(idx + 14);
  if (tid.length() == 0 || tid.length() > 64) {
    sendError(400, "invalid transactionId");
    return;
  }

  Services::RelayTransactionRecord rec;
  if (!Services::relaysController.getTransactionResult(tid, rec)) {
    // Not (yet) in the result ring — could be still queued, or evicted.
    // Report honestly as PENDING rather than guessing.
    String out = "{\"transactionId\":\"" + tid + "\",\"state\":\"PENDING\"}";
    sendSuccess("OK", out);
    return;
  }

  const char* resultMap[] = { "EXECUTED", "BLOCKED", "REJECTED", "FAILED", "UNKNOWN" };
  StaticJsonDocument<512> doc;
  doc["transactionId"] = rec.transactionId;
  doc["state"] = "TERMINAL";
  doc["result"] = resultMap[(size_t)rec.result];
  doc["channel"] = rec.channel;
  doc["desiredState"] = rec.desiredState;
  doc["reportedState"] = rec.reportedState;
  doc["stateSequence"] = rec.stateSequence;
  doc["message"] = rec.message;
  doc["completedAtMs"] = rec.completedAtMs;
  String out;
  serializeJson(doc, out);
  sendSuccess("OK", out);
}

// POST /api/relays/{channel}/on
// POST /api/relays/{channel}/off
// POST /api/relays/{channel}/pulse
// POST /api/relays/{channel}/acknowledge
// POST /api/relays/{channel}/clear
static void handleRelayCommand() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(768)) return;

  String uri = http.uri();
  int8_t channel = extractChannelFromPath(uri);
  if (channel < 0) { sendError(400, "Invalid channel (must be 0-7)"); return; }

  // Determine action from URI suffix
  String action;
  if (uri.endsWith("/on")) action = "on";
  else if (uri.endsWith("/off")) action = "off";
  else if (uri.endsWith("/pulse")) action = "pulse";
  else if (uri.endsWith("/acknowledge")) action = "acknowledge";
  else if (uri.endsWith("/clear")) action = "clear";
  else { sendError(400, "Unknown relay action"); return; }

  // Parse body
  String raw = http.arg("plain");
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }

  const char* requestId = doc["requestId"] | "";
  const char* source = doc["source"] | "MANUAL";
  uint32_t pulseMs = doc["durationMs"] | 0;

  // Tag with (type, action) for canonical pipeline
  StaticJsonDocument<768> cmdDoc;
  cmdDoc["type"] = "relay";
  cmdDoc["action"] = action;
  cmdDoc["channel"] = (uint8_t)channel;
  cmdDoc["source"] = source;
  if (action == "pulse") cmdDoc["durationMs"] = pulseMs;
  cmdDoc["requestId"] = requestId;
  cmdDoc["transactionId"] = doc["transactionId"] | "";
  cmdDoc["version"] = doc["version"] | 0;
  cmdDoc["issuedAt"] = doc["issuedAt"] | 0;
  cmdDoc["expiresAt"] = doc["expiresAt"] | 0;

  // [CORE-02] Envelope gate — version + transactionId + issuedAt + expiresAt
  // are mandatory for mutations. Replay protection must never silently
  // degrade to journal-retention-only (audit p.62-63, p.330).
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(cmdDoc, envErr)) {
      sendError(400, envErr.c_str());
      return;
    }
  }

  // Freshness gate
  String expiryErr;
  if (Services::CommandCanonicalizer::isCommandExpired(cmdDoc, expiryErr)) {
    sendError(400, expiryErr.c_str());
    return;
  }

  // Canonicalize + hash
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }

  // Decide transaction
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command");
    return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }

  // [RG-RELAY-02] Queue with FULL transaction identity — identity must
  // survive into the executor so the final result is correlatable.
  Services::QueuedRelayCommand qc = {};
  strncpy(qc.transactionId, canon.transactionId.c_str(), sizeof(qc.transactionId) - 1);
  strncpy(qc.requestId, canon.transactionId.c_str(), sizeof(qc.requestId) - 1);
  strncpy(qc.commandHash, canon.commandHash.c_str(), sizeof(qc.commandHash) - 1);
  strncpy(qc.command, action.c_str(), sizeof(qc.command) - 1);
  qc.channel = (uint8_t)channel;
  qc.desiredState = (action == "on" || action == "pulse");
  qc.pulseDurationMs = pulseMs;
  strncpy(qc.source, source, sizeof(qc.source) - 1);
  qc.issuedAt = cmdDoc["issuedAt"] | 0U;
  qc.expiresAt = cmdDoc["expiresAt"] | 0U;
  qc.safetyGeneration = Services::relaysController.safetyGeneration();
  qc.enqueuedAtMs = millis();

  if (!Services::relaysController.queueCommand(qc)) {
    sendError(503, "Relay command queue full — retry with the SAME transactionId");
    return;
  }

  // Build ACK — asynchronous submission accepted (audit p.73-75)
  String ack;
  StaticJsonDocument<512> ackDoc;
  ackDoc["ok"] = true;
  ackDoc["result"] = "QUEUED";
  ackDoc["state"] = "QUEUED";
  ackDoc["channel"] = (uint8_t)channel;
  ackDoc["message"] = "Command queued for execution";
  ackDoc["transactionId"] = canon.transactionId;
  serializeJson(ackDoc, ack);

  // [TXN-02/CORE-05] Journal durability failure must NOT produce a success
  // ACK — the command is queued in RAM but not durable; report 503 so the
  // operator can retry with the SAME transactionId.
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack)) {
    sendError(503, "Transaction journal persistence failed — command NOT durably recorded; retry with the same transactionId");
    return;
  }

  // Respond — command queued successfully
  sendSuccess("Command queued", ack);
}

// POST /api/relays/all_off
// [audit p.77, RG-RELAY-07] all_off follows the SAME transaction lifecycle as
// per-channel commands: it is enqueued and executed by relayTask — no longer
// a direct hardware mutation from the HTTP task context.
static void handleAllOff() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(384)) return;

  String raw = http.arg("plain");
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }

  const char* requestId = doc["requestId"] | "";

  // Tag for canonical pipeline
  StaticJsonDocument<384> cmdDoc;
  cmdDoc["type"] = "relay";
  cmdDoc["action"] = "all_off";
  cmdDoc["requestId"] = requestId;
  cmdDoc["transactionId"] = doc["transactionId"] | "";
  cmdDoc["version"] = doc["version"] | 0;
  cmdDoc["issuedAt"] = doc["issuedAt"] | 0;
  cmdDoc["expiresAt"] = doc["expiresAt"] | 0;

  // [CORE-02] Envelope gate — same as per-channel commands
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(cmdDoc, envErr)) {
      sendError(400, envErr.c_str());
      return;
    }
  }

  // [P1-6] Freshness gate — same as per-channel commands
  String expiryErr;
  if (Services::CommandCanonicalizer::isCommandExpired(cmdDoc, expiryErr)) {
    sendError(400, expiryErr.c_str());
    return;
  }

  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }

  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command");
    return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }

  // [RG-RELAY-07] Enqueue all_off — executed by relayTask like every other
  // normal mutation. The per-channel final result is retrievable via
  // GET /api/relays/transactions/{transactionId}.
  Services::QueuedRelayCommand qc = {};
  strncpy(qc.transactionId, canon.transactionId.c_str(), sizeof(qc.transactionId) - 1);
  strncpy(qc.requestId, canon.transactionId.c_str(), sizeof(qc.requestId) - 1);
  strncpy(qc.commandHash, canon.commandHash.c_str(), sizeof(qc.commandHash) - 1);
  strncpy(qc.command, "all_off", sizeof(qc.command) - 1);
  qc.channel = 0;
  qc.desiredState = false;
  strncpy(qc.source, "MANUAL", sizeof(qc.source) - 1);
  qc.issuedAt = cmdDoc["issuedAt"] | 0U;
  qc.expiresAt = cmdDoc["expiresAt"] | 0U;
  qc.safetyGeneration = Services::relaysController.safetyGeneration();
  qc.enqueuedAtMs = millis();

  if (!Services::relaysController.queueCommand(qc)) {
    sendError(503, "Relay command queue full — retry with the SAME transactionId");
    return;
  }

  String ack;
  StaticJsonDocument<384> ackDoc;
  ackDoc["ok"] = true;
  ackDoc["result"] = "QUEUED";
  ackDoc["state"] = "QUEUED";
  ackDoc["message"] = "All-off command queued for execution";
  ackDoc["transactionId"] = canon.transactionId;
  serializeJson(ackDoc, ack);

  // [TXN-02/CORE-05] Durability failure → no success ACK
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack)) {
    sendError(503, "Transaction journal persistence failed — command NOT durably recorded; retry with the same transactionId");
    return;
  }

  sendSuccess("All-off queued", ack);
}

void registerRoutes() {
  http.on("/api/relays", HTTP_GET, handleGetRelays);
  http.on("/api/relays/all_off", HTTP_POST, handleAllOff);
  http.on("/api/relays/transactions/", HTTP_GET, handleGetTransaction);

  // Per-channel commands — pattern-routed via onNotFound catch-all
  // Arduino WebServer doesn't support path params, so we register a
  // catch-all at "/api/relays/" and dispatch by URI suffix.
  http.on("/api/relays/", HTTP_POST, []() {
    String uri = http.uri();
    if (uri.endsWith("/on") || uri.endsWith("/off") || uri.endsWith("/pulse")) {
      handleRelayCommand();
    } else if (uri.endsWith("/acknowledge") || uri.endsWith("/clear")) {
      handleRelayCommand();  // acknowledge/clear use same handler
    } else {
      sendError(404, "Not Found");
    }
  });
}

} // namespace RelayHandlers
} // namespace Web

#endif // PLTS_ENABLE_RELAYS
