// Utils/I2cRecovery.cpp — [v1.6.3 / NOISE-3] I²C bus lockup self-healing
// ============================================================================
// Inverter environments induce ground bounce and EMI bursts that can make
// an I²C slave clock-stretch mid-transfer and then RELEASE SDA low — from
// that moment every transaction on the bus fails forever (NACK on address),
// even after the EMI burst is long gone. The old behavior: sensors enter a
// permanent cooldown, telemetry silently loses the DC channel + ambient
// data until the next reboot.
//
// The standard bus-clear sequence (I²C spec §3.1.16 / NXP AN10216):
// up to 9 SCL pulses until SDA releases, then a STOP condition, then hand
// the pins back to the Wire driver. Total time ≈ 100 µs. Called by the
// INA219 and SHT31 drivers on ANY bus error, BEFORE the cooldown decision
// — the very next poll re-probes the sensor, so a recovered bus costs one
// skipped sample instead of a reboot.
// ============================================================================
#include "I2cRecovery.h"
#include <Wire.h>
#include <Arduino.h>

namespace Utils {

// Returns true when SDA was already high (bus healthy / recovered quickly).
bool i2cRecover(uint8_t sdaPin, uint8_t sclPin) {
  // Detach the Wire driver (it owns the pins through the I2C peripheral).
  Wire.end();

  // Bit-bang the clear sequence with open-drain levels (safe for 3.3 V bus).
  pinMode(sdaPin, OUTPUT_OPEN_DRAIN);
  pinMode(sclPin, OUTPUT_OPEN_DRAIN);
  digitalWrite(sdaPin, HIGH);   // release SDA
  digitalWrite(sclPin, HIGH);   // release SCL

  bool sdaHigh = (digitalRead(sdaPin) == HIGH);
  if (!sdaHigh) {
    // Slave holds SDA low — clock it out, up to 9 pulses.
    for (uint8_t i = 0; i < I2C_RECOVER_CLOCKS; i++) {
      digitalWrite(sclPin, LOW);
      delayMicroseconds(5);
      digitalWrite(sclPin, HIGH);
      delayMicroseconds(5);
      if (digitalRead(sdaPin) == HIGH) { sdaHigh = true; break; }
    }
  }

  // STOP condition: SDA low→high while SCL is high.
  digitalWrite(sdaPin, LOW);
  delayMicroseconds(5);
  digitalWrite(sclPin, HIGH);
  delayMicroseconds(5);
  digitalWrite(sdaPin, HIGH);
  delayMicroseconds(5);

  // Hand the pins back to the Wire driver at the standard bus speed.
  Wire.begin(sdaPin, sclPin);
  Wire.setClock(100000);
  return sdaHigh;
}

}  // namespace Utils
