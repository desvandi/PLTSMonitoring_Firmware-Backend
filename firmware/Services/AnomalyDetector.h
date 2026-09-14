// =============================================================================
// Services/AnomalyDetector.h — Deterministic anomaly detection (brief §59)
// -----------------------------------------------------------------------------
// [PARITY-4 2026-09-06] SINGLE evaluator for the LEVEL-threshold alarm codes
// (BATTERY_VOLTAGE_LOW/HIGH, BATTERY_OVERCURRENT_*, TEMPERATURE_*,
// HUMIDITY_HIGH, BATTERY_SOC_LOW). Thresholds are OPERATOR CONFIG (NVS
// "plts_alarm", REST /api/config + MQTT config.update — see Core/Globals.h
// cfgAlarm*). The duplicate voltage-alarm block that used to live in
// firmware_v1.ino was removed (it fought this evaluator on severity and
// clear semantics).
// Rate-based thresholds below (dV/dt, dI/dt, dT/dt, dSOC/dt) remain
// engineering constants — they characterize sensor health, not operator
// alarm policy.
// Detects:
//   - current spike (|dI/dt| > threshold)
//   - current stuck (no variation over window)
//   - voltage jump (|dV/dt| > threshold)
//   - voltage impossible (out of plausible range)
//   - sensor disagreement (e.g. INA219 sign vs ADC)
//   - temperature rapid rise
//   - humidity abnormality
//   - telemetry gap (sequence discontinuity)
//   - SOC discontinuity (|dSOC/dt| > threshold)
//   - energy counter discontinuity (negative accumulator, impossible flow)
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_ANOMALY_DETECTOR_H
#define PLTS_SERVICES_ANOMALY_DETECTOR_H

#include <Arduino.h>
#include "../Core/Types.h"

namespace Services {

// [AUDIT 2026-09 ROUND 6 / p.458] Every evaluated quantity now carries its
// own QUALITY — the detector gates each evaluation family on the quality
// of the measurement it consumes, instead of trusting isfinite(value).
// A frozen/stale sensor whose producer still publishes a finite last value
// must not be evaluated as a live reading.
struct AnomalyContext {
  float voltage;             // current V reading
  float current;             // current I reading (signed, + = charging)
  float temperatureC;        // ambient T
  float humidityPct;
  float soc;
  uint32_t voltageSeq;
  uint32_t currentSeq;
  uint32_t telemetrySeq;
  Core::MeasurementQuality voltageQ;
  Core::MeasurementQuality currentQ;
  Core::MeasurementQuality temperatureQ;   // [p.458] SHT31 quality gate
  Core::MeasurementQuality humidityQ;      // [p.458]
  Core::MeasurementQuality socQ;           // [p.458] SOC is Estimated by design —
                                            // the gate is "known", not "Valid"
};

class AnomalyDetector {
public:
  void begin();
  void tick(const AnomalyContext& ctx, uint32_t nowSec);

  // [AUDIT 2026-09 ROUND 6 / p.458] Per-family eligibility. Direct physical
  // quantities (V/I/T/H) require a truly VALID measurement — a Derived/
  // Estimated stand-in is not evidence about the physical world for alarm
  // purposes. SOC is different BY DESIGN: the coulomb engine legitimately
  // reports Estimated; "known" (not NotAvailable/SensorError/Invalid) is
  // the honest gate for SOC alarms.
  static bool envEligible(Core::MeasurementQuality q) {
    return q == Core::MeasurementQuality::Valid;
  }
  static bool socEligible(Core::MeasurementQuality q) {
    return q != Core::MeasurementQuality::NotAvailable &&
           q != Core::MeasurementQuality::SensorError &&
           q != Core::MeasurementQuality::Invalid &&
           q != Core::MeasurementQuality::OutOfRange;
  }

private:
  // [p.459] Baselines hold the LAST ELIGIBLE sample of each quantity. They
  // are only written from quality-gated branches — a SensorError sample
  // must never poison the rate/jump baselines (the valid → invalid → valid
  // sequence used to compute |Δ| against the bad sample and raise phantom
  // voltage-jump/current-spike alarms).
  float _lastVoltage = 0.0f;
  float _lastCurrent = 0.0f;
  float _lastTemp = 0.0f;
  float _lastHum = 0.0f;
  float _lastSoc = 0.0f;
  // [p.459] Timestamp of each baseline sample — rate detectors divide by
  // the time between ELIGIBLE samples, never by the last tick interval.
  uint32_t _lastVoltageSec = 0;
  uint32_t _lastCurrentSec = 0;
  uint32_t _lastTempSec = 0;
  uint32_t _lastSocSec = 0;
  bool  _haveVoltage = false;   // [p.459] baseline primed by an eligible sample
  bool  _haveCurrent = false;
  bool  _haveTemp = false;
  bool  _haveHum = false;
  bool  _haveSoc = false;
  uint32_t _lastTickSec = 0;
  uint32_t _lastTelemetrySeq = 0;

  // Current stuck detection: track samples over window. Only eligible
  // (Valid) samples enter the window; a quality break RESETS it so samples
  // from different sensor eras never mix into one "stuck" verdict.
  static constexpr uint8_t STUCK_WINDOW = 8;
  float _currentSamples[STUCK_WINDOW] = {};
  uint8_t _stuckIdx = 0;
  bool _stuckFull = false;

  // Thresholds
  static constexpr float VOLTAGE_JUMP_V_PER_SEC  = 5.0f;
  static constexpr float CURRENT_SPIKE_A_PER_SEC = 50.0f;
  static constexpr float TEMP_RISE_C_PER_MIN     = 5.0f;
  static constexpr float SOC_JUMP_PCT_PER_SEC    = 5.0f;

  // [p.460] Clear a numerical alarm ONLY if it is present and not already
  // cleared — clear() persists on every call, so an unconditional clear on
  // every ineligible tick would burn NVS for nothing.
  static bool _clearIfActive(const char* code);
};

extern AnomalyDetector anomalyDetector;

} // namespace Services

#endif // PLTS_SERVICES_ANOMALY_DETECTOR_H
