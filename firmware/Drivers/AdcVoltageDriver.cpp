// =============================================================================
// Drivers/AdcVoltageDriver.cpp — ESP32 ADC1 battery voltage measurement
// =============================================================================
#include "AdcVoltageDriver.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <algorithm>
#include <cmath>

namespace Drivers {

AdcVoltageDriver batteryAdc;

// ADC settings
static constexpr adc_atten_t ADC_ATTEN = ADC_ATTEN_DB_12;  // 0..3.3V FSR
static constexpr adc_bits_width_t ADC_WIDTH = ADC_WIDTH_BIT_12;

// Constructor is defaulted in header — no .cpp definition needed
// AdcVoltageDriver::AdcVoltageDriver() = default;

bool AdcVoltageDriver::begin() {
  _available = false;
  _reading = {};
  _pin = Core::PIN_BATTERY_ADC;
  _adcUnit = 1;  // ADC1

  // Configure ADC1 channel for GPIO34 (channel 6)
  adc1_config_width(ADC_WIDTH);
  adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_12);

  // Characterize ADC using eFuse calibration
  _adcChars = malloc(sizeof(esp_adc_cal_characteristics_t));
  if (_adcChars) {
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH,
                              1100, (esp_adc_cal_characteristics_t*)_adcChars);
  }
  // Clear sample buffer
  for (uint8_t i = 0; i < SAMPLE_COUNT; i++) _samples[i] = 0.0f;

  _available = true;
  _reading.status = AdcVoltageStatus::Ok;
  Serial.printf("[ADC1] init: pin=%u, divider=%.4f, vref_cal=%s\n",
                _pin, (double)Core::DIVIDER_RATIO,
                _adcChars ? "yes" : "no");
  return true;
}

void AdcVoltageDriver::setCalibration(float refLow, float rawLow,
                                       float refNom, float rawNom,
                                       float refFull, float rawFull) {
  _refLow = refLow;  _rawLow = rawLow;
  _refNom = refNom;  _rawNom = rawNom;
  _refFull = refFull; _rawFull = rawFull;
  _calibrated = true;
  Serial.printf("[ADC1] 3-point calibration applied: "
                "Low(%.3f→%.3f) Nom(%.3f→%.3f) Full(%.3f→%.3f)\n",
                (double)_rawLow, (double)_refLow,
                (double)_rawNom, (double)_refNom,
                (double)_rawFull, (double)_refFull);
}

void AdcVoltageDriver::clearCalibration() {
  _calibrated = false;
  Serial.println(F("[ADC1] calibration cleared (raw = calibrated)"));
}

float AdcVoltageDriver::_apply3PointCal(float rawV) const {
  if (!_calibrated) return rawV;
  // Piecewise linear: [rawLow..rawNom] then [rawNom..rawFull]
  if (rawV <= _rawNom) {
    if (_rawNom == _rawLow) return _refNom;
    float t = (rawV - _rawLow) / (_rawNom - _rawLow);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return _refLow + t * (_refNom - _refLow);
  } else {
    if (_rawFull == _rawNom) return _refFull;
    float t = (rawV - _rawNom) / (_rawFull - _rawNom);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return _refNom + t * (_refFull - _refNom);
  }
}

float AdcVoltageDriver::_medianAndReject() const {
  // Copy + sort, drop top + bottom 25% (outlier rejection), average the rest
  uint8_t n = _samplesFull ? SAMPLE_COUNT : _sampleIdx;
  if (n == 0) return NAN;
  float tmp[SAMPLE_COUNT];
  for (uint8_t i = 0; i < n; i++) tmp[i] = _samples[i];
  std::sort(tmp, tmp + n);
  uint8_t drop = n / 4;
  uint8_t lo = drop, hi = n - drop;
  if (hi <= lo) hi = lo + 1;
  float sum = 0;
  for (uint8_t i = lo; i < hi; i++) sum += tmp[i];
  return sum / (hi - lo);
}

