// =============================================================================
// Drivers/RtcDriver.cpp — DS3231 RTC + NTP fallback
// =============================================================================
#include "RtcDriver.h"
#include "../Core/Config.h"
#include <Wire.h>
#include <time.h>

namespace Drivers {

RtcDriver rtc;

bool RtcDriver::_ds3231Read(uint8_t reg, uint8_t& out) {
  Wire.beginTransmission(0x68);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x68, 1) != 1) return false;
  out = Wire.read();
  return true;
}

bool RtcDriver::_ds3231Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x68);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool RtcDriver::_ds3231GetTime(struct tm& t) {
  uint8_t sec, min, hr, wd, day, mon, yr;
  if (!_ds3231Read(0x00, sec) || !_ds3231Read(0x01, min) ||
      !_ds3231Read(0x02, hr)  || !_ds3231Read(0x03, wd) ||
      !_ds3231Read(0x04, day) || !_ds3231Read(0x05, mon) ||
      !_ds3231Read(0x06, yr)) return false;
  t.tm_sec  = bcd2dec(sec & 0x7F);
  t.tm_min  = bcd2dec(min);
  t.tm_hour = bcd2dec(hr & 0x3F);
  t.tm_wday = bcd2dec(wd) - 1;
  t.tm_mday = bcd2dec(day);
  t.tm_mon  = bcd2dec(mon & 0x1F) - 1;
  t.tm_year = bcd2dec(yr) + 100;  // 2000 + yr - 1900
  return true;
}

bool RtcDriver::begin() {
  // Detect DS3231 by writing 0 then reading 0x00 register
  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() == 0) {
    _hwPresent = true;
    // Sanity: year must be plausible (>= 2024)
    struct tm t;
    if (_ds3231GetTime(t) && (t.tm_year + 1900) >= 2024) {
      _quality = Core::TimeQuality::Valid;
      time_t epoch = mktime(&t);
      _syncedUnixTime = (uint32_t)epoch;
      _syncedAtMs = millis();
      Serial.printf("[RTC] DS3231 valid: epoch=%u\n", _syncedUnixTime);
    } else {
      _quality = Core::TimeQuality::Unsynced;
      Serial.println(F("[RTC] DS3231 present but time not plausible — needs NTP sync"));
    }
  } else {
    _hwPresent = false;
    _quality = Core::TimeQuality::Unsynced;
    Serial.println(F("[RTC] No DS3231 detected — using NTP/uptime fallback"));
  }
  return true;
}

void RtcDriver::tick() {
  // If we have DS3231, periodically verify time is still valid.
  // Quality is updated by TimeManager on NTP sync.
  // Nothing to do here for now — kept for future health checks.
}

uint32_t RtcDriver::getUnixTime() {
  if (_quality == Core::TimeQuality::Valid) {
    if (_hwPresent) {
      struct tm t;
      if (_ds3231GetTime(t)) {
        time_t e = mktime(&t);
        return (uint32_t)e;
      }
    }
    // Software: synced time + (millis() - syncedAtMs) / 1000
    uint32_t delta = (millis() - _syncedAtMs) / 1000;
    return _syncedUnixTime + delta;
  }
  return 0;  // unsynced — DO NOT fabricate
}

void RtcDriver::setUnixTime(uint32_t t) {
  _syncedUnixTime = t;
  _syncedAtMs = millis();
  _quality = Core::TimeQuality::Valid;
}

void RtcDriver::setFromNtp(uint32_t unixSec) {
  setUnixTime(unixSec);
  // Push to DS3231 if present
  if (_hwPresent) {
    time_t e = (time_t)unixSec;
    struct tm* t = localtime(&e);
    _ds3231Write(0x00, dec2bcd(t->tm_sec));
    _ds3231Write(0x01, dec2bcd(t->tm_min));
    _ds3231Write(0x02, dec2bcd(t->tm_hour));
    _ds3231Write(0x03, dec2bcd(t->tm_wday + 1));
    _ds3231Write(0x04, dec2bcd(t->tm_mday));
    _ds3231Write(0x05, dec2bcd(t->tm_mon + 1));
    _ds3231Write(0x06, dec2bcd((t->tm_year + 1900) - 2000));
  }
  Serial.printf("[RTC] NTP sync: epoch=%u (DS3231=%s)\n",
                unixSec, _hwPresent ? "yes" : "no");
}

void RtcDriver::recordBootTime() {
  // Record boot time for boot-loop detection (NVS-side, handled in HealthSupervisor)
}

} // namespace Drivers

// Global helpers (declared in RtcDriver.h) — used by Crypto for JWT iat/exp
uint32_t getCurrUnixTime() { return Drivers::rtc.getUnixTime(); }
uint32_t getMonotonicSec() { return (uint32_t)(millis() / 1000); }

// Global time validity flag referenced by Common.h::timeIsValid()
bool g_timeValid = false;
uint32_t g_timeUnixEpoch = 0;
