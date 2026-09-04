// =============================================================================
// Drivers/Ina219Driver.cpp — INA219 raw I²C, polarity-corrected current
// =============================================================================
#include "Ina219Driver.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/I2cRecovery.h"   // [v1.6.3 / NOISE-3] bus lockup self-heal
#include <Wire.h>
#include <cmath>

namespace Drivers {

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

bool Ina219Driver::begin() {
  _available = false;
  _reading = {};
  _reading.status = Ina219Status::NotInitialized;

  if (!_writeRegister(REG_CONFIG, CONFIG_RESET)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] reset failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);  // 100 µs minimum after reset

  if (!_writeRegister(REG_CONFIG, CONFIG_NORMAL)) {
    _reading.status = Ina219Status::I2cError;
    Serial.printf("[INA219 0x%02X] config write failed\n", _address);
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
  Serial.printf("[INA219 0x%02X] init: shunt=%.4f mΩ, cal=0x%04X, sign=%+.1f\n",
                _address, (double)(_shuntOhms * 1000.0f), cal, _signCorrection);
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
    // [v1.6.3 / NOISE-3] Recover the bus BEFORE the cooldown decision —
    // inverter EMI can clock-stretch a slave into holding SDA low forever;
    // without recovery every later transaction fails and the channel goes
    // cold until reboot. A recovered bus costs exactly one skipped sample.
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
}

} // namespace Drivers
