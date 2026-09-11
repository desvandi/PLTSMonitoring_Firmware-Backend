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
//
// [PRODUCTION-GRADE 2026-09] — audit p.89-96, p.309 remediation:
//   - setChannel(): the shadow register (_outputState) is committed ONLY
//     after a successful hardware write. On failure the driver enters
//     SHADOW_UNKNOWN and refuses further normal mutations until a verified
//     safe re-initialization (write 0xFF + readback) succeeds — a failed
//     write can never corrupt a LATER channel command via a stale bit.
//   - _readInput(): failure is now DISTINGUISHABLE from a legitimate 0xFF
//     (bool _readInput(uint8_t& value)) — no more false-positive readback
//     verification at begin().
//   - All bus access is serialized through Utils::i2cBus (FreeRTOS recursive
//     mutex) — the header's old "thread-safe via I²C mutex" claim is now
//     backed by an actual mutex.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H
#define PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H

#include <Arduino.h>
#include "../Core/Config.h"   // [CI fix] MUST be before #if PLTS_ENABLE_RELAYS
#if PLTS_ENABLE_RELAYS

namespace Drivers {

class RelayExpanderDriver {
public:
  /// Initialize the PCF8574 expander. MUST be called before any setChannel().
  /// Writes 0xFF (all OFF) immediately — fail-safe boot.
  /// Returns true if I²C communication verified (write + readback, with
  /// read-failure explicitly distinguished from a legit 0xFF readback).
  bool begin(uint8_t i2cAddress = Core::PCF8574_I2C_ADDRESS_DEFAULT);

  /// Set a single channel (0-7) ON or OFF.
  /// Thread-safe via Utils::i2cBus mutex (Wire is not reentrant).
  /// Returns false if driver unavailable, shadow state unknown (previous
  /// write failed — recoverWithAllOff() required), or channel out of range.
  bool setChannel(uint8_t channel, bool on);

  /// Read the current output register state (8-bit bitmap).
  /// Returns 0xFF if driver unavailable.
  uint8_t readState();

  /// Check if the expander is available (I²C communication verified).
  bool isAvailable() const { return _available; }

  /// True when a previous write failed and the shadow register no longer
  /// reflects the hardware — normal mutations are refused until recovery.
  bool isShadowUnknown() const { return _shadowUnknown; }

  /// Get the configured I²C address.
  uint8_t getAddress() const { return _address; }

  /// Emergency ALL OFF — drives all 8 channels OFF immediately.
  /// Used by E-WAVE safety cascade and factory reset. Bypasses the
  /// shadow-unknown refusal: safety writes are ALWAYS attempted.
  void allOff();

  /// [audit p.92-93] Verified safe recovery after a failed write:
  ///   write 0xFF → readback verify → (on success) shadow = 0xFF, known.
  /// Returns true when the driver is usable again (ALL channels OFF).
  bool recoverWithAllOff();

private:
  bool _available = false;
  bool _shadowUnknown = false;  // last write outcome unknown
  uint8_t _address = Core::PCF8574_I2C_ADDRESS_DEFAULT;
  uint8_t _outputState = Core::PCF8574_POWER_ON_STATE;  // mirror of PCF8574 output register

  /// Write the 8-bit output register to the PCF8574.
  /// bit=1 → port HIGH → relay OFF (active-LOW)
  /// bit=0 → port LOW → relay ON
  bool _writeOutput(uint8_t value);

  /// [audit p.94-95] Read the 8-bit register from the PCF8574.
  /// Returns false on communication failure; on success `value` holds the
  /// real byte (which may legitimately be 0xFF = all OFF).
  bool _readInput(uint8_t& value);
};

extern RelayExpanderDriver relayExpander;

} // namespace Drivers

#endif // PLTS_ENABLE_RELAYS
#endif // PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H
