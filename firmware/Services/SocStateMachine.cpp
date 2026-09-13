// =============================================================================
// Services/SocStateMachine.cpp — SOC Engine with quality-gated integration
// =============================================================================
#include "SocStateMachine.h"
#include "EnergyCounters.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Storage/ConfigStore.h"
#include "../Drivers/RtcDriver.h"
#include "LogService.h"
#include "AlarmRegistry.h"
#include "../Utils/Crc.h"
#include <Preferences.h>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace Services {

SocStateMachine socStateMachine;

// [FW-12] NVS persistence layout — versioned + CRC-protected
namespace {
constexpr uint32_t SOC_NVS_MAGIC   = 0x534F4331u;   // "SOC1"
constexpr uint32_t SOC_STATE_VERSION = 2u;
struct SocPersistState {
  uint32_t magic;
  uint32_t version;
  float    soc;
  float    coulombBaselineAh;
  uint32_t lastSyncUnix;
  float    capacityAhBasis;   // capacity the baseline was computed against
  uint32_t crc32;             // over all preceding fields
};
}

// Rough LiFePO4 15S open-circuit voltage → SOC map (used for sync only)
static float voltageToSocRough(float v) {
  if (v >= 54.0f) return 100.0f;
  if (v >= 53.0f) return 95.0f + (v - 53.0f) * 5.0f;
  if (v >= 52.0f) return 80.0f + (v - 52.0f) * 15.0f;
  if (v >= 50.0f) return 50.0f + (v - 50.0f) * 15.0f;
  if (v >= 48.0f) return 20.0f + (v - 48.0f) * 15.0f;
  if (v >= 45.0f) return (v - 45.0f) * 10.0f;
  return 0.0f;
}

void SocStateMachine::begin() {
  _soc = 0.0f;
  _socValid = false;                 // [P1-012] UNKNOWN until restored/synced
  _coulombBaselineAh = 0.0f;
  _state = Core::SocState::Normal;
  _lastSyncTs = 0;
  _fullCandidateStartMs = 0;
  _estimatedUsableAh = Core::cfgBatteryCapacityAh;
  _lastCorrection = {};
  _lastMonotonicMs = 0;
  _lastCurrent = 0.0f;
  _frozen = false;
  _lastIntegrationQuality = Core::MeasurementQuality::NotAvailable;
  _restStartMonotonicMs = 0;
  _restWindowSatisfied = false;

  // [FW-12] Restore persisted SOC state (validates magic/version/CRC/capacity)
  loadFromNVS();

  Serial.printf("[SOC] init: %s | nominalCap=%.1f Ah, fullV=%.1f, endA=%.2f, persistS=%u\n",
                _socValid ? "RESTORED from NVS" : "UNKNOWN (no valid persisted state)",
                (double)Core::cfgBatteryCapacityAh, (double)Core::cfgFullVoltage,
                (double)Core::cfgFullChargeCurrentThreshold,
                Core::cfgFullChargePersistenceSec);
}

Core::MeasurementQuality SocStateMachine::getSocQuality() const {
  if (!_socValid) return Core::MeasurementQuality::NotAvailable;   // UNKNOWN
  if (_frozen && _lastIntegrationQuality != Core::MeasurementQuality::Valid) {
    // [P1-011] Propagate input quality — SOC is only as good as its inputs.
    return _lastIntegrationQuality;
  }
  return Core::MeasurementQuality::Estimated;
}

bool SocStateMachine::saveToNVS() {
  SocPersistState st = {};
  st.magic = SOC_NVS_MAGIC;
  st.version = SOC_STATE_VERSION;
  st.soc = _socValid ? _soc : 0.0f;
  st.coulombBaselineAh = _coulombBaselineAh;
  st.lastSyncUnix = _lastSyncTs;
  st.capacityAhBasis = Core::cfgBatteryCapacityAh;
  // CRC over everything except the trailing crc32 field itself
  st.crc32 = Utils::crc32((const uint8_t*)&st, sizeof(st) - sizeof(uint32_t));

  Preferences p;
  if (!p.begin("plts_soc", false)) {
    _persistFailures++;   // [p.452] honest failure accounting — never fake "saved"
    return false;
  }
  size_t w = p.putBytes("state", &st, sizeof(st));
  p.end();
  if (w != sizeof(st)) {
    _persistFailures++;
    return false;
  }
  return true;
}

