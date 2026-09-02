// =============================================================================
// Services/HealthSupervisor.h — 5-state health (brief §44)
// -----------------------------------------------------------------------------
// States: HEALTHY / WARNING / DEGRADED / FAILED / RECOVERING
// 9 tasks (brief §78): Ina219Battery, Acs712, AdcVoltage, Sht31, Mqtt, Telemetry,
//                       Ota, HealthMonitor, Persistence.
// Heartbeat timeout: 10s → WARNING, 30s → DEGRADED.
// Boot-loop detection: 3 boots in 60s → RECOVERING.
// Reset-reason classifier.
// Watchdog counter + brownout counter persisted to NVS.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_HEALTH_SUPERVISOR_H
#define PLTS_SERVICES_HEALTH_SUPERVISOR_H

#include <Arduino.h>
#include <cstdint>
#include "../Core/Types.h"
#include "AlarmRegistry.h"

namespace Services {

// RC-11: TaskId is now canonical in Core::Types.h (Core::TaskId).
// Removed duplicate Services::TaskId enum.
using Core::TaskId;
using Core::TASK_COUNT;

struct HealthSnapshot {
  uint32_t uptimeSeconds;
  uint32_t bootCount;
  uint8_t  lastResetReason;
  uint32_t watchdogResets;
  uint32_t brownoutResets;
  size_t   freeHeap;
  size_t   minFreeHeap;
  size_t   largestFreeBlock;
  int8_t   wifiRssi;
  uint32_t wifiReconnectCount;
  uint32_t mqttReconnectCount;
  Core::TimeQuality timeQuality;
  Core::SensorHealth ina219Health;
  Core::SensorHealth batteryAdcHealth;
  Core::SensorHealth acs712Health;
  Core::SensorHealth sht31Health;
  bool     filesystemOk;
  bool     nvsOk;
  bool     mqttConnected;
  uint32_t telemetrySequence;
  uint32_t taskHeartbeatAgeMs[TASK_COUNT];
  Core::AlarmSeverity highestAlarm;
  Core::SystemState systemState;
  bool     bootLoopDetected;
  uint8_t  bootsInLast60s;
};

class HealthSupervisor {
public:
  void begin();
  void tick();
  void recordBoot();
  void recordWifiReconnect();
  void recordMqttReconnect();
  void recordHeartbeat(TaskId id);

  void setSensorHealth(const char* name, Core::SensorHealth s);
  void setTimeQuality(Core::TimeQuality q);
  Core::TimeQuality getTimeQuality() const { return _timeQuality; }

  HealthSnapshot getSnapshot() const { return _snapshot; }
  uint32_t nextTelemetrySequence();
  Core::SystemState getSystemState() const { return _snapshot.systemState; }

  static const char* resetReasonStr(uint8_t reason);

  bool shouldInhibitOta() const {
    return _snapshot.systemState == Core::SystemState::Failed ||
           _snapshot.bootLoopDetected;
  }
  bool shouldInhibitConfigChanges() const {
    return _snapshot.systemState == Core::SystemState::Failed;
  }

private:
  HealthSnapshot _snapshot = {};
  Core::TimeQuality _timeQuality = Core::TimeQuality::Unsynced;
  Core::SensorHealth _ina219Health   = Core::SensorHealth::Offline;
  Core::SensorHealth _batteryAdcHealth = Core::SensorHealth::Offline;
  Core::SensorHealth _acs712Health    = Core::SensorHealth::Offline;
  Core::SensorHealth _sht31Health     = Core::SensorHealth::Offline;
  unsigned long _lastHeartbeatMs[TASK_COUNT] = {};
  unsigned long _lastTickMs = 0;
  bool _initialized = false;

  static constexpr uint8_t BOOT_LOOP_WINDOW = 8;
  uint32_t _bootTimestamps[BOOT_LOOP_WINDOW] = {};
  uint8_t  _bootTimestampIdx = 0;

  void _recomputeSystemState();
};

extern HealthSupervisor health;

} // namespace Services

#endif // PLTS_SERVICES_HEALTH_SUPERVISOR_H
