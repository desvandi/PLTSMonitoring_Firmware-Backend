// =============================================================================
// Services/EnergyCounters.h — Energy Engine with quality-gated integration
// -----------------------------------------------------------------------------
// Phase 13-D changes:
//   - tick() now accepts monotonicMs instead of wall-clock nowSec
//   - Quality gate: ONLY Valid quality is integrated
//   - Stale/Suspect/Calibrating/SensorError/NotAvailable → BLOCK integration
//   - dt computed from monotonic millis() — NOT wall-clock
//   - Invalid dt (0 or > MAX_INTERVAL) → SKIP (no fabricated fallback)
//
// Invariants (directive §69):
//   - monotonic dt
//   - no wall-clock dependency
//   - dt bounds enforced
//   - invalid dt does not fabricate integration
//   - sign convention: I > 0 = charging, I < 0 = discharging
//   - chargeAh >= 0, dischargeAh >= 0 always
//   - NaN/Inf rejected
//   - sensor quality respected
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_ENERGY_COUNTERS_H
#define PLTS_SERVICES_ENERGY_COUNTERS_H

#include <Arduino.h>
#include "../Core/Types.h"
#include "../Core/Config.h"

namespace Services {

// Maximum allowed integration interval (milliseconds).
// If dt exceeds this, measurement gap is too large — SKIP integration.
static constexpr uint32_t ENERGY_MAX_DT_MS = 300000;  // 5 minutes

struct EnergyCounters {
  float chargeAh;
  float dischargeAh;
  float netAh;          // chargeAh - dischargeAh (signed)
  float chargeWh;
  float dischargeWh;
  float netWh;
  float efc;            // dischargeAh / nominal capacity (depth-of-discharge cycles)
  float peakChargeA;    // session peak (positive current)
  float peakDischargeA; // session peak (negative current magnitude)
};

class EnergyCounterService {
public:
  void begin();

  // Phase 13-D: tick() accepts MONOTONIC milliseconds (not wall-clock seconds).
  // RC-13 fix: dt computed from millis() — clock-jump immune.
  // Returns false if measurement was skipped (invalid quality, invalid dt, or NaN/Inf).
  bool tick(float voltage, float current,
            Core::MeasurementQuality vq,
            Core::MeasurementQuality iq,
            uint32_t monotonicMs);

  // Phase 13-D: quality gate — does quality authorize integration?
  static bool qualityAllowsIntegration(Core::MeasurementQuality q) {
    // ONLY Valid quality authorizes energy integration.
    // Stale, Suspect, Calibrating, SensorError, NotAvailable → BLOCK.
    // Derived and Estimated are not applicable for raw measurement quality.
    return q == Core::MeasurementQuality::Valid;
  }

  EnergyCounters get() const { return _c; }
  void reset(const char* reason);
  // [AUDIT 2026-09 ROUND 5 / p.439] saveToNVS now REPORTS failure instead of
  // silently returning: begin() failure, putBytes short-write, or a read-back
  // mismatch all return false (and count in persistFailures()). The caller
  // (persistenceTask) converts a failure into STORAGE_ERROR — an operator
  // seeing "counters saved" that actually hit a failed NVS write would
  // discover the silent energy-history discontinuity only after the next
  // reboot rolled the counters back.
  bool saveToNVS();
  void loadFromNVS();
  uint32_t persistFailures() const { return _persistFailures; }
  // [p.440] True when the loaded state came from the legacy six-key format
  // (migration path) rather than the atomic CRC blob — exposed for fleet
  // visibility via /api/diagnostics.
  bool loadedFromLegacy() const { return _legacyLoadUsed; }

  // Phase 13-D: get integration state for diagnostics
  bool isIntegrating() const { return _integrating; }
  uint32_t getLastDtMs() const { return _lastDtMs; }
  uint32_t getLastIntegrationMs() const { return _lastIntegrationMs; }

private:
  EnergyCounters _c = {};
  uint32_t _lastMonotonicMs = 0;  // RC-13: monotonic timestamp of last tick
  uint32_t _lastDtMs = 0;          // last computed dt
  uint32_t _lastIntegrationMs = 0;  // last successful integration timestamp
  float    _lastVoltage = 0.0f;
  float    _lastCurrent = 0.0f;
  bool     _integrating = false;    // true when last tick successfully integrated
  uint32_t _persistFailures = 0;   // [p.439] failed NVS writes (this boot)
  bool     _legacyLoadUsed = false; // [p.440] state loaded via legacy path this boot
};

extern EnergyCounterService energyCounters;

} // namespace Services

#endif // PLTS_SERVICES_ENERGY_COUNTERS_H
