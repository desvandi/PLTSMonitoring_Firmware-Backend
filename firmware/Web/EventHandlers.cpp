// =============================================================================
// Web/EventHandlers.cpp — GET /api/events (audit log)
// =============================================================================
#include "EventHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/LogService.h"

namespace Web {
namespace EventHandlers {
void handleGet() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  uint16_t maxBytes = 4096;
  if (http.hasArg("maxBytes")) maxBytes = (uint16_t)http.arg("maxBytes").toInt();
  String audit = Services::Log.getAuditText(maxBytes);
  String data = "{\"auditLog\":";
  // Escape newlines for JSON
  String escaped;
  escaped.reserve(audit.length() + 16);
  for (size_t i = 0; i < audit.length(); i++) {
    char c = audit[i];
    if (c == '"') escaped += "\\\"";
    else if (c == '\\') escaped += "\\\\";
    else if (c == '\n') escaped += "\\n";
    else if (c == '\r') escaped += "\\r";
    else escaped += c;
  }
  data += "\"" + escaped + "\"}";
  sendSuccess("OK", data);
}
void registerRoutes() {
  http.on("/api/events", HTTP_GET, handleGet);
}
} // namespace EventHandlers
} // namespace Web
