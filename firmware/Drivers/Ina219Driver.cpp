// =============================================================================
// Drivers/Ina219Driver.cpp — INA219 raw I²C, polarity-corrected current
// [v1.9.0 / DYNAMIC-GAIN] Dynamic PGA switching (±80mV / ±160mV) with hysteresis
// =============================================================================
#include "Ina219Driver.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/I2cRecovery.h"   // [v1.6.3 / NOISE-3] bus lockup self-heal
#include <Wire.h>
#include <cmath>

namespace Drivers {

// [v1.9.0] Local constants (previously in header — moved here because the
// dynamic gain switching changes the config register value at runtime)
static constexpr float    CURRENT_LSB_A   = 0.004f;  // 4 mA/bit → 100A = 25000 counts
static constexpr uint16_t CONFIG_NORMAL   = Core::INA219_CONFIG_PGA_80MV;  // default = high-res

Ina219Driver ina219Battery(Core::INA219_ADDRESS,
                            Core::INA219_SHUNT_OHM,
                            Core::INA219_SIGN_CORRECTION);

Ina219Driver::Ina219Driver(uint8_t address, float shuntOhms, float signCorrection)
  : _address(address & 0x7F), _shuntOhms(shuntOhms), _signCorrection(signCorrection) {}

uint16_t Ina219Driver::_computeCalibration() const {
  if (_shuntOhms <= 0.0f) return 0;
  float cal = 0.04096f / (CURRENT_LSB_A * _shuntOhms);
  if (cal < 1.0f || cal > 65535.0f) return 0;
  return (uint16_t)cal;
}

bool Ina219Driver::_writeRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(_address);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Ina219Driver::_readRegister(uint8_t reg, uint16_t& out) {
  Wire.beginTransmission(_address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)_address, 2) != 2) return false;
  uint32_t start = millis();
  while (Wire.available() < 2) {
    if (millis() - start > 10) return false;
  }
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = ((uint16_t)hi << 8) | lo;
  return true;
}

// [v1.9.0 / DYNAMIC-GAIN] Write the INA219 config register for the given PGA mode.
// Returns true on success. The calibration register is preserved (PGA change
// does not affect calibration — only the shunt voltage measurement range).
bool Ina219Driver::_applyPgaMode(Ina219PgaMode mode) {
  uint16_t config = (mode == Ina219PgaMode::Pga80mV)
                    ? Core::INA219_CONFIG_PGA_80MV
                    : Core::INA219_CONFIG_PGA_160MV;
  if (!_writeRegister(REG_CONFIG, config)) {
    Serial.printf("[INA219 0x%02X] PGA switch to %s FAILED (I2C error)\n",
                  _address, pgaModeToStr(mode));
    return false;
  }
  _pgaMode = mode;
  // Small delay for the new conversion to settle (INA219 conversion time
  // for 12-bit/128-sample = 68 ms worst case; we don't block — next tick
  // will read the settled value after READ_INTERVAL_MS=500ms)
  return true;
}

// [v1.9.0 / DYNAMIC-GAIN] Hysteresis logic: switch PGA based on current magnitude.
//   |I| >= 100A  → switch UP   to 160mV (avoid saturation at 106A in 80mV mode)
//   |I| < 90A    → switch DOWN to 80mV  (regain high resolution for standby)
// The 10A gap prevents chattering when load hovers near the threshold.
void Ina219Driver::_evaluatePgaSwitch(float absCurrent) {
  if (_pgaMode == Ina219PgaMode::Pga80mV && absCurrent >= Core::INA219_PGA_SWITCH_UP_A) {
    // Switch UP to 160mV to avoid imminent saturation
    Serial.printf("[INA219 0x%02X] PGA 80mV → 160mV (I=%.2fA ≥ %.0fA threshold)\n",
                  _address, (double)absCurrent, (double)Core::INA219_PGA_SWITCH_UP_A);
    _applyPgaMode(Ina219PgaMode::Pga160mV);
  } else if (_pgaMode == Ina219PgaMode::Pga160mV && absCurrent < Core::INA219_PGA_SWITCH_DOWN_A) {
    // Switch DOWN to 80mV to regain high resolution
    Serial.printf("[INA219 0x%02X] PGA 160mV → 80mV (I=%.2fA < %.0fA threshold)\n",
                  _address, (double)absCurrent, (double)Core::INA219_PGA_SWITCH_DOWN_A);
    _applyPgaMode(Ina219PgaMode::Pga80mV);
  }
}

