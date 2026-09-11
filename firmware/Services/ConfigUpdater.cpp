// =============================================================================
// Services/ConfigUpdater.cpp — two-phase config mutation (BLOCKER B fix)
// =============================================================================
#include "ConfigUpdater.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Storage/ConfigStore.h"
#include "../Services/LogService.h"
#include <cmath>
#include <cstring>

namespace Services {

ConfigUpdater::Result ConfigUpdater::applyUpdate(JsonDocument& doc) {
  Result res;
  res.ok = false;

  // ---------------------------------------------------------------------------
  // PHASE 1a — range-validate every PRESENT field into candidate locals.
  // Absent fields inherit the current RAM value (partial update semantics).
  // No Core:: global is touched in this phase.
  // ---------------------------------------------------------------------------
  float capacityAh   = Core::cfgBatteryCapacityAh;
  float nominalV     = Core::cfgBatteryNominalVoltage;
  float fullV        = Core::cfgFullVoltage;
  float lowV         = Core::cfgLowVoltage;
  float idleThr      = Core::cfgIdleCurrentThreshold;
  float fullChargeI = Core::cfgFullChargeCurrentThreshold;
  uint32_t fullChargePersist = Core::cfgFullChargePersistenceSec;
  uint16_t telemetryInterval = Core::cfgTelemetryIntervalSec;

  bool hasBatteryField = false;

  if (doc.containsKey("batteryCapacityAh")) {
    float v = doc["batteryCapacityAh"] | NAN;
    if (!isfinite(v) || v < 10 || v > 1000) { res.message = "batteryCapacityAh out of range [10,1000]"; return res; }
    capacityAh = v; hasBatteryField = true;
  }
  if (doc.containsKey("batteryNominalV")) {
    float v = doc["batteryNominalV"] | NAN;
    if (!isfinite(v) || v < 24 || v > 48) { res.message = "batteryNominalV out of range [24,48]"; return res; }
    nominalV = v; hasBatteryField = true;
  }
  if (doc.containsKey("fullVoltage")) {
    float v = doc["fullVoltage"] | NAN;
    if (!isfinite(v) || v < 50 || v > 56) { res.message = "fullVoltage out of range [50,56]"; return res; }
    fullV = v; hasBatteryField = true;
  }
  if (doc.containsKey("lowVoltage")) {
    float v = doc["lowVoltage"] | NAN;
    if (!isfinite(v) || v < 40 || v > 50) { res.message = "lowVoltage out of range [40,50]"; return res; }
    lowV = v; hasBatteryField = true;
  }
  if (doc.containsKey("idleCurrentThreshold")) {
    float v = doc["idleCurrentThreshold"] | NAN;
    if (!isfinite(v) || v < 0.1f || v > 5.0f) { res.message = "idleCurrentThreshold out of range [0.1,5.0]"; return res; }
    idleThr = v; hasBatteryField = true;
  }
  if (doc.containsKey("fullChargeCurrentThreshold")) {
    float v = doc["fullChargeCurrentThreshold"] | NAN;
    if (!isfinite(v) || v < 0.5f || v > 10.0f) { res.message = "fullChargeCurrentThreshold out of range [0.5,10.0]"; return res; }
    fullChargeI = v; hasBatteryField = true;
  }
  if (doc.containsKey("fullChargePersistenceSec")) {
    uint32_t v = doc["fullChargePersistenceSec"] | 0U;
    if (v < 60 || v > 7200) { res.message = "fullChargePersistenceSec out of range [60,7200]"; return res; }
    fullChargePersist = v; hasBatteryField = true;
  }
  if (doc.containsKey("telemetryIntervalSec")) {
    uint16_t v = doc["telemetryIntervalSec"] | 0U;
    if (v < 1 || v > 60) { res.message = "telemetryIntervalSec out of range [1,60]"; return res; }
    telemetryInterval = v; hasBatteryField = true;
  }

  // Cross-field invariant evaluated on the CANDIDATE (audit p.278): a request
  // that raises lowVoltage above the CURRENT fullVoltage while ALSO raising
  // fullVoltage must not be rejected against the stale value.
  if (lowV >= fullV) {
    res.message = "lowVoltage must be < fullVoltage";
    return res;
  }

  // ---------------- deviceName / timezone (string candidates) ----------------
  char deviceNameBuf[40];
  strncpy(deviceNameBuf, Core::deviceName, sizeof(deviceNameBuf) - 1);
  deviceNameBuf[sizeof(deviceNameBuf) - 1] = '\0';
  bool hasDeviceName = false;

  char timezoneBuf[40];
  strncpy(timezoneBuf, Core::cfgTimezone, sizeof(timezoneBuf) - 1);
  timezoneBuf[sizeof(timezoneBuf) - 1] = '\0';
  bool hasTimezone = false;

  if (doc.containsKey("deviceName")) {
    const char* dn = doc["deviceName"];
    if (dn && strlen(dn) > 0 && strlen(dn) < sizeof(deviceNameBuf)) {
      strncpy(deviceNameBuf, dn, sizeof(deviceNameBuf) - 1);
      deviceNameBuf[sizeof(deviceNameBuf) - 1] = '\0';
      hasDeviceName = true;
    } else {
      res.message = "deviceName invalid or too long";
      return res;
    }
  }
  if (doc.containsKey("timezone")) {
    const char* tz = doc["timezone"];
    if (tz && strlen(tz) < sizeof(timezoneBuf)) {
      strncpy(timezoneBuf, tz, sizeof(timezoneBuf) - 1);
      timezoneBuf[sizeof(timezoneBuf) - 1] = '\0';
      hasTimezone = true;
    } else {
      res.message = "timezone invalid or too long";
      return res;
    }
  }

  // ---------------- BMS comm candidates ----------------------------------------
  char bmsProtocolBuf[24];
  strncpy(bmsProtocolBuf, Core::cfgBmsProtocol, sizeof(bmsProtocolBuf) - 1);
  bmsProtocolBuf[sizeof(bmsProtocolBuf) - 1] = '\0';
  uint32_t bmsPollMs = Core::cfgBmsPollIntervalMs;
  uint8_t bmsSlaveId = Core::cfgBmsModbusSlaveId;
  char bmsTcpHostBuf[64];
  strncpy(bmsTcpHostBuf, Core::cfgBmsModbusTcpHost, sizeof(bmsTcpHostBuf) - 1);
  bmsTcpHostBuf[sizeof(bmsTcpHostBuf) - 1] = '\0';
  uint16_t bmsTcpPort = Core::cfgBmsModbusTcpPort;
  bool bmsChanged = false;

  if (doc.containsKey("bmsProtocol")) {
    const char* p = doc["bmsProtocol"];
    bool valid = p && (strcmp(p, "auto") == 0 || strcmp(p, "none") == 0 ||
                       strcmp(p, "pylontech_can") == 0 || strcmp(p, "modbus_rtu") == 0 ||
                       strcmp(p, "modbus_tcp") == 0);
    if (!valid) { res.message = "bmsProtocol must be auto|none|pylontech_can|modbus_rtu|modbus_tcp"; return res; }
    strncpy(bmsProtocolBuf, p, sizeof(bmsProtocolBuf) - 1);
    bmsProtocolBuf[sizeof(bmsProtocolBuf) - 1] = '\0';
    bmsChanged = true;
  }
  if (doc.containsKey("bmsPollIntervalMs")) {
    uint32_t v = doc["bmsPollIntervalMs"] | 0U;
    if (v < 1000 || v > 600000) { res.message = "bmsPollIntervalMs out of range [1000,600000]"; return res; }
    bmsPollMs = v; bmsChanged = true;
  }
  if (doc.containsKey("bmsModbusSlaveId")) {
    uint8_t v = doc["bmsModbusSlaveId"] | 0U;
    if (v < 1 || v > 247) { res.message = "bmsModbusSlaveId out of range [1,247]"; return res; }
    bmsSlaveId = v; bmsChanged = true;
  }
  if (doc.containsKey("bmsModbusTcpHost")) {
    const char* h = doc["bmsModbusTcpHost"];
    if (!h || strlen(h) == 0 || strlen(h) >= sizeof(bmsTcpHostBuf)) { res.message = "bmsModbusTcpHost too long"; return res; }
    strncpy(bmsTcpHostBuf, h, sizeof(bmsTcpHostBuf) - 1);
    bmsTcpHostBuf[sizeof(bmsTcpHostBuf) - 1] = '\0';
    bmsChanged = true;
  }
  if (doc.containsKey("bmsModbusTcpPort")) {
    uint16_t v = doc["bmsModbusTcpPort"] | 0U;
    if (v < 1 || v > 65535) { res.message = "bmsModbusTcpPort out of range [1,65535]"; return res; }
    bmsTcpPort = v; bmsChanged = true;
  }

  // ---------------- alarm threshold candidates ----------------------------------
  float alWarn  = Core::cfgAlarmVoltageLowWarnV;
  float alCrit  = Core::cfgAlarmVoltageLowCriticalV;
  float ahWarn  = Core::cfgAlarmVoltageHighWarnV;
  float ahCrit  = Core::cfgAlarmVoltageHighCriticalV;
  float acWarn  = Core::cfgAlarmCurrentHighWarnA;
  float acCrit  = Core::cfgAlarmCurrentHighCriticalA;
  float atWarn  = Core::cfgAlarmTemperatureHighWarnC;
  float atCrit  = Core::cfgAlarmTemperatureHighCriticalC;
  float ahumWarn = Core::cfgAlarmHumidityHighWarnPct;
  float asocWarn = Core::cfgAlarmSocLowWarnPct;
  float asocCrit = Core::cfgAlarmSocLowCriticalPct;
  bool alarmChanged = false;

  if (doc.containsKey("voltageLowWarn")) {
    float v = doc["voltageLowWarn"] | NAN;
    if (!isfinite(v) || !(v >= 40 && v <= 50)) { res.message = "voltageLowWarn out of range [40,50]"; return res; }
    alWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("voltageLowCritical")) {
    float v = doc["voltageLowCritical"] | NAN;
    if (!isfinite(v) || !(v >= 40 && v <= 50)) { res.message = "voltageLowCritical out of range [40,50]"; return res; }
    alCrit = v; alarmChanged = true;
  }
  if (doc.containsKey("voltageHighWarn")) {
    float v = doc["voltageHighWarn"] | NAN;
    if (!isfinite(v) || !(v >= 50 && v <= 60)) { res.message = "voltageHighWarn out of range [50,60]"; return res; }
    ahWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("voltageHighCritical")) {
    float v = doc["voltageHighCritical"] | NAN;
    if (!isfinite(v) || !(v >= 50 && v <= 60)) { res.message = "voltageHighCritical out of range [50,60]"; return res; }
    ahCrit = v; alarmChanged = true;
  }
  if (doc.containsKey("currentHighWarn")) {
    float v = doc["currentHighWarn"] | NAN;
    if (!isfinite(v) || !(v >= 10 && v <= 150)) { res.message = "currentHighWarn out of range [10,150]"; return res; }
    acWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("currentHighCritical")) {
    float v = doc["currentHighCritical"] | NAN;
    if (!isfinite(v) || !(v >= 10 && v <= 160)) { res.message = "currentHighCritical out of range [10,160]"; return res; }
    acCrit = v; alarmChanged = true;
  }
  if (doc.containsKey("temperatureHighWarn")) {
    float v = doc["temperatureHighWarn"] | NAN;
    if (!isfinite(v) || !(v >= -20 && v <= 80)) { res.message = "temperatureHighWarn out of range [-20,80]"; return res; }
    atWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("temperatureHighCritical")) {
    float v = doc["temperatureHighCritical"] | NAN;
    if (!isfinite(v) || !(v >= -20 && v <= 90)) { res.message = "temperatureHighCritical out of range [-20,90]"; return res; }
    atCrit = v; alarmChanged = true;
  }
  if (doc.containsKey("humidityHighWarn")) {
    float v = doc["humidityHighWarn"] | NAN;
    if (!isfinite(v) || !(v >= 50 && v <= 100)) { res.message = "humidityHighWarn out of range [50,100]"; return res; }
    ahumWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("socLowWarn")) {
    float v = doc["socLowWarn"] | NAN;
    if (!isfinite(v) || !(v >= 5 && v <= 50)) { res.message = "socLowWarn out of range [5,50]"; return res; }
    asocWarn = v; alarmChanged = true;
  }
  if (doc.containsKey("socLowCritical")) {
    float v = doc["socLowCritical"] | NAN;
    if (!isfinite(v) || !(v >= 2 && v <= 50)) { res.message = "socLowCritical out of range [2,50]"; return res; }
    asocCrit = v; alarmChanged = true;
  }

  // Tier-order invariants on the CANDIDATE (audit p.279 — evaluate against the
  // post-update values, not the stale pre-update ones).
  if (alCrit >= alWarn)   { res.message = "voltageLowCritical must be < voltageLowWarn"; return res; }
  if (ahCrit <= ahWarn)   { res.message = "voltageHighCritical must be > voltageHighWarn"; return res; }
  if (acCrit <= acWarn)   { res.message = "currentHighCritical must be > currentHighWarn"; return res; }
  if (atCrit <= atWarn)   { res.message = "temperatureHighCritical must be > temperatureHighWarn"; return res; }
  if (asocCrit >= asocWarn) { res.message = "socLowCritical must be < socLowWarn"; return res; }

  // ---------------------------------------------------------------------------
  // PHASE 2 — COMMIT. The candidate is fully valid; mutate RAM + persist.
  // ---------------------------------------------------------------------------
  Core::cfgBatteryCapacityAh = capacityAh;
  Core::cfgBatteryNominalVoltage = nominalV;
  Core::cfgFullVoltage = fullV;
  Core::cfgLowVoltage = lowV;
  Core::cfgIdleCurrentThreshold = idleThr;
  Core::cfgFullChargeCurrentThreshold = fullChargeI;
  Core::cfgFullChargePersistenceSec = fullChargePersist;
  Core::cfgTelemetryIntervalSec = telemetryInterval;
  if (hasDeviceName) strncpy(Core::deviceName, deviceNameBuf, sizeof(Core::deviceName) - 1);
  if (hasTimezone)   strncpy(Core::cfgTimezone, timezoneBuf, sizeof(Core::cfgTimezone) - 1);

  strncpy(Core::cfgBmsProtocol, bmsProtocolBuf, sizeof(Core::cfgBmsProtocol) - 1);
  Core::cfgBmsProtocol[sizeof(Core::cfgBmsProtocol) - 1] = '\0';
  Core::cfgBmsPollIntervalMs = bmsPollMs;
  Core::cfgBmsModbusSlaveId = bmsSlaveId;
  strncpy(Core::cfgBmsModbusTcpHost, bmsTcpHostBuf, sizeof(Core::cfgBmsModbusTcpHost) - 1);
  Core::cfgBmsModbusTcpHost[sizeof(Core::cfgBmsModbusTcpHost) - 1] = '\0';
  Core::cfgBmsModbusTcpPort = bmsTcpPort;

  Core::cfgAlarmVoltageLowWarnV = alWarn;
  Core::cfgAlarmVoltageLowCriticalV = alCrit;
  Core::cfgAlarmVoltageHighWarnV = ahWarn;
  Core::cfgAlarmVoltageHighCriticalV = ahCrit;
  Core::cfgAlarmCurrentHighWarnA = acWarn;
  Core::cfgAlarmCurrentHighCriticalA = acCrit;
  Core::cfgAlarmTemperatureHighWarnC = atWarn;
  Core::cfgAlarmTemperatureHighCriticalC = atCrit;
  Core::cfgAlarmHumidityHighWarnPct = ahumWarn;
  Core::cfgAlarmSocLowWarnPct = asocWarn;
  Core::cfgAlarmSocLowCriticalPct = asocCrit;

  // Persistence — [BLOCKER E / audit p.281-282] a failed save must surface.
  if (hasBatteryField) {
    if (!Storage::config.saveBatteryConfig()) {
      res.message = "config applied in RAM but NVS persistence FAILED — durability degraded";
      Services::Log.append(Core::LogType::ConfigurationChanged, res.message, -1);
      // Still applied (RAM is the runtime authority) but the caller reports
      // the degraded durability honestly instead of a plain success.
      res.ok = true;
      res.message = "config updated (WARNING: NVS save failed — change may not survive reboot)";
      return res;
    }
  }
  if (alarmChanged) {
    Storage::config.saveAlarmConfig();
  }
  Storage::config.saveDeviceConfig();

  res.ok = true;
  res.message = "config updated";
  res.bmsConfigChanged = bmsChanged;
  res.alarmConfigChanged = alarmChanged;
  return res;
}

} // namespace Services
