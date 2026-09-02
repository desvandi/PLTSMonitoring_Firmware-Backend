// =============================================================================
// Web/LogHandlers.cpp
// =============================================================================
#include "LogHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/LogService.h"

namespace Web {
namespace LogHandlers {
void handleGetLogs() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  uint16_t limit = 200;
  int8_t filter = -1;
  if (http.hasArg("limit")) limit = http.arg("limit").toInt();
  if (http.hasArg("type")) filter = (int8_t)http.arg("type").toInt();
  if (limit > 500) limit = 500;
  String data = Services::Log.getActivityJson(limit, filter);
  sendSuccess("OK", data);
}
void registerRoutes() {
  http.on("/api/logs", HTTP_GET, handleGetLogs);
}
} // namespace LogHandlers
} // namespace Web
