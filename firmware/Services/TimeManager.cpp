// =============================================================================
// Services/TimeManager.cpp
// =============================================================================
#include "TimeManager.h"
#include "../Core/Globals.h"
#include "../Drivers/RtcDriver.h"
#include "../Core/Config.h"
#include "../Utils/Crypto.h"
#include "HealthSupervisor.h"
#include "LogService.h"
#include <time.h>
#include <WiFi.h>

namespace Services {

TimeManager timeManager;

void TimeManager::begin() {
  _synced = false;
  // If RtcDriver reports valid time (e.g. from DS3231), adopt it
  if (Drivers::rtc.isValid()) {
    _synced = true;
    _syncedUnixTime = Drivers::rtc.getUnixTime();
    _syncedAtMs = millis();
    Services::health.setTimeQuality(Core::TimeQuality::Valid);
    Log.append(Core::LogType::TimeSynced, "Time valid from RTC at boot", 0);
  }
  startNtpSync();
}

void TimeManager::startNtpSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - _lastNtpAttemptMs < 30000) return;
  _lastNtpAttemptMs = millis();
  // Configure NTP with default Core::cfgTimezone
  configTzTime(Core::cfgTimezone, "pool.ntp.org", "time.nist.gov");
  Serial.println(F("[TIME] NTP sync initiated"));
}

void TimeManager::tick() {
  if (WiFi.status() != WL_CONNECTED) return;
  // Try to read NTP-synced time
  time_t now = time(nullptr);
  if (now > 1700000000) {  // sanity: epoch > Nov 2023
    if (!_synced) {
      _synced = true;
      _syncedUnixTime = (uint32_t)now;
      _syncedAtMs = millis();
      Drivers::rtc.setFromNtp((uint32_t)now);
      Services::health.setTimeQuality(Core::TimeQuality::Valid);
      Log.append(Core::LogType::TimeSynced, "NTP synced", 0);
      // [WAVE-6 / FW6-2] Persist the boot-epoch estimate the moment the wall
      // clock becomes TRUE — after a later reboot without NTP (no DS3231),
      // JWT lifetime math resumes from here instead of the 1700000000 base.
      Utils::persistEpochEstimate();
    }
  } else {
    // NTP not yet synced
    if (millis() - _lastNtpAttemptMs > 60000) {
      startNtpSync();
    }
  }
}

uint32_t TimeManager::getUnixTime() const {
  if (_synced) return _syncedUnixTime + (millis() - _syncedAtMs) / 1000;
  return 0;
}

void TimeManager::setSyncedFromNtp(uint32_t unixSec) {
  _synced = true;
  _syncedUnixTime = unixSec;
  _syncedAtMs = millis();
  Drivers::rtc.setFromNtp(unixSec);
  Services::health.setTimeQuality(Core::TimeQuality::Valid);
  // [WAVE-6 / FW6-2] Same rationale as tick(): every true wall-clock source
  // refreshes the persisted boot-epoch estimate.
  Utils::persistEpochEstimate();
}

} // namespace Services
