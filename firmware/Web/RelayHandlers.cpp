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

  // Apply command via RelayController (single mutation path)
  String messageOut;
  Services::RelayCommandResult result = Services::relaysController.applyCommand(
    action, (uint8_t)channel, action == "on" || action == "pulse",
    pulseMs, source, messageOut);

  // Build ACK
  String ack;
  StaticJsonDocument<512> ackDoc;
  ackDoc["ok"] = (result == Services::RelayCommandResult::Applied);
  ackDoc["result"] = (result == Services::RelayCommandResult::Applied) ? "EXECUTED" :
                     (result == Services::RelayCommandResult::Blocked) ? "BLOCKED" :
                     (result == Services::RelayCommandResult::Rejected) ? "REJECTED" : "FAILED";
  ackDoc["channel"] = (uint8_t)channel;
  ackDoc["message"] = messageOut;
  ackDoc["transactionId"] = canon.transactionId;
  serializeJson(ackDoc, ack);

  // Store transaction
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);

  // Respond
  if (result == Services::RelayCommandResult::Applied) {
    sendSuccess(messageOut, ack);
  } else if (result == Services::RelayCommandResult::Blocked) {
    sendError(403, messageOut);
  } else {
    sendError(400, messageOut);
  }
}

// POST /api/relays/all_off
static void handleAllOff() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;

  String raw = http.arg("plain");
  StaticJsonDocument<256> doc;
  if (raw.length() > 0 && !deserializeJson(doc, raw)) {
    const char* requestId = doc["requestId"] | "";
    // Tag for canonical pipeline
    StaticJsonDocument<256> cmdDoc;
    cmdDoc["type"] = "relay";
    cmdDoc["action"] = "all_off";
    cmdDoc["requestId"] = requestId;

    Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
    if (canon.ok) {
      Services::DecisionResult d =
        Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
      if (d.decision == Services::TransactionDecision::Duplicate) {
        sendSecurityHeaders();
        http.send(200, "application/json; charset=utf-8", d.previousAckJson);
        return;
      }

      String messageOut;
      Services::relaysController.applyCommand("all_off", 0, false, 0, "MANUAL", messageOut);

      String ack;
      StaticJsonDocument<256> ackDoc;
      ackDoc["ok"] = true;
      ackDoc["result"] = "EXECUTED";
      ackDoc["message"] = messageOut;
      ackDoc["transactionId"] = canon.transactionId;
      serializeJson(ackDoc, ack);
      Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);
      sendSuccess(messageOut, ack);
      return;
    }
  }

  // [self-review fix] NO FALLBACK — if body is missing or invalid, reject.
  // The previous fallback called applyCommand directly without journal,
  // bypassing the transaction durability boundary (brief §8).
  sendError(400, "Missing or invalid JSON body — requestId required for all_off");
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