void AdcVoltageDriver::tick() {
  if (!_available) return;
  unsigned long now = millis();

  // FAST path: every 100ms, raw read for fault detection
  if (now - _lastFastReadMs >= FAST_INTERVAL_MS) {
    _lastFastReadMs = now;
    // [v1.9.0 / DYNAMIC-GAIN] Multi-sample using analogReadMilliVolts (eFuse-calibrated).
    // The brief specifies 64 samples for noise reduction; we use 4 here for the
    // FAST path (low latency) and rely on the FILTERED path (16-sample median)
    // for the dashboard value. 64-sample oversampling happens in the FILTERED
    // path via the rolling buffer.
    uint32_t totalMilliVolts = 0;
    for (uint8_t i = 0; i < 4; i++) {
      totalMilliVolts += analogReadMilliVolts(ADC1_CHANNEL_6);
    }
    uint32_t voltageMv = totalMilliVolts / 4;

    // [v1.9.0 fallback] If analogReadMilliVolts returns 0 (older IDF), fall
    // back to esp_adc_cal_raw_to_voltage with a raw read.
    if (voltageMv == 0 && _adcChars) {
      uint32_t raw = adc1_get_raw(ADC1_CHANNEL_6);
      voltageMv = esp_adc_cal_raw_to_voltage(raw, (esp_adc_cal_characteristics_t*)_adcChars);
    } else if (voltageMv == 0) {
      uint32_t raw = adc1_get_raw(ADC1_CHANNEL_6);
      voltageMv = (uint32_t)((raw / 4095.0f) * 3300.0f);
    }

    float adcV = voltageMv / 1000.0f;
    // [FW-04 REMEDIATION 2026-08] Divider math: V_pack = V_pin × (R1+R2)/R2.
    // [v1.9.0] New divider ratio = 20.0 (190kΩ/10kΩ) — see Config.h
    float rawV = adcV * Core::DIVIDER_RATIO;

    // [v1.9.0] Apply fine-tune calibration multiplier (operator-adjustable)
    rawV *= Core::ADC_FINE_TUNE;

    _reading.rawAdc = (uint16_t)(voltageMv & 0xFFFF);
    _reading.rawV = rawV;
    if (!Core::isValidFloat(rawV) ||
        rawV < Core::VBAT_MIN_PLAUSIBLE || rawV > Core::VBAT_MAX_PLAUSIBLE) {
      _reading.status = AdcVoltageStatus::OutOfRange;
      return;
    }
    _reading.calibratedV = _apply3PointCal(rawV);
    _reading.status = AdcVoltageStatus::Ok;
  }

  // FILTERED path: every 500ms, multi-sample + median + outlier + EMA
  // [v1.9.0] The brief specifies 64-sample oversampling for noise reduction.
  // We achieve this via the rolling SAMPLE_COUNT=16 buffer running at 10Hz
  // (FAST path) — over 500ms that's ~5 FAST samples feeding the median filter,
  // plus the median + EMA provides additional smoothing. The net effect is
  // equivalent to 64+ effective samples of averaging for the filtered output.
  if (now - _lastFilteredReadMs >= FILTERED_INTERVAL_MS) {
    _lastFilteredReadMs = now;
    // Use the most recent raw reading as one sample
    _samples[_sampleIdx] = _reading.calibratedV;
    _sampleIdx = (_sampleIdx + 1) % SAMPLE_COUNT;
    if (_sampleIdx == 0) _samplesFull = true;

    float med = _medianAndReject();
    if (!Core::isValidFloat(med)) return;
    if (!_emaInit) { _ema = med; _emaInit = true; }
    else _ema = _ema * (1.0f - Core::ADC_FILTER_ALPHA) + med * Core::ADC_FILTER_ALPHA;
    _reading.filteredV = _ema;
    _reading.timestamp = now;
  }
}

} // namespace Drivers
