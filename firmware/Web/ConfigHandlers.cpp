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
#include "../Services/ConfigUpdater.h"
#include "../Services/LogService.h"
#include "../Comm/BatteryCommManager.h"
#include "../Comm/BatteryProtocol.h"
#include <ArduinoJson.h>

namespace Web {
namespace ConfigHandlers {

void handleGetConfig() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  // [PARITY-4] 2048 → 3072: the alarm-threshold readback (flat + nested)
  // roughly doubles the serialized size.
  StaticJsonDocument<3072> doc;
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
  // [PARITY-4] operator alarm thresholds — flat keys (mutation surface, same
  // names as the canonicalizer whitelist) AND a nested `alarmThresholds`
  // object (read surface, the exact PWA Configuration Center schema that
  // previously rendered nothing because no firmware ever served it).
  doc["voltageLowWarn"] = Core::cfgAlarmVoltageLowWarnV;
  doc["voltageLowCritical"] = Core::cfgAlarmVoltageLowCriticalV;
  doc["voltageHighWarn"] = Core::cfgAlarmVoltageHighWarnV;
  doc["voltageHighCritical"] = Core::cfgAlarmVoltageHighCriticalV;
  doc["currentHighWarn"] = Core::cfgAlarmCurrentHighWarnA;
  doc["currentHighCritical"] = Core::cfgAlarmCurrentHighCriticalA;
  doc["temperatureHighWarn"] = Core::cfgAlarmTemperatureHighWarnC;
  doc["temperatureHighCritical"] = Core::cfgAlarmTemperatureHighCriticalC;
  doc["humidityHighWarn"] = Core::cfgAlarmHumidityHighWarnPct;
  doc["socLowWarn"] = Core::cfgAlarmSocLowWarnPct;
  doc["socLowCritical"] = Core::cfgAlarmSocLowCriticalPct;
  JsonObject alarm = doc.createNestedObject("alarmThresholds");
  alarm["voltageLowWarn"] = Core::cfgAlarmVoltageLowWarnV;
  alarm["voltageLowCritical"] = Core::cfgAlarmVoltageLowCriticalV;
  alarm["voltageHighWarn"] = Core::cfgAlarmVoltageHighWarnV;
  alarm["voltageHighCritical"] = Core::cfgAlarmVoltageHighCriticalV;
  alarm["currentHighWarn"] = Core::cfgAlarmCurrentHighWarnA;
  alarm["currentHighCritical"] = Core::cfgAlarmCurrentHighCriticalA;
  alarm["temperatureHighWarn"] = Core::cfgAlarmTemperatureHighWarnC;
  alarm["temperatureHighCritical"] = Core::cfgAlarmTemperatureHighCriticalC;
  alarm["humidityHighWarn"] = Core::cfgAlarmHumidityHighWarnPct;
  alarm["socLowWarn"] = Core::cfgAlarmSocLowWarnPct;
  alarm["socLowCritical"] = Core::cfgAlarmSocLowCriticalPct;
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
  // [CORE-02] Envelope gate — version/transactionId/issuedAt/expiresAt are
  // mandatory for mutations (freshness claim always present; replay
  // protection never degrades to journal-retention-only).
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(doc, envErr)) {
      sendError(400, envErr);
      return;
    }
  }
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

  // [PRODUCTION-GRADE 2026-09 / audit p.274-279, BLOCKER B] Two-phase
  // mutation via the SHARED ConfigUpdater (same service the MQTT path
  // uses): every present field is validated into a candidate FIRST; RAM is
  // touched only when the whole candidate (including cross-field tier
  // ordering) is valid. A rejected request can no longer leave earlier
  // fields mutated in RAM.
  Services::ConfigUpdater::Result r = Services::ConfigUpdater::applyUpdate(doc);
  if (!r.ok) {
    sendError(400, r.message);
    return;
  }

  // Hot-apply BMS comm changes without reboot (bounded: rebuilds clients).
  if (r.bmsConfigChanged) {
    Comm::batteryComm.reconfigure();
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          String("BMS comm reconfigured proto=") + Core::cfgBmsProtocol);
  }

  // [BLOCKER E / audit p.281-282] Journal durability failure must not
  // produce a plain success ACK.
  String ack = "{\"success\":true,\"message\":\"" + r.message + "\"}";
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack)) {
    sendError(503, "Config applied but transaction journal persistence failed — durability degraded");
    return;
  }

  if (r.message.indexOf("WARNING") >= 0) {
    // Applied in RAM, NVS save failed — honest degraded-ACK (not a lie, not a silent success).
    sendError(500, r.message);
    return;
  }
  sendSuccess("Config updated", "{}");
}

void registerRoutes() {
  http.on("/api/config", HTTP_GET, handleGetConfig);
  http.on("/api/config", HTTP_POST, handlePostConfig);
}

} // namespace ConfigHandlers
} // namespace Web
