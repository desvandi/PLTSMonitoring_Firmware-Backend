// =============================================================================
// Services/SocStateMachine.h — SOC Engine with quality-gated integration
// -----------------------------------------------------------------------------
// REMEDIATION 2026-08 (Audit #1+#2+#3 — FW-12 / P1-006):
//   SOC is now PERSISTED (NVS namespace "plts_soc") with validity metadata:
//     soc, coulombBaselineAh, lastSyncUnix, capacityAh basis, state version,
//     magic + CRC32. Reboot policy (directive §13 / addendum §13):
//       - valid persisted state + matching capacity basis → RESTORE, quality
//         = ESTIMATED (coulomb basis), lastSync preserved
//       - corrupt / version mismatch / capacity-basis mismatch / first boot
//         → SOC = UNKNOWN (value NaN, quality NOT_AVAILABLE) — NEVER a
//           fabricated default (not 50%, not 100%)
//       - UNKNOWN resolves only via: full-charge confirmation (→ 100%),
//         manual operator sync (REST API), or boot OCV sync after the pack
//         has been at rest (|I| < idle threshold) for REST_WINDOW_SEC
//
// Phase 13-D changes (retained):
#include "../Core/Common.h"
//   - tick() now accepts monotonicMs instead of wall-clock nowSec
//   - Quality gate: ONLY Valid quality is integrated
//   - SOC bounds enforced: 0 <= SOC <= 100 always
//   - NaN/Inf rejected
//   - Invalid measurement → SOC FREEZE (no phantom movement)
//
// Invariants (directive §70):
//   - initialization deterministic
//   - SOC bounded 0..100
//   - monotonic integration
//   - invalid input rejected
//   - quality respected
//   - no phantom SOC movement during sensor failure
//   - full-charge logic defined
//   - persistence implemented (NVS checkpoint + metadata)  ← closed 2026-08
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_SOC_STATE_MACHINE_H
#define PLTS_SERVICES_SOC_STATE_MACHINE_H

#include <Arduino.h>
#include "../Core/Types.h"
#include "../Core/Config.h"

namespace Services {

// [AUDIT 2026-09 ROUND 5 / p.447 + p.446] Evidence for the two V-based SOC
// synchronization events (full-charge confirmation and boot OCV-at-rest).
// Populated by energyTask from BMS + environment data every cycle — the SOC
// engine itself stays free of Comm/ dependencies (plain data in, decision
// out). Default: everything unknown/false → legacy V+I behavior, so a system
// WITHOUT a BMS keeps working exactly as before (documented fallback).
struct SocSyncEvidence {
  // BMS-side evidence (valid only when bmsHealthy is true)
  bool  bmsHealthy;       // LOCKED + fresh + plausible SOC + faultFlags == 0 + no current mismatch
  bool  bmsAgreesFull;    // healthy BMS reports SOC >= BMS_FULL_AGREE_PCT
  bool  cellsBad;         // cell voltages PRESENT and (overvoltage OR imbalance beyond limits)
                           // (unknown cells ≠ bad — a BMS that reports no cell data
                           //  must not veto the V+I path)
  bool  mismatchActive;   // sustained BMS↔shunt current disagreement — both
                           // instruments under dispute, no new SOC basis
  // Environment-side evidence (NaN-safe flags computed by the caller)
  bool  tempValid;         // battery (BMS) or ambient (SHT31) temperature is a valid float
  float temperatureC;      // the selected temperature reading
};

struct SocBaselineCorrectionEvent {
  uint32_t timestamp;
  float    oldSoc;
  float    newSoc;
  float    voltage;
  float    current;
  uint32_t durationSec;
  char     reason[32];
};

class SocStateMachine {
public:
  void begin();

  // Phase 13-D: tick() accepts MONOTONIC milliseconds (not wall-clock seconds).
  // RC-13 fix: dt computed from millis() — clock-jump immune.
  // Only integrates when quality == Valid (directive §28, §70).
  // On invalid quality: SOC FREEZE — retains last valid SOC, no phantom movement.
  void tick(float voltage, float current,
            Core::MeasurementQuality vq,
            Core::MeasurementQuality iq,
            uint32_t monotonicMs);

  // [P1-012] NaN when SOC is UNKNOWN (never a fabricated number).
  float    getSoc() const { return _socValid ? _soc : NAN; }
  bool     isSocValid() const { return _socValid; }

  // [P1-011] Quality propagation: SOC quality derives from its inputs.
  Core::MeasurementQuality getSocQuality() const;

