// =============================================================================
// Drivers/RtcDriver.h — Optional DS3231 RTC or NTP-backed software RTC
// -----------------------------------------------------------------------------
// Provides Unix epoch time + TimeQuality (Valid/Unsynced).
// Never fabricates timestamps: if unsynced, exposes uptime-only fallback.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_RTC_H
#define PLTS_DRIVERS_RTC_H

#include <Arduino.h>
#include <cstdint>
#include "../Core/Types.h"

namespace Drivers {

class RtcDriver {
public:
  bool begin();  // tries DS3231; if absent, sets state UNSYNCED
  void tick();

  uint32_t getUnixTime();  // 0 if unsynced
  void     setUnixTime(uint32_t t);
  bool     isValid() const { return _quality == Core::TimeQuality::Valid; }
  Core::TimeQuality getQuality() const { return _quality; }

  // Sync from NTP (called by TimeManager)
  void     setFromNtp(uint32_t unixSec);
  // Monotonic fallback (uptime seconds since boot)
  uint32_t getUptimeSec() const { return millis() / 1000; }

  // Used by boot-loop detection: timestamp this boot.
  void     recordBootTime();

private:
  bool     _hwPresent = false;
  Core::TimeQuality _quality = Core::TimeQuality::Unsynced;
  uint32_t _syncedUnixTime = 0;       // last NTP/RTC sync point
  uint32_t _syncedAtMs = 0;          // millis() at sync
  // DS3231 register helpers
  bool _ds3231Read(uint8_t reg, uint8_t& out);
  bool _ds3231Write(uint8_t reg, uint8_t val);
  bool _ds3231GetTime(struct tm& t);
};

extern RtcDriver rtc;

} // namespace Drivers

// Globals used by Crypto (JWT sign/verify) — defined in main .ino
uint32_t getCurrUnixTime();
uint32_t getMonotonicSec();

#endif // PLTS_DRIVERS_RTC_H
