// =============================================================================
#include "../Drivers/Sht31Driver.h"
// Web/DiagnosticsHandlers.cpp
// =============================================================================
#include "DiagnosticsHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/HealthSupervisor.h"
#include "../Services/TelemetrySpool.h"
#include "../Services/EnergyCounters.h"
#include "../Services/SocStateMachine.h"
#include "../Services/AlarmRegistry.h"
#include "../Drivers/Ina219Driver.h"
#include "../Comm/BatteryCommManager.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include <cmath>

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
  // [AUDIT 2026-09 ROUND 5 / p.437] Regular-ring reboot persistence evidence.
  doc["spoolFsRestored"] = Services::telemetrySpool.fsRestoredCount();
  doc["spoolFsWriteFailures"] = Services::telemetrySpool.fsWriteFailures();
  // [AUDIT 2026-09 ROUND 5 / p.439/p.440/p.451/p.452] Persistence-failure and
  // registry-saturation observability — the auditor's core asks: failures must
  // be countable, never silently assumed successful.
  doc["energyPersistFailures"] = Services::energyCounters.persistFailures();
  doc["energyLoadedFromLegacyKeys"] = Services::energyCounters.loadedFromLegacy();
  doc["socPersistFailures"] = Services::socStateMachine.persistFailures();
  doc["alarmPersistFailures"] = Services::alarms.persistFailures();
  doc["alarmRegistryOverflow"] = Services::alarms.overflowCount();
  // [AUDIT 2026-09 ROUND 6 / p.455] Persisted-snapshot generation — proves
  // which NVS transaction the current alarm state came from (boot log line
  // records the same number; a post-mortem can correlate the two).
  doc["alarmStateGeneration"] = Services::alarms.generation();
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

