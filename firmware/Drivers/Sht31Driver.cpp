// =============================================================================
// Drivers/Sht31Driver.cpp — Ambient T/H via non-blocking state machine
// =============================================================================
#include "Sht31Driver.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/I2cRecovery.h"   // [v1.6.3 / NOISE-3] bus lockup self-heal
#include <Wire.h>
#include <cmath>

namespace Drivers {

Sht31Driver sht31;

// Constructor is inline in header — no .cpp definition needed

bool Sht31Driver::_sendCommand(uint16_t cmd) {
  Wire.beginTransmission(_address);
  Wire.write((uint8_t)(cmd >> 8));
  Wire.write((uint8_t)(cmd & 0xFF));
  return Wire.endTransmission() == 0;
}

bool Sht31Driver::_read6(uint8_t* buf) {
  if (Wire.requestFrom((int)_address, 6) != 6) return false;
  uint32_t start = millis();
  while (Wire.available() < 6) {
    if (millis() - start > 50) return false;
  }
  for (uint8_t i = 0; i < 6; i++) buf[i] = Wire.read();
  return true;
}

uint8_t Sht31Driver::_crc8(const uint8_t* data, size_t len) const {
  uint8_t crc = CRC_INIT;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x80) crc = (crc << 1) ^ CRC_POLYNOMIAL;
      else            crc = (crc << 1);
    }
  }
  return crc;
}

bool Sht31Driver::begin() {
  _available = false;
  _reading = {};
  _reading.status = Sht31Status::NotInitialized;
  // Soft-reset
  if (!_sendCommand(0x30A2)) {
    Serial.printf("[SHT31 0x%02X] reset failed — sensor not present?\n", _address);
    return false;
  }
  delay(2);
  // Clear status register
  if (!_sendCommand(0x3041)) {
    Serial.printf("[SHT31 0x%02X] clear-status failed\n", _address);
    return false;
  }
  _available = true;
  _state = State::Idle;
  _reading.status = Sht31Status::Ok;
  Serial.printf("[SHT31 0x%02X] init OK — ambient/enclosure T/H\n", _address);
  return true;
}

void Sht31Driver::tick() {
  if (!_available) return;
  unsigned long now = millis();

  // Cooldown after errors
  if (_nextRetryMs > 0) {
    if (now < _nextRetryMs) {
      _reading.status = Sht31Status::Cooldown;
      return;
    }
    _nextRetryMs = 0;
    _consecutiveErrors = 0;
  }

  // Stale detection
  if (_lastReadMs > 0 && now - _lastReadMs > STALE_THRESHOLD_MS) {
    _reading.status = Sht31Status::Stale;
  }

  switch (_state) {
    case State::Idle: {
      if (now - _lastReadMs < READ_INTERVAL_MS) return;
      // Send single-shot measurement command (clock-stretching disabled)
      if (!_sendCommand(0x2400)) {
        // [v1.6.3 / NOISE-3] Recover the bus BEFORE the cooldown decision
        // (same rationale as the INA219 driver — one skipped sample instead
        // of a cold channel until reboot).
        Utils::i2cRecover(Core::PIN_I2C_SDA, Core::PIN_I2C_SCL);
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
        }
        _reading.status = Sht31Status::I2cError;
        return;
      }
      _state = State::Converting;
      _stateChangeMs = now;
      return;
    }
    case State::Converting: {
      if (now - _stateChangeMs < CONVERSION_MS) return;
      uint8_t buf[6];
      if (!_read6(buf)) {
        _state = State::Idle;
        Utils::i2cRecover(Core::PIN_I2C_SDA, Core::PIN_I2C_SCL);   // [v1.6.3 / NOISE-3]
        if (++_consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
          _nextRetryMs = now + RECOVERY_RETRY_MS;
        }
        _reading.status = Sht31Status::I2cError;
        return;
      }
      // CRC check
      if (_crc8(buf, 2) != buf[2] || _crc8(buf + 3, 2) != buf[5]) {
        _state = State::Idle;
        _reading.status = Sht31Status::CrcMismatch;
        _lastReadMs = now;
        return;
      }
      uint16_t tRaw = ((uint16_t)buf[0] << 8) | buf[1];
      uint16_t hRaw = ((uint16_t)buf[3] << 8) | buf[4];
      float t = -45.0f + 175.0f * (float)tRaw / 65535.0f;
      float h = 100.0f * (float)hRaw / 65535.0f;
      if (h > 100.0f) h = 100.0f;
      if (h < 0.0f) h = 0.0f;
      _reading.temperatureC = t + _tempOffset;
      _reading.humidityPct  = h + _humOffset;
      _reading.timestamp    = now;
      _reading.status       = Sht31Status::Ok;
      _lastReadMs = now;
      _consecutiveErrors = 0;
      _state = State::Idle;
      return;
    }
    case State::Ready:
      _state = State::Idle;
      return;
  }
}

} // namespace Drivers
