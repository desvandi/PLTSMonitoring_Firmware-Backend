// =============================================================================
// Services/LogService.cpp
// =============================================================================
#include "LogService.h"
#include "../Storage/FileSystem.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Drivers/RtcDriver.h"
#include <ArduinoJson.h>
#include <Preferences.h>   // [audit-2 S-17] preserveAuditLogAcrossReset

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
  // [p.472] BOOT GUARD — fail-closed. LogService is the system-wide log
  // sink: without its mutex, append()/audit() from up to seven tasks mutate
  // the ring AND the LittleFS mirror concurrently (LittleFS has no internal
  // locking — a corrupt log is the good outcome; FS-layer crashes are the
  // bad one). begin() runs first in setup() (firmware_v1.ino:307) while the
  // task watchdog is already subscribed — so on failure we log to Serial
  // (this service cannot log into itself) and halt WITHOUT feeding the
  // watchdog: the ~10 s TWDT panic reset records an honest reset reason in
  // the crash chain (BOOT/CRASHLOOP), and the device never enters the
  // multi-task state with an unlocked log service.
  if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();
  if (_mutex == nullptr) {
    Serial.println(F("[FATAL] LogService mutex creation failed (heap exhausted at boot) "
                    "— refusing to enter multi-task state (p.472 fail-closed)"));
    Serial.flush();
    while (true) {
      delay(10000);   // no esp_task_wdt_reset() on purpose → panic reset
    }
  }
  _head = 0; _count = 0; _nextId = 1;
  _auditBuf = "";
  _auditDirty = false;
}

// [p.472] Fail-closed acquisition, same shape as AlarmRegistry::_lock()
// (round-9 p.471) with ONE deliberate difference: the failure log is
// Serial-only. LogService is the log sink — calling Log.append() here would
// recurse into the very service whose mutex is unavailable.
bool LogService::_lock() const {
  if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();   // retry-create ONCE
  if (_mutex != nullptr) {
    // [FW-24] Bounded wait preserved: log contention is tolerable — drop
    // rather than block a task. A timeout returns false WITHOUT counting a
    // lockFailure (the mutex exists; this is the designed drop-on-contention
    // semantics, not the p.472 unavailable-mutex class).
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) return true;
    return false;
  }
  // FAIL-CLOSED: no mutex → no log access. Count + rate-limited Serial CRIT
  // (atomic accounting — no synchronization exists on this path).
  _lockFailures.fetch_add(1, std::memory_order_relaxed);
  uint32_t last = _lastLockFailLogMs.load(std::memory_order_relaxed);
  uint32_t now = millis();
  if (now - last > 60000UL &&
      _lastLockFailLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    Serial.printf("[LOG][CRIT] LogService mutex UNAVAILABLE — operation REJECTED "
                  "(fail-closed, p.472); lockFailures=%lu\n",
                  (unsigned long)_lockFailures.load(std::memory_order_relaxed));
  }
  return false;
}

void LogService::_unlock() const {
  // Only ever called after _lock() returned TRUE — the handle is non-null
  // and HELD here by construction.
  xSemaphoreGive(_mutex);
}

void LogService::append(Core::LogType type, const String& message, int8_t channel) {
  // [FW-24] Lock across the ring mutation AND the filesystem mirror —
  // LittleFS has no internal locking; concurrent open/append from multiple
  // tasks corrupts the log or crashes the FS layer.
  // [p.472] Fail-closed: mutex unavailable → the entry is DROPPED (same
  // degradation as the FW-24 contention drop, now also covering the
  // no-lock case) and counted in lockFailures().
  if (!_lock()) {
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
  _unlock();
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
  // [p.472] Fail-closed: no mutex → no audit line (counted, visible).
  if (!_lock()) return;
  uint32_t ts = Drivers::rtc.getUnixTime();
  char line[256];
  snprintf(line, sizeof(line), "[%u] action=%s param=%s old=%s new=%s src=%s rev=%u\n",
           ts, action, parameter, oldValue, newValue, source, revision);
  _auditBuf += line;
  _auditDirty = true;
  _rotateAuditIfNeeded();
  _unlock();
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
  // [p.472] Fail-closed: _auditDirty is read under the lock (an unlocked
  // read is exactly the reader race this round removes). No mutex → no
  // flush (the buffer stays RAM-only until the lock returns; counted).
  if (!_lock()) return;
  if (!_auditDirty) { _unlock(); return; }
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "a");
  if (!f) { _unlock(); return; }
  f.print(_auditBuf);
  f.close();
  _auditBuf = "";
  _auditDirty = false;
  _unlock();
}

