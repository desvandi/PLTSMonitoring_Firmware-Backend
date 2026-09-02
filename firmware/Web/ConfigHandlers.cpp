// =============================================================================
#include "../Drivers/Sht31Driver.h"
// Web/ConfigHandlers.cpp
// =============================================================================
#include "ConfigHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Storage/ConfigStore.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include "../Comm/BatteryCommManager.h"
#include "../Comm/BatteryProtocol.h"
#include <ArduinoJson.h>

namespace Web {
namespace ConfigHandlers {

void handleGetConfig() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  StaticJsonDocument<2048> doc;
  doc["configVersion"] = Core::calibration.version;
  doc["batteryCapacityAh"] = Core::cfgBatteryCapacityAh;
  doc["batteryNominalV"] = Core::cfgBatteryNominalVoltage;
  doc["fullVoltage"] = Core::cfgFullVoltage;
  doc["lowVoltage"] = Core::cfgLowVoltage;
  doc["idleCurrentThreshold"] = Core::cfgIdleCurrentThreshold;
  doc["fullChargeCurrentThreshold"] = Core::cfgFullChargeCurrentThreshold;
  doc["fullChargePersistenceSec"] = Core::cfgFullChargePersistenceSec;
  doc["telemetryIntervalSec"] = Core::cfgTelemetryIntervalSec;
  // v1.6.0 — BMS/inverter comm configuration
  doc["bmsProtocol"] = Core::cfgBmsProtocol;
  doc["bmsPollIntervalMs"] = Core::cfgBmsPollIntervalMs;
  doc["bmsModbusSlaveId"] = Core::cfgBmsModbusSlaveId;
  doc["bmsModbusTcpHost"] = Core::cfgBmsModbusTcpHost;
  doc["bmsModbusTcpPort"] = Core::cfgBmsModbusTcpPort;
  doc["deviceName"] = Core::deviceName;
  doc["timezone"] = Core::cfgTimezone;
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void handlePostConfig() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(Core::HTTP_MAX_BODY_SIZE)) return;
  String raw = http.arg("plain");
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  // Tag with canonical (type, action)
  doc["type"] = "config";
  doc["action"] = "update";
  // [P2-1 REMEDIATION 2026-09] Freshness gate BEFORE the journal decides —
  // REST must not be the weak sibling of the MQTT path (same contract as
  // MqttConfigReceiver: an expired command can neither be applied nor
  // safely deduplicated once its ring slot is gone).
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr)) {
      sendError(400, expiryErr);
      return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command");
    return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }
  // Apply config (validated fields only)
  if (doc.containsKey("batteryCapacityAh")) {
    float v = doc["batteryCapacityAh"];
    if (v >= 10 && v <= 1000) Core::cfgBatteryCapacityAh = v;
  }
  if (doc.containsKey("fullVoltage")) {
    float v = doc["fullVoltage"];
    if (v >= 50 && v <= 56) Core::cfgFullVoltage = v;
  }
  if (doc.containsKey("lowVoltage")) {
    float v = doc["lowVoltage"];
    if (v >= 40 && v <= 50) Core::cfgLowVoltage = v;
  }
  if (doc.containsKey("idleCurrentThreshold")) {
    float v = doc["idleCurrentThreshold"];
    if (v >= 0.1f && v <= 5.0f) Core::cfgIdleCurrentThreshold = v;
  }
  if (doc.containsKey("fullChargeCurrentThreshold")) {
    float v = doc["fullChargeCurrentThreshold"];
    if (v >= 0.5f && v <= 10.0f) Core::cfgFullChargeCurrentThreshold = v;
  }
  if (doc.containsKey("fullChargePersistenceSec")) {
    uint32_t v = doc["fullChargePersistenceSec"];
    if (v >= 60 && v <= 7200) Core::cfgFullChargePersistenceSec = v;
  }
  if (doc.containsKey("telemetryIntervalSec")) {
    uint16_t v = doc["telemetryIntervalSec"];
    if (v >= 1 && v <= 60) Core::cfgTelemetryIntervalSec = v;
  }
  // v1.6.0 — BMS/inverter comm fields (validated; unknown protocol rejected)
  bool bmsConfigChanged = false;
  if (doc.containsKey("bmsProtocol")) {
    const char* p = doc["bmsProtocol"];
    bool valid = p && (strcmp(p, "auto") == 0 || strcmp(p, "none") == 0 ||
                       strcmp(p, "pylontech_can") == 0 || strcmp(p, "modbus_rtu") == 0 ||
                       strcmp(p, "modbus_tcp") == 0);
    if (!valid) { sendError(400, "bmsProtocol must be auto|none|pylontech_can|modbus_rtu|modbus_tcp"); return; }
    strncpy(Core::cfgBmsProtocol, p, sizeof(Core::cfgBmsProtocol) - 1);
    Core::cfgBmsProtocol[sizeof(Core::cfgBmsProtocol) - 1] = '\0';
    bmsConfigChanged = true;
  }
  if (doc.containsKey("bmsPollIntervalMs")) {
    uint32_t v = doc["bmsPollIntervalMs"];
    if (v >= 1000 && v <= 600000) { Core::cfgBmsPollIntervalMs = v; bmsConfigChanged = true; }
    else { sendError(400, "bmsPollIntervalMs must be 1000..600000"); return; }
  }
  if (doc.containsKey("bmsModbusSlaveId")) {
    uint8_t v = doc["bmsModbusSlaveId"];
    if (v >= 1 && v <= 247) { Core::cfgBmsModbusSlaveId = v; bmsConfigChanged = true; }
    else { sendError(400, "bmsModbusSlaveId must be 1..247"); return; }
  }
  if (doc.containsKey("bmsModbusTcpHost")) {
    const char* h = doc["bmsModbusTcpHost"];
    if (h && strlen(h) < sizeof(Core::cfgBmsModbusTcpHost)) {
      strncpy(Core::cfgBmsModbusTcpHost, h, sizeof(Core::cfgBmsModbusTcpHost) - 1);
      Core::cfgBmsModbusTcpHost[sizeof(Core::cfgBmsModbusTcpHost) - 1] = '\0';
      bmsConfigChanged = true;
    } else { sendError(400, "bmsModbusTcpHost too long"); return; }
  }
  if (doc.containsKey("bmsModbusTcpPort")) {
    uint16_t v = doc["bmsModbusTcpPort"];
    if (v >= 1 && v <= 65535) { Core::cfgBmsModbusTcpPort = v; bmsConfigChanged = true; }
    else { sendError(400, "bmsModbusTcpPort must be 1..65535"); return; }
  }
  if (doc.containsKey("deviceName")) {
    const char* n = doc["deviceName"];
    if (n) { strncpy(Core::deviceName, n, 39); Core::deviceName[39] = '\0'; }
  }
  if (doc.containsKey("timezone")) {
    const char* t = doc["timezone"];
    if (t) { strncpy(Core::cfgTimezone, t, 39); Core::cfgTimezone[39] = '\0'; }
  }
  Storage::config.saveBatteryConfig();
  Storage::config.saveDeviceConfig();

  // Hot-apply BMS comm changes without reboot (bounded: rebuilds clients).
  if (bmsConfigChanged) {
    Comm::batteryComm.reconfigure();
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          String("BMS comm reconfigured proto=") + Core::cfgBmsProtocol);
  }

  String ack = "{\"success\":true,\"message\":\"Config updated\"}";
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);
  sendSuccess("Config updated", "{}");
}

void registerRoutes() {
  http.on("/api/config", HTTP_GET, handleGetConfig);
  http.on("/api/config", HTTP_POST, handlePostConfig);
}

} // namespace ConfigHandlers
} // namespace Web
