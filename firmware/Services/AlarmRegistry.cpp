// =============================================================================
// Services/AlarmRegistry.cpp
// =============================================================================
#include "AlarmRegistry.h"
#include "LogService.h"
#include "../Core/Types.h"
#include "../Drivers/RtcDriver.h"
#include "../Utils/Crc.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace Services {

AlarmRegistry alarms;

// [FW-23] NVS persistence layout — versioned + CRC-protected
namespace {
constexpr uint32_t ALARM_NVS_MAGIC = 0x414C524Du;   // "ALRM"
constexpr uint32_t ALARM_STATE_VERSION = 1u;
struct AlarmPersistHeader {
  uint32_t magic;
  uint32_t version;
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t crc32;             // over header-without-crc + alarm array
};
}

void AlarmRegistry::begin() {
  _count = 0;
  _dirty = false;
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    _alarms[i] = {};
    _alarms[i].code[0] = '\0';
  }
  // [FW-23] Restore alarm state — alarm conditions surviving a reboot is
  // part of deterministic recovery (directive §21: alarm state must be
  // recovered after every reset).
  loadFromNVS();
}

uint8_t AlarmRegistry::_findIdx(const char* code) const {
  if (!code) return 0xFF;
  for (uint8_t i = 0; i < _count; i++) {
    if (strncmp(_alarms[i].code, code, Alarm::CODE_LEN) == 0) return i;
  }
  return 0xFF;
}

void AlarmRegistry::raise(const char* code, Core::AlarmSeverity sev, const char* message) {
  if (!code) return;
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) {
    // New alarm
    if (_count >= MAX_ALARMS) {
      // Evict oldest CLEARED
      uint8_t oldest = 0xFF;
      uint32_t oldestTime = 0xFFFFFFFF;
      for (uint8_t i = 0; i < _count; i++) {
        if (_alarms[i].lifecycle == Core::AlarmLifecycle::Cleared &&
            _alarms[i].clearedAt < oldestTime) {
          oldestTime = _alarms[i].clearedAt;
          oldest = i;
        }
      }
      if (oldest != 0xFF) {
        for (uint8_t i = oldest; i < _count - 1; i++) _alarms[i] = _alarms[i + 1];
        _count--;
        idx = _count;
      } else {
        // No cleared alarms — drop the lowest-severity oldest active
        idx = 0;
        for (uint8_t i = 1; i < _count; i++) {
          if ((uint8_t)_alarms[i].severity < (uint8_t)_alarms[idx].severity) idx = i;
        }
      }
    }
    Alarm& a = _alarms[idx];
    strncpy(a.code, code, Alarm::CODE_LEN - 1);
    a.code[Alarm::CODE_LEN - 1] = '\0';
    a.severity = sev;
    a.lifecycle = Core::AlarmLifecycle::Active;
    a.raisedAt = Drivers::rtc.getUnixTime();
    a.acknowledgedAt = 0;
    a.clearedAt = 0;
    a.lastUpdatedAt = a.raisedAt;
    if (message) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
    } else {
      a.message[0] = '\0';
    }
    if (idx == _count) _count++;
    _dirty = true;
    Log.append(Core::LogType::AlarmActive,
               String("[ALARM:") + code + "] " + (message ? message : ""), -1);
  } else {
    // Refresh existing alarm
    Alarm& a = _alarms[idx];
    // Upgrade severity (never downgrade)
    if ((uint8_t)sev > (uint8_t)a.severity) a.severity = sev;
    a.lastUpdatedAt = Drivers::rtc.getUnixTime();
    if (message && message[0]) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
    }
  }
}

void AlarmRegistry::clear(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  Alarm& a = _alarms[idx];
  a.lifecycle = Core::AlarmLifecycle::Cleared;
  a.clearedAt = Drivers::rtc.getUnixTime();
  a.lastUpdatedAt = a.clearedAt;
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
  Log.append(Core::LogType::AlarmCleared, String("Alarm cleared: ") + code, -1);
}

