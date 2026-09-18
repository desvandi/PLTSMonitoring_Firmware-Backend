// =============================================================================
// Drivers/RelayExpanderDriver.cpp — PCF8574 I²C 8-channel relay driver
// =============================================================================
#include "RelayExpanderDriver.h"
#if PLTS_ENABLE_RELAYS
#include "../Core/Config.h"
#include "../Utils/I2cBusGuard.h"
#include "../Services/LogService.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Drivers {

RelayExpanderDriver relayExpander;

// [GATE-1 / PH8-01] Bounded mutex helpers — FreeRTOS mutex (priority
// inheritance). Created lazily but BEFORE any cross-task use is possible:
// begin() runs in setup() (single-tasked), and every runtime path calls
// _ensureWriteMutex() under the (already created) guarantee.
void RelayExpanderDriver::_ensureWriteMutex() {
  if (_safetyWriteMutex == nullptr) {
    _safetyWriteMutex = xSemaphoreCreateMutex();
  }
}

bool RelayExpanderDriver::_writeLock(uint32_t waitMs) {
  _ensureWriteMutex();
  return xSemaphoreTake((SemaphoreHandle_t)_safetyWriteMutex,
                        pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void RelayExpanderDriver::_writeUnlock() {
  if (_safetyWriteMutex != nullptr) {
    xSemaphoreGive((SemaphoreHandle_t)_safetyWriteMutex);
  }
}

bool RelayExpanderDriver::begin(uint8_t i2cAddress) {
  _address = i2cAddress;
  _ensureWriteMutex();   // [GATE-1 / PH8-01] created in single-tasked setup()

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

  // [GATE-1 / PH8-01] ALL mutations (ON and OFF) are serialized through the
  // write mutex. ON-direction additionally re-checks the safety latch INSIDE
  // the critical region. The OFF path MUST also hold the mutex: the
  // read-modify-write of _outputState against a stale base could otherwise
  // resurrect ON bits that a concurrent emergency 0xFF write had just
  // cleared (stale-OFF-erases-emergency race — same class as PH8-01).
  // OFF-direction writes are still ALWAYS allowed (safe direction) — the
  // latch check below only gates ON.
  if (!_writeLock(ON_LOCK_MS)) {
    // Cannot serialize against the emergency path — fail-closed, refuse the
    // mutation. NEVER write without holding this ordering.
    Serial.println("[RELAY] setChannel refused — safety write mutex unavailable (fail-closed)");
    return false;
  }

  bool ok = false;
  if (on && _safetyLatched.load(std::memory_order_acquire)) {
    Serial.println("[RELAY] setChannel(ON) refused — SAFETY LATCHED (emergency epoch; operator ARM required)");
  } else {
    ok = _setChannelUnlocked(channel, on);
  }
  _writeUnlock();
  return ok;
}

/// [GATE-1 / PH8-01] Unlocked mutation — caller MUST hold _safetyWriteMutex
/// for ON-direction writes (OFF-direction writes are safe-direction and may
/// run without it).
bool RelayExpanderDriver::_setChannelUnlocked(uint8_t channel, bool on) {
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

// [GATE-1 / PH8-01] EMERGENCY BARRIER — see header for the interleaving
// proof. Latch is set ATOMICALLY and FIRST (independent of every mutex:
// even if the physical write never completes, every subsequent ON-direction
// setChannel() is already refused). The bank OFF write is ONE 0xFF
// transaction under the write mutex (bounded wait; fallback direct write).
void RelayExpanderDriver::forceSafetyAllOff() {
  _safetyLatched.store(true, std::memory_order_release);   // LATCH FIRST
  if (!_available) return;

  uint8_t allOffState = Core::PCF8574_POWER_ON_STATE;      // 0xFF = all OFF
  if (_writeLock(FORCE_ALLOFF_LOCK_MS)) {
    if (_writeOutput(allOffState)) {
      _outputState = allOffState;
      _shadowUnknown = false;
    } else {
      _shadowUnknown = true;
    }
    _writeUnlock();
  } else {
    // [PH8-02 fallback] Executor wedged >250 ms inside the write mutex
    // (hung I²C transaction). The LATCH is already set — ON writes are
    // refused from this instant. Attempt the direct transaction anyway:
    // the I2cBusGuard still serializes bus access, and 0xFF is the
    // fail-safe direction. Outcome honesty via shadowUnknown + alarms at
    // the controller level.
    Serial.println("[RELAY] forceSafetyAllOff: write mutex starved — direct 0xFF attempt (degraded)");
    if (_writeOutput(allOffState)) {
      _outputState = allOffState;
      _shadowUnknown = false;
    } else {
      _shadowUnknown = true;
    }
  }
}

// [GATE-1 / PH8-01] ONLY the explicit operator ARM path calls this —
// EmergencySupervisor::_arm() after crash-chain/sensor/E-stop gates pass.
void RelayExpanderDriver::clearSafetyLatch() {
  _safetyLatched.store(false, std::memory_order_release);
  Serial.println("[RELAY] safety latch CLEARED by operator ARM — relay ON authority restored");
}

uint8_t RelayExpanderDriver::readState() {
  if (!_available) return 0xFF;
  if (_shadowUnknown) return 0xFF;  // unknown → report fail-safe value, caller checks isShadowUnknown()
  return _outputState;
}

void RelayExpanderDriver::allOff() {
  // Safety path — ALWAYS attempted even when the shadow is unknown: driving
  // 0xFF is the fail-safe direction, and re-asserting it can only help.
  // [GATE-1 / PH8-01] Serialized through the write mutex like every other
  // register mutation (stale-RMW resurrection guard). Does NOT touch the
  // safety latch — see forceSafetyAllOff() for the latched barrier.
  if (!_available) return;
  uint8_t allOffState = Core::PCF8574_POWER_ON_STATE;  // 0xFF = all OFF
  if (_writeLock(FORCE_ALLOFF_LOCK_MS)) {
    if (_writeOutput(allOffState)) {
      _outputState = allOffState;
      _shadowUnknown = false;
    } else {
      _shadowUnknown = true;
    }
    _writeUnlock();
  } else {
    if (_writeOutput(allOffState)) {
      _outputState = allOffState;
      _shadowUnknown = false;
    } else {
      _shadowUnknown = true;
    }
  }
}

bool RelayExpanderDriver::recoverWithAllOff() {
  if (!_available) return false;
  // Attempt: write all-off, then verify via readback (with failure
  // distinguished from a legit 0xFF).
  // [GATE-1 / PH8-01] Serialized like every register mutation.
  if (!_writeLock(FORCE_ALLOFF_LOCK_MS)) return false;
  uint8_t allOffState = Core::PCF8574_POWER_ON_STATE;
  bool ok = _writeOutput(allOffState);
  uint8_t readback = 0;
  if (ok) ok = _readInput(readback);
  if (ok && readback != allOffState) ok = false;
  if (ok) {
    _outputState = allOffState;
    _shadowUnknown = false;
  }
  _writeUnlock();
  return ok;
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
