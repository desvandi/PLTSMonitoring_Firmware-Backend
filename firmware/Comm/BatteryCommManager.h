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
// =============================================================================

#pragma once
#ifndef PLTS_COMM_BATTERY_COMM_MANAGER_H
#define PLTS_COMM_BATTERY_COMM_MANAGER_H

#include "BatteryProtocol.h"
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
  float crossCheckShunt(float shuntCurrentA, uint32_t nowMs);
  bool   isMismatchActive() const { return _mismatchActive; }
  float  getLastMismatchA() const { return _lastMismatchA; }

  // BMS-side SOC provenance decision. The energyTask still owns the final
  // merge (shunt fallback), but this is the authoritative BMS answer.
  bool   socAuthoritative() const;     // locked + fresh + SOC plausible

  // Runtime reconfiguration (from ConfigHandlers POST /api/config/battery)
  void reconfigure();                  // re-reads cfg* and rebuilds clients

private:
  void _rebuildClients();
  BatteryProtocolClient* _clientFor(ProtocolId id);
  void _setState(State s);
  void _log(const char* event, ProtocolId p);

  mutable SemaphoreHandle_t _mutex = nullptr;
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