// -----------------------------------------------------------------------------
// [AUDIT ROUND 4 / INA219 hardware acceptance — auditor items 4 & 5]
// The auditor's finding: INA219_SIGN_CORRECTION is still "ASSUMED — NOT
// HARDWARE VERIFIED", and the PGA gain design (80/160 mV, hysteresis) needs
// physical acceptance. Those facts stay TRUE — this endpoint does NOT change
// them. What it does is give the acceptance engineer everything needed to
// EXECUTE the procedures (INA-001..INA-004) in one authenticated call:
//   - the config register AS READ BACK from the chip (proves PGA bits),
//   - fresh RAW shunt/bus registers + their decoded values (proves scaling),
//   - the exact constant chain (shunt Ω, sign correction, LSB, EMA) that
//     turns registers into amperes — so a discrepancy can be attributed, not
//     guessed,
//   - the live BMS cross-check (same sign convention: +charging) — the
//     fastest sign verdict available on a live PLTS.
// Honest limits: the sign constant keeps its ASSUMED status until the
// physical acceptance evidence lands in docs/hardware-acceptance/.
// -----------------------------------------------------------------------------
void handleGetIna219() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }

  StaticJsonDocument<2560> doc;
  Drivers::Ina219Diag d = Drivers::ina219Battery.readDiagnostic();
  Drivers::Ina219Reading r = Drivers::ina219Battery.getReading();

  doc["available"] = Drivers::ina219Battery.isAvailable();
  doc["ok"] = d.ok;
  if (!d.ok) {
    doc["reason"] = "I2C read failed or sensor not initialized";
    String out; serializeJson(doc, out);
    sendSuccess("OK", out);
    return;
  }

  // Register-level evidence (fresh readback, not the cached pipeline)
  JsonObject reg = doc.createNestedObject("registers");
  char hex[8];
  snprintf(hex, sizeof(hex), "0x%04X", d.configReg);      reg["config"] = hex;
  snprintf(hex, sizeof(hex), "0x%04X", d.shuntReg);       reg["shuntRaw"] = hex;
  snprintf(hex, sizeof(hex), "0x%04X", d.busReg);         reg["busRaw"] = hex;
  snprintf(hex, sizeof(hex), "0x%04X", d.calibrationReg); reg["calibration"] = hex;
  reg["shuntVoltageV"] = d.shuntVoltageV;
  reg["busVoltageV"] = d.busVoltageV;

  // Config-register decode (acceptance: PGA bits must match the mode)
  uint16_t expected = (Drivers::ina219Battery.getPgaMode() == Drivers::Ina219PgaMode::Pga80mV)
                      ? Core::INA219_CONFIG_PGA_80MV : Core::INA219_CONFIG_PGA_160MV;
  JsonObject cfg = doc.createNestedObject("configDecode");
  cfg["readback"]   = (d.configReg == expected);
  cfg["pgaBits"]    = (d.configReg >> 11) & 0b11;
  cfg["brng"]       = (d.configReg >> 13) & 0b1;
  cfg["badc"]       = (d.configReg >> 7) & 0b1111;
  cfg["sadc"]       = (d.configReg >> 3) & 0b1111;
  cfg["mode"]       = d.configReg & 0b111;
  cfg["pgaMode"]    = Drivers::ina219Battery.getPgaModeStr();

  // The constant chain (attribution of any discrepancy)
  JsonObject chain = doc.createNestedObject("chain");
  chain["shuntOhm"]         = Core::INA219_SHUNT_OHM;
  chain["signCorrection"]   = Drivers::ina219Battery.getSignCorrection();
  chain["signCorrectionStatus"] =
      "ASSUMED - NOT HARDWARE VERIFIED (pending INA-004 acceptance)";
  chain["currentLsbA"]      = 0.004f;
  chain["emaAlpha"]         = Core::CURRENT_SMOOTH_ALPHA;
  chain["spikeRejectA"]     = Core::CURRENT_SPIKE_REJECT_A;

  // Derived values as the pipeline reports them (EMA-smoothed)
  JsonObject der = doc.createNestedObject("derived");
  der["currentA"] = r.currentA;
  der["powerW"]   = r.powerW;
  der["status"]   = (int)r.status;
  der["ageMs"]    = (millis() - r.timestamp);

  // PGA policy constants under test
  JsonObject pga = doc.createNestedObject("pgaPolicy");
  pga["mode"]        = Drivers::ina219Battery.getPgaModeStr();
  pga["switchUpA"]   = Core::INA219_PGA_SWITCH_UP_A;
  pga["switchDownA"] = Core::INA219_PGA_SWITCH_DOWN_A;
  pga["max80mVA"]    = Core::INA219_PGA_80MV_MAX_A;
  pga["max160mVA"]   = Core::INA219_PGA_160MV_MAX_A;

  // Live BMS cross-check (independent sensor, same +charging convention)
  JsonObject x = doc.createNestedObject("bmsCrossCheck");
  Comm::BmsData bms = Comm::batteryComm.getData();
  bool bmsLive = Comm::batteryComm.isLocked() && !std::isnan(bms.current) &&
                 bms.isFresh(millis(), 10000);
  x["available"] = bmsLive;
  if (bmsLive) {
    x["bmsCurrentA"]  = bms.current;
    x["inaCurrentA"] = r.currentA;
    x["deltaA"]      = bms.current - r.currentA;
    // Sign agreement is only meaningful when both sides are clearly away
    // from zero; near zero the sign is noise.
    bool meaningful = std::fabs(bms.current) > 2.0f && std::fabs(r.currentA) > 2.0f;
    x["signMeaningful"] = meaningful;
    x["signAgreement"] = meaningful
        ? ((bms.current > 0) == (r.currentA > 0)) : false;
    x["mismatchAlarmActive"] = Comm::batteryComm.isMismatchActive();
  }

  // Acceptance references
  JsonObject acc = doc.createNestedObject("acceptance");
  JsonArray ids = acc.createNestedArray("procedures");
  ids.add("INA-001 (low-current accuracy)");
  ids.add("INA-002 (PGA up/down transitions + hysteresis)");
  ids.add("INA-003 (saturation + transient)");
  ids.add("INA-004 (current sign — closes the ASSUMED flag)");
  acc["evidenceDir"] = "docs/hardware-acceptance/";
  acc["signMatrix"]  = "canonical convention: +charging / -discharging "
                       "(verify charge > 0 during PV surplus, discharge < 0 "
                       "under night load)";

  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void registerRoutes() {
  http.on("/api/diagnostics", HTTP_GET, handleGet);
  http.on("/api/diagnostics/ina219", HTTP_GET, handleGetIna219);
}

} // namespace DiagnosticsHandlers
} // namespace Web
