// =============================================================================
// Drivers/Sht31Driver.h — SHT31 ambient T/H sensor (NOT battery temperature)
// -----------------------------------------------------------------------------
// I²C 0x44, non-blocking state machine (Idle → Converting → Ready).
// CRC-8 verification (poly 0x31, init 0xFF).
// Stale-data detection (last successful read > 15s → STALE).
// I2C failure recovery (same as INA219: 10 errors → 60s cooldown).
//
// IMPORTANT: This is AMBIENT / ENCLOSURE temperature, not battery temperature.
// Label must reflect this everywhere (brief §6).
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_SHT31_H
#define PLTS_DRIVERS_SHT31_H

#include <Arduino.h>
#include <cstdint>

namespace Drivers {

enum class Sht31Status : uint8_t {
  Ok              = 0,
  NotInitialized  = 1,
  I2cError        = 2,
  CrcMismatch     = 3,
  Stale           = 4,
  Cooldown        = 5,
};

struct Sht31Reading {
  float    temperatureC;   // °C — Ambient / Enclosure
  float    humidityPct;     // %
  uint32_t timestamp;       // millis() of last successful read
  Sht31Status status;
};

class Sht31Driver {
public:
  explicit Sht31Driver(uint8_t address = 0x44) : _address(address) {}
  bool begin();
  void tick();  // non-blocking state machine
  bool isAvailable() const { return _available; }
  Sht31Reading getReading() const { return _reading; }
  float getTemperature() const { return _reading.temperatureC; }
  float getHumidity()    const { return _reading.humidityPct; }
  uint32_t getLastReadMs() const { return _reading.timestamp; }
  Sht31Status getStatus() const { return _reading.status; }

  // Offsets applied post-read (set by Calibration service)
  void setTempOffset(float c) { _tempOffset = c; }
  void setHumOffset(float p)  { _humOffset = p; }

private:
  bool     _available = false;
  uint8_t  _address = 0;
  Sht31Reading _reading = {};

  enum class State : uint8_t { Idle = 0, Converting = 1, Ready = 2 };
  State    _state = State::Idle;
  unsigned long _stateChangeMs = 0;
  unsigned long _lastReadMs = 0;

  uint8_t  _consecutiveErrors = 0;
  unsigned long _nextRetryMs = 0;
  static constexpr uint8_t  MAX_CONSECUTIVE_ERRORS = 10;
  static constexpr uint32_t RECOVERY_RETRY_MS      = 60000;
  static constexpr uint16_t READ_INTERVAL_MS       = 5000;
  static constexpr uint16_t CONVERSION_MS          = 20;    // SHT31 ms fetch
  static constexpr uint16_t STALE_THRESHOLD_MS     = 15000;
  static constexpr uint8_t  CRC_POLYNOMIAL          = 0x31;
  static constexpr uint8_t  CRC_INIT                = 0xFF;

  float    _tempOffset = 0.0f;
  float    _humOffset  = 0.0f;

  bool     _sendCommand(uint16_t cmd);
  bool     _read6(uint8_t* buf);
  uint8_t  _crc8(const uint8_t* data, size_t len) const;
};

extern Sht31Driver sht31;

} // namespace Drivers

#endif // PLTS_DRIVERS_SHT31_H
