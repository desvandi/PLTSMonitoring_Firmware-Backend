// =============================================================================
// Services/HealthSupervisor.cpp
// =============================================================================
#include "HealthSupervisor.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Types.h"
#include "../Drivers/RtcDriver.h"
#include <esp_system.h>
#include <Preferences.h>
#include <cstring>

namespace Services {

HealthSupervisor health;

// [FW-19] resetReasonStr delegated to the canonical Core::resetReasonStr.
const char* HealthSupervisor::resetReasonStr(uint8_t reason) {
  return Core::resetReasonStr(reason);
}

void HealthSupervisor::begin() {
  _snapshot = {};
  _snapshot.wifiRssi = -127;
  _snapshot.timeQuality = Core::TimeQuality::Unsynced;
  _snapshot.systemState = Core::SystemState::Healthy;
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    _snapshot.taskHeartbeatAgeMs[i] = 0;
    _lastHeartbeatMs[i] = 0;
  }
  for (uint8_t i = 0; i < BOOT_LOOP_WINDOW; i++) _bootTimestamps[i] = 0;
  _bootTimestampIdx = 0;

  Preferences p;
  if (p.begin("plts_health", false)) {
    _snapshot.bootCount = p.getUInt("boot_cnt", 0) + 1;
    p.putUInt("boot_cnt", _snapshot.bootCount);
    _snapshot.watchdogResets = p.getUInt("wdt_cnt", 0);
    _snapshot.brownoutResets = p.getUInt("brn_cnt", 0);
    p.getBytes("boot_ts", _bootTimestamps, sizeof(_bootTimestamps));
    _bootTimestampIdx = _snapshot.bootCount % BOOT_LOOP_WINDOW;
    p.end();
    _snapshot.nvsOk = true;
  }
  _snapshot.freeHeap = ESP.getFreeHeap();
  _snapshot.minFreeHeap = ESP.getMinFreeHeap();
  _snapshot.largestFreeBlock = _snapshot.freeHeap;
  _initialized = true;

  Serial.printf("[HEALTH] boot: count=%u, wdt=%u, brn=%u\n",
                _snapshot.bootCount, _snapshot.watchdogResets, _snapshot.brownoutResets);
}

void HealthSupervisor::recordBoot() {
  uint8_t reason = (uint8_t)esp_reset_reason();
  _snapshot.lastResetReason = reason;

  Preferences p;
  uint8_t prevReason = 0, prevPrevReason = 0;
  if (p.begin("plts_health", true)) {
    prevReason = p.getUChar("last_rst", 0);
    prevPrevReason = p.getUChar("prev_rst", 0);
    p.end();
  }

  // [FW-19] Canonical classification via Core::isWatchdogReset/isBrownoutReset.
  if (Core::isWatchdogReset(reason)) {
    _snapshot.watchdogResets++;
    alarms.raise("WATCHDOG_RESET", Core::AlarmSeverity::Warning,
                 "Watchdog reset detected on boot (see resetReason)");
  } else if (Core::isBrownoutReset(reason)) {
    _snapshot.brownoutResets++;
    alarms.raise("BROWNOUT_RESET", Core::AlarmSeverity::Warning,
                 "Brownout reset detected on boot");
  }

  // [FW-18 REMEDIATION 2026-08] Boot-loop detection was fundamentally broken:
  // timestamps were stored as millis() (~0 at boot on EVERY boot), so
  // (now - ts) < 60000 matched ALL historical entries and every boot after
  // the ring filled raised a spurious BOOT_LOOP CRITICAL → systemState=FAILED
  // → OTA permanently inhibited. Now: RTC unix timestamps (valid only when
  // > 0) + an honest consecutive-watchdog-boot heuristic.
  uint32_t bootTs = Drivers::rtc.getUnixTime();   // 0 when clock unsynced
  _bootTimestamps[_bootTimestampIdx] = bootTs;
  _bootTimestampIdx = (_bootTimestampIdx + 1) % BOOT_LOOP_WINDOW;

  uint8_t boots60s = 0;
  if (bootTs > 0) {                  // clock valid — time-window detection
    for (uint8_t i = 0; i < BOOT_LOOP_WINDOW; i++) {
      if (_bootTimestamps[i] > 0 && (bootTs - _bootTimestamps[i]) < 60) boots60s++;
    }
  }
  _snapshot.bootsInLast60s = boots60s;

  if (boots60s >= 3) {
    // 3+ boots within 60s with a VALID clock — unambiguous boot loop.
    _snapshot.bootLoopDetected = true;
    alarms.raise("BOOT_LOOP", Core::AlarmSeverity::Critical,
                 "Boot loop detected — 3+ boots in 60 seconds");
  } else if (Core::isWatchdogReset(reason) &&
             Core::isWatchdogReset(prevReason) &&
             Core::isWatchdogReset(prevPrevReason)) {
    // Three consecutive watchdog boots — crash loop even without a clock.
    _snapshot.bootLoopDetected = true;
    alarms.raise("BOOT_LOOP", Core::AlarmSeverity::Critical,
                 "Boot loop suspected — 3 consecutive watchdog resets");
  }

  if (p.begin("plts_health", false)) {
    p.putUInt("wdt_cnt", _snapshot.watchdogResets);
    p.putUInt("brn_cnt", _snapshot.brownoutResets);
    p.putBytes("boot_ts", _bootTimestamps, sizeof(_bootTimestamps));
    p.putUChar("prev_rst", prevReason);
    p.putUChar("last_rst", reason);
    p.end();
  }
}