bool Ina219Driver::begin() {
  _available = false;
  _reading = {};
  _reading.status = Ina219Status::NotInitialized;
  _pgaMode = Ina219PgaMode::Pga80mV;  // start in high-resolution mode

  if (!_writeRegister(REG_CONFIG, CONFIG_RESET)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] reset failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);  // 100 µs minimum after reset

  // [v1.9.0] Apply initial PGA mode (±80mV for high-resolution standby)
  if (!_applyPgaMode(Ina219PgaMode::Pga80mV)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] initial PGA config write failed\n", _address);
    return false;
  }
  uint16_t cal = _computeCalibration();
  if (cal == 0) {
    Serial.printf("[INA219 0x%02X] calibration invalid (shunt=%.6f Ω)\n",
                  _address, (double)_shuntOhms);
    return false;
  }
  if (!_writeRegister(REG_CALIBRATION, cal)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] calibration write failed\n", _address);
    return false;
  }
  _available = true;
  _reading.status = Ina219Status::Ok;
  _reading.pgaMode = _pgaMode;
  Serial.printf("[INA219 0x%02X] init: shunt=%.4f mΩ, cal=0x%04X, sign=%+.1f, pga=%s\n",
                _address, (double)(_shuntOhms * 1000.0f), cal, _signCorrection,
                pgaModeToStr(_pgaMode));
  return true;
}

void Ina219Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();

  // Cooldown after consecutive errors
  if (_nextRetryMs > 0) {
    if (now < _nextRetryMs) {
      _reading.status = Ina219Status::Cooldown;
      return;
    }
    _nextRetryMs = 0;
    _consecutiveErrors = 0;
    Serial.printf("[INA219 0x%02X] retrying after recovery cooldown\n", _address);
  }
  if (now - _lastReadMs < READ_INTERVAL_MS) return;
  _lastReadMs = now;

  uint16_t shuntRaw = 0, busRaw = 0;
  if (!_readRegister(REG_SHUNT, shuntRaw) || !_readRegister(REG_BUS, busRaw)) {
    _reading.status = Ina219Status::I2cError;
    Utils::i2cRecover(Core::PIN_I2C_SDA, Core::PIN_I2C_SCL);
    if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
      _nextRetryMs = now + RECOVERY_RETRY_MS;
      Serial.printf("[INA219 0x%02X] %u consecutive errors — cooldown %lus\n",
                    _address, _consecutiveErrors, (unsigned long)(RECOVERY_RETRY_MS / 1000));
    }
    return;
  }
  _consecutiveErrors = 0;

  // Shunt voltage: 16-bit signed, LSB = 10 µV
  int16_t shuntSigned = (int16_t)shuntRaw;
  float shuntV = shuntSigned * 0.00001f;

  // [v1.9.0 / DYNAMIC-GAIN] Saturation detection:
  // In ±80mV mode, the shunt register saturates at ±80mV (raw = ±8000).
  // If we're in 80mV mode and reading near saturation, switch to 160mV
  // immediately — the current is likely above 100A but we can't measure it.
  if (_pgaMode == Ina219PgaMode::Pga80mV) {
    float absShuntMv = std::fabs(shuntV) * 1000.0f;
    if (absShuntMv > 78.0f) {  // within 2mV of saturation
      Serial.printf("[INA219 0x%02X] PGA 80mV saturation detected (shunt=%.2fmV) — emergency switch to 160mV\n",
                    _address, (double)absShuntMv);
      _applyPgaMode(Ina219PgaMode::Pga160mV);
      // Skip this sample — the next tick will read with the new PGA
      _reading.status = Ina219Status::OutOfRange;
      return;
    }
  }

  // Bus voltage: bits15-3, LSB = 4 mV, bits2-0 status
  uint16_t busFixed = (busRaw >> 3) & 0x1FFF;
  float busV = busFixed * 0.004f;
  if (busFixed == 0x1FFF) busV = 0;  // overflow

  // Current (A) from shunt V / Rshunt, polarity-corrected
  float rawCurrent = (shuntV / _shuntOhms) * _signCorrection;
  if (!Core::isValidFloat(rawCurrent) || std::fabs(rawCurrent) > Core::CURRENT_SPIKE_REJECT_A) {
    _reading.status = Ina219Status::OutOfRange;
    return;
  }
  if (!_emaInit) { _emaCurrent = rawCurrent; _emaInit = true; }
  else {
    _emaCurrent = _emaCurrent * (1.0f - Core::CURRENT_SMOOTH_ALPHA)
                + rawCurrent * Core::CURRENT_SMOOTH_ALPHA;
  }

  _reading.shuntVoltageV = shuntV;
  _reading.busVoltageV = busV;
  _reading.currentA = _emaCurrent;
  _reading.powerW = busV * _emaCurrent;  // signed power
  _reading.timestamp = now;
  _reading.status = Ina219Status::Ok;
  _reading.pgaMode = _pgaMode;

  // [v1.9.0 / DYNAMIC-GAIN] Evaluate PGA switch AFTER updating the reading.
  // We use the raw (non-EMA) current magnitude for the switching decision
  // so the driver reacts quickly to load transients. The EMA-smoothed value
  // is what gets reported; the raw value drives the PGA decision.
  _evaluatePgaSwitch(std::fabs(rawCurrent));
}

} // namespace Drivers
