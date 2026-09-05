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

// [v1.9.1 FIX / INA-01] Verify config register was written correctly by
// reading it back. The INA219 config register is read/write, so we can
// confirm the PGA bits + BADC/SADC fields match what we intended.
// Returns true if the readback matches the expected value.
//
// [v1.9.2 FIX] Bit-field extraction corrected per TI datasheet SBOS448G:
//   BRNG = bit 13, PG = bits 12:11, BADC = bits 10:7, SADC = bits 6:3, MODE = bits 2:0
bool Ina219Driver::_verifyConfigRegister(uint16_t expected) {
  uint16_t readback = 0;
  if (!_readRegister(REG_CONFIG, readback)) {
    Serial.printf("[INA219 0x%02X] config readback FAILED (I2C error)\n", _address);
    return false;
  }
  if (readback != expected) {
    Serial.printf("[INA219 0x%02X] config MISMATCH: wrote 0x%04X, read back 0x%04X\n",
                  _address, expected, readback);
    // [v1.9.2] Decode the mismatch for diagnostics using CORRECT bit-field layout
    uint8_t pga_written = (expected >> 11) & 0b11;    // bits 12:11
    uint8_t pga_read    = (readback  >> 11) & 0b11;
    uint8_t badc_written = (expected >> 7) & 0b1111;  // bits 10:7
    uint8_t badc_read    = (readback  >> 7) & 0b1111;
    uint8_t sadc_written = (expected >> 3) & 0b1111;  // bits 6:3
    uint8_t sadc_read    = (readback  >> 3) & 0b1111;
    uint8_t mode_written = expected & 0b111;           // bits 2:0
    uint8_t mode_read    = readback & 0b111;
    Serial.printf("[INA219 0x%02X]   PGA: wrote=%d, read=%d | BADC: wrote=%d, read=%d | SADC: wrote=%d, read=%d | MODE: wrote=%d, read=%d\n",
                  _address, pga_written, pga_read, badc_written, badc_read,
                  sadc_written, sadc_read, mode_written, mode_read);
    return false;
  }
  // [v1.9.2] Decode readback for diagnostics using CORRECT bit-field layout
  uint8_t pga  = (readback >> 11) & 0b11;    // bits 12:11
  uint8_t brng = (readback >> 13) & 0b1;     // bit 13
  uint8_t badc = (readback >> 7) & 0b1111;   // bits 10:7
  uint8_t sadc = (readback >> 3) & 0b1111;   // bits 6:3
  uint8_t mode = readback & 0b111;           // bits 2:0
  const char* pga_str[] = {"±40mV", "±80mV", "±160mV", "±320mV"};
  Serial.printf("[INA219 0x%02X] config readback OK: 0x%04X (BRNG=%d %s, PGA=%d %s, BADC=%d, SADC=%d, MODE=%d)\n",
                _address, readback,
                brng, brng ? "32V" : "16V",
                pga, pga_str[pga],
                badc, sadc, mode);
  return true;
}

// [v1.9.1 FIX / DYNAMIC-GAIN] Write the INA219 config register for the given PGA mode.
// Returns true on success. After writing, verifies the register via readback.
bool Ina219Driver::_applyPgaMode(Ina219PgaMode mode) {
  uint16_t config = (mode == Ina219PgaMode::Pga80mV)
                    ? Core::INA219_CONFIG_PGA_80MV
                    : Core::INA219_CONFIG_PGA_160MV;
  if (!_writeRegister(REG_CONFIG, config)) {
    Serial.printf("[INA219 0x%02X] PGA switch to %s FAILED (I2C write error)\n",
                  _address, pgaModeToStr(mode));
    return false;
  }
  // [v1.9.1 FIX / INA-01] Verify the register was actually written
  if (!_verifyConfigRegister(config)) {
    Serial.printf("[INA219 0x%02X] PGA switch to %s FAILED (readback mismatch)\n",
                  _address, pgaModeToStr(mode));
    return false;
  }
  _pgaMode = mode;
  // [v1.9.1 FIX / INA-02] Mark that the NEXT sample must be discarded.
  // After a PGA register write, the INA219 needs one full conversion cycle
  // (68ms for 12-bit/128-sample) to settle with the new gain. The next tick
  // (500ms later) will skip the reading and just clear this flag.
  _discardNextSample = true;
  return true;
}

// [v1.9.1 FIX / DYNAMIC-GAIN] Hysteresis logic: switch PGA based on current magnitude.
//   |I| >= 100A  → switch UP   to 160mV (avoid saturation at 106A in 80mV mode)
//   |I| < 90A    → switch DOWN to 80mV  (regain high resolution for standby)
// The 10A gap prevents chattering when load hovers near the threshold.
void Ina219Driver::_evaluatePgaSwitch(float absCurrent) {
  if (_pgaMode == Ina219PgaMode::Pga80mV && absCurrent >= Core::INA219_PGA_SWITCH_UP_A) {
    Serial.printf("[INA219 0x%02X] PGA 80mV → 160mV (I=%.2fA ≥ %.0fA threshold)\n",
                  _address, (double)absCurrent, (double)Core::INA219_PGA_SWITCH_UP_A);
    _applyPgaMode(Ina219PgaMode::Pga160mV);
  } else if (_pgaMode == Ina219PgaMode::Pga160mV && absCurrent < Core::INA219_PGA_SWITCH_DOWN_A) {
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
  _discardNextSample = false;

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

  // [v1.9.1 FIX / INA-02] Discard the first sample after a PGA switch.
  // The INA219 needs one full conversion cycle (68ms for 12-bit/128-sample)
  // to settle with the new gain. Reading immediately after the register write
  // can produce a stale or transitional value. We skip one tick (500ms) which
  // is well beyond the 68ms settling time, then resume normal readings.
  if (_discardNextSample) {
    _discardNextSample = false;
    _reading.status = Ina219Status::Ok;  // not an error — just settling
    Serial.printf("[INA219 0x%02X] PGA settling — sample discarded (pga=%s)\n",
                  _address, pgaModeToStr(_pgaMode));
    return;
  }

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
      // Skip this sample — the next tick will discard (settle) then read
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