void SocStateMachine::loadFromNVS() {
  Preferences p;
  if (!p.begin("plts_soc", true)) return;   // no namespace yet → first boot
  SocPersistState st = {};
  size_t got = p.getBytes("state", &st, sizeof(st));
  p.end();
  if (got != sizeof(st)) return;                        // wrong size → invalid
  if (st.magic != SOC_NVS_MAGIC || st.version != SOC_STATE_VERSION) return;
  uint32_t crc = Utils::crc32((const uint8_t*)&st, sizeof(st) - sizeof(uint32_t));
  if (crc != st.crc32) {
    Log.append(Core::LogType::SocBaselineCorrected,
               "Persisted SOC state corrupt (CRC) — SOC = UNKNOWN", 0);
    return;                                             // corrupt → UNKNOWN
  }
  // Capacity basis changed (battery replaced / reconfigured) → old baseline
  // is meaningless → UNKNOWN rather than a rescaled guess.
  if (std::fabs(st.capacityAhBasis - Core::cfgBatteryCapacityAh) > 0.5f) {
    Log.append(Core::LogType::SocBaselineCorrected,
               "Persisted SOC capacity basis mismatch — SOC = UNKNOWN", 0);
    return;
  }
  if (!Core::isValidFloat(st.soc) || st.soc < 0.0f || st.soc > 100.0f) return;

  // [p.445] Single time-domain enforcement on load: lastSyncUnix must be a
  // plausible UNIX epoch for this product. Builds ≤ 1.9.4 wrote UPTIME
  // seconds into this field on the full-charge path (monotonicMs / 1000),
  // producing values like 43812 that read as 1970-01-01 epoch dates after a
  // reboot. Anything older than 2020-01-01 is treated as "unknown sync
  // time" (0) — the SOC value itself is still valid (CRC + basis checks
  // passed), only the sync timestamp is discarded.
  if (st.lastSyncUnix != 0 && st.lastSyncUnix < 1577836800u) {
    Log.append(Core::LogType::SocBaselineCorrected,
               "Persisted SOC sync timestamp in wrong time domain (legacy "
               "uptime-seconds) — sync time reset to unknown", 0);
    st.lastSyncUnix = 0;
  }

  _soc = st.soc;
  _coulombBaselineAh = st.coulombBaselineAh;
  _lastSyncTs = st.lastSyncUnix;
  _socValid = true;
}

float SocStateMachine::_voltageToSoc(float v) const {
  return voltageToSocRough(v);
}

