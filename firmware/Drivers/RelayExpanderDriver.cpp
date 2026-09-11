// =============================================================================
// Drivers/RelayExpanderDriver.cpp — PCF8574 I²C 8-channel relay driver
// =============================================================================
#include "RelayExpanderDriver.h"
#if PLTS_ENABLE_RELAYS
#include "../Core/Config.h"
#include "../Utils/I2cBusGuard.h"
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

  // Verify by reading back — [P0-3 FIX + audit p.94] fail-closed on mismatch
  // AND on read FAILURE. The old `_readInput()` returned 0xFF on a failed
  // read, which is a legitimate value (all OFF) — a dead bus could pass
  // verification. `bool _readInput(uint8_t&)` removes that ambiguity.
  uint8_t readback;
  if (!_readInput(readback)) {
    Serial.printf("[RELAY] ERROR: PCF8574 readback FAILED (no data) — FAIL-CLOSED\n");
    _available = false;
    return false;
  }
  if (readback != _outputState) {
    Serial.printf("[RELAY] ERROR: PCF8574 readback 0x%02X != expected 0x%02X — FAIL-CLOSED\n",
                  readback, _outputState);
    _available = false;
    return false;
  }

  _shadowUnknown = false;
  _available = true;
  Serial.printf("[RELAY] PCF8574 initialized at 0x%02X — 8 channels, all OFF (fail-safe)\n", _address);
  return true;
}

bool RelayExpanderDriver::setChannel(uint8_t channel, bool on) {
  if (!_available) return false;
  if (channel >= Core::RELAY_CHANNEL_COUNT) return false;

  // [audit p.92] After an unverified write the shadow register does not
  // reflect hardware. ANY subsequent normal mutation could apply the stale
  // bit of the previously failed channel (e.g. CH0 stuck ON). Refuse normal
  // mutation until recoverWithAllOff() re-synchronizes the shadow.
  if (_shadowUnknown) {
    Serial.println("[RELAY] setChannel refused — shadow UNKNOWN, recovery required");
    return false;
  }

  // [audit p.89-92] Compute the NEXT state WITHOUT touching the live shadow.
  // Commit _outputState ONLY after the hardware write succeeds, so a failed
  // write never leaves a bit set for a channel whose hardware state is
  // actually unknown.
  uint8_t mask = (1 << channel);
  uint8_t next = _outputState;
  if (on) {
    next &= ~mask;  // clear bit = LOW = relay ON (active-LOW)
  } else {
    next |= mask;   // set bit = HIGH = relay OFF
  }

  if (!_writeOutput(next)) {
    // Hardware write failed — the physical output register content is now
    // UNKNOWN (the byte may or may not have reached the expander).
    _shadowUnknown = true;
    Serial.printf("[RELAY] write FAILED ch=%u — shadow UNKNOWN until recovery\n", channel);
    return false;
  }

  _outputState = next;  // commit shadow only after verified success
  return true;
}

uint8_t RelayExpanderDriver::readState() {
  if (!_available) return 0xFF;
  if (_shadowUnknown) return 0xFF;  // unknown → report fail-safe value, caller checks isShadowUnknown()
  return _outputState;
}

void RelayExpanderDriver::allOff() {
  // Safety path — ALWAYS attempted even when the shadow is unknown: driving
  // 0xFF is the fail-safe direction, and re-asserting it can only help.
  if (!_available) return;
  uint8_t allOffState = Core::PCF8574_POWER_ON_STATE;  // 0xFF = all OFF
  if (_writeOutput(allOffState)) {
    _outputState = allOffState;
    _shadowUnknown = false;
  } else {
    _shadowUnknown = true;
  }
}

bool RelayExpanderDriver::recoverWithAllOff() {
  if (!_available) return false;
  // Attempt: write all-off, then verify via readback (with failure
  // distinguished from a legit 0xFF).
  uint8_t allOffState = Core::PCF8574_POWER_ON_STATE;
  if (!_writeOutput(allOffState)) return false;
  uint8_t readback;
  if (!_readInput(readback)) return false;
  if (readback != allOffState) return false;
  _outputState = allOffState;
  _shadowUnknown = false;
  return true;
}

bool RelayExpanderDriver::_writeOutput(uint8_t value) {
  // [audit p.96-99] All bus access serialized via the shared FreeRTOS
  // recursive mutex — multiple tasks (sensorTask, relayTask, networkTask)
  // drive the same Wire peripheral.
  Utils::I2cBusGuard& guard = Utils::i2cBus;
  if (!guard.lock(50)) return false;
  Wire.beginTransmission(_address);
  Wire.write(value);
  uint8_t status = Wire.endTransmission();
  guard.unlock();
  return (status == 0);  // 0 = success
}

bool RelayExpanderDriver::_readInput(uint8_t& value) {
  Utils::I2cBusGuard& guard = Utils::i2cBus;
  if (!guard.lock(50)) return false;
  Wire.requestFrom(_address, (uint8_t)1);
  if (!Wire.available()) {
    guard.unlock();
    return false;  // communication failure — NOT a value
  }
  value = Wire.read();
  guard.unlock();
  return true;
}

} // namespace Drivers

#endif // PLTS_ENABLE_RELAYS
