// =============================================================================
// Services/LogService.h — Activity log (JSON-lines, rotates @ 200) +
//                          Audit log (plain text, rotates @ 8KB)
// -----------------------------------------------------------------------------
// Brief §61 — events:
//   DEVICE_BOOT, WIFI_CONNECTED, WIFI_DISCONNECTED, TIME_SYNCED,
//   SENSOR_FAILURE, SENSOR_RECOVERED, ALARM_ACTIVE, ALARM_ACKNOWLEDGED,
//   ALARM_CLEARED, SOC_BASELINE_CORRECTED, CALIBRATION_CHANGED,
//   CONFIGURATION_CHANGED, OTA_STARTED, OTA_SUCCESS, OTA_FAILED,
//   STORAGE_ERROR.
//
// NEVER log passwords, tokens, or secrets.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_LOG_SERVICE_H
#define PLTS_SERVICES_LOG_SERVICE_H

#include <Arduino.h>
#include "../Core/Types.h"
#include "../Core/Config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Services {

class LogService {
public:
  void begin();
  // Append to activity log (JSON-lines, ring buffer in RAM + mirror to FS)
  // [FW-24 REMEDIATION 2026-08] append()/audit()/flushToDisk() are called from
  // MULTIPLE FreeRTOS tasks (sensor, measurement, energy, telemetry, network,
  // health, persistence) with no synchronization — LittleFS is not thread-safe
  // and the RAM ring raced. All public mutators now take _mutex.
  void append(Core::LogType type, const String& message, int8_t channel = -1);
  // Append to audit log (plain text, used for config/calibration changes)
  void audit(const char* action, const char* parameter,
             const char* oldValue, const char* newValue,
             const char* source, uint32_t revision);
  // Activity log retrieval
  String getActivityJson(uint16_t limit, int8_t filterType) const;
  String getAuditText(uint16_t maxBytes) const;
  uint16_t getActivityCount() const;
  uint16_t getAuditBytes() const;
  // Flush in-RAM log to FS (called by PersistenceTask)
  void flushToDisk();

private:
  static constexpr uint16_t MAX_ENTRIES = Core::MAX_ACTIVITY_LOG_ENTRIES;
  struct Entry {
    uint32_t id;
    uint32_t timestamp;
    Core::LogType type;
    int8_t channel;
    char message[96];
  };
  Entry _entries[MAX_ENTRIES] = {};
  uint16_t _head = 0;
  uint16_t _count = 0;
  uint32_t _nextId = 1;
  String _auditBuf;
  bool _auditDirty = false;
  SemaphoreHandle_t _mutex = nullptr;   // [FW-24] created in begin()

  void _writeActivityToFs(const Entry& e);
  void _rotateAuditIfNeeded();
};

extern LogService Log;

} // namespace Services

#endif // PLTS_SERVICES_LOG_SERVICE_H
