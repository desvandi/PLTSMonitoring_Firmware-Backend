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
// Config 0x0FFF (16V FSR, ±80mV PGA, 12-bit/128-sample averaging, shunt+bus continuous)
// [v1.9.2 CANONICAL FIX] Corrected from 0x152B (v1.9.1) which had wrong bit-field
// layout — actually decoded to ±160mV/10-bit-SADC/triggered-mode. Verified against
// TI datasheet SBOS448G: BRNG=bit13, PG=bits12:11, BADC=bits10:7, SADC=bits6:3, MODE=bits2:0.
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

// [v1.9.0 / DYNAMIC-GAIN] PGA mode enum for dynamic gain switching.
// The driver switches between these two modes based on current magnitude
// to cover the full 1A-150A dynamic range without saturation or noise.
enum class Ina219PgaMode : uint8_t {
  Pga80mV   = 0,   // ±80 mV range — high resolution for standby (1-100A)
  Pga160mV  = 1,   // ±160 mV range — full range for peak load (100-150A)
};

// Convert PGA mode to string for telemetry payload ("80mV" / "160mV")
inline const char* pgaModeToStr(Ina219PgaMode mode) {
  return mode == Ina219PgaMode::Pga80mV ? "80mV" : "160mV";
}

struct Ina219Reading {
  float    shuntVoltageV;  // raw V across shunt (signed)
  float    busVoltageV;    // V at INA219 VBUS pin (informational only)
  float    currentA;       // signed, post-polarity-correction (positive = CHARGING)
  float    powerW;         // busV × currentA (signed, DERIVED)
  uint32_t timestamp;
  Ina219Status status;
  Ina219PgaMode pgaMode;   // [v1.9.0] current PGA mode (for telemetry)
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
  // [v1.9.0] PGA mode accessors
  Ina219PgaMode getPgaMode() const { return _pgaMode; }
  const char* getPgaModeStr() const { return pgaModeToStr(_pgaMode); }

private:
  uint8_t  _address;
  float    _shuntOhms;
  float    _signCorrection;
  bool     _available = false;
  Ina219Reading _reading = {};
  unsigned long _lastReadMs = 0;
  float    _emaCurrent = 0.0f;
  bool     _emaInit = false;

  // [v1.9.0 / DYNAMIC-GAIN] PGA state + switching logic
  Ina219PgaMode _pgaMode = Ina219PgaMode::Pga80mV;  // start in high-resolution mode
  bool     _applyPgaMode(Ina219PgaMode mode);  // write config register + verify + set discard flag
  void     _evaluatePgaSwitch(float absCurrent);  // hysteresis logic
  // [v1.9.1 FIX / INA-01] Verify config register via readback after write
  bool     _verifyConfigRegister(uint16_t expected);
  // [v1.9.1 FIX / INA-02] Discard first sample after PGA switch (settling time)
  bool     _discardNextSample = false;

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

  bool    _writeRegister(uint8_t reg, uint16_t value);
  bool    _readRegister(uint8_t reg, uint16_t& out);
  uint16_t _computeCalibration() const;
};

extern Ina219Driver ina219Battery;

} // namespace Drivers

#endif // PLTS_DRIVERS_INA219_H
