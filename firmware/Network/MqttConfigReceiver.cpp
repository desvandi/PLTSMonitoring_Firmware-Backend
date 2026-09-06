// =============================================================================
// Network/MqttConfigReceiver.cpp — MQTT config/calibration command receiver
// =============================================================================
#include "MqttConfigReceiver.h"
#include "MqttTransport.h"
#include "MqttTelemetryPublisher.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Storage/ConfigStore.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/SocStateMachine.h"
#include "../Services/EnergyCounters.h"
#include "../Services/AuthManager.h"
#include "../Drivers/Acs712Driver.h"
#include "../Drivers/Sht31Driver.h"   // [v1.6.3] live-apply calibration offsets
#include "../Comm/BatteryCommManager.h" // [PARITY-4] BMS comm live-reconfigure (REST parity)
#if PLTS_ENABLE_RELAYS
#include "../Services/RelayController.h"   // [v1.8.0] relay command dispatch
#endif
#include <ArduinoJson.h>
#include <Preferences.h>
#include <cstring>
#include <cmath>

namespace Network {

MqttConfigReceiver mqttConfigReceiver;

// ---------------------------------------------------------------------------
// Schema for ACK body (brief §51):
//   { "transactionId": str,
//     "ok": bool,
//     "code": "ACCEPTED" | "DUPLICATE" | "CONFLICT" | "REJECTED" |
//             "BAD_SCHEMA" | "UNAUTHORIZED",
//     "message": str,
//     "source": "mqtt",
//     "appliedAt": unixSec }
// ---------------------------------------------------------------------------

void MqttConfigReceiver::begin() {
  _received = 0;
  _accepted = 0;
  _rejected = 0;
  _duplicates = 0;
  Services::Log.append(Core::LogType::ConfigurationChanged,
                        "MqttConfigReceiver initialized", -1);
}

void MqttConfigReceiver::handle(const char* topic, const uint8_t* payload, size_t len) {
  (void)topic;  // topic is always plts/<deviceId>/config — already ACL'd by broker
  _received++;

  // --- 1. Deserialize + schema-validate envelope --------------------------------
  // Body must be valid JSON ≤ HTTP_MAX_BODY_SIZE (defense-in-depth even on MQTT).
  if (len == 0 || len > Core::HTTP_MAX_BODY_SIZE) {
    _publishAck("", false, "BAD_SCHEMA", "empty or oversized payload");
    _rejected++;
    return;
  }

  JsonDocument doc;
  {
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err != DeserializationError::Ok) {
      _publishAck("", false, "BAD_SCHEMA",
                  String("JSON parse error: ") + err.c_str());
      _rejected++;
      return;
    }
  }

  // Required envelope fields (canonical contract §3.3):
  //   type, action, transactionId, version, issuedAt, expiresAt
  // (requestId is OPTIONAL on MQTT — transactionId is the dedup key.)
  const char* typeC = doc["type"] | "";
  const char* actionC = doc["action"] | "";
  const char* tidC = doc["transactionId"] | "";
  int protocolVer = doc["version"] | 0;

  String type(typeC);
  String action(actionC);
  String tid(tidC);
  type.toLowerCase();
  action.toLowerCase();

  if (type.length() == 0 || action.length() == 0) {
    _publishAck(tid.c_str(), false, "BAD_SCHEMA", "missing type or action");
    _rejected++;
    return;
  }

  // --- 2. Validate transactionId format (PD-001) -------------------------------
  {
    String errOut;
    if (!Services::CommandCanonicalizer::validateTransactionId(tid, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
  }

  // --- 3. Validate protocol version ---------------------------------------------
  {
    String errOut;
    if (!Services::CommandCanonicalizer::validateProtocolVersion(protocolVer, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
  }

  // --- 4. Expiry check (defense-in-depth) --------------------------------------
  // [P2-1 REMEDIATION 2026-09] Now routed through the SHARED gate so every
  // ingress (REST + MQTT) enforces identical freshness semantics — see
  // CommandCanonicalizer::isCommandExpired + TransactionJournal.h for the
  // full retention contract.
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr)) {
      _publishAck(tid.c_str(), false, "REJECTED", expiryErr);
      _rejected++;
      return;
    }
  }

  // --- 5. Whitelist (type, action) — fail-closed -------------------------------
  if (!Services::CommandCanonicalizer::isKnownCommandType(type, action)) {
    _publishAck(tid.c_str(), false, "REJECTED",
                "unknown (type, action): " + type + "." + action);
    _rejected++;
    return;
  }

  // --- 6. Field whitelist — reject unknown fields BEFORE hashing --------------
  // This prevents injection of fields the receiver doesn't understand
  // (canonical contract §3.3 — fail-closed schema).
  {
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
      if (!Services::CommandCanonicalizer::isFieldAllowed(type, kv.key().c_str())) {
        _publishAck(tid.c_str(), false, "REJECTED",
                    "unknown field: " + String(kv.key().c_str()));
        _rejected++;
        return;
      }
    }
  }

