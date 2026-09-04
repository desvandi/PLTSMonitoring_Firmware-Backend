// =============================================================================
// Drivers/EmergencyRelayDriver.h — E-WAVE v1.6.0 emergency relay + E-stop GPIO
// -----------------------------------------------------------------------------
// PORT of the firmware-generic emergency hardware layer into the modular
// firmware (README §13 limitation #7 closure).
//
// HARDWARE CONTRACT (must match firmware-generic byte-for-byte in behavior):
//   Relay module 5V with optocoupler, ACTIVE-LOW input.
//     GPIO LOW  -> opto conducts  -> relay ENERGIZED -> kontaktor path CLOSED
//                  -> system RUN.
//     GPIO HIGH / Hi-Z (boot, crash, WDT reset) -> relay DE-ENERGIZED ->
//                  system ISOLATED.
//     The ESP32 must be ALIVE to keep the system running — that is the
//     fail-safe contract ("sistem tidak boleh berbohong pada operator",
//     termasuk pada dirinya sendiri saat mati).
//   Physical E-stop (normally-closed) breaks the relay module's negative
//     supply: pressed -> module powerless -> relay OFF, INDEPENDENT of the
//     ESP32. The sense line below only LATCHES the state so releasing the
//     button never re-energizes (operator ARM required).
//   Sneak-path note (firmware-generic, preserved): while E-stop is OPEN, the
//     opto LED can only find a return through the ESP32 GPIO when it drives
//     LOW; the sense-poll reacts within one tick by driving the GPIO HIGH,
//     and a dead/Hi-Z ESP32 sinks nothing.
//
// begin() MUST be called as the FIRST hardware action of setup() — before
// LittleFS, WiFi, or anything that can hang (boot isolation, test E1 parity).
// -----------------------------------------------------------------------------
// Feature flag: PLTS_ENABLE_EMERGENCY (default 1). When 0 the whole module
// compiles out and the system stays monitoring-only (v1.6.3 behavior).
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_EMERGENCY_RELAY_DRIVER_H
#define PLTS_DRIVERS_EMERGENCY_RELAY_DRIVER_H

// [W12-fix] Config.h comes BEFORE the feature guard: include order must never decide whether a feature exists (Rs485Console bug class — a TU including this header first compiled the whole feature out).
#include "../Core/Config.h"

#if PLTS_ENABLE_EMERGENCY

#include <Arduino.h>
#include <cstdint>

namespace Drivers {

class EmergencyRelayDriver {
public:
  EmergencyRelayDriver() = default;

  // FAIL-SAFE FIRST: drive the relay pin ISOLATED (HIGH on active-LOW module)
  // before LittleFS/WiFi/anything that can hang. ESP32 GPIOs are Hi-Z at
  // reset -> module opto OFF -> isolated anyway; this makes it EXPLICIT and
  // covers the window until user code runs. Uses compile-time default pins
  // (runtime config does not exist yet — applyPins() re-applies later).
  bool begin();

  // Runtime pin re-configuration (operator-adjustable via GAS CONFIG command
  // / NVS). Re-applies the CURRENT energized state on the new pin so a pin
  // change can never silently toggle the relay.
  void applyPins(uint8_t relayPin, int8_t estopPin, bool estopEnabled);

  // Relay control. energized=true -> RUN (GPIO LOW), false -> ISOLATED (HIGH).
  void setEnergized(bool energized);
  bool isEnergized() const { return _energized; }

  // E-stop sense (INPUT_PULLUP; OPEN=HIGH on a normally-closed line).
  // Returns false when the E-stop channel is disabled (pin < 0) — honest
  // "not monitored", never a fabricated "closed".
  bool isEstopOpen() const;
  bool isEstopEnabled() const { return _estopEnabled; }

  uint8_t relayPin() const { return _relayPin; }
  int8_t  estopPin() const { return _estopPin; }
  bool isAvailable() const { return _available; }

  static constexpr bool RELAY_ACTIVE_LOW = true;   // module opto: IN=LOW -> energized

private:
  void _relayWrite(bool energized);

  uint8_t _relayPin    = Core::PIN_EMERGENCY_RELAY;   // runtime-configurable, default 27
  int8_t  _estopPin    = Core::PIN_EMERGENCY_ESTOP;   // -1 = disabled
  bool    _estopEnabled = true;
  bool    _energized   = false;      // boot state: ISOLATED (fail-safe)
  bool    _available   = false;
};

extern EmergencyRelayDriver emergencyRelay;

} // namespace Drivers

#endif // PLTS_ENABLE_EMERGENCY
#endif // PLTS_DRIVERS_EMERGENCY_RELAY_DRIVER_H
