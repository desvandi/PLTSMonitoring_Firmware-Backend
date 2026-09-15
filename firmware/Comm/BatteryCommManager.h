// =============================================================================
// Comm/BatteryCommManager.h — Auto-detect orchestrator + provenance owner
// -----------------------------------------------------------------------------
// STATE MACHINE (never silent — every transition is logged via LogService):
//
//   DISABLED ──(config "none")──────────────────────────────► [stays]
//       │ config "auto" or explicit protocol
//       ▼
//   PROBING ──client locked (2 consecutive valid reads)──► LOCKED
//       │  all enabled clients exhausted, cooldown 60 s    │ 3 consecutive
//       ▼                                                  ▼ failures
//   PROBING (retry cycle) ◄──── cooldown 60 s ────── LOST (alarm raised)
//       │ no client ever succeeds (silent bench)           │ recovery: 2 valid
//       ▼                                                  ▼ reads again
//   IDLE_NO_BMS ── INA219/ACS712 shunt path is the truth ◄─ LOCKED
//
//   Explicit protocol config (not "auto") → PROBING probes ONLY that client.
//
// PROVENANCE CASCADE (single source of truth for soc_source):
//   BMS is LOCKED and data fresh      → SOC provenance = BMS_DIRECT
//   BMS absent/lost                   → SOC provenance = SHUNT_COULOMB
//   SOC resolved at boot via OCV rest → SOC provenance = OCV_ESTIMATED
//   SOC unknown                       → SOC provenance = UNKNOWN
// The manager exposes the BMS side; the .ino energyTask merges with the
// shunt side (it owns SocStateMachine).
//
// REDUNDANCY CROSS-CHECK (honesty amplifier, closes limitation L2):
//   When BMS current AND INA219 shunt current are both valid:
//     mismatch_A = |I_bms − I_shunt|
//     mismatch_A > max(0.5 A, 5%·|I_shunt|) sustained ≥ 3 polls
//       → BMS_CURRENT_MISMATCH alarm + SOC degraded to SUSPECT quality
//   A wrong Modbus sign convention (L2-class error) is caught here within
//   one poll cycle instead of silently inverting energy accounting.
//
// THREAD MODEL: tick() runs in bmsTask (own stack, WDT-guarded). Consumers
// (energyTask, Web handlers) call the getters which copy under mutex.
//
// [AUDIT 2026-09 ROUND 10 / p.474] SNAPSHOT + LOCK CONTRACT — fail-closed:
//   A. begin() treats mutex-creation failure as FATAL (boot guard — Serial +
//      Log FATAL, halt WITHOUT feeding the task watchdog → TWDT panic reset,
//      honest crash-chain). The manager never runs its state machine with
//      unsynchronized readers/writers attached.
//   B. crossCheckShunt() previously read _data.current BEFORE even checking
//      the mutex and never took it at all — a reader running in energyTask
//      against tick()'s mutex-guarded _data writes in bmsTask (torn struct
//      reads; the header's own "getters copy under mutex" contract was
//      violated by this one path). It now takes the mutex FIRST, reads the
//      BMS snapshot + mutates the cross-check streak state under it.
//   C. getData()/socAuthoritative()/isMismatchActive()/getLastMismatchA()
//      all acquire the mutex (readers run in web + energy tasks).
//   D. Acquisition fails closed: _lock() retries creation ONCE; still null →
//      count (atomic) + rate-limited CRIT log + return false.
//      A take TIMEOUT is NOT a lock failure (counter untouched): tick()
//      keeps its bounded-wait skip-cycle semantics, getters keep theirs.
//
//   Degraded-mode (mutex unavailable) per-entry contract:
//     tick()             → skip this cycle (unchanged semantics)
//     getData()          → zero-value BmsData (NAN fields, not fresh —
//                          consumers already treat that as "no BMS data")
//     crossCheckShunt()  → NAN, cross-check state untouched
//     socAuthoritative() → false (BMS loses SOC authority — conservative:
//                          the shunt path becomes the truth, exactly the
//                          documented fallback)
//     isMismatchActive() → false (no alarm interlock action on unknown
//                          state; with socAuthoritative() also false the
//                          BMS is not consumed as truth anyway)
//     getLastMismatchA() → NAN
//     lockFailures()     → lock-free atomic read — the ONE accessor that
//                          works in the degraded mode (diagnostics path)
// =============================================================================

#pragma once
#ifndef PLTS_COMM_BATTERY_COMM_MANAGER_H
#define PLTS_COMM_BATTERY_COMM_MANAGER_H

#include "BatteryProtocol.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Comm {

class BatteryCommManager {
public:
  enum class State : uint8_t {
    Disabled,      // config "none" — comm layer fully off
    Probing,       // cycling candidates
    Locked,        // one protocol locked, polling
    Lost,          // was locked, lost it — cooling down before re-probe
    IdleNoBms      // probed everything, nothing answered (shunt path truth)
  };