  // --- 7. Canonicalize + hash (shared with REST — PD-001) -----------------------
  // Tag with (type, action) in case REST sender omitted them.
  doc["type"] = type;
  doc["action"] = action;
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) {
    _publishAck(tid.c_str(), false, "BAD_SCHEMA", canon.errorMessage);
    _rejected++;
    return;
  }

  // --- 8. Decide NEW / DUPLICATE / CONFLICT via TransactionJournal -------------
  String previousAck;
  Services::TransactionDecision decision =
    Services::journal.decide(canon.transactionId, canon.commandHash, previousAck);

  if (decision == Services::TransactionDecision::Duplicate) {
    // Idempotent replay — re-publish the previous ACK verbatim.
    // This is the contract guarantee: clients may retry safely.
    _duplicates++;
    if (previousAck.length() > 0) {
      String ackTopic = mqttTransport.getDeviceTopic("ack");
      mqttTransport.publish(ackTopic.c_str(), previousAck.c_str(),
                             previousAck.length(), false);
    } else {
      // No stored ACK — synthesize a generic DUPLICATE ack.
      _publishAck(canon.transactionId.c_str(), true, "DUPLICATE",
                  "transaction already processed (ACK not in journal)");
    }
    return;
  }

  if (decision == Services::TransactionDecision::Conflict) {
    // Same transactionId but different hash — reject (canonical contract §3.3).
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "CONFLICT",
                "transactionId reused with different payload");
    return;
  }

  // --- 9. NEW — apply the command via canonical path ----------------------------
  ApplyResult r = _applyCommand(type, action, doc);

  // --- 10. Store transaction + ACK in journal (2-phase commit) -----------------
  // ACK body — same schema regardless of ok=true/false (per §51)
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = canon.transactionId;
    ack["ok"] = r.ok;
    ack["code"] = r.code;
    ack["message"] = r.message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }

  // Store in journal BEFORE publishing ACK (so a crash between store + publish
  // is recoverable — client retries, journal says DUPLICATE, replays ACK).
  Services::journal.storeTransaction(canon.transactionId,
                                      canon.commandHash, ackJson);

  // Publish ACK
  {
    String ackTopic = mqttTransport.getDeviceTopic("ack");
    mqttTransport.publish(ackTopic.c_str(), ackJson.c_str(),
                           ackJson.length(), false);
  }

  if (r.ok) {
    _accepted++;
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT command accepted: " + type + "." + action +
                          " tid=" + canon.transactionId, -1);
  } else {
    _rejected++;
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT command rejected: " + type + "." + action +
                          " tid=" + canon.transactionId + " code=" + r.code, -1);
  }
}

