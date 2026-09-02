// =============================================================================
// Web/BatteryStatusSerializer.h — Shared REST + MQTT telemetry serializer
// -----------------------------------------------------------------------------
// Per brief §36, §83:
//   - NaN-safe: invalid → null + quality=SENSOR_ERROR
//   - SOC labeled ESTIMATED
//   - AC power labeled ESTIMATED
//   - Ambient (NOT battery) temperature label
//
// Phase 13-B: Uses Core::SystemStatus (canonical, from Globals.h).
//   Removed reference to non-existent TelemetrySnapshot + m.unit.
//   Uses Core::isValidFloat + qualityToStr/sourceToStr.
// =============================================================================
#pragma once
#ifndef PLTS_WEB_BATTERY_STATUS_SERIALIZER_H
#define PLTS_WEB_BATTERY_STATUS_SERIALIZER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "../Core/Types.h"
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Services/AlarmRegistry.h"  // for Alarm struct (RC-11: single canonical owner)

namespace Web {

using Core::isValidFloat;

// Serialize a single measurement as a nested object (NaN-safe).
inline void serializeMeasurement(JsonObject obj, const char* key,
                                 const Core::Measurement& m) {
  JsonObject sub = obj.createNestedObject(key);
  if (isValidFloat(m.value)) sub["value"].set(m.value);
  else                        sub["value"].set(nullptr);
  sub["quality"]    = Core::qualityToStr(m.quality);
  sub["source"]     = Core::sourceToStr(m.source);
  sub["timestamp"]  = m.timestamp;
  sub["sequence"]   = m.sequence;
}

// Serialize the full telemetry snapshot (shared by REST + MQTT).
// Produces a JSON string. NaN-safe — invalid measurements emit null, never 0.
inline String serialize(const Core::SystemStatus& s) {
  String out;
  StaticJsonDocument<8192> doc;

  doc["protocolVersion"] = s.protocolVersion;
  doc["firmwareVersion"] = s.firmwareVersion;
  doc["deviceId"]        = s.deviceId ? s.deviceId : "";
  doc["deviceName"]      = s.deviceName ? s.deviceName : "";
  doc["sequence"]        = s.sequence;
  doc["timestamp"]       = s.timestamp;
  doc["timeQuality"]     = Core::timeQualityToStr(s.timeQuality);
  doc["uptimeSeconds"]   = s.uptimeSeconds;
  doc["bootCount"]       = s.bootCount;
  doc["resetReason"]     = s.resetReason ? s.resetReason : "";

  // Battery block
  JsonObject bat = doc.createNestedObject("battery");
  JsonObject batV = bat.createNestedObject("voltage");
  if (isValidFloat(s.battery.voltage.value)) batV["value"].set(s.battery.voltage.value);
  else                                       batV["value"].set(nullptr);
  batV["quality"] = Core::qualityToStr(s.battery.voltage.quality);
  batV["source"]  = Core::sourceToStr(s.battery.voltage.source);

  JsonObject batI = bat.createNestedObject("current");
  if (isValidFloat(s.battery.current.value)) batI["value"].set(s.battery.current.value);
  else                                       batI["value"].set(nullptr);
  batI["quality"] = Core::qualityToStr(s.battery.current.quality);
  batI["source"]  = Core::sourceToStr(s.battery.current.source);

  JsonObject batP = bat.createNestedObject("power");
  if (isValidFloat(s.battery.power.value)) batP["value"].set(s.battery.power.value);
  else                                      batP["value"].set(nullptr);
  batP["quality"] = Core::qualityToStr(s.battery.power.quality);
  batP["source"]  = Core::sourceToStr(s.battery.power.source);

  bat["direction"] = Core::directionToStr(s.battery.direction);

  JsonObject soc = bat.createNestedObject("soc");
  soc["value"]    = s.battery.soc.value;
  soc["quality"]  = Core::qualityToStr(s.battery.soc.quality);
  soc["source"]   = Core::sourceToStr(s.battery.soc.source);
  soc["method"]   = s.battery.soc.method ? s.battery.soc.method : "ESTIMATED";
  soc["lastSync"] = s.battery.soc.lastSync;
  soc["confidence"] = s.battery.soc.confidence ? s.battery.soc.confidence : "MEDIUM";
  // v1.6.0 — provenance: WHERE the SOC comes from (BMS vs shunt vs OCV).
  // Absent in older firmware payloads — consumers must treat absent as UNKNOWN.
  soc["provenance"] = Core::socProvenanceToStr(s.battery.soc.provenance);

  bat["remainingAh"]               = s.battery.remainingAh;
  bat["chargeAh"]                  = s.battery.chargeAh;
  bat["dischargeAh"]               = s.battery.dischargeAh;
  bat["chargeWh"]                  = s.battery.chargeWh;
  bat["dischargeWh"]                = s.battery.dischargeWh;
  bat["netWh"]                     = s.battery.netWh;
  bat["efc"]                       = s.battery.efc;
  bat["estimatedUsableCapacityAh"] = s.battery.estimatedUsableCapacityAh;
  bat["peakChargeCurrent"]         = s.battery.peakChargeCurrent;
  bat["peakDischargeCurrent"]      = s.battery.peakDischargeCurrent;

  // v1.6.0 — External BMS/inverter block (NaN-safe, absent fields = null).
  // Honest by construction: a disconnected BMS shows connected=false with
  // null values — never fabricated zeros.
  JsonObject bms = bat.createNestedObject("bms");
  bms["connected"]            = s.bms.connected;
  bms["protocol"]             = s.bms.protocol ? s.bms.protocol : "NONE";
  bms["state"]                = s.bms.state ? s.bms.state : "DISABLED";
  if (isValidFloat(s.bms.voltage))   bms["voltage"].set(s.bms.voltage);
  else                               bms["voltage"].set(nullptr);
  if (isValidFloat(s.bms.current))   bms["current"].set(s.bms.current);
  else                               bms["current"].set(nullptr);
  if (isValidFloat(s.bms.temperature)) bms["temperature"].set(s.bms.temperature);
  else                                 bms["temperature"].set(nullptr);
  if (isValidFloat(s.bms.soh))       bms["soh"].set(s.bms.soh);
  else                               bms["soh"].set(nullptr);
  if (isValidFloat(s.bms.cellVoltageMin)) bms["cellVoltageMin"].set(s.bms.cellVoltageMin);
  else                                     bms["cellVoltageMin"].set(nullptr);
  if (isValidFloat(s.bms.cellVoltageMax)) bms["cellVoltageMax"].set(s.bms.cellVoltageMax);
  else                                     bms["cellVoltageMax"].set(nullptr);
  bms["cellCount"]            = s.bms.cellCount;
  if (isValidFloat(s.bms.chargeCurrentLimit))    bms["chargeCurrentLimit"].set(s.bms.chargeCurrentLimit);
  else                                           bms["chargeCurrentLimit"].set(nullptr);
  if (isValidFloat(s.bms.dischargeCurrentLimit)) bms["dischargeCurrentLimit"].set(s.bms.dischargeCurrentLimit);
  else                                           bms["dischargeCurrentLimit"].set(nullptr);
  bms["cycleCount"]           = s.bms.cycleCount;
  bms["faultFlags"]           = s.bms.faultFlags;
  bms["moduleCount"]          = s.bms.moduleCount;
  bms["lastSeenMs"]           = s.bms.lastSeenMs;
  if (isValidFloat(s.bms.currentMismatchA)) bms["currentMismatchA"].set(s.bms.currentMismatchA);
  else                                      bms["currentMismatchA"].set(nullptr);

  // AC block
  JsonObject ac = doc.createNestedObject("ac");
  JsonObject acRms = ac.createNestedObject("rmsCurrent");
  if (isValidFloat(s.ac.rmsCurrent.value)) acRms["value"].set(s.ac.rmsCurrent.value);
  else                                      acRms["value"].set(nullptr);
  acRms["quality"] = Core::qualityToStr(s.ac.rmsCurrent.quality);
  acRms["source"]  = Core::sourceToStr(s.ac.rmsCurrent.source);

  JsonObject acPeak = ac.createNestedObject("peakCurrent");
  if (isValidFloat(s.ac.peakCurrent.value)) acPeak["value"].set(s.ac.peakCurrent.value);
  else                                      acPeak["value"].set(nullptr);
  acPeak["quality"] = Core::qualityToStr(s.ac.peakCurrent.quality);

  JsonObject acAvg = ac.createNestedObject("averageCurrent");
  if (isValidFloat(s.ac.averageCurrent.value)) acAvg["value"].set(s.ac.averageCurrent.value);
  else                                          acAvg["value"].set(nullptr);
  acAvg["quality"] = Core::qualityToStr(s.ac.averageCurrent.quality);

  JsonObject acPwr = ac.createNestedObject("estimatedPower");
  if (isValidFloat(s.ac.estimatedPower.value)) acPwr["value"].set(s.ac.estimatedPower.value);
  else                                          acPwr["value"].set(nullptr);
  acPwr["quality"] = Core::qualityToStr(s.ac.estimatedPower.quality);
  JsonObject acAssumptions = acPwr.createNestedObject("assumptions");
  acAssumptions["voltage"]      = s.ac.estimatedPower.assumptions.voltage;
  acAssumptions["powerFactor"]  = s.ac.estimatedPower.assumptions.powerFactor;

  ac["signalQuality"] = Core::acSignalQualityToStr(s.ac.signalQuality);

#if PLTS_ENABLE_PZEM_AC
  // v1.7.0 — REAL AC meter (PZEM-004T, optional). Absent from <= v1.6.x
  // payloads entirely (compiled out) — consumers must treat the missing
  // block as "no meter, estimatedPower is the estimate".
  JsonObject acMeter = ac.createNestedObject("meter");
  acMeter["connected"] = s.ac.meter.connected;
  if (isValidFloat(s.ac.meter.voltage))     acMeter["voltage"].set(s.ac.meter.voltage);
  else                                      acMeter["voltage"].set(nullptr);
  if (isValidFloat(s.ac.meter.current))     acMeter["current"].set(s.ac.meter.current);
  else                                      acMeter["current"].set(nullptr);
  if (isValidFloat(s.ac.meter.power))       acMeter["power"].set(s.ac.meter.power);
  else                                      acMeter["power"].set(nullptr);
  if (isValidFloat(s.ac.meter.energy))      acMeter["energy"].set(s.ac.meter.energy);
  else                                      acMeter["energy"].set(nullptr);
  if (isValidFloat(s.ac.meter.frequency))   acMeter["frequency"].set(s.ac.meter.frequency);
  else                                      acMeter["frequency"].set(nullptr);
  if (isValidFloat(s.ac.meter.powerFactor)) acMeter["powerFactor"].set(s.ac.meter.powerFactor);
  else                                      acMeter["powerFactor"].set(nullptr);
#endif

  // Environment block
  JsonObject env = doc.createNestedObject("environment");
  JsonObject envT = env.createNestedObject("temperature");
  if (isValidFloat(s.environment.temperature.value)) envT["value"].set(s.environment.temperature.value);
  else                                                envT["value"].set(nullptr);
  envT["quality"] = Core::qualityToStr(s.environment.temperature.quality);
  envT["source"]  = Core::sourceToStr(s.environment.temperature.source);

  JsonObject envH = env.createNestedObject("humidity");
  if (isValidFloat(s.environment.humidity.value)) envH["value"].set(s.environment.humidity.value);
  else                                              envH["value"].set(nullptr);
  envH["quality"] = Core::qualityToStr(s.environment.humidity.quality);
  envH["source"]  = Core::sourceToStr(s.environment.humidity.source);

  JsonObject envD = env.createNestedObject("dewPoint");
  if (isValidFloat(s.environment.dewPoint.value)) envD["value"].set(s.environment.dewPoint.value);
  else                                              envD["value"].set(nullptr);
  envD["quality"] = Core::qualityToStr(s.environment.dewPoint.quality);

  env["label"]           = s.environment.label ? s.environment.label : "Ambient / Enclosure Temperature";
  env["condensationRisk"] = s.environment.condensationRisk;

  // Health block
  JsonObject health = doc.createNestedObject("health");
  health["systemState"] = Core::systemStateToStr(s.health.systemState);
  JsonObject sh = health.createNestedObject("sensorHealth");
  sh["ina219"]    = Core::sensorHealthToStr(s.health.sensorHealth.ina219);
  sh["batteryAdc"] = Core::sensorHealthToStr(s.health.sensorHealth.batteryAdc);
  sh["acs712"]    = Core::sensorHealthToStr(s.health.sensorHealth.acs712);
  sh["sht31"]     = Core::sensorHealthToStr(s.health.sensorHealth.sht31);
  sh["bmsComm"]   = Core::sensorHealthToStr(s.health.sensorHealth.bmsComm);   // v1.6.0
  health["freeHeap"]             = s.health.freeHeap;
  health["minFreeHeap"]          = s.health.minFreeHeap;
  health["wifiRssi"]             = s.health.wifiRssi;
  health["wifiReconnectCount"]   = s.health.wifiReconnectCount;
  health["mqttConnected"]        = s.health.mqttConnected;
  health["ntpSynced"]            = s.health.ntpSynced;
  health["storageOk"]            = s.health.storageOk;
  health["spoolSize"]            = s.health.spoolSize;
  health["highestAlarmSeverity"]  = Core::severityToStr(s.health.highestAlarmSeverity);

#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE emergency layer block (additive; <= v1.6.3 consumers
  // treat absent as DISABLED — the PWA/GAS contract is neutral-frontend).
  JsonObject emg = doc.createNestedObject("emergency");
  emg["state"]          = s.emergency.state ? s.emergency.state : "DISABLED";
  emg["reason"]         = s.emergency.reason ? s.emergency.reason : "";
  emg["estopOpen"]      = s.emergency.estopOpen;
  emg["relayEnergized"] = s.emergency.relayEnergized;
  emg["trips"]          = s.emergency.trips;
  emg["tripAtMs"]       = s.emergency.tripAtMs;
  emg["crashChain"]     = s.emergency.crashChain;
#endif

  // Active alarms
  JsonArray alarms = doc.createNestedArray("activeAlarms");
  for (uint8_t i = 0; i < s.activeAlarmCount; i++) {
    JsonObject a = alarms.createNestedObject();
    a["code"]      = s.activeAlarms[i].code;
    a["severity"]  = Core::severityToStr(s.activeAlarms[i].severity);
    a["lifecycle"] = Core::lifecycleToStr(s.activeAlarms[i].lifecycle);
    a["raisedAt"]  = s.activeAlarms[i].raisedAt;
    a["message"]   = s.activeAlarms[i].message;
  }

  serializeJson(doc, out);
  return out;
}

} // namespace Web

#endif // PLTS_WEB_BATTERY_STATUS_SERIALIZER_H
