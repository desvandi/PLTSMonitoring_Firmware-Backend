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
  // [audit-2 S-2 FIX] Out-of-range values now return 400 with the field
  // name and accepted range — previously silently dropped, leaving the
  // operator thinking the value was applied. Now consistent with the MQTT
  // path (MqttConfigReceiver) which already rejects with explicit errors.
  if (doc.containsKey("batteryCapacityAh")) {
    float v = doc["batteryCapacityAh"];
    if (v < 10 || v > 1000) { sendError(400, "batteryCapacityAh must be 10..1000"); return; }
    Core::cfgBatteryCapacityAh = v;
  }
  if (doc.containsKey("fullVoltage")) {
    float v = doc["fullVoltage"];
    if (v < 50 || v > 56) { sendError(400, "fullVoltage must be 50..56"); return; }
    Core::cfgFullVoltage = v;
  }
  if (doc.containsKey("lowVoltage")) {
    float v = doc["lowVoltage"];
    if (v < 40 || v > 50) { sendError(400, "lowVoltage must be 40..50"); return; }
    // Cross-field validation: lowVoltage must be < fullVoltage
    if (v >= Core::cfgFullVoltage) {
      sendError(400, "lowVoltage must be < fullVoltage"); return;
    }
    Core::cfgLowVoltage = v;
  }
  if (doc.containsKey("idleCurrentThreshold")) {
    float v = doc["idleCurrentThreshold"];
    if (v < 0.1f || v > 5.0f) { sendError(400, "idleCurrentThreshold must be 0.1..5.0"); return; }
    Core::cfgIdleCurrentThreshold = v;
  }
  if (doc.containsKey("fullChargeCurrentThreshold")) {
    float v = doc["fullChargeCurrentThreshold"];
    if (v < 0.5f || v > 10.0f) { sendError(400, "fullChargeCurrentThreshold must be 0.5..10.0"); return; }
    Core::cfgFullChargeCurrentThreshold = v;
  }
  if (doc.containsKey("fullChargePersistenceSec")) {
    uint32_t v = doc["fullChargePersistenceSec"];
    if (v < 60 || v > 7200) { sendError(400, "fullChargePersistenceSec must be 60..7200"); return; }
    Core::cfgFullChargePersistenceSec = v;
  }
  if (doc.containsKey("telemetryIntervalSec")) {
    uint16_t v = doc["telemetryIntervalSec"];
    if (v < 1 || v > 60) { sendError(400, "telemetryIntervalSec must be 1..60"); return; }
    Core::cfgTelemetryIntervalSec = v;
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
  // [PARITY-4] two-tier alarm thresholds — validated + persisted + applied
  // live by AnomalyDetector on the next tick. Cross-field rule: within each
  // direction the warn tier must sit on the safe side of the critical tier
  // (low: warn > critical; high: warn < critical), mirroring the range +
  // tier-order sanitize in ConfigStore::loadAlarmConfig.
  {
    bool alarmChanged = false;
    if (doc.containsKey("voltageLowWarn")) {
      float v = doc["voltageLowWarn"];
      if (v < 40 || v > 50) { sendError(400, "voltageLowWarn must be 40..50"); return; }
      Core::cfgAlarmVoltageLowWarnV = v; alarmChanged = true;
    }
    if (doc.containsKey("voltageLowCritical")) {
      float v = doc["voltageLowCritical"];
      if (v < 40 || v > 50) { sendError(400, "voltageLowCritical must be 40..50"); return; }
      Core::cfgAlarmVoltageLowCriticalV = v; alarmChanged = true;
    }
    if (doc.containsKey("voltageHighWarn")) {
      float v = doc["voltageHighWarn"];
      if (v < 50 || v > 60) { sendError(400, "voltageHighWarn must be 50..60"); return; }
      Core::cfgAlarmVoltageHighWarnV = v; alarmChanged = true;
    }
    if (doc.containsKey("voltageHighCritical")) {
      float v = doc["voltageHighCritical"];
      if (v < 50 || v > 60) { sendError(400, "voltageHighCritical must be 50..60"); return; }
      Core::cfgAlarmVoltageHighCriticalV = v; alarmChanged = true;
    }
    if (doc.containsKey("currentHighWarn")) {
      float v = doc["currentHighWarn"];
      if (v < 10 || v > 150) { sendError(400, "currentHighWarn must be 10..150"); return; }
      Core::cfgAlarmCurrentHighWarnA = v; alarmChanged = true;
    }
    if (doc.containsKey("currentHighCritical")) {
      float v = doc["currentHighCritical"];
      if (v < 10 || v > 160) { sendError(400, "currentHighCritical must be 10..160"); return; }
      Core::cfgAlarmCurrentHighCriticalA = v; alarmChanged = true;
    }
    if (doc.containsKey("temperatureHighWarn")) {
      float v = doc["temperatureHighWarn"];
      if (v < -20 || v > 80) { sendError(400, "temperatureHighWarn must be -20..80"); return; }
      Core::cfgAlarmTemperatureHighWarnC = v; alarmChanged = true;
    }
    if (doc.containsKey("temperatureHighCritical")) {
      float v = doc["temperatureHighCritical"];
      if (v < -20 || v > 90) { sendError(400, "temperatureHighCritical must be -20..90"); return; }
      Core::cfgAlarmTemperatureHighCriticalC = v; alarmChanged = true;
    }
    if (doc.containsKey("humidityHighWarn")) {
      float v = doc["humidityHighWarn"];
      if (v < 50 || v > 100) { sendError(400, "humidityHighWarn must be 50..100"); return; }
      Core::cfgAlarmHumidityHighWarnPct = v; alarmChanged = true;
    }
    if (doc.containsKey("socLowWarn")) {
      float v = doc["socLowWarn"];
      if (v < 5 || v > 50) { sendError(400, "socLowWarn must be 5..50"); return; }
      Core::cfgAlarmSocLowWarnPct = v; alarmChanged = true;
    }
    if (doc.containsKey("socLowCritical")) {
      float v = doc["socLowCritical"];
      if (v < 2 || v > 50) { sendError(400, "socLowCritical must be 2..50"); return; }
      Core::cfgAlarmSocLowCriticalPct = v; alarmChanged = true;
    }
    // Tier order (only enforced when BOTH tiers were provided or both are
    // in RAM after the partial update — cheap and unambiguous).
    if (Core::cfgAlarmVoltageLowCriticalV >= Core::cfgAlarmVoltageLowWarnV) {
      sendError(400, "voltageLowCritical must be < voltageLowWarn"); return;
    }
    if (Core::cfgAlarmVoltageHighCriticalV <= Core::cfgAlarmVoltageHighWarnV) {
      sendError(400, "voltageHighCritical must be > voltageHighWarn"); return;
    }
    if (Core::cfgAlarmCurrentHighCriticalA <= Core::cfgAlarmCurrentHighWarnA) {
      sendError(400, "currentHighCritical must be > currentHighWarn"); return;
    }
    if (Core::cfgAlarmTemperatureHighCriticalC <= Core::cfgAlarmTemperatureHighWarnC) {
      sendError(400, "temperatureHighCritical must be > temperatureHighWarn"); return;
    }
    if (Core::cfgAlarmSocLowCriticalPct >= Core::cfgAlarmSocLowWarnPct) {
      sendError(400, "socLowCritical must be < socLowWarn"); return;
    }
    if (alarmChanged) Storage::config.saveAlarmConfig();
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
