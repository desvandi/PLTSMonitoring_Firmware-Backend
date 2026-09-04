// =============================================================================
// Drivers/RelayExpanderDriver.h — PCF8574 I²C 8-channel relay driver
// -----------------------------------------------------------------------------
// [v1.8.0] 8-channel relay control via PCF8574 I²C port expander.
// This is the LOW-LEVEL driver — the ONLY code that talks to the PCF8574
// hardware. All relay mutations go through RelayController → RelayEngine →
// this driver. NO BYPASS: no other subsystem calls Wire directly for relays.
//
// Fail-safe contract:
//   - PCF8574 power-on state = 0xFF (all HIGH) = all relays OFF (active-LOW)
//   - begin() re-asserts 0xFF BEFORE any other init
//   - If I²C communication fails, _available=false, all relay commands no-op
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H
#define PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H

#include <Arduino.h>
#if PLTS_ENABLE_RELAYS

namespace Drivers {

class RelayExpanderDriver {
public:
  /// Initialize the PCF8574 expander. MUST be called before any setChannel().
  /// Writes 0xFF (all OFF) immediately — fail-safe boot.
  /// Returns true if I²C communication verified.
  bool begin(uint8_t i2cAddress = Core::PCF8574_I2C_ADDRESS_DEFAULT);

  /// Set a single channel (0-7) ON or OFF.
  /// Thread-safe via I²C mutex (Wire is not reentrant).
  /// Returns false if driver unavailable or channel out of range.
  bool setChannel(uint8_t channel, bool on);

  /// Read the current output register state (8-bit bitmap).
  /// Returns 0xFF if driver unavailable.
  uint8_t readState();

  /// Check if the expander is available (I²C communication verified).
  bool isAvailable() const { return _available; }

  /// Get the configured I²C address.
  uint8_t getAddress() const { return _address; }

  /// Emergency ALL OFF — drives all 8 channels OFF immediately.
  /// Used by E-WAVE safety cascade and factory reset.
  void allOff();

private:
  bool _available = false;
  uint8_t _address = Core::PCF8574_I2C_ADDRESS_DEFAULT;
  uint8_t _outputState = Core::PCF8574_POWER_ON_STATE;  // mirror of PCF8574 output register

  /// Write the 8-bit output register to the PCF8574.
  /// bit=1 → port HIGH → relay OFF (active-LOW)
  /// bit=0 → port LOW → relay ON
  bool _writeOutput(uint8_t value);

  /// Read the 8-bit input/output register from the PCF8574.
  uint8_t _readInput();
};

extern RelayExpanderDriver relayExpander;

} // namespace Drivers

#endif // PLTS_ENABLE_RELAYS
#endif // PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H
