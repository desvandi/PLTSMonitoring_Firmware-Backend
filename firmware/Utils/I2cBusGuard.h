// Utils/I2cBusGuard.h — [PRODUCTION-GRADE 2026-09] shared I²C bus mutex
// ============================================================================
// [audit p.96-100] Multiple FreeRTOS tasks (sensorTask: INA219/SHT31,
// relayTask: PCF8574, networkTask: relay mutations via all_off, RTC reads)
// all drive the single global `Wire` peripheral. Arduino's Wire class is NOT
// documented as cross-task/cross-core safe for concurrent transactions, and
// RelayExpanderDriver.h claims "thread-safe via I²C mutex" while no mutex
// existed — a documentation/implementation contradiction.
//
// This guard turns that claim TRUE: one process-wide recursive FreeRTOS
// mutex serializes every I²C transaction (begin..endTransmission), regardless
// of which task or core initiates it.
//
// Usage:
//   Utils::I2cBusGuard guard;          // constructor-free RAII-less C++:
//   if (guard.lock(50)) {               // 50 ms budget
//     Wire.beginTransmission(addr);
//     ...
//     Wire.endTransmission();
//     guard.unlock();
//   }
//
// Design notes (audit p.100):
//   - FreeRTOS MUTEX, not portENTER_CRITICAL: an I²C transaction can run for
//     hundreds of microseconds to milliseconds at 100 kHz; a spinlock
//     critical section of that length would starve the other core.
//   - Recursive (xSemaphoreCreateRecursiveMutex) so nested acquisition within
//     one task (e.g. driver calls helper that also locks) cannot self-deadlock.
// ============================================================================
#ifndef UTILS_I2C_BUS_GUARD_H
#define UTILS_I2C_BUS_GUARD_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Utils {

class I2cBusGuard {
public:
  I2cBusGuard() {
    _mutex = xSemaphoreCreateRecursiveMutex();
  }

  // Attempt to take the bus within `timeoutMs`. Returns true when the caller
  // owns the bus and MUST call unlock() afterwards.
  bool lock(uint32_t timeoutMs = 50) {
    if (_mutex == nullptr) return false;
    TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTakeRecursive(_mutex, ticks) == pdTRUE;
  }

  void unlock() {
    if (_mutex == nullptr) return;
    xSemaphoreGiveRecursive(_mutex);
  }

private:
  SemaphoreHandle_t _mutex;
};

// Process-wide singleton — every I²C participant must use THE SAME guard.
extern I2cBusGuard i2cBus;

}  // namespace Utils

#endif  // UTILS_I2C_BUS_GUARD_H
