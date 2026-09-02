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
  // Take a snapshot under mutex
  Core::SystemStatus snap;
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = latestStatus;
    xSemaphoreGive(telemetryMutex);
  } else {
    sendError(503, "Telemetry mutex timeout");
    return;
  }
  
  
  String body = Web::serialize(snap);
  sendSecurityHeaders();
  http.send(200, "application/json; charset=utf-8", body);
}

void registerRoutes() {
  http.on("/api/status", HTTP_GET, handleStatus);
}

} // namespace StatusHandlers
} // namespace Web
