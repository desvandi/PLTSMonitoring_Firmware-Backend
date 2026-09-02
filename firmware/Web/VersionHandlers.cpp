// =============================================================================
// Web/VersionHandlers.cpp
// =============================================================================
#include "VersionHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Config.h"
#include <ArduinoJson.h>

namespace Web {
namespace VersionHandlers {
void handleGet() {
  StaticJsonDocument<512> doc;
  doc["firmwareVersion"] = Core::FIRMWARE_VERSION;
  doc["protocolVersion"] = Core::PROTOCOL_VERSION;
  doc["configVersion"] = Core::CONFIG_SCHEMA_VERSION;
  doc["calibrationVersion"] = Core::CALIBRATION_SCHEMA_VERSION;
  doc["buildDate"] = Core::FIRMWARE_BUILD_DATE;
  doc["deviceModel"] = "ESP32-WROOM-32";
#ifdef PRODUCTION_BUILD
  doc["buildProfile"] = "PRODUCTION";
#elif defined(STAGING_BUILD)
  doc["buildProfile"] = "STAGING";
#else
  doc["buildProfile"] = "DEVELOPMENT";
#endif
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}
void registerRoutes() {
  http.on("/api/version", HTTP_GET, handleGet);
}
} // namespace VersionHandlers
} // namespace Web
