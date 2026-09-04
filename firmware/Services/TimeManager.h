// =============================================================================
// Services/TimeManager.h — NTP sync + monotonic uptime fallback
// -----------------------------------------------------------------------------
// Per brief §18: TIME_VALID / TIME_UNSYNCED. Never fabricate timestamps.
// Default timezone: Asia/Jakarta (configurable).
// Monotonic uptime fallback — used as last resort when NTP unavailable.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_TIME_MANAGER_H
#define PLTS_SERVICES_TIME_MANAGER_H

#include <Arduino.h>
#include "../Core/Types.h"

namespace Services {

class TimeManager {
public:
  void begin();
  void tick();
  bool isSynced() const { return _synced; }
  uint32_t getUnixTime() const;
  Core::TimeQuality getQuality() const {
    return _synced ? Core::TimeQuality::Valid : Core::TimeQuality::Unsynced;
  }
  void setSyncedFromNtp(uint32_t unixSec);
  // Monotonic fallback (uptime seconds since boot)
  uint32_t getUptimeSec() const { return millis() / 1000; }
  // Start NTP sync (returns immediately; tick() drives completion)
  void startNtpSync();

private:
  bool _synced = false;
  uint32_t _syncedUnixTime = 0;
  unsigned long _syncedAtMs = 0;
  unsigned long _lastNtpAttemptMs = 0;
};

extern TimeManager timeManager;

} // namespace Services

#endif // PLTS_SERVICES_TIME_MANAGER_H