void AlarmRegistry::acknowledge(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  Alarm& a = _alarms[idx];
  a.lifecycle = Core::AlarmLifecycle::Acknowledged;
  a.acknowledgedAt = Drivers::rtc.getUnixTime();
  a.lastUpdatedAt = a.acknowledgedAt;
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
  Log.append(Core::LogType::AlarmAcknowledged, String("Alarm acknowledged: ") + code, -1);
}

void AlarmRegistry::acknowledgeAll() {
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle == Core::AlarmLifecycle::Active) {
      _alarms[i].lifecycle = Core::AlarmLifecycle::Acknowledged;
      _alarms[i].acknowledgedAt = Drivers::rtc.getUnixTime();
    }
  }
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
}

uint8_t AlarmRegistry::countActive() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared) n++;
  }
  return n;
}

uint8_t AlarmRegistry::countAll() const { return _count; }

uint8_t AlarmRegistry::copyActiveAlarms(Alarm* dst, uint8_t max) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _count && n < max; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared) {
      dst[n++] = _alarms[i];
    }
  }
  return n;
}

void AlarmRegistry::saveToNVS() {
  AlarmPersistHeader hdr = {};
  hdr.magic = ALARM_NVS_MAGIC;
  hdr.version = ALARM_STATE_VERSION;
  hdr.count = _count;

  // CRC over the header (minus its own crc field) + the in-use alarm slots
  uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
  if (_count > 0) {
    crc = Utils::crc32((const uint8_t*)_alarms, _count * sizeof(Alarm), crc);
  }
  hdr.crc32 = crc;

  Preferences p;
  if (p.begin("plts_alarm", false)) {
    p.putBytes("hdr", &hdr, sizeof(hdr));
    if (_count > 0) p.putBytes("arr", _alarms, _count * sizeof(Alarm));
    p.end();
    _dirty = false;
  }
}

void AlarmRegistry::loadFromNVS() {
  Preferences p;
  if (!p.begin("plts_alarm", true)) return;
  AlarmPersistHeader hdr = {};
  size_t gotHdr = p.getBytes("hdr", &hdr, sizeof(hdr));
  if (gotHdr != sizeof(hdr) || hdr.magic != ALARM_NVS_MAGIC ||
      hdr.version != ALARM_STATE_VERSION || hdr.count > MAX_ALARMS) {
    p.end();
    return;
  }
  Alarm buf[MAX_ALARMS] = {};
  size_t gotArr = (hdr.count > 0) ? p.getBytes("arr", buf, hdr.count * sizeof(Alarm)) : 0;
  p.end();
  if (hdr.count > 0 && gotArr != hdr.count * sizeof(Alarm)) return;

  uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
  if (hdr.count > 0) {
    crc = Utils::crc32((const uint8_t*)buf, hdr.count * sizeof(Alarm), crc);
  }
  if (crc != hdr.crc32) {
    Log.append(Core::LogType::StorageError,
               "Persisted alarm state corrupt (CRC) — starting empty", -1);
    return;   // corrupt → empty, honest
  }
  for (uint8_t i = 0; i < hdr.count; i++) _alarms[i] = buf[i];
  _count = hdr.count;
  _dirty = false;
}

const Alarm* AlarmRegistry::getAlarm(uint8_t idx) const {
  if (idx >= _count) return nullptr;
  return &_alarms[idx];
}

const Alarm* AlarmRegistry::find(const char* code) const {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return nullptr;
  return &_alarms[idx];
}

Core::AlarmSeverity AlarmRegistry::highestActiveSeverity() const {
  Core::AlarmSeverity h = Core::AlarmSeverity::Info;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared &&
        (uint8_t)_alarms[i].severity > (uint8_t)h) {
      h = _alarms[i].severity;
    }
  }
  return h;
}

} // namespace Services
