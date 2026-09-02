// =============================================================================
// Services/LogService.cpp
// =============================================================================
#include "LogService.h"
#include "../Storage/FileSystem.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Drivers/RtcDriver.h"
#include <ArduinoJson.h>

namespace Services {

LogService Log;

static const char* logTypeStr(Core::LogType t) {
  switch (t) {
    case Core::LogType::Boot:           return "DEVICE_BOOT";
    case Core::LogType::WifiConnected:        return "WIFI_CONNECTED";
    case Core::LogType::WifiDisconnected:     return "WIFI_DISCONNECTED";
    case Core::LogType::TimeSynced:            return "TIME_SYNCED";
    case Core::LogType::SensorFailure:        return "SENSOR_FAILURE";
    case Core::LogType::SensorRecovered:      return "SENSOR_RECOVERED";
    case Core::LogType::AlarmActive:            return "ALARM_ACTIVE";
    case Core::LogType::AlarmAcknowledged:      return "ALARM_ACKNOWLEDGED";
    case Core::LogType::AlarmCleared:           return "ALARM_CLEARED";
    case Core::LogType::SocBaselineCorrected:   return "SOC_BASELINE_CORRECTED";
    case Core::LogType::CalibrationChanged:    return "CALIBRATION_CHANGED";
    case Core::LogType::ConfigurationChanged:   return "CONFIGURATION_CHANGED";
    case Core::LogType::OtaStarted:              return "OTA_STARTED";
    case Core::LogType::OtaSuccess:             return "OTA_SUCCESS";
    case Core::LogType::OtaFailed:               return "OTA_FAILED";
    case Core::LogType::StorageError:           return "STORAGE_ERROR";
    case Core::LogType::Login:                    return "LOGIN";
    case Core::LogType::Logout:                   return "LOGOUT";
    case Core::LogType::AuthFail:                 return "AUTH_FAIL";
    case Core::LogType::Reboot:                   return "REBOOT";
    case Core::LogType::FactoryReset:             return "FACTORY_RESET";
    case Core::LogType::Info:                    return "INFO";
    case Core::LogType::Custom:                    return "ERROR";
  }
  return "UNKNOWN";
}

void LogService::begin() {
  _head = 0; _count = 0; _nextId = 1;
  _auditBuf = "";
  _auditDirty = false;
  // [FW-24] Cross-task serialization for RAM ring + LittleFS mirror writes.
  if (!_mutex) _mutex = xSemaphoreCreateMutex();
}

void LogService::append(Core::LogType type, const String& message, int8_t channel) {
  // [FW-24] Lock across the ring mutation AND the filesystem mirror —
  // LittleFS has no internal locking; concurrent open/append from multiple
  // tasks corrupts the log or crashes the FS layer.
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return;   // log contention is tolerable — drop rather than block a task
  }
  Entry& e = _entries[_head];
  e.id = _nextId++;
  e.timestamp = Drivers::rtc.getUnixTime();
  e.type = type;
  e.channel = channel;
  strncpy(e.message, message.c_str(), sizeof(e.message) - 1);
  e.message[sizeof(e.message) - 1] = '\0';
  _head = (_head + 1) % MAX_ENTRIES;
  if (_count < MAX_ENTRIES) _count++;

  _writeActivityToFs(e);
  if (_mutex) xSemaphoreGive(_mutex);
}

void LogService::_writeActivityToFs(const Entry& e) {
  // Append JSON line to activity log
  File f = Storage::fs.raw().open(Core::PATH_ACTIVITY_LOG, "a");
  if (!f) return;
  StaticJsonDocument<256> doc;
  doc["id"] = e.id;
  doc["timestamp"] = e.timestamp;
  doc["type"] = logTypeStr(e.type);
  doc["channel"] = e.channel;
  doc["message"] = e.message;
  String line;
  serializeJson(doc, line);
  f.println(line);
  f.close();
  // Rotate if too large
  size_t sz = LittleFS.totalBytes();
  if (LittleFS.usedBytes() > sz * 8 / 10) {
    Storage::fs.raw().remove("/activity.log.old");
    Storage::fs.raw().rename(Core::PATH_ACTIVITY_LOG, "/activity.log.old");
  }
}

void LogService::audit(const char* action, const char* parameter,
                       const char* oldValue, const char* newValue,
                       const char* source, uint32_t revision) {
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  uint32_t ts = Drivers::rtc.getUnixTime();
  char line[256];
  snprintf(line, sizeof(line), "[%u] action=%s param=%s old=%s new=%s src=%s rev=%u\n",
           ts, action, parameter, oldValue, newValue, source, revision);
  _auditBuf += line;
  _auditDirty = true;
  _rotateAuditIfNeeded();
  if (_mutex) xSemaphoreGive(_mutex);
}

void LogService::_rotateAuditIfNeeded() {
  if (_auditBuf.length() < Core::AUDIT_LOG_ROTATE_BYTES) return;
  // [FW-24 REMEDIATION 2026-08] The old rotation opened the audit log in "w"
  // (TRUNCATE) mode and wrote ONLY the RAM buffer — every byte previously
  // flushed to disk by flushToDisk() was DESTROYED on each rotation.
  // Correct rotation: existing file → audit.log.old, buffer → fresh file.
  Storage::fs.raw().remove("/audit.log.old");
  Storage::fs.raw().rename(Core::PATH_AUDIT_LOG, "/audit.log.old");
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "w");
  if (f) { f.print(_auditBuf); f.close(); }
  _auditBuf = "";
  _auditDirty = false;
}

void LogService::flushToDisk() {
  if (!_auditDirty) return;
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "a");
  if (!f) { if (_mutex) xSemaphoreGive(_mutex); return; }
  f.print(_auditBuf);
  f.close();
  _auditBuf = "";
  _auditDirty = false;
  if (_mutex) xSemaphoreGive(_mutex);
}

String LogService::getActivityJson(uint16_t limit, int8_t filterType) const {
  StaticJsonDocument<8192> doc;
  JsonArray arr = doc.createNestedArray("logs");
  uint16_t start = (_count < MAX_ENTRIES) ? 0 : _head;
  uint16_t emitted = 0;
  for (uint16_t i = 0; i < _count && emitted < limit; i++) {
    uint16_t idx = (start + i) % MAX_ENTRIES;
    const Entry& e = _entries[idx];
    if (filterType >= 0 && (int8_t)e.type != filterType) continue;
    JsonObject o = arr.createNestedObject();
    o["id"] = e.id;
    o["timestamp"] = e.timestamp;
    o["type"] = logTypeStr(e.type);
    o["channel"] = e.channel;
    o["message"] = e.message;
    emitted++;
  }
  String out; serializeJson(doc, out);
  return out;
}

String LogService::getAuditText(uint16_t maxBytes) const {
  String s = _auditBuf;
  // Append FS-stored audit log too
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "r");
  if (f) {
    while (f.available() && s.length() < maxBytes) {
      s += (char)f.read();
    }
    f.close();
  }
  if (s.length() > maxBytes) s = s.substring(s.length() - maxBytes);
  return s;
}

uint16_t LogService::getActivityCount() const { return _count; }
uint16_t LogService::getAuditBytes() const { return _auditBuf.length(); }

} // namespace Services
