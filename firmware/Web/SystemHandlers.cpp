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
  delay(500);
  // [audit-2 S-16 FIX] Wipe ALL NVS namespaces — previously missed
  // plts_alarm, plts_time, plts_soc, plts_emg, plts_auth, plts_emg_calib.
  // Incomplete wipe left stale alarm state + crash-loop counters + auth
  // tokens after factory reset → device could boot into CRASHLOOP hold
  // or show stale alarms. Now wipe the complete namespace set.
  Preferences p;
  static const char* const NAMESPACES[] = {
    "plts",          // main config
    "plts_health",   // health supervisor
    "plts_energy",   // energy history
    "plts_ota",      // OTA state
    "plts_txn",      // transaction journal (dedup)
    "plts_spool",    // telemetry spool
    "plts_batt",     // battery snapshot
    "plts_alarm",    // [audit-2 S-16] alarm state with CRC
    "plts_time",     // [audit-2 S-16] epoch estimate
    "plts_soc",      // [audit-2 S-16] SOC integrator
    "plts_emg",      // [audit-2 S-16] emergency relay state + trip counter
    "plts_auth",     // [audit-2 S-16] refresh tokens
    "plts_relays",   // [v1.8.0] 8-channel relay config + lockout states
  };
  for (const char* ns : NAMESPACES) {
    if (p.begin(ns, false)) { p.clear(); p.end(); }
  }
  // [audit-2 S-17] Preserve audit log across factory reset — forensic
  // evidence of who triggered the reset should survive. Audit log is in
  // LittleFS PATH_AUDIT_LOG. Save to a temporary NVS blob before format,
  // restore after.
  // [CI fix] These functions are defined in LogService.cpp inside namespace Services.
  // Declare them properly with namespace qualification.
  bool auditPreserved = Services::preserveAuditLogAcrossReset();
  LittleFS.format();
  if (auditPreserved) Services::restoreAuditLogAfterReset();
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