void SocStateMachine::tick(float voltage, float current,
                            Core::MeasurementQuality vq,
                            Core::MeasurementQuality iq, uint32_t monotonicMs) {

  // ===========================================================================
  // INVARIANT: Quality gate (directive §28, §70)
  // ONLY Valid quality is integrated.
  // On invalid quality: SOC FREEZE — no phantom movement.
  // ===========================================================================
  if (!qualityAllowsIntegration(vq) || !qualityAllowsIntegration(iq)) {
    // SOC FREEZE: retain last valid SOC, mark as frozen
    _frozen = true;
    _lastIntegrationQuality = (vq != Core::MeasurementQuality::Valid) ? vq : iq;
    // Reset monotonic timestamp so next valid measurement doesn't integrate across gap
    _lastMonotonicMs = monotonicMs;
    _lastCurrent = current;
    return;
  }

  // NaN/Inf rejection
  if (!Core::isValidFloat(voltage) || !Core::isValidFloat(current)) {
    _frozen = true;
    _lastIntegrationQuality = Core::MeasurementQuality::SensorError;
    _lastMonotonicMs = monotonicMs;
    return;
  }

  // Quality is Valid — unfreeze
  _frozen = false;
  _lastIntegrationQuality = Core::MeasurementQuality::Valid;

  // ===========================================================================
  // RC-13: Coulomb counting using MONOTONIC elapsed time
  // (directive §17, §70: monotonic integration)
  // ===========================================================================
  if (_lastMonotonicMs == 0) {
    _lastMonotonicMs = monotonicMs;
    _lastCurrent = current;
    return;  // First sample — no integration
  }

  uint32_t dt_ms = monotonicMs - _lastMonotonicMs;  // unsigned: rollover-safe

  // dt bounds enforcement (directive §18: no fabricated fallback)
  if (dt_ms == 0 || dt_ms > Services::ENERGY_MAX_DT_MS) {
    _lastMonotonicMs = monotonicMs;
    _lastCurrent = current;
    return;
  }

  // Trapezoidal integration: ΔAh = avgI × dt_hours
  float dt_hours = (float)dt_ms / 3600000.0f;
  float avgI = (current + _lastCurrent) * 0.5f;
  float dAh = avgI * dt_hours;
  _coulombBaselineAh += dAh;

  // ===========================================================================
  // INVARIANT: SOC bounded [0, 100] (directive §26, §70)
  // clampSoc handles NaN/Inf → 0, < 0 → 0, > 100 → 100
  // ===========================================================================
  float capAh = Core::cfgBatteryCapacityAh > 0 ? Core::cfgBatteryCapacityAh : Core::BATTERY_CAPACITY_AH;
  // [FW-12] Only meaningful when a valid SOC basis exists. Without a restored
  // or synced basis, coulomb counting accumulates silently from an arbitrary
  // 50% anchor — we keep counting the baseline but SOC stays UNKNOWN until a
  // synchronization event (full charge / manual / OCV-at-rest) sets the basis.
  if (_socValid) {
    float newSoc = 50.0f + (_coulombBaselineAh / capAh) * 100.0f;
    _soc = clampSoc(newSoc);
  }

  _lastMonotonicMs = monotonicMs;
  _lastCurrent = current;

  // [FW-12] Boot-OCV-at-rest resolution: while SOC is UNKNOWN, track how long
  // the pack has been at rest. Open-circuit voltage is only a valid SOC basis
  // at rest — this is evidence, not fabrication.
  //
  // [AUDIT 2026-09 ROUND 5 / p.446] OCV-based resolution now additionally
  // REJECTS the sync when the available evidence says the pack voltage is
  // not a trustworthy SOC proxy:
  //   - BMS cell imbalance  → the pack mean OCV does not represent any cell;
  //   - temperature outside [OCV_TEMP_MIN_C, OCV_TEMP_MAX_C] → LiFePO4 OCV
  //     is too temperature-dependent (rejection, NOT an invented
  //     compensation curve — no characterized dataset exists for that).
  // When the evidence is UNKNOWN (no BMS, no temp sensor) the legacy
  // rest-window-only path applies — the documented fallback.
  if (!_socValid) {
    bool atRest = (std::fabs(current) < Core::cfgIdleCurrentThreshold) &&
                  Core::isValidFloat(voltage) &&
                  voltage >= Core::cfgLowVoltage && voltage <= Core::cfgFullVoltage;
    // Evidence-based rejection (unknown evidence ≠ rejection — fail-open only
    // for genuinely absent instruments, matching the rest of the design).
    bool ocvBlocked = _evidence.cellsBad;
    if (_evidence.tempValid &&
        (_evidence.temperatureC < OCV_TEMP_MIN_C ||
         _evidence.temperatureC > OCV_TEMP_MAX_C)) {
      ocvBlocked = true;
    }
    if (atRest && ocvBlocked) {
      // Restart the rest window — the moment evidence turns sane again the
      // 30-minute window restarts from scratch. One rate-limited log line.
      if (_restStartMonotonicMs != 0) {
        static uint32_t lastOcvRejectLogMs = 0;
        if (monotonicMs - lastOcvRejectLogMs > 300000UL) {   // 5 min
          lastOcvRejectLogMs = monotonicMs;
          Log.append(Core::LogType::SocBaselineCorrected,
                     "OCV-at-rest SOC resolution BLOCKED (cell imbalance / "
                     "temperature out of range) — SOC stays UNKNOWN", 0);
        }
      }
      _restStartMonotonicMs = 0;
      _restWindowSatisfied = false;
    } else if (atRest) {
      if (_restStartMonotonicMs == 0) _restStartMonotonicMs = monotonicMs;
      if ((monotonicMs - _restStartMonotonicMs) >= REST_WINDOW_SEC * 1000u &&
          !_restWindowSatisfied) {
        _restWindowSatisfied = true;
        float ocvSoc = voltageToSocRough(voltage);
        _soc = clampSoc(ocvSoc);
        _coulombBaselineAh = (ocvSoc - 50.0f) * capAh / 100.0f;
        _socValid = true;
        _lastSyncTs = Drivers::rtc.getUnixTime();   // [p.445] epoch domain, 0 = unsynced-honest
        _state = Core::SocState::Synchronized;
        _socFromOcv = true;    // v1.6.0 provenance: OCV is the basis now
        _recordBaselineCorrection(NAN, ocvSoc, voltage, current,
                                  REST_WINDOW_SEC, "BOOT_OCV_AFTER_REST");
      }
    } else {
      _restStartMonotonicMs = 0;      // rest broken — restart the window
    }
  }

  // ===========================================================================
  // Full-charge detection (directive §33, §70)
  // V >= V_FULL AND I <= I_END FOR >= T_FULL
  // ===========================================================================
  bool vFull = (voltage >= Core::cfgFullVoltage);
  bool iTrickle = (current >= 0) && (current < Core::cfgFullChargeCurrentThreshold);
  bool isIdle = (std::fabs(current) < Core::cfgIdleCurrentThreshold);

  switch (_state) {
    case Core::SocState::Normal:
      if (vFull && iTrickle) {
        _state = Core::SocState::FullCandidate;
        _fullCandidateStartMs = monotonicMs;
      } else if (current > Core::cfgIdleCurrentThreshold) {
        _state = Core::SocState::Charging;
      }
      break;
    case Core::SocState::Charging:
      if (vFull && iTrickle) {
        _state = Core::SocState::FullCandidate;
        _fullCandidateStartMs = monotonicMs;
      } else if (isIdle) {
        _state = Core::SocState::Normal;
      }
      break;
    case Core::SocState::FullCandidate: {
      // Directive §38: voltage transient must not reset SOC too easily
      if (!vFull || !iTrickle) {
        _state = Core::SocState::Normal;
        _fullCandidateStartMs = 0;
      } else if ((monotonicMs - _fullCandidateStartMs) >= (Core::cfgFullChargePersistenceSec * 1000)) {
        // ================================================================
        // [AUDIT 2026-09 ROUND 5 / p.447] Full-charge is a SOC BASIS event.
        // Declaring "100%" from pack V + I alone can false-positive when a
        // cell is already overcharged, the pack is imbalanced, the battery
        // is out of temperature range, or a healthy BMS gauge reports a
        // different SOC. Authority policy (explicit, fail-closed):
        //   - BMS healthy (locked + fresh + no faults + no current mismatch)
        //     → the BMS is the SOC authority: confirmation additionally
        //     requires agreement (BMS SOC >= BMS_FULL_AGREE_PCT). A healthy
        //     BMS reporting "not full" DEFERS confirmation — it never forces
        //     100%. Cells reported and bad (overvoltage/imbalance) → defer.
        //   - BMS↔shunt current mismatch ACTIVE → defer: with the two
        //     current instruments in dispute, neither V+I reading chain is
        //     trustworthy enough to mint a new 100% SOC basis.
        //   - Temperature evidence available and outside the window → defer.
        //   - No BMS / no temp sensor (unknown evidence) → legacy V+I path
        //     applies (documented fallback — this is how a BMS-less system
        //     anchors its coulomb engine at all).
        // ================================================================
        bool bmsBlocks = _evidence.bmsHealthy &&
                        (!_evidence.bmsAgreesFull || _evidence.cellsBad);
        bool mismatchBlocks = _evidence.mismatchActive;   // instruments in dispute
        bool tempBlocks = _evidence.tempValid &&
                         (_evidence.temperatureC < OCV_TEMP_MIN_C ||
                          _evidence.temperatureC > OCV_TEMP_MAX_C);
        if (bmsBlocks || mismatchBlocks || tempBlocks) {
          static uint32_t lastDeferLogMs = 0;
          if (monotonicMs - lastDeferLogMs > 300000UL) {   // 5 min rate limit
            lastDeferLogMs = monotonicMs;
            Log.append(Core::LogType::SocBaselineCorrected,
                       "Full-charge confirmation DEFERRED (BMS authority "
                       "disagrees / cells or temperature out of range) — "
                       "V+I candidate stays unconfirmed", 0);
          }
          // Stay FullCandidate — re-evaluated next tick; the candidate window
          // keeps running because the V+I condition itself still holds.
          break;
        }
        // Persistence elapsed + evidence does not contradict → confirm full
        float oldSoc = _soc;
        _soc = 100.0f;
        _coulombBaselineAh = Core::cfgBatteryCapacityAh * 0.5f;
        _socValid = true;                // [FW-12] full charge = valid basis
        _socFromOcv = false;             // v1.6.0: full-charge sync replaces OCV basis
        _state = Core::SocState::FullConfirmed;
        _lastSyncTs = Drivers::rtc.getUnixTime();  // [p.445] UNIX EPOCH — was monotonicMs/1000 (uptime-seconds domain mixed into a field persisted and consumed as Unix time; after reboot it read as a 1970 date). 0 when RTC unsynced = honest "unknown".
        saveToNVS();                       // [FW-12] persist immediately
        _recordBaselineCorrection(oldSoc, 100.0f, voltage, current,
                                    (monotonicMs - _fullCandidateStartMs) / 1000,
                                    "FULL_CHARGE_CONFIRMED");
        if (_estimatedUsableAh < Core::cfgBatteryCapacityAh * 0.95f ||
            _estimatedUsableAh > Core::cfgBatteryCapacityAh * 1.05f) {
          _estimatedUsableAh = Core::cfgBatteryCapacityAh;
        }
      }
      break;
    }
    case Core::SocState::FullConfirmed:
      if (current < -Core::cfgIdleCurrentThreshold) {
        _state = Core::SocState::Normal;
      }
      break;
    case Core::SocState::Synchronized:
      _state = Core::SocState::Normal;
      break;
  }

  // Low SOC alarm (directive §34: 45V = alarm, NOT SOC=0%)
  // [P1-012] UNKNOWN SOC raises no numeric alarm — it is surfaced as
  // SOC_NOT_AVAILABLE, a distinct condition from "low".
  if (!_socValid) {
    alarms.clear(Core::AlarmCode::BATTERY_SOC_LOW);
  } else if (_soc < Core::BATTERY_CRIT_SOC_PCT) {
    alarms.raise(Core::AlarmCode::BATTERY_SOC_LOW, Core::AlarmSeverity::Critical,
                 "Battery SOC critically low");
  } else if (_soc < Core::BATTERY_LOW_SOC_PCT) {
    alarms.raise(Core::AlarmCode::BATTERY_SOC_LOW, Core::AlarmSeverity::Warning,
                 "Battery SOC low");
  } else {
    alarms.clear(Core::AlarmCode::BATTERY_SOC_LOW);
  }
}