  static constexpr uint32_t LOCK_SUCCESSES_REQUIRED   = 2;    // hysteresis in
  static constexpr uint32_t LOST_FAILURES_REQUIRED    = 3;    // hysteresis out
  static constexpr uint32_t REPROBE_COOLDOWN_MS       = 60000;
  static constexpr uint32_t PROBE_ATTEMPTS_PER_CLIENT = 3;
  static constexpr uint32_t PROBE_RESPONSE_WINDOW_MS = 1500;
  static constexpr uint32_t MISMATCH_SUSTAIN_POLLS   = 3;

  void begin();
  void end();                       // before OTA reboot
  void tick(uint32_t nowMs);        // called from bmsTask loop

  // ---- Consumer getters (mutex-copied snapshots) ----
  State       getState() const;
  const char* stateStr() const;
  ProtocolId  activeProtocol() const;
  const char* activeProtocolStr() const;
  bool        isLocked() const;
  BmsData     getData() const;      // snapshot copy
  uint32_t    getLockMs() const;    // when we locked (uptime ms)
  uint32_t    getProbeCycleCount() const;

  // ---- Redundancy cross-check (called by energyTask each cycle) ----
  // shuntCurrentA: INA219 signed current (NaN when invalid).
  // Returns the current mismatch in A (NAN when not computable).
  // [p.474] Takes _mutex BEFORE reading _data (the old shape read
  // _data.current first and never locked — the reader race this round
  // removes). NAN + untouched streak state when the lock is unavailable.
  float crossCheckShunt(float shuntCurrentA, uint32_t nowMs);
  // [p.474] Reader-side serialization (written by energyTask under the
  // mutex, read from web + energy tasks). Fail-closed contract in the
  // header block above.
  bool   isMismatchActive() const;
  float  getLastMismatchA() const;

  // BMS-side SOC provenance decision. The energyTask still owns the final
  // merge (shunt fallback), but this is the authoritative BMS answer.
  // [p.474] Reads its _state/_data/_mismatchActive snapshot under _mutex
  // (called from web handlers AND energyTask — the old unlocked read was
  // documented as benign single-writer, but the round-10 snapshot contract
  // makes every cross-task reader take the lock). Fail-closed → false.
  bool   socAuthoritative() const;

  // [p.474] Lock-free atomic read — the ONE accessor working when the mutex
  // is unavailable (diagnostics path). Non-zero = at least one operation
  // was refused fail-closed since boot.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

  // Runtime reconfiguration (from ConfigHandlers POST /api/config/battery)
  void reconfigure();                  // re-reads cfg* and rebuilds clients

private:
  void _rebuildClients();
  BatteryProtocolClient* _clientFor(ProtocolId id);
  void _setState(State s);
  void _log(const char* event, ProtocolId p);

  mutable SemaphoreHandle_t _mutex = nullptr;

  // [p.474] Fail-closed accounting — ATOMIC by necessity: written on the
  // mutex-UNAVAILABLE path where no synchronization exists.
  mutable std::atomic<uint32_t> _lockFailures{0};
  mutable std::atomic<uint32_t> _lastLockFailLogMs{0};   // rate-limit bookkeeping

  // [p.474] Fail-closed acquisition: retry-create ONCE; still null → count
  // + rate-limited CRIT log + false. A take TIMEOUT returns false WITHOUT
  // counting (bounded-wait skip semantics are pre-existing design, not the
  // unavailable-mutex class).
  bool _lock(uint32_t timeoutMs) const;
  void _unlock() const;

  State        _state = State::Disabled;
  ProtocolId   _active = ProtocolId::None;
  BmsData      _data;                 // last valid data of the active client
  uint32_t     _lockMs = 0;
  uint32_t     _probeCycle = 0;

  // probe iteration state
  uint8_t      _probeIdx = 0;
  uint32_t     _probeAttempts = 0;
  uint32_t     _probeStartMs = 0;
  bool         _probeWaiting = false;
  BatteryProtocolClient* _probing = nullptr;
  uint32_t     _lockSuccesses = 0;
  uint32_t     _lostFailures = 0;
  uint32_t     _lostSinceMs = 0;

  // poll state (Locked)
  uint32_t     _lastPollMs = 0;
  bool         _pollAwaiting = false;
  uint32_t     _pollSentMs = 0;
  uint32_t     _pollFailures = 0;

  // cross-check
  float        _lastMismatchA = NAN;
  uint32_t     _mismatchStreak = 0;
  bool         _mismatchActive = false;

  // storage for enabled client instances (rebuilt on reconfigure)
  void*        _clients[4] = {nullptr, nullptr, nullptr, nullptr};
  uint8_t      _clientCount = 0;
};

extern BatteryCommManager batteryComm;

} // namespace Comm

#endif // PLTS_COMM_BATTERY_COMM_MANAGER_H
