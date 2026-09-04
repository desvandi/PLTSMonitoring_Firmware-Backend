// =============================================================================
// Services/AnomalyDetector.h — Deterministic anomaly detection (brief §59)
// -----------------------------------------------------------------------------
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
};

class AnomalyDetector {
public:
  void begin();
  void tick(const AnomalyContext& ctx, uint32_t nowSec);

private:
  float _lastVoltage = 0.0f;
  float _lastCurrent = 0.0f;
  float _lastTemp = 0.0f;
  float _lastHum = 0.0f;
  float _lastSoc = 0.0f;
  uint32_t _lastTickSec = 0;
  uint32_t _lastTelemetrySeq = 0;

  // Current stuck detection: track samples over window
  static constexpr uint8_t STUCK_WINDOW = 8;
  float _currentSamples[STUCK_WINDOW] = {};
  uint8_t _stuckIdx = 0;
  bool _stuckFull = false;

  // Thresholds
  static constexpr float VOLTAGE_JUMP_V_PER_SEC  = 5.0f;
  static constexpr float CURRENT_SPIKE_A_PER_SEC = 50.0f;
  static constexpr float TEMP_RISE_C_PER_MIN     = 5.0f;
  static constexpr float SOC_JUMP_PCT_PER_SEC    = 5.0f;
};

extern AnomalyDetector anomalyDetector;

} // namespace Services

#endif // PLTS_SERVICES_ANOMALY_DETECTOR_H
