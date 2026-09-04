// =============================================================================
// Web/RelayHandlers.cpp — REST endpoints for 8-channel relay control
// -----------------------------------------------------------------------------
// [v1.8.0] All relay mutations go through the canonical command pipeline:
//   auth → CSRF → freshness → canonicalize → journal → RelayController
// NO BYPASS: no direct GPIO writes from REST.
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

// POST /api/relays/{channel}/on
// POST /api/relays/{channel}/off
// POST /api/relays/{channel}/pulse
static void handleRelayCommand() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(512)) return;

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
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }

  const char* requestId = doc["requestId"] | "";
  const char* source = doc["source"] | "MANUAL";
  uint32_t pulseMs = doc["durationMs"] | 0;

  // Tag with (type, action) for canonical pipeline
  StaticJsonDocument<512> cmdDoc;
  cmdDoc["type"] = "relay";
  cmdDoc["action"] = action;
  cmdDoc["channel"] = (uint8_t)channel;
  cmdDoc["source"] = source;
  if (action == "pulse") cmdDoc["durationMs"] = pulseMs;
  cmdDoc["requestId"] = requestId;
  cmdDoc["transactionId"] = doc["transactionId"] | "";
  cmdDoc["commandSequence"] = doc["commandSequence"] | 0;
  cmdDoc["issuedAt"] = doc["issuedAt"] | 0;
  cmdDoc["expiresAt"] = doc["expiresAt"] | 0;

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

  // [P1-004] Queue command for single-threaded execution in relayTask.
  // If queue is full, return 503 (not 200 — no false ACK).
  String sourceStr = source;
  bool queued = Services::relaysController.queueCommand(
    action, (uint8_t)channel, action == "on" || action == "pulse",
    pulseMs, sourceStr);
  if (!queued) {
    sendError(503, "Relay command queue full — retry after a brief delay");
    return;
  }

  // Build ACK — command queued for execution
  String ack;
  StaticJsonDocument<512> ackDoc;
  ackDoc["ok"] = true;
  ackDoc["result"] = "QUEUED";
  ackDoc["channel"] = (uint8_t)channel;
  ackDoc["message"] = "Command queued for execution";
  ackDoc["transactionId"] = canon.transactionId;
  serializeJson(ackDoc, ack);

  // Store transaction
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);

  // Respond — command queued successfully
  sendSuccess("Command queued", ack);
}

// POST /api/relays/all_off
// [P1-6] Freshness gate + transactionId required + [P1-7] per-channel result
static void handleAllOff() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(256)) return;

  String raw = http.arg("plain");
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }

  const char* requestId = doc["requestId"] | "";
  if (strlen(requestId) == 0) {
    sendError(400, "requestId required for relay mutation");
    return;
  }

  // Tag for canonical pipeline
  StaticJsonDocument<256> cmdDoc;
  cmdDoc["type"] = "relay";
  cmdDoc["action"] = "all_off";
  cmdDoc["requestId"] = requestId;
  cmdDoc["transactionId"] = doc["transactionId"] | "";
  cmdDoc["issuedAt"] = doc["issuedAt"] | 0;
  cmdDoc["expiresAt"] = doc["expiresAt"] | 0;

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

  // [P1-7] Execute with per-channel result tracking
  Services::AllOffResult result = Services::relaysController.allOffWithResult();

  // Build honest ACK — report per-channel success/failure
  String ack;
  StaticJsonDocument<512> ackDoc;
  ackDoc["ok"] = (result.failed == 0);
  ackDoc["result"] = (result.failed == 0) ? "EXECUTED" : "PARTIAL";
  ackDoc["requested"] = result.requested;
  ackDoc["success"] = result.success;
  ackDoc["failed"] = result.failed;
  if (result.failed > 0) {
    ackDoc["detail"] = result.detail;
  }
  ackDoc["message"] = String("All OFF: ") + String(result.success) + " ok, " +
                       String(result.failed) + " failed";
  ackDoc["transactionId"] = canon.transactionId;
  serializeJson(ackDoc, ack);
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);

  if (result.failed > 0) {
    sendError(500, String("Partial failure: ") + result.detail);
  } else {
    sendSuccess("All channels OFF", ack);
  }
}

void registerRoutes() {
  http.on("/api/relays", HTTP_GET, handleGetRelays);
  http.on("/api/relays/all_off", HTTP_POST, handleAllOff);

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
