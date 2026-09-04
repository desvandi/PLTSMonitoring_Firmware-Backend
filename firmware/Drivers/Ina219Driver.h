// =============================================================================
// Drivers/Ina219Driver.h — INA219 battery current sensor (raw I²C, no Adafruit)
// -----------------------------------------------------------------------------
// Single instance: ina219Battery @ I2C 0x40.
//
// Hardware (brief §2):
//   - 100A / 75mV shunt → Rshunt = 0.75 mΩ
//   - VIN+ = battery-side, VIN- = inverter-side
//   - Positive raw current = DISCHARGE (current leaving battery)
//   - Polarity correction in software: positive output = CHARGING
//
// Config 0x3FFB (32V FSR, ±320mV PGA, 16-sample averaging, shunt+bus continuous)
//
// Failure handling:
//   - 10 consecutive I2C errors → 60s cooldown (frees bus for other sensors)
//   - Sensor failure → isAvailable()=false, system continues (brief §46)
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_INA219_H
#define PLTS_DRIVERS_INA219_H

#include <Arduino.h>
#include <cstdint>

namespace Drivers {

enum class Ina219Status : uint8_t {
  Ok              = 0,
  NotInitialized  = 1,
  I2cError        = 2,
  OutOfRange      = 3,
  Cooldown        = 4,
};

struct Ina219Reading {
  float    shuntVoltageV;  // raw V across shunt (signed)
  float    busVoltageV;    // V at INA219 VBUS pin (informational only)
  float    currentA;       // signed, post-polarity-correction (positive = CHARGING)
  float    powerW;         // busV × currentA (signed, DERIVED)
  uint32_t timestamp;
  Ina219Status status;
};

class Ina219Driver {
public:
  Ina219Driver(uint8_t address, float shuntOhms, float signCorrection);

  bool   begin();
  void   tick();
  bool   isAvailable() const { return _available; }
  Ina219Reading getReading() const { return _reading; }
  float  getCurrent()      const { return _reading.currentA; }
  float  getBusVoltage()   const { return _reading.busVoltageV; }
  float  getShuntVoltage() const { return _reading.shuntVoltageV; }
  uint32_t getLastReadMs() const { return _reading.timestamp; }
  Ina219Status getStatus() const { return _reading.status; }

private:
  uint8_t  _address;
  float    _shuntOhms;
  float    _signCorrection;
  bool     _available = false;
  Ina219Reading _reading = {};
  unsigned long _lastReadMs = 0;
  float    _emaCurrent = 0.0f;
  bool     _emaInit = false;

  uint8_t  _consecutiveErrors = 0;
  unsigned long _nextRetryMs = 0;
  static constexpr uint16_t MAX_CONSECUTIVE_ERRORS = 10;
  static constexpr uint32_t RECOVERY_RETRY_MS      = 60000;
  static constexpr uint16_t READ_INTERVAL_MS       = 500;   // 2 Hz

  enum Reg : uint8_t {
    REG_CONFIG      = 0x00,
    REG_SHUNT       = 0x01,
    REG_BUS         = 0x02,
    REG_POWER       = 0x03,
    REG_CURRENT     = 0x04,
    REG_CALIBRATION = 0x05,
  };
  static constexpr uint16_t CONFIG_RESET   = 0x399F;
  static constexpr uint16_t CONFIG_NORMAL  = 0x3FFB;
  static constexpr float    CURRENT_LSB_A   = 0.004f;  // 4 mA/bit → 100A = 25000 counts

  bool    _writeRegister(uint8_t reg, uint16_t value);
  bool    _readRegister(uint8_t reg, uint16_t& out);
  uint16_t _computeCalibration() const;
};

extern Ina219Driver ina219Battery;

} // namespace Drivers

#endif // PLTS_DRIVERS_INA219_H