  Core::SocState getState() const { return _state; }
  // v1.6.0 — safe state string for logging/provenance logic.
  const char* getStateStrSafe() const { return Core::socStateToStr(_state); }
  // v1.6.0 — true while the ONLY basis of the current SOC is the boot-time
  // OCV-at-rest estimate (no manual/BMS/full-charge sync has replaced it).
  // Used to label provenance OCV_ESTIMATED honestly.
  bool     socCameFromOcv() const { return _socFromOcv; }
  uint32_t getLastSyncTs() const { return _lastSyncTs; }
  float    getEstimatedUsableCapacityAh() const { return _estimatedUsableAh; }

  // Phase 13-D: is SOC currently frozen due to quality?
  bool     isFrozen() const { return _frozen; }
  Core::MeasurementQuality getLastIntegrationQuality() const { return _lastIntegrationQuality; }

  void synchronizeFromVoltage(float voltage, const char* reason);
  void setSoc(float soc, const char* reason);
  void resetToFull();
  SocBaselineCorrectionEvent getLastCorrection() const { return _lastCorrection; }
  void setCoulombBaseline(float baselineAh, const char* reason);

  // [p.447/p.446] Evidence input — call every cycle BEFORE tick().
  void setSyncEvidence(const SocSyncEvidence& e) { _evidence = e; }
  const SocSyncEvidence& getSyncEvidence() const { return _evidence; }

  // [FW-12] Persistence — checkpoint (call from persistenceTask ~5 min and
  // before deliberate reboots). Load happens in begin().
  // [p.452] Returns false when the NVS write failed (begin/putBytes) so the
  // persistence task can raise STORAGE_ERROR — never assume "saved".
  bool saveToNVS();
  void loadFromNVS();
  uint32_t persistFailures() const { return _persistFailures; }

  // Phase 13-D: quality gate
  static bool qualityAllowsIntegration(Core::MeasurementQuality q) {
    return q == Core::MeasurementQuality::Valid;
  }

  // [FW-12] Boot-OCV policy window: UNKNOWN resolves via OCV only after the
  // pack has been at rest this long (open-circuit voltage is only valid at
  // rest — integrating a loaded voltage would fabricate SOC).
  static constexpr uint32_t REST_WINDOW_SEC = 1800;   // 30 min at rest

  // [p.446] OCV temperature window — outside it, a LiFePO4 OCV reading is
  // too temperature-dependent to be trusted as an SOC basis (rejection, not
  // a fabricated compensation curve — no characterized dataset exists yet).
  static constexpr float OCV_TEMP_MIN_C =   0.0f;
  static constexpr float OCV_TEMP_MAX_C =  45.0f;

private:
  float    _soc = 0.0f;
  bool     _socValid = false;            // [P1-012] UNKNOWN until proven
  float    _coulombBaselineAh = 0.0f;
  Core::SocState _state = Core::SocState::Normal;
  uint32_t _lastSyncTs = 0;              // UNIX EPOCH domain [p.445 — single domain]
  uint32_t _fullCandidateStartMs = 0;  // Phase 13-D: monotonic ms
  float    _estimatedUsableAh = 200.0f;
  SocBaselineCorrectionEvent _lastCorrection = {};
  SocSyncEvidence _evidence = {};        // [p.446/p.447] default: all unknown
  uint32_t _persistFailures = 0;         // [p.452] NVS write failures (this boot)

  // Phase 13-D: monotonic time tracking
  uint32_t _lastMonotonicMs = 0;
  float    _lastCurrent = 0.0f;
  bool     _frozen = false;  // true when quality blocks integration
  Core::MeasurementQuality _lastIntegrationQuality = Core::MeasurementQuality::NotAvailable;

  // [FW-12] Boot OCV-at-rest tracker
  uint32_t _restStartMonotonicMs = 0;
  bool     _restWindowSatisfied = false;
  bool     _socFromOcv = false;   // v1.6.0: provenance — OCV is the current basis

  void _recordBaselineCorrection(float oldSoc, float newSoc, float v, float i,
                                  uint32_t durationSec, const char* reason);
  float _voltageToSoc(float v) const;

  // Phase 13-D: enforce SOC bounds [0, 100]
  static float clampSoc(float soc) {
    if (!Core::isValidFloat(soc)) return 0.0f;  // NaN/Inf → 0
    if (soc < 0.0f) return 0.0f;
    if (soc > 100.0f) return 100.0f;
    return soc;
  }
};

extern SocStateMachine socStateMachine;

} // namespace Services

#endif // PLTS_SERVICES_SOC_STATE_MACHINE_H
