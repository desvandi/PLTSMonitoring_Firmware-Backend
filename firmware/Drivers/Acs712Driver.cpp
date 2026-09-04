// =============================================================================
// Drivers/Acs712Driver.cpp — AC current RMS sampling
// -----------------------------------------------------------------------------
// Sample rate: 1000 Hz (one sample per 1ms) over a 40 ms window → 40 samples.
// Algorithm per brief §2-9:
//   1. For each window:
//      a. Read SAMPLES_PER_WINDOW raw ADC values at SAMPLE_INTERVAL_US intervals.
//      b. Subtract zero-offset (calibrated).
//      c. Compute RMS = sqrt(sum(s_i^2)/N)  (true RMS)
//      d. Compute peak = max(|s_i|)         (peak detection)
//      e. Compute average = sum(s_i)/N       (should be ~0 for AC)
//      f. Signal quality:
//          peak_v > MIN_GOOD_PEAK_MV       → GOOD
//          peak_v > MIN_DEGRADED_PEAK_MV    → DEGRADED
//          else                            → POOR
//
// IMPORTANT: We use micros() for sample timing — delayMicroseconds would block
// the loop. We sample on each tick() call once SAMPLE_INTERVAL_US has elapsed.
// In practice, tick() is called at >> 1kHz from the SensorTask.
// =============================================================================
#include "Acs712Driver.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <cmath>

namespace Drivers {

Acs712Driver acs712;

// GPIO35 = ADC1_CHANNEL_7
static constexpr adc1_channel_t ACS712_ADC_CHANNEL = ADC1_CHANNEL_7;
static constexpr adc_atten_t ADC_ATTEN = ADC_ATTEN_DB_12;
static constexpr adc_bits_width_t ADC_WIDTH = ADC_WIDTH_BIT_12;

// Constructor is defaulted in header

float Acs712Driver::_adcToCurrent(int32_t raw) const {
  // Convert raw ADC count to current in A.
  // Voltage at ADC pin (post-characterization): raw * 3.3 / 4095 (or use eFuse cal)
  // Then current = (V - V_zero_offset_in_volts) / sensitivity_v_per_a
  // For simplicity (and assuming ACS712 powered at 3.3V → Vref=1.65V at 0A),
  // we work in millivolts:
  //   V_mv = (raw - zeroOffset) * 3300 / 4095
  //   I_A = V_mv / sensitivity_mv_per_a
  float vMv = ((float)(raw - _zeroOffset)) * 3300.0f / 4095.0f;
  return vMv / _sensitivityMvPerA;
}

bool Acs712Driver::begin() {
  _available = false;
  _reading = {};
  _pin = Core::PIN_ACS712_ADC;
  adc1_config_width(ADC_WIDTH);
  adc1_config_channel_atten(ACS712_ADC_CHANNEL, ADC_ATTEN);
  _adcChars = malloc(sizeof(esp_adc_cal_characteristics_t));
  if (_adcChars) {
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH,
                              1100, (esp_adc_cal_characteristics_t*)_adcChars);
  }
  for (uint16_t i = 0; i < SAMPLES_PER_WINDOW; i++) _samples[i] = 0;
  _available = true;
  _reading.status = Acs712Status::Ok;
  Serial.printf("[ACS712] init: pin=%u, sampleRate=%uHz, window=%ums, samples=%u, "
                "sensitivity=%.1fmV/A, zeroOffset=%.1f\n",
                _pin, SAMPLE_RATE_HZ, WINDOW_MS, SAMPLES_PER_WINDOW,
                (double)_sensitivityMvPerA, (double)_zeroOffset);
  return true;
}

float Acs712Driver::captureZeroOffset() {
  // Average 64 raw samples to get zero-offset
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 64; i++) {
    sum += adc1_get_raw(ACS712_ADC_CHANNEL);
    delayMicroseconds(500);
  }
  _zeroOffset = (float)(sum / 64);
  Serial.printf("[ACS712] captured zero offset: %.2f\n", (double)_zeroOffset);
  return _zeroOffset;
}

void Acs712Driver::tick() {
  if (!_available) return;
  unsigned long now = micros();

  // If no window is in progress, check if it's time to start one.
  // We sample one ADC value per tick call if SAMPLE_INTERVAL_US has elapsed.
  // For better timing accuracy, we use a tight loop here (blocking ≤ 40ms).
  // In SensorTask we run this with dedicated priority.

  // Window-based collection: every WINDOW_MS, do a burst of SAMPLES_PER_WINDOW
  // samples at SAMPLE_INTERVAL_US intervals.
  if (now - _lastWindowMs < (unsigned long)(WINDOW_MS * 1000)) return;
  _lastWindowMs = now;

  // Burst-sample
  uint32_t sumSq = 0;
  int32_t sumLin = 0;
  int32_t peakAbs = 0;
  for (uint16_t i = 0; i < SAMPLES_PER_WINDOW; i++) {
    uint32_t t0 = micros();
    int32_t raw = adc1_get_raw(ACS712_ADC_CHANNEL);
    int32_t offsetRemoved = raw - (int32_t)_zeroOffset;
    _samples[i] = offsetRemoved;
    sumSq += (uint32_t)(((int64_t)offsetRemoved * offsetRemoved) >> 12);
    sumLin += offsetRemoved;
    int32_t absV = offsetRemoved < 0 ? -offsetRemoved : offsetRemoved;
    if (absV > peakAbs) peakAbs = absV;
    // Wait for next sample interval
    while (micros() - t0 < SAMPLE_INTERVAL_US) { /* spin */ }
  }

  float rmsA = sqrtf((float)sumSq / SAMPLES_PER_WINDOW) * 3300.0f / 4095.0f / _sensitivityMvPerA;
  // Re-scale sumSq was divided by 4096 (>>12) to fit in uint32 → restore
  rmsA = rmsA * sqrtf(4096.0f);
  float peakA = (float)peakAbs * 3300.0f / 4095.0f / _sensitivityMvPerA;
  float avgA = ((float)sumLin / SAMPLES_PER_WINDOW) * 3300.0f / 4095.0f / _sensitivityMvPerA;

  // Plausibility check
  if (!Core::isValidFloat(rmsA) || rmsA < 0 || rmsA > Core::AC_OVERCURRENT_THRESHOLD * 1.5f) {
    _reading.status = Acs712Status::OutOfRange;
    return;
  }

  // Signal quality based on peak mV
  float peakMv = (float)peakAbs * 3300.0f / 4095.0f;
  if (peakMv > MIN_GOOD_PEAK_MV)      _reading.signalQuality = Acs712SignalQuality::Good;
  else if (peakMv > MIN_DEGRADED_PEAK_MV) _reading.signalQuality = Acs712SignalQuality::Degraded;
  else                                  _reading.signalQuality = Acs712SignalQuality::Poor;

  _reading.rmsCurrentA = rmsA;
  _reading.peakCurrentA = peakA;
  _reading.averageCurrentA = avgA;
  _reading.sampleCount = SAMPLES_PER_WINDOW;
  _reading.timestamp = millis();
  _reading.status = Acs712Status::Ok;
}

} // namespace Drivers