void HealthSupervisor::recordWifiReconnect() { _snapshot.wifiReconnectCount++; }
void HealthSupervisor::recordMqttReconnect() { _snapshot.mqttReconnectCount++; }
void HealthSupervisor::recordHeartbeat(TaskId id) {
  if ((uint8_t)id >= TASK_COUNT) return;
  _lastHeartbeatMs[(uint8_t)id] = millis();
}

void HealthSupervisor::setSensorHealth(const char* name, Core::SensorHealth s) {
  if (strcmp(name, "ina219") == 0)        _ina219Health = s;
  else if (strcmp(name, "batteryAdc") == 0) _batteryAdcHealth = s;
  else if (strcmp(name, "acs712") == 0)    _acs712Health = s;
  else if (strcmp(name, "sht31") == 0)     _sht31Health = s;
}
void HealthSupervisor::setTimeQuality(Core::TimeQuality q) {
  _timeQuality = q;
  _snapshot.timeQuality = q;
  if (q == Core::TimeQuality::Unsynced) {
    alarms.raise("TIME_UNSYNCED", Core::AlarmSeverity::Warning,
                 "Time not synced — telemetry timestamps unreliable");
  } else {
    alarms.clear("TIME_UNSYNCED");
  }
}

uint32_t HealthSupervisor::nextTelemetrySequence() {
  _snapshot.telemetrySequence++;
  return _snapshot.telemetrySequence;
}

void HealthSupervisor::tick() {
  if (!_initialized) return;
  unsigned long now = millis();
  if (now - _lastTickMs < 1000) return;
  _lastTickMs = now;

  _snapshot.uptimeSeconds = millis() / 1000;
  _snapshot.freeHeap = ESP.getFreeHeap();
  _snapshot.minFreeHeap = ESP.getMinFreeHeap();
  if (_snapshot.minFreeHeap > _snapshot.freeHeap) {
    _snapshot.minFreeHeap = _snapshot.freeHeap;
  }

  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    if (_lastHeartbeatMs[i] == 0) _snapshot.taskHeartbeatAgeMs[i] = (uint32_t)-1;
    else                          _snapshot.taskHeartbeatAgeMs[i] = now - _lastHeartbeatMs[i];
  }

  static const char* TASK_NAMES[TASK_COUNT] = {
    "INA219_BATTERY", "ACS712", "ADC_VOLTAGE", "SHT31",
    "MQTT", "TELEMETRY", "OTA", "HEALTH_MONITOR", "PERSISTENCE"
  };
  for (uint8_t i = 0; i < TASK_COUNT; i++) {
    if (_snapshot.taskHeartbeatAgeMs[i] == (uint32_t)-1) continue;
    if (_snapshot.taskHeartbeatAgeMs[i] > 30000) {
      char buf[80];
      snprintf(buf, sizeof(buf), "Task %s stalled — heartbeat age %ums",
               TASK_NAMES[i], _snapshot.taskHeartbeatAgeMs[i]);
      alarms.raise("TASK_STALL", Core::AlarmSeverity::Warning, buf);
    }
  }

  if (_snapshot.freeHeap < 20000) {
    alarms.raise("LOW_HEAP", Core::AlarmSeverity::Warning,
                 "Free heap below 20 KB");
  }
  _snapshot.highestAlarm = alarms.highestActiveSeverity();
  _recomputeSystemState();
}

void HealthSupervisor::_recomputeSystemState() {
  Core::SystemState newState = Core::SystemState::Healthy;

  if (_snapshot.bootLoopDetected) {
    newState = Core::SystemState::Failed;
  } else if (!_snapshot.filesystemOk || !_snapshot.nvsOk) {
    newState = Core::SystemState::Failed;
  } else {
    bool anySensorFault = false;
    if (_ina219Health == Core::SensorHealth::Offline ||
        _ina219Health == Core::SensorHealth::Error) anySensorFault = true;
    if (_batteryAdcHealth == Core::SensorHealth::Offline ||
        _batteryAdcHealth == Core::SensorHealth::Error) anySensorFault = true;
    if (_acs712Health == Core::SensorHealth::Offline ||
        _acs712Health == Core::SensorHealth::Error) anySensorFault = true;
    if (_sht31Health == Core::SensorHealth::Offline ||
        _sht31Health == Core::SensorHealth::Error) anySensorFault = true;
    if (anySensorFault) newState = Core::SystemState::Degraded;

    for (uint8_t i = 0; i < TASK_COUNT; i++) {
      if (_snapshot.taskHeartbeatAgeMs[i] > 30000) {
        newState = Core::SystemState::Degraded;
        break;
      }
    }
    if (_snapshot.highestAlarm == Core::AlarmSeverity::Warning &&
        (uint8_t)newState < (uint8_t)Core::SystemState::Warning) {
      newState = Core::SystemState::Warning;
    }
    if (_snapshot.highestAlarm == Core::AlarmSeverity::Critical) {
      newState = Core::SystemState::Failed;
    }
  }
  _snapshot.systemState = newState;
}

} // namespace Services
