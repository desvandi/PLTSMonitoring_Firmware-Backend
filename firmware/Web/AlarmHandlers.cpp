// =============================================================================
// Web/AlarmHandlers.cpp
// =============================================================================
#include "AlarmHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/AlarmRegistry.h"
#include <ArduinoJson.h>

namespace Web {
namespace AlarmHandlers {

void handleGetAlarms() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  // v1.6.1 — canonical contract compliance (02_CANONICAL_API_CONTRACT.md):
  // GET /api/alarms → {active, history}. Bentuk lama {alarms:[...]} tidak
  // pernah cocok dengan kontrak maupun konsumen PWA (Alarm Center crash
  // `.map is not a function` saat dilayani langsung oleh firmware).
  StaticJsonDocument<4096> doc;
  JsonArray active = doc.createNestedArray("active");
  JsonArray history = doc.createNestedArray("history");
  for (uint8_t i = 0; i < Services::alarms.countAll(); i++) {
    const Services::Alarm* a = Services::alarms.getAlarm(i);
    if (!a) continue;
    JsonArray& dst = (a->lifecycle == Core::AlarmLifecycle::Active) ? active : history;
    JsonObject o = dst.createNestedObject();
    o["code"] = a->code;
    o["severity"] = Core::severityToStr(a->severity);
    o["lifecycle"] = Core::lifecycleToStr(a->lifecycle);
    o["raisedAt"] = a->raisedAt;
    o["acknowledgedAt"] = a->acknowledgedAt;
    o["clearedAt"] = a->clearedAt;
    o["message"] = a->message;
  }
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void handleAcknowledge() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  // Extract code from path: /api/alarms/{code}/acknowledge
  String uri = http.uri();
  int s1 = uri.indexOf("/alarms/");
  if (s1 < 0) { sendError(400, "Missing alarm code"); return; }
  s1 += 8;
  int s2 = uri.indexOf("/acknowledge", s1);
  if (s2 < 0) { sendError(400, "Bad path"); return; }
  String code = uri.substring(s1, s2);
  Services::alarms.acknowledge(code.c_str());
  sendSuccess("Alarm acknowledged", "{}");
}

void registerRoutes() {
  http.on("/api/alarms", HTTP_GET, handleGetAlarms);
  // Catch-all for /api/alarms/{code}/acknowledge
  http.on("/api/alarms/acknowledge-all", HTTP_POST, []() {
    if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
    if (!requireCsrf()) return;
    Services::alarms.acknowledgeAll();
    sendSuccess("All alarms acknowledged", "{}");
  });
}

} // namespace AlarmHandlers
} // namespace Web
