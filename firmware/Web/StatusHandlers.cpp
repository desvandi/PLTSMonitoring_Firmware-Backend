// =============================================================================
#include "../Drivers/Sht31Driver.h"
// Web/StatusHandlers.cpp — GET /api/status (shared serializer)
// =============================================================================
#include "StatusHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "BatteryStatusSerializer.h"
#include "../Core/Globals.h"
#include <ArduinoJson.h>

namespace Web {
namespace StatusHandlers {

void handleStatus() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  // [AUDIT 2026-09 ROUND 8 / p.470] serializeLatestStatusLocked() deep-copies
  // the active-alarm list under telemetryMutex BEFORE serializing — the old
  // `snap = latestStatus` struct copy left snap.activeAlarms aliasing
  // publishTelemetry()'s static buffer, and Web::serialize(snap) ran AFTER
  // the mutex was released, so a concurrent telemetry publish could rewrite
  // the list mid-serialization (torn entries: new code + old message).
  String body = Web::serializeLatestStatusLocked();
  if (body.length() == 0) {
    sendError(503, "Telemetry mutex timeout");
    return;
  }
  sendSecurityHeaders();
  http.send(200, "application/json; charset=utf-8", body);
}

void registerRoutes() {
  http.on("/api/status", HTTP_GET, handleStatus);
}

} // namespace StatusHandlers
} // namespace Web
