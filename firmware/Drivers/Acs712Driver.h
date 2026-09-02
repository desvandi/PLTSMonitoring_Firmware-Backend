// =============================================================================
// Drivers/Acs712Driver.h — ACS712 AC current sensor (high-rate RMS sampling)
// -----------------------------------------------------------------------------
// Brief §2-9:
//   - ACS712 on ADC1 GPIO35 (NOT ADC2 — WiFi conflict)
//   - Sample at 1000 Hz over a 40 ms window = 2 cycles of 50 Hz = 40 samples
//     (covers full mains period, supports peak detection)
//   - Zero-offset removal (configurable, calibrated at factory or field)
//   - RMS calculation: sqrt(sum(s_i^2)/N)  (true RMS, AC-coupled)
//   - Peak detection: max(|s_i|) over window
//   - Signal quality: GOOD if peak > 5% FS, DEGRADED if 1-5%, POOR if <1%
//
// Estimated AC power (labeled ESTIMATED — brief §83):
//   P_est = V_assumed × I_rms × PF_assumed
//   NO direct AC voltage measurement — V_assumed = 220V, PF_assumed = 0.9.
//   Future: PZEM/Modbus AC meter for measured AC V/PF/energy.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_ACS712_H
#define PLTS_DRIVERS_ACS712_H

#include <Arduino.h>
#include <cstdint>
#include "../Core/Config.h"
#include "../Core/Types.h"

namespace Drivers {

enum class Acs712SignalQuality : uint8_t {
  Good     = 0,
  Degraded = 1,
  Poor      = 2,
};
inline const char* acs712SignalQualityStr(Acs712SignalQuality q) {
  switch (q) {
    case Acs712SignalQuality::Good:     return "GOOD";
    case Acs712SignalQuality::Degraded: return "DEGRADED";
    case Acs712SignalQuality::Poor:      return "POOR";
  }
  return "POOR";
}

enum class Acs712Status : uint8_t {
  Ok              = 0,
  NotInitialized  = 1,
  OutOfRange       = 2,
  Invalid          = 3,
};

struct Acs712Reading {
  float    rmsCurrentA;       // true RMS over window (post-offset)
  float    peakCurrentA;     // max(|sample|) over window
  float    averageCurrentA;  // arithmetic mean (should be ~0 for AC)
  uint16_t sampleCount;      // samples in last window
  Acs712SignalQuality signalQuality;
  uint32_t timestamp;
  Acs712Status status;
};

class Acs712Driver {
public:
  Acs712Driver() = default;
  bool begin();
  void tick();
  bool isAvailable() const { return _available; }
  Acs712Reading getReading() const { return _reading; }
  float getRmsCurrent() const { return _reading.rmsCurrentA; }
  float getPeakCurrent() const { return _reading.peakCurrentA; }
  float getAverageCurrent() const { return _reading.averageCurrentA; }
  Acs712SignalQuality getSignalQuality() const { return _reading.signalQuality; }
  uint32_t getLastReadMs() const { return _reading.timestamp; }
  Acs712Status getStatus() const { return _reading.status; }

  // Calibration — called by CalibrationHandlers
  void setZeroOffset(float offset) { _zeroOffset = offset; }
  void setSensitivity(float mvPerA) { _sensitivityMvPerA = mvPerA; }
  float getZeroOffset() const { return _zeroOffset; }
  // Capture zero offset from current reading (must be at zero current)
  float captureZeroOffset();

private:
  bool     _available = false;
  Acs712Reading _reading = {};
  uint8_t  _pin = 0;
  float    _zeroOffset = 1650.0f;       // ADC counts at 0A (mid-supply ~1.65V × 4095/3.3)
  float    _sensitivityMvPerA = 100.0f; // ACS712 20A variant default
  void*    _adcChars = nullptr;
  unsigned long _lastWindowMs = 0;

  // Sample buffer for one window (40 samples @ 1 kHz)
  static constexpr uint16_t SAMPLE_RATE_HZ     = Core::ACS712_SAMPLE_RATE_HZ;
  static constexpr uint16_t WINDOW_MS           = Core::ACS712_WINDOW_MS;
  static constexpr uint16_t SAMPLES_PER_WINDOW  = Core::ACS712_SAMPLES_PER_WINDOW;
  static constexpr uint16_t SAMPLE_INTERVAL_US  = 1000000u / SAMPLE_RATE_HZ;
  static constexpr uint16_t MIN_GOOD_PEAK_MV    = 100;   // 5% FS (~0.5A on 20A range)
  static constexpr uint16_t MIN_DEGRADED_PEAK_MV = 20;   // 1% FS (~0.1A)

  int32_t _samples[SAMPLES_PER_WINDOW];  // ADC counts (offset removed)

  float _adcToCurrent(int32_t raw) const;
};

extern Acs712Driver acs712;

} // namespace Drivers

#endif // PLTS_DRIVERS_ACS712_H