String LogService::getActivityJson(uint16_t limit, int8_t filterType) const {
  // [p.472] READER-SIDE SERIALIZATION. This getter runs in the web-server
  // task while append() mutates _head/_count/_entries[] from up to seven
  // writer tasks — the old unlocked read could interleave with a writer
  // mid-entry (torn message) or with a _head advance between the start-index
  // computation and the entry reads (incoherent snapshot). Fail-closed to an
  // honest empty list flagged with lockUnavailable.
  if (!_lock()) {
    return String("{\"logs\":[],\"lockUnavailable\":true}");
  }
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
  _unlock();
  String out; serializeJson(doc, out);
  return out;
}

String LogService::getAuditText(uint16_t maxBytes) const {
  // [p.472] Reader-side serialization: _auditBuf is a String mutated by
  // audit()/_rotateAuditIfNeeded()/flushToDisk() from writer tasks; copying
  // it unlocked could observe a moved/rotated buffer mid-operation.
  if (!_lock()) {
    return String();   // honest empty — the degraded mode is flagged by lockFailures()
  }
  String s = _auditBuf;
  // Append FS-stored audit log too
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "r");
  if (f) {
    while (f.available() && s.length() < maxBytes) {
      s += (char)f.read();
    }
    f.close();
  }
  _unlock();
  if (s.length() > maxBytes) s = s.substring(s.length() - maxBytes);
  return s;
}

// [p.472] Reader-side serialization for the advisory counters; fail-closed
// to 0 (conservative) when the mutex is unavailable.
uint16_t LogService::getActivityCount() const {
  if (!_lock()) return 0;
  uint16_t c = _count;
  _unlock();
  return c;
}
uint16_t LogService::getAuditBytes() const {
  if (!_lock()) return 0;
  uint16_t b = (uint16_t)_auditBuf.length();
  _unlock();
  return b;
}

// [audit-2 S-17] Preserve audit log across factory reset. Save the current
// LittleFS PATH_AUDIT_LOG into NVS namespace "plts_audit" before format,
// then restore after. NVS is not wiped by LittleFS.format(). The audit log
// is capped at 4 KB (most recent) to fit in NVS.
bool preserveAuditLogAcrossReset() {
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "r");
  if (!f) return false;
  // Read last 4 KB (most recent entries)
  static const size_t MAX_AUDIT_PRESERVE = 4096;
  if (f.size() > MAX_AUDIT_PRESERVE) {
    f.seek(f.size() - MAX_AUDIT_PRESERVE);
  }
  uint8_t buf[MAX_AUDIT_PRESERVE];
  size_t n = f.read(buf, sizeof(buf));
  f.close();
  Preferences p;
  if (!p.begin("plts_audit", false)) return false;
  p.putBytes("log", buf, n);
  p.putBool("valid", true);
  p.end();
  return true;
}

void restoreAuditLogAfterReset() {
  Preferences p;
  if (!p.begin("plts_audit", true)) return;
  if (!p.getBool("valid", false)) { p.end(); return; }
  size_t n = p.getBytesLength("log");
  if (n == 0 || n > 4096) { p.end(); return; }
  uint8_t* buf = (uint8_t*)malloc(n);
  if (!buf) { p.end(); return; }
  size_t got = p.getBytes("log", buf, n);
  p.end();
  if (got != n) { free(buf); return; }
  Storage::fs.raw().mkdir("/log");
  File f = Storage::fs.raw().open(Core::PATH_AUDIT_LOG, "w");
  if (f) {
    f.write(buf, got);
    f.close();
  }
  free(buf);
  // Clear the NVS backup so we don't restore twice
  Preferences p2;
  if (p2.begin("plts_audit", false)) {
    p2.clear();
    p2.end();
  }
}

} // namespace Services
