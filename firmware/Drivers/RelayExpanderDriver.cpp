// =============================================================================
// Drivers/RelayExpanderDriver.cpp — PCF8574 I²C 8-channel relay driver
// =============================================================================
#include "RelayExpanderDriver.h"
#if PLTS_ENABLE_RELAYS
#include "../Core/Config.h"
#include <Wire.h>

namespace Drivers {

RelayExpanderDriver relayExpander;

bool RelayExpanderDriver::begin(uint8_t i2cAddress) {
  _address = i2cAddress;

  // [Audit PHASE C] Validate I²C address range
  if (_address < Core::PCF8574_I2C_ADDRESS_MIN ||
      _address > Core::PCF8574_I2C_ADDRESS_MAX) {
    Serial.printf("[RELAY] ERROR: PCF8574 address 0x%02X out of range [0x%02X..0x%02X]\n",
                  _address, Core::PCF8574_I2C_ADDRESS_MIN, Core::PCF8574_I2C_ADDRESS_MAX);
    _available = false;
    return false;
  }

  // [Boot glitch prevention] Write 0xFF (all OFF) BEFORE anything else.
  // PCF8574 power-on state is 0xFF, but we re-assert to handle brownout
  // recovery where the expander may have retained state.
  _outputState = Core::PCF8574_POWER_ON_STATE;
  if (!_writeOutput(_outputState)) {
    Serial.printf("[RELAY] ERROR: PCF8574 at 0x%02X not responding\n", _address);
    _available = false;
    return false;
  }

  // Verify by reading back — [P0-3 FIX] fail-closed on mismatch.
  // Hardware contract requires readback verification. A mismatch means
  // either the PCF8574 is not what we think it is, or the I²C bus is
  // unreliable. Either way, relay control is unsafe — refuse to initialize.
  uint8_t readback = _readInput();
  if (readback != _outputState) {
    Serial.printf("[RELAY] ERROR: PCF8574 readback 0x%02X != expected 0x%02X — FAIL-CLOSED\n",
                  readback, _outputState);
    _available = false;
    return false;
  }

  _available = true;
  Serial.printf("[RELAY] PCF8574 initialized at 0x%02X — 8 channels, all OFF (fail-safe)\n", _address);
  return true;
}

bool RelayExpanderDriver::setChannel(uint8_t channel, bool on) {
  if (!_available) return false;
  if (channel >= Core::RELAY_CHANNEL_COUNT) return false;

  // Active-LOW: relay ON = bit=0, relay OFF = bit=1
  uint8_t mask = (1 << channel);
  if (on) {
    _outputState &= ~mask;  // clear bit = LOW = relay ON
  } else {
    _outputState |= mask;   // set bit = HIGH = relay OFF
  }

  return _writeOutput(_outputState);
}

uint8_t RelayExpanderDriver::readState() {
  if (!_available) return 0xFF;
  return _outputState;
}

void RelayExpanderDriver::allOff() {
  if (!_available) return;
  _outputState = Core::PCF8574_POWER_ON_STATE;  // 0xFF = all OFF
  _writeOutput(_outputState);
}

bool RelayExpanderDriver::_writeOutput(uint8_t value) {
  Wire.beginTransmission(_address);
  Wire.write(value);
  uint8_t status = Wire.endTransmission();
  return (status == 0);  // 0 = success
}

uint8_t RelayExpanderDriver::_readInput() {
  Wire.requestFrom(_address, (uint8_t)1);
  if (Wire.available()) {
    return Wire.read();
  }
  return 0xFF;  // default: all OFF (safe)
}

} // namespace Drivers

#endif // PLTS_ENABLE_RELAYS
