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
//
// [GATE-1 / PH8-01 + PH8-02 REMEDIATION 2026-09 — SAFETY LATCH]
// audit Phase 8 S0: the safety-generation check in RelayController ran BEFORE
// the physical I²C write — emergencyTask (core 0, prio 3) could preempt
// relayTask (core 0, prio 2) between "generation check PASS" and
// setChannel(ON), execute the full E-WAVE cascade, and the resumed relayTask
// would still write ON. Software generations alone are NOT an atomic
// physical-output interlock.
//
// This driver is now the ATOMIC BARRIER between the two authorities:
//   _safetyLatched (std::atomic<bool>, boot=true — the system boots isolated)
//     set by   forceSafetyAllOff()   — latch FIRST (atomic), then ONE 0xFF
//                                    transaction (single write = whole bank)
//     cleared by clearSafetyLatch() — ONLY the explicit operator ARM path
//                                    (EmergencySupervisor::_arm, after the
//                                    crash-chain/sensor/E-stop gates pass)
//   setChannel(ch, ON) is REFUSED while latched — the check runs under the
//   same _safetyWriteMutex that forceSafetyAllOff() takes for its physical
//   write, so the latch-check + I²C-write pair is serialized against the
//   emergency 0xFF write. Any interleaving ends with the bank OFF:
//     (a) ON write completes first -> emergency 0xFF overwrites it   -> OFF
//     (b) emergency completes first -> latch check rejects the ON    -> OFF
//     (c) emergency preempts mid-ON -> waits on the write mutex, ON
//         completes, then 0xFF overwrites                            -> OFF
//   Fallback (write mutex starved > FORCE_ALLOFF_LOCK_MS, i.e. the executor
//   is wedged inside a hung I²C transaction): the latch is still set (it is
//   atomic and independent of the mutex) and a direct 0xFF transaction is
//   attempted — the I2cBusGuard still serializes the bus. Residual risk in
//   that pathological window is documented as REQUIRING the physical barrier
//   (E-WAVE kontaktor cutting the relay-bank enable) — see PH8-07 hardware
//   recommendation; software state is never claimed as physical proof.
//   [PH8-02] forceSafetyAllOff() needs NO RelayController mutex, NO queue, NO
//   journal, NO network — callable from the EmergencySupervisor starvation
//   watchdog when its own mutex is held by a stuck task.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H
#define PLTS_DRIVERS_RELAY_EXPANDER_DRIVER_H

#include <Arduino.h>
#include <atomic>
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
  /// NOTE: does NOT manage the safety latch — see forceSafetyAllOff().
  void allOff();

  /// [GATE-1 / PH8-01] Latched emergency state — ON-direction writes in
  /// setChannel() are refused while this is true. Atomic read.
  bool safetyLatched() const {
    return _safetyLatched.load(std::memory_order_acquire);
  }

  /// [GATE-1 / PH8-01] EMERGENCY BARRIER: latch the safety gate (atomic,
  /// FIRST — independent of every mutex), then drive the whole bank OFF in
  /// ONE 0xFF transaction under the write mutex. Callable from ANY task
  /// (emergencyTask starvation watchdog included) — needs no controller
  /// state, no queue, no journal, no network. Idempotent.
  void forceSafetyAllOff();

  /// [GATE-1 / PH8-01] Release the safety latch — the ONLY caller is the
  /// explicit operator ARM path (EmergencySupervisor::_arm after gates).
  /// Never called from a command, queue, or network path.
  void clearSafetyLatch();

  /// [audit p.92-93] Verified safe recovery after a failed write:
  ///   write 0xFF → readback verify → (on success) shadow = 0xFF, known.
  /// Returns true when the driver is usable again (ALL channels OFF).
  /// Does NOT touch the safety latch (recovery is bus health, not ARM).
  bool recoverWithAllOff();

private:
  bool _available = false;
  bool _shadowUnknown = false;  // last write outcome unknown
  uint8_t _address = Core::PCF8574_I2C_ADDRESS_DEFAULT;
  uint8_t _outputState = Core::PCF8574_POWER_ON_STATE;  // mirror of PCF8574 output register

  /// [GATE-1 / PH8-01] Atomic safety latch — boot = LATCHED (the system boots
  /// isolated; the relay ON authority returns only with an operator ARM).
  std::atomic<bool> _safetyLatched{true};

  /// [GATE-1 / PH8-01] Serializes {latch-check + I²C write} (ON path) against
  /// {latch-set + 0xFF write} (emergency path). FreeRTOS mutex (priority
  /// inheritance — emergencyTask prio 3 never stays blocked behind a prio-2
  /// ON writer longer than one bounded I²C transaction). Kept opaque.
  void* _safetyWriteMutex = nullptr;
  static const uint32_t ON_LOCK_MS = 100;            // ON-path fail-closed bound
  static const uint32_t FORCE_ALLOFF_LOCK_MS = 250;  // emergency bounded wait
  bool _writeLock(uint32_t waitMs);
  void _writeUnlock();
  void _ensureWriteMutex();

  /// [GATE-1 / PH8-01] Mutation without latch handling — caller holds the
  /// write mutex for ON-direction writes. (OFF is safe-direction, lock-free.)
  bool _setChannelUnlocked(uint8_t channel, bool on);

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