// ---------------------------------------------------------------------------
// _applyCommand — dispatch to the same ConfigStore / AlarmRegistry APIs used
// by the REST handlers. NEVER bypass canonicalization (already done above)
// or auth (broker ACL already enforced).
//
// IMPORTANT: this method MUST NOT re-validate the canonical hash or expiry —
// that has already been done. It only performs the type-specific mutation.
// ---------------------------------------------------------------------------
MqttConfigReceiver::ApplyResult
MqttConfigReceiver::_applyCommand(const String& type, const String& action,
                                    JsonDocument& doc) {
  // ---------- config.update ----------
  // [FW-22 REMEDIATION 2026-08] Range validation now IDENTICAL to the REST
  // path (Web::ConfigHandlers). Previously MQTT config.update accepted ANY
  // value (negative capacity, 0 V thresholds) and persisted it — a divergent
  // validation surface violating the single-command-model requirement.
  if (type == "config" && action == "update") {
    if (doc.containsKey("batteryCapacityAh")) {
      float v = doc["batteryCapacityAh"];
      if (v >= 10 && v <= 1000) Core::cfgBatteryCapacityAh = v;
      else return { false, "REJECTED", "batteryCapacityAh out of range [10,1000]" };
    }
    if (doc.containsKey("batteryNominalV")) {
      float v = doc["batteryNominalV"];
      if (v >= 24 && v <= 48) Core::cfgBatteryNominalVoltage = v;
      else return { false, "REJECTED", "batteryNominalV out of range [24,48]" };
    }
    if (doc.containsKey("fullVoltage")) {
      float v = doc["fullVoltage"];
      if (v >= 50 && v <= 56) Core::cfgFullVoltage = v;
      else return { false, "REJECTED", "fullVoltage out of range [50,56]" };
    }
    if (doc.containsKey("lowVoltage")) {
      float v = doc["lowVoltage"];
      if (v >= 40 && v <= 50) Core::cfgLowVoltage = v;
      else return { false, "REJECTED", "lowVoltage out of range [40,50]" };
    }
    if (doc.containsKey("idleCurrentThreshold")) {
      float v = doc["idleCurrentThreshold"];
      if (v >= 0.1f && v <= 5.0f) Core::cfgIdleCurrentThreshold = v;
      else return { false, "REJECTED", "idleCurrentThreshold out of range [0.1,5]" };
    }
    if (doc.containsKey("fullChargeCurrentThreshold")) {
      float v = doc["fullChargeCurrentThreshold"];
      if (v >= 0.5f && v <= 10.0f) Core::cfgFullChargeCurrentThreshold = v;
      else return { false, "REJECTED", "fullChargeCurrentThreshold out of range [0.5,10]" };
    }
    if (doc.containsKey("fullChargePersistenceSec")) {
      uint32_t v = doc["fullChargePersistenceSec"];
      if (v >= 60 && v <= 7200) Core::cfgFullChargePersistenceSec = v;
      else return { false, "REJECTED", "fullChargePersistenceSec out of range [60,7200]" };
    }
    if (doc.containsKey("telemetryIntervalSec")) {
      uint16_t v = doc["telemetryIntervalSec"];
      if (v >= 1 && v <= 60) Core::cfgTelemetryIntervalSec = v;
      else return { false, "REJECTED", "telemetryIntervalSec out of range [1,60]" };
    }
    if (doc.containsKey("deviceName")) {
      const char* dn = doc["deviceName"];
      if (dn) strncpy(Core::deviceName, dn, sizeof(Core::deviceName) - 1);
      Core::deviceName[sizeof(Core::deviceName) - 1] = '\0';
    }
    if (doc.containsKey("timezone")) {
      const char* tz = doc["timezone"];
      if (tz) strncpy(Core::cfgTimezone, tz, sizeof(Core::cfgTimezone) - 1);
      Core::cfgTimezone[sizeof(Core::cfgTimezone) - 1] = '\0';
    }
    // [PARITY-4] BMS comm fields — previously the canonicalizer whitelist
    // rejected them ("unknown field"), so MQTT could never configure BMS
    // comm even though the REST path did. Ranges mirror Web::ConfigHandlers.
    bool bmsConfigChanged = false;
    if (doc.containsKey("bmsProtocol")) {
      const char* p = doc["bmsProtocol"];
      bool valid = p && (strcmp(p, "auto") == 0 || strcmp(p, "none") == 0 ||
                         strcmp(p, "pylontech_can") == 0 || strcmp(p, "modbus_rtu") == 0 ||
                         strcmp(p, "modbus_tcp") == 0);
      if (!valid) return { false, "REJECTED", "bmsProtocol must be auto|none|pylontech_can|modbus_rtu|modbus_tcp" };
      strncpy(Core::cfgBmsProtocol, p, sizeof(Core::cfgBmsProtocol) - 1);
      Core::cfgBmsProtocol[sizeof(Core::cfgBmsProtocol) - 1] = '\0';
      bmsConfigChanged = true;
    }
    if (doc.containsKey("bmsPollIntervalMs")) {
      uint32_t v = doc["bmsPollIntervalMs"];
      if (v < 1000 || v > 600000) return { false, "REJECTED", "bmsPollIntervalMs out of range [1000,600000]" };
      Core::cfgBmsPollIntervalMs = v; bmsConfigChanged = true;
    }
    if (doc.containsKey("bmsModbusSlaveId")) {
      uint8_t v = doc["bmsModbusSlaveId"];
      if (v < 1 || v > 247) return { false, "REJECTED", "bmsModbusSlaveId out of range [1,247]" };
      Core::cfgBmsModbusSlaveId = v; bmsConfigChanged = true;
    }
    if (doc.containsKey("bmsModbusTcpHost")) {
      const char* h = doc["bmsModbusTcpHost"];
      if (!h || strlen(h) >= sizeof(Core::cfgBmsModbusTcpHost)) return { false, "REJECTED", "bmsModbusTcpHost too long" };
      strncpy(Core::cfgBmsModbusTcpHost, h, sizeof(Core::cfgBmsModbusTcpHost) - 1);
      Core::cfgBmsModbusTcpHost[sizeof(Core::cfgBmsModbusTcpHost) - 1] = '\0';
      bmsConfigChanged = true;
    }
    if (doc.containsKey("bmsModbusTcpPort")) {
      uint16_t v = doc["bmsModbusTcpPort"];
      if (v < 1 || v > 65535) return { false, "REJECTED", "bmsModbusTcpPort out of range [1,65535]" };
      Core::cfgBmsModbusTcpPort = v; bmsConfigChanged = true;
    }
    // [PARITY-4] two-tier alarm thresholds — validation IDENTICAL to the REST
    // path (Web::ConfigHandlers): ranges, then tier order on the post-update
    // RAM state. Applied live: AnomalyDetector reads cfgAlarm* every tick.
    {
      bool alarmChanged = false;
      if (doc.containsKey("voltageLowWarn")) {
        float v = doc["voltageLowWarn"];
        if (!(v >= 40 && v <= 50)) return { false, "REJECTED", "voltageLowWarn out of range [40,50]" };
        Core::cfgAlarmVoltageLowWarnV = v; alarmChanged = true;
      }
      if (doc.containsKey("voltageLowCritical")) {
        float v = doc["voltageLowCritical"];
        if (!(v >= 40 && v <= 50)) return { false, "REJECTED", "voltageLowCritical out of range [40,50]" };
        Core::cfgAlarmVoltageLowCriticalV = v; alarmChanged = true;
      }
      if (doc.containsKey("voltageHighWarn")) {
        float v = doc["voltageHighWarn"];
        if (!(v >= 50 && v <= 60)) return { false, "REJECTED", "voltageHighWarn out of range [50,60]" };
        Core::cfgAlarmVoltageHighWarnV = v; alarmChanged = true;
      }
      if (doc.containsKey("voltageHighCritical")) {
        float v = doc["voltageHighCritical"];
        if (!(v >= 50 && v <= 60)) return { false, "REJECTED", "voltageHighCritical out of range [50,60]" };
        Core::cfgAlarmVoltageHighCriticalV = v; alarmChanged = true;
      }
      if (doc.containsKey("currentHighWarn")) {
        float v = doc["currentHighWarn"];
        if (!(v >= 10 && v <= 150)) return { false, "REJECTED", "currentHighWarn out of range [10,150]" };
        Core::cfgAlarmCurrentHighWarnA = v; alarmChanged = true;
      }
      if (doc.containsKey("currentHighCritical")) {
        float v = doc["currentHighCritical"];
        if (!(v >= 10 && v <= 160)) return { false, "REJECTED", "currentHighCritical out of range [10,160]" };
        Core::cfgAlarmCurrentHighCriticalA = v; alarmChanged = true;
      }
      if (doc.containsKey("temperatureHighWarn")) {
        float v = doc["temperatureHighWarn"];
        if (!(v >= -20 && v <= 80)) return { false, "REJECTED", "temperatureHighWarn out of range [-20,80]" };
        Core::cfgAlarmTemperatureHighWarnC = v; alarmChanged = true;
      }
      if (doc.containsKey("temperatureHighCritical")) {
        float v = doc["temperatureHighCritical"];
        if (!(v >= -20 && v <= 90)) return { false, "REJECTED", "temperatureHighCritical out of range [-20,90]" };
        Core::cfgAlarmTemperatureHighCriticalC = v; alarmChanged = true;
      }
      if (doc.containsKey("humidityHighWarn")) {
        float v = doc["humidityHighWarn"];
        if (!(v >= 50 && v <= 100)) return { false, "REJECTED", "humidityHighWarn out of range [50,100]" };
        Core::cfgAlarmHumidityHighWarnPct = v; alarmChanged = true;
      }
      if (doc.containsKey("socLowWarn")) {
        float v = doc["socLowWarn"];
        if (!(v >= 5 && v <= 50)) return { false, "REJECTED", "socLowWarn out of range [5,50]" };
        Core::cfgAlarmSocLowWarnPct = v; alarmChanged = true;
      }
      if (doc.containsKey("socLowCritical")) {
        float v = doc["socLowCritical"];
        if (!(v >= 2 && v <= 50)) return { false, "REJECTED", "socLowCritical out of range [2,50]" };
        Core::cfgAlarmSocLowCriticalPct = v; alarmChanged = true;
      }
      if (Core::cfgAlarmVoltageLowCriticalV >= Core::cfgAlarmVoltageLowWarnV)
        return { false, "REJECTED", "voltageLowCritical must be < voltageLowWarn" };
      if (Core::cfgAlarmVoltageHighCriticalV <= Core::cfgAlarmVoltageHighWarnV)
        return { false, "REJECTED", "voltageHighCritical must be > voltageHighWarn" };
      if (Core::cfgAlarmCurrentHighCriticalA <= Core::cfgAlarmCurrentHighWarnA)
        return { false, "REJECTED", "currentHighCritical must be > currentHighWarn" };
      if (Core::cfgAlarmTemperatureHighCriticalC <= Core::cfgAlarmTemperatureHighWarnC)
        return { false, "REJECTED", "temperatureHighCritical must be > temperatureHighWarn" };
      if (Core::cfgAlarmSocLowCriticalPct >= Core::cfgAlarmSocLowWarnPct)
        return { false, "REJECTED", "socLowCritical must be < socLowWarn" };
      if (alarmChanged) Storage::config.saveAlarmConfig();
    }
    Storage::config.saveBatteryConfig();
    if (bmsConfigChanged) {
      Comm::batteryComm.reconfigure();
      Services::Log.append(Core::LogType::ConfigurationChanged,
                            String("MQTT: BMS comm reconfigured proto=") + Core::cfgBmsProtocol, -1);
    }
    return { true, "ACCEPTED", "config updated" };
  }

  // ---------- calibration.update ----------
  // [WAVE-5 / FW-E1] Range validation — previously this path accepted ANY
  // float. acs712Sensitivity is a DIVISOR in the current computation: 0 →
  // inf/NaN, negative → silently sign-inverted current (data lies).
  // config.update got this treatment in FW-22; calibration now matches.
  if (type == "calibration" && action == "update") {
    if (doc.containsKey("version"))
      Core::calibration.version = doc["version"] | Core::calibration.version;
    if (doc.containsKey("acs712Offset")) {
      float v = doc["acs712Offset"] | NAN;
      if (!isfinite(v) || v < 0.0f || v > 3300.0f)
        return { false, "REJECTED", "acs712Offset out of range [0,3300] mV-ish ADC counts" };
      Core::calibration.acs712Offset = v;
      // [v1.6.3] Apply DIRECTLY to the driver — previously the value landed
      // in Core::calibration (persisted, applied at NEXT BOOT) but the live
      // driver kept the old value, so the operator calibrated, saw no
      // change, and concluded the sensor was broken.
      Drivers::acs712.setZeroOffset(v);
    }
    if (doc.containsKey("acs712Sensitivity")) {
      float v = doc["acs712Sensitivity"] | NAN;
      if (!isfinite(v) || v < 10.0f || v > 400.0f)
        return { false, "REJECTED", "acs712Sensitivity out of range [10,400] mV/A" };
      Core::calibration.acs712Sensitivity = v;
      Drivers::acs712.setSensitivity(v);   // [v1.6.3] live-apply (see above)
    }
    if (doc.containsKey("sht31TempOffset")) {
      float v = doc["sht31TempOffset"] | NAN;
      if (!isfinite(v) || v < -50.0f || v > 50.0f)
        return { false, "REJECTED", "sht31TempOffset out of range [-50,50] C" };
      Core::calibration.sht31TempOffset = v;
      Drivers::sht31.setTempOffset(v);     // [v1.6.3] live-apply
    }
    if (doc.containsKey("sht31HumOffset")) {
      float v = doc["sht31HumOffset"] | NAN;
      if (!isfinite(v) || v < -50.0f || v > 50.0f)
        return { false, "REJECTED", "sht31HumOffset out of range [-50,50] %RH" };
      Core::calibration.sht31HumOffset = v;
      Drivers::sht31.setHumOffset(v);      // [v1.6.3] live-apply
    }
    if (doc.containsKey("source")) {
      const char* src = doc["source"];
      if (src) strncpy(Core::calibration.source, src, 15);
      Core::calibration.source[15] = '\0';
    }
    Storage::config.saveCalibration(true);
    return { true, "ACCEPTED", "calibration updated" };
  }

  // ---------- calibration.point ----------
  if (type == "calibration" && action == "point") {
    const char* which = doc["which"] | "";
    float reference = doc["reference"] | 0.0f;
    float raw = doc["raw"] | 0.0f;
    String w(which); w.toLowerCase();
    if (w == "low" || w == "nominal" || w == "full") {
      // Defer to VoltageCalibration service for the actual point capture —
      // same API as Web::CalibrationHandlers.
      // (VoltageCalibration exposes setPoint(which, reference, raw).)
      // For now, mutate calibration struct directly + persist.
      uint32_t now = (uint32_t)::time(nullptr);
      if (w == "low") {
        Core::calibration.voltageLow.reference = reference;
        Core::calibration.voltageLow.raw = raw;
        Core::calibration.voltageLow.timestamp = now;
      } else if (w == "nominal") {
        Core::calibration.voltageNominal.reference = reference;
        Core::calibration.voltageNominal.raw = raw;
        Core::calibration.voltageNominal.timestamp = now;
      } else {  // full
        Core::calibration.voltageFull.reference = reference;
        Core::calibration.voltageFull.raw = raw;
        Core::calibration.voltageFull.timestamp = now;
      }
      Storage::config.saveCalibration(true);
      return { true, "ACCEPTED", "calibration point " + w + " updated" };
    }
    return { false, "REJECTED", "invalid calibration point: " + w };
  }

  // ---------- calibration.acs712_zero ----------
  if (type == "calibration" && action == "acs712_zero") {
    // [FW-22 CLOSED 2026-08] Real execution: capture the zero-current offset
    // from the driver NOW (64-sample average, ~1 s window at zero current)
    // and persist it. Previously this stub logged + returned ACCEPTED without
    // performing ANY mutation — an ACK that lied.
    float captured = Drivers::acs712.captureZeroOffset();
    Core::calibration.acs712Offset = captured;
    Storage::config.saveCalibration(true);
    Services::Log.append(Core::LogType::CalibrationChanged,
                          "MQTT: ACS712 zero-cal captured", -1);
    return { true, "ACCEPTED", "acs712 zero-cal applied" };
  }

  // ---------- alarm.acknowledge ----------
  if (type == "alarm" && action == "acknowledge") {
    const char* code = doc["code"] | "";
    if (strlen(code) == 0) return { false, "REJECTED", "missing code" };
    // [FW-22 CLOSED 2026-08] Real execution via the canonical AlarmRegistry
    // API — previously a log-only stub that ACKed without acknowledging.
    const Services::Alarm* a = Services::alarms.find(code);
    if (!a) return { false, "REJECTED", "alarm not found" };
    Services::alarms.acknowledge(code);   // persists immediately (FW-23)
    Services::Log.append(Core::LogType::AlarmAcknowledged,
                          String("MQTT: ack alarm ") + code, -1);
    return { true, "ACCEPTED", "alarm acknowledged" };
  }

  // ---------- alarm.acknowledgeAll ----------
  if (type == "alarm" && action == "acknowledgeAll") {
    // [FW-22 CLOSED 2026-08] Real execution — acknowledge every ACTIVE alarm.
    Services::alarms.acknowledgeAll();    // persists immediately (FW-23)
    Services::Log.append(Core::LogType::AlarmAcknowledged,
                          "MQTT: ack all alarms", -1);
    return { true, "ACCEPTED", "all alarms acknowledged" };
  }

  // ---------- system.reboot ----------
  if (type == "system" && action == "reboot") {
    // [FW-22 CLOSED 2026-08] Real execution: persist state NOW (SOC, energy,
    // sequence high-water mark) so the reboot is a clean checkpoint, then
    // defer the actual esp_restart() so this ACK is delivered first.
    Services::socStateMachine.saveToNVS();
    Services::energyCounters.saveToNVS();
    Storage::config.saveTelemetrySequence(telemetrySequence + Core::SEQ_REBOOT_MARGIN);
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: reboot requested — scheduling in 500ms", -1);
    Network::mqttConfigReceiver.requestDeferredReboot(500);
    return { true, "ACCEPTED", "reboot scheduled" };
  }

  // ---------- system.factory_reset_prepare / confirm ----------
  if (type == "system" && action == "factory_reset_prepare") {
    // [FW-22 CLOSED 2026-08] Real execution: issue a 60 s one-time token via
    // AuthManager (same two-step flow as REST).
    String token = Services::auth.prepareFactoryReset();
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: factory_reset_prepare (60s TTL)", -1);
    return { true, "ACCEPTED", "factory reset token issued (60s TTL)" };
  }
  if (type == "system" && action == "factory_reset_confirm") {
    const char* token = doc["token"] | "";
    if (strlen(token) == 0) return { false, "REJECTED", "missing token" };
    // [FW-22 CLOSED 2026-08] Real execution: verify the one-time token, then
    // erase persisted state (same namespace wipe as the REST path) and
    // reboot into first-boot provisioning.
    if (!Services::auth.confirmFactoryReset(token)) {
      return { false, "REJECTED", "invalid or expired factory reset token" };
    }
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: factory_reset_confirm — erasing state", -1);
    Preferences p;
    p.begin("plts", false);        p.clear(); p.end();
    p.begin("plts_health", false); p.clear(); p.end();
    p.begin("plts_energy", false); p.clear(); p.end();
    p.begin("plts_ota", false);    p.clear(); p.end();
    p.begin("plts_soc", false);    p.clear(); p.end();
    p.begin("plts_alarm", false);  p.clear(); p.end();
    p.begin("plts_txn", false);    p.clear(); p.end();
    p.begin("plts_spool", false);  p.clear(); p.end();
    p.begin("plts_batt", false);   p.clear(); p.end();
    Network::mqttConfigReceiver.requestDeferredReboot(500);
    return { true, "ACCEPTED", "factory reset applied — rebooting" };
  }

  // ---------- relay.* (v1.8.0) ----------
  #if PLTS_ENABLE_RELAYS
  if (type == "relay") {
    uint8_t channel = doc["channel"] | 0;
    String source = doc["source"] | "MANUAL";
    uint32_t pulseMs = doc["durationMs"] | 0;
    bool desired = (action == "on" || action == "pulse");

    String messageOut;
    Services::RelayCommandResult result = Services::relaysController.applyCommand(
      action, channel, desired, pulseMs, source, messageOut);

    bool ok = (result == Services::RelayCommandResult::Applied);
    String code = (result == Services::RelayCommandResult::Applied) ? "EXECUTED" :
                  (result == Services::RelayCommandResult::Blocked) ? "BLOCKED" :
                  (result == Services::RelayCommandResult::Rejected) ? "REJECTED" : "FAILED";
    return ApplyResult{ ok, code.c_str(), messageOut };
  }
  #endif

  // Unreachable — whitelist check above already rejected unknown types.
  return { false, "REJECTED", "unreachable" };
}

// ---------------------------------------------------------------------------
// _publishAck — helper to publish a synthesized ACK when we don't have a
// stored one (e.g., on bad schema before canonicalization).
// ---------------------------------------------------------------------------
void MqttConfigReceiver::_publishAck(const char* transactionId, bool ok,
                                       const char* code, const String& message) {
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = transactionId ? transactionId : "";
    ack["ok"] = ok;
    ack["code"] = code;
    ack["message"] = message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }
  String ackTopic = mqttTransport.getDeviceTopic("ack");
  mqttTransport.publish(ackTopic.c_str(), ackJson.c_str(),
                        ackJson.length(), false);
}

// [FW-22] Deferred reboot driver — called from networkTask's 100 Hz loop.
void MqttConfigReceiver::tick() {
  if (_rebootAtMs == 0) return;
  if ((int32_t)(millis() - _rebootAtMs) >= 0) {
    Services::Log.append(Core::LogType::ConfigurationChanged,
                         "Deferred reboot firing now", -1);
    delay(100);            // let the socket flush the ACK
    ESP.restart();
  }
}

} // namespace Network