void SocStateMachine::_recordBaselineCorrection(float oldSoc, float newSoc,
                                                  float v, float i,
                                                  uint32_t durationSec,
                                                  const char* reason) {
  _lastCorrection.timestamp = Drivers::rtc.getUnixTime();
  _lastCorrection.oldSoc = oldSoc;
  _lastCorrection.newSoc = newSoc;
  _lastCorrection.voltage = v;
  _lastCorrection.current = i;
  _lastCorrection.durationSec = durationSec;
  strncpy(_lastCorrection.reason, reason, sizeof(_lastCorrection.reason)-1);
  _lastCorrection.reason[sizeof(_lastCorrection.reason)-1] = '\0';
  char buf[128];
  snprintf(buf, sizeof(buf), "SOC sync: %.1f→%.1f (V=%.2f I=%.2f dur=%us reason=%s)",
           (double)oldSoc, (double)newSoc, (double)v, (double)i,
           durationSec, reason);
  Log.append(Core::LogType::SocBaselineCorrected, buf, 0);
}

void SocStateMachine::synchronizeFromVoltage(float voltage, const char* reason) {
  float oldSoc = _soc;
  float newSoc = _voltageToSoc(voltage);
  _soc = clampSoc(newSoc);
  _coulombBaselineAh = (newSoc - 50.0f) * Core::cfgBatteryCapacityAh / 100.0f;
  _lastSyncTs = Drivers::rtc.getUnixTime();
  _state = Core::SocState::Synchronized;
  _socValid = true;                    // [FW-12] manual/explicit sync = valid basis
  _socFromOcv = false;                 // v1.6.0: explicit sync replaces OCV basis
  _recordBaselineCorrection(oldSoc, newSoc, voltage, 0.0f, 0, reason);
  saveToNVS();                         // [FW-12] persist immediately on sync
}

