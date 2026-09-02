// =============================================================================
// Drivers/AdcVoltageDriver.h — ESP32 ADC1 GPIO34 battery voltage measurement
// -----------------------------------------------------------------------------
// Pipeline (brief §4):
//   raw → hw calibration (eFuse ADC calibration) → divider ratio conversion
//   → 3-point voltage calibration → plausibility check → EMA filter
//
// Two output paths:
//   - RAW / FAST    — for fault detection (every 100ms, low latency)
//   - FILTERED      — for dashboard / SOC / energy (median + outlier rejection
//                     + EMA, every 500ms)
//
// IMPORTANT: ADC1 only — never ADC2 (WiFi conflict). GPIO34 is input-only.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_ADC_VOLTAGE_H
#define PLTS_DRIVERS_ADC_VOLTAGE_H

#include <Arduino.h>
#include <cstdint>
#include "../Core/Types.h"

namespace Drivers {

enum class AdcVoltageStatus : uint8_t {
  Ok             = 0,
  NotInitialized = 1,
  OutOfRange      = 2,
  Invalid         = 3,
};

struct AdcVoltageReading {
  float    rawV;          // post-divider-ratio, pre-3-point-cal
  float    calibratedV;   // post-3-point-cal
  float    filteredV;     // post-EMA
  uint16_t rawAdc;
  uint32_t timestamp;
  AdcVoltageStatus status;
};

class AdcVoltageDriver {
public:
  AdcVoltageDriver() = default;
  bool begin();
  void tick();
  bool isAvailable() const { return _available; }
  AdcVoltageReading getReading() const { return _reading; }
  float getFilteredV() const { return _reading.filteredV; }
  float getRawV()      const { return _reading.rawV; }
  uint32_t getLastReadMs() const { return _reading.timestamp; }
  AdcVoltageStatus getStatus() const { return _reading.status; }

  // Inject 3-point calibration points (called by VoltageCalibration service).
  // The calibration converts rawV → calibratedV via piecewise-linear interpolation.
  void setCalibration(float refLow, float rawLow,
                      float refNom, float rawNom,
                      float refFull, float rawFull);
  void clearCalibration();  // revert to identity (raw = calibrated)

private:
  bool     _available = false;
  uint8_t  _adcUnit = 1;
  uint8_t  _pin = 0;
  AdcVoltageReading _reading = {};
  unsigned long _lastFastReadMs = 0;
  unsigned long _lastFilteredReadMs = 0;

  // 3-point calibration state (set externally)
  bool   _calibrated = false;
  float  _refLow, _rawLow, _refNom, _rawNom, _refFull, _rawFull;

  // Multi-sample buffer + median + outlier rejection
  static constexpr uint8_t  SAMPLE_COUNT         = 16;
  static constexpr uint16_t FAST_INTERVAL_MS     = 100;
  static constexpr uint16_t FILTERED_INTERVAL_MS = 500;
  float _samples[SAMPLE_COUNT];
  uint8_t _sampleIdx = 0;
  bool    _samplesFull = false;
  float   _ema = 0.0f;
  bool    _emaInit = false;

  // ESP32 ADC calibration (eFuse-based)
  void*   _adcChars = nullptr;  // esp_adc_cal_characteristics_t* (opaque)

  float  _apply3PointCal(float rawV) const;
  float  _medianAndReject() const;
};

extern AdcVoltageDriver batteryAdc;

} // namespace Drivers

#endif // PLTS_DRIVERS_ADC_VOLTAGE_H
