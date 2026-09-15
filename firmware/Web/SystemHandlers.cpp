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
#include "../Services/FactoryReset.h"
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
  // [AUDIT 2026-09 ROUND 11 / p.476] Empty string = fail-closed refusal
  // (no serialization → no token issued). NEVER answer 200 with an empty
  // token — the client would present it back and the flow would lie.
  if (token.length() != 32) {
    sendError(503, "Factory reset unavailable (auth lock) — token not issued, retry");
    return;
  }
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
    // [p.476] False covers an invalid/expired token AND the fail-closed
    // lock refusal (no serialization → token NOT consumed, wipe NOT
    // authorized). Both refuse conservatively; authLockFailures on
    // /api/diagnostics distinguishes the degradation.
    sendError(400, "Invalid or expired token");
    return;
  }
  sendSuccess("Factory reset confirmed — executing", "{}");
  delay(500);
  // [PRODUCTION-GRADE 2026-09 / audit p.145-146, STORAGE-GATE-06] Factory
  // reset is a RECOVERABLE transaction: an IN_PROGRESS marker is persisted
  // BEFORE the first namespace is wiped. On boot, setup() detects the marker
  // and completes the wipe + format deterministically
  // (see firmware_v1.ino handlePendingFactoryReset()).
  //
  // [audit p.427] The wipe itself now lives in Services/FactoryReset.cpp —
  // the SHARED, single-source sweep used by REST confirm, MQTT confirm AND
  // the boot-time completion. The three paths previously kept three copies
  // of the namespace list and had diverged (MQTT wiped only 9 of 13,
  // leaving relay lockouts / emergency counters / auth tokens alive).
  Services::executeFactoryResetWipe();
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