void SocStateMachine::setSoc(float soc, const char* reason) {
  soc = clampSoc(soc);  // Phase 13-D: enforce bounds
  float oldSoc = _soc;
  _soc = soc;
  _coulombBaselineAh = (soc - 50.0f) * Core::cfgBatteryCapacityAh / 100.0f;
  _lastSyncTs = Drivers::rtc.getUnixTime();
  _socValid = true;                    // [FW-12] operator-set SOC = valid basis
  _socFromOcv = false;                 // v1.6.0: operator/BMS sync replaces OCV basis
  _recordBaselineCorrection(oldSoc, soc, 0.0f, 0.0f, 0, reason);
  saveToNVS();                         // [FW-12] persist immediately
}

void SocStateMachine::setCoulombBaseline(float baselineAh, const char* reason) {
  float oldSoc = _soc;
  _coulombBaselineAh = baselineAh;
  float soc = 50.0f + (baselineAh / Core::cfgBatteryCapacityAh) * 100.0f;
  _soc = clampSoc(soc);
  _lastSyncTs = Drivers::rtc.getUnixTime();
  _recordBaselineCorrection(oldSoc, _soc, 0.0f, 0.0f, 0, reason);
}

void SocStateMachine::resetToFull() {
  setSoc(100.0f, "MANUAL_RESET_FULL");
}

} // namespace Services
