// =============================================================================
// Web/SystemHandlers.cpp
// =============================================================================
#include "SystemHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Config.h"
#include <Preferences.h>
#include <LittleFS.h>
#include "../Services/AuthManager.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>

namespace Web {
namespace SystemHandlers {

void handleReboot() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  sendSuccess("Rebooting", "{}");
  delay(500);
  ESP.restart();
}

void handleFactoryResetPrepare() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  String token = Services::auth.prepareFactoryReset();
  String data = "{\"token\":\"" + token + "\",\"ttlSec\":60}";
  sendSuccess("Factory reset prepared — confirm within 60s", data);
}

void handleFactoryResetConfirm() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  const char* token = doc["token"] | "";
  if (!Services::auth.confirmFactoryReset(token)) {
    sendError(400, "Invalid or expired token");
    return;
  }
  sendSuccess("Factory reset confirmed — executing", "{}");
  // Wipe NVS + LittleFS
  delay(500);
  // Soft wipe: clear all NVS namespaces
  Preferences p;
  p.begin("plts", false); p.clear(); p.end();
  p.begin("plts_health", false); p.clear(); p.end();
  p.begin("plts_energy", false); p.clear(); p.end();
  p.begin("plts_ota", false); p.clear(); p.end();
  p.begin("plts_txn", false); p.clear(); p.end();
  p.begin("plts_spool", false); p.clear(); p.end();
  p.begin("plts_batt", false); p.clear(); p.end();
  LittleFS.format();
  delay(500);
  ESP.restart();
}

void registerRoutes() {
  http.on("/api/reboot", HTTP_POST, handleReboot);
  http.on("/api/factory_reset/prepare", HTTP_POST, handleFactoryResetPrepare);
  http.on("/api/factory_reset/confirm", HTTP_POST, handleFactoryResetConfirm);
}

} // namespace SystemHandlers
} // namespace Web
