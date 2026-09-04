// Utils/I2cRecovery.h — [v1.6.3 / NOISE-3] I²C bus lockup self-healing
// ============================================================================
// Public surface: one function. Called by sensor drivers (INA219, SHT31)
// on ANY I²C bus error, BEFORE deciding to cool the sensor down — a
// recovered bus skips one sample instead of losing the channel.
// ============================================================================
#ifndef UTILS_I2C_RECOVERY_H
#define UTILS_I2C_RECOVERY_H

#include <stdint.h>

namespace Utils {

// 9 clocks is the I²C spec's worst case for a slave mid-byte (spec AN10216).
static constexpr uint8_t I2C_RECOVER_CLOCKS = 9;

// Bit-bangs the bus-clear sequence (9x SCL + STOP) and re-initializes Wire
// on the given pins. Returns true when SDA released (bus healthy again).
bool i2cRecover(uint8_t sdaPin, uint8_t sclPin);

}  // namespace Utils

#endif  // UTILS_I2C_RECOVERY_H
