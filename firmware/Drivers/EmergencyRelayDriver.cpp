// =============================================================================
// Drivers/EmergencyRelayDriver.cpp — E-WAVE v1.6.0 emergency relay + E-stop
// -----------------------------------------------------------------------------
// See EmergencyRelayDriver.h for the hardware fail-safe contract.
// Ported from firmware-generic/src/plts_firmware_v1.ino (E-WAVE v1.6.0),
// adapted to the modular driver pattern (Acs712Driver as template).
// =============================================================================
#include "EmergencyRelayDriver.h"

#if PLTS_ENABLE_EMERGENCY

#include "../Services/LogService.h"

namespace Drivers {

EmergencyRelayDriver emergencyRelay;

bool EmergencyRelayDriver::begin() {
  // FAIL-SAFE FIRST — isolated before anything else can hang.
  pinMode(_relayPin, OUTPUT);
  _relayWrite(false);
  if (_estopEnabled && _estopPin >= 0) {
    pinMode((uint8_t)_estopPin, INPUT_PULLUP);
  }
  _available = true;
  Serial.printf("[EMG] relay init: pin=%u active-%s state=ISOLATED estop=%d\n",
                _relayPin, RELAY_ACTIVE_LOW ? "LOW" : "HIGH", _estopPin);
  Services::Log.append(Core::LogType::Boot,
      String("EMERGENCY_RELAY_ISOLATED pin=") + _relayPin + " (fail-safe boot)");
  return true;
}

void EmergencyRelayDriver::applyPins(uint8_t relayPin, int8_t estopPin, bool estopEnabled) {
  // A pin change on an energized system is a wiring-level event: re-drive the
  // CURRENT state on the new pin first, then re-init the sense line. The relay
  // must never glitch through ISOLATED because the operator moved a pin.
  bool wasEnergized = _energized;
  _relayPin = relayPin;
  _estopPin = estopPin;
  _estopEnabled = estopEnabled;
  pinMode(_relayPin, OUTPUT);
  _relayWrite(wasEnergized);
  if (_estopEnabled && _estopPin >= 0) {
    pinMode((uint8_t)_estopPin, INPUT_PULLUP);
  }
  Services::Log.append(Core::LogType::ConfigurationChanged,
      String("EMERGENCY_PINS_APPLIED relay=") + _relayPin +
      " estop=" + _estopPin + " energized=" + (wasEnergized ? 1 : 0));
}

void EmergencyRelayDriver::setEnergized(bool energized) {
  _relayWrite(energized);   // updates both GPIO level and shadow state
}

bool EmergencyRelayDriver::isEstopOpen() const {
  if (!_estopEnabled || _estopPin < 0) return false;   // not monitored — honest false
  return digitalRead((uint8_t)_estopPin) == HIGH;      // NC line: CLOSED=LOW, OPEN=HIGH
}

void EmergencyRelayDriver::_relayWrite(bool energized) {
  // ACTIVE-LOW module: energized -> LOW, isolated -> HIGH.
  // (Hi-Z at reset/crash de-energizes the module — the ESP32 must stay alive
  // to keep the system running; that is the fail-safe contract.)
  bool level = energized ? LOW : HIGH;
  digitalWrite(_relayPin, level);
  _energized = energized;
}

} // namespace Drivers

#endif // PLTS_ENABLE_EMERGENCY
