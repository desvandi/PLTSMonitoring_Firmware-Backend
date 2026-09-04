// =============================================================================
#include "../Drivers/Sht31Driver.h"
// Web/DiagnosticsHandlers.cpp
// =============================================================================
#include "DiagnosticsHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/HealthSupervisor.h"
#include "../Services/TelemetrySpool.h"
#include <ArduinoJson.h>
#include <esp_system.h>

namespace Web {
namespace DiagnosticsHandlers {

void handleGet() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  Services::HealthSnapshot h = Services::health.getSnapshot();
  StaticJsonDocument<4096> doc;
  doc["uptimeSeconds"] = h.uptimeSeconds;
  doc["bootCount"] = h.bootCount;
  doc["resetReason"] = Services::HealthSupervisor::resetReasonStr(h.lastResetReason);
  doc["watchdogResets"] = h.watchdogResets;
  doc["brownoutResets"] = h.brownoutResets;
  doc["freeHeap"] = h.freeHeap;
  doc["minFreeHeap"] = h.minFreeHeap;
  doc["wifiRssi"] = h.wifiRssi;
  doc["wifiReconnectCount"] = h.wifiReconnectCount;
  doc["mqttReconnectCount"] = h.mqttReconnectCount;
  doc["mqttConnected"] = h.mqttConnected;
  doc["ntpSynced"] = (h.timeQuality == Core::TimeQuality::Valid);
  doc["filesystemOk"] = h.filesystemOk;
  doc["nvsOk"] = h.nvsOk;
  doc["spoolSize"] = Services::telemetrySpool.pendingCount();
  doc["criticalSpoolSize"] = Services::telemetrySpool.criticalPendingCount();
  doc["spoolDrops"] = Services::telemetrySpool.dropCount();
  doc["spoolReplays"] = Services::telemetrySpool.replayCount();
  doc["systemState"] = Core::systemStateToStr(h.systemState);
  doc["bootLoopDetected"] = h.bootLoopDetected;
  doc["bootsInLast60s"] = h.bootsInLast60s;
  JsonObject tasks = doc.createNestedObject("taskHeartbeatAgeMs");
  // [audit-2 K-1 FIX] Previous code had a hardcoded `names[]` array with only
  // 9 entries while TaskId enum has 12 (added BmsComm, Emergency, GasEmergency
  // in v1.6/v1.7). Looping `i < TASK_COUNT` (=12) indexed names[9..11] out of
  // bounds — undefined behavior, could crash or emit garbage JSON keys.
  // Use the canonical Core::taskIdToStr() which is always in sync with the enum.
  for (uint8_t i = 0; i < Services::TASK_COUNT; i++) {
    tasks[Core::taskIdToStr(static_cast<Core::TaskId>(i))] = h.taskHeartbeatAgeMs[i];
  }
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void registerRoutes() {
  http.on("/api/diagnostics", HTTP_GET, handleGet);
}

} // namespace DiagnosticsHandlers
} // namespace Web
