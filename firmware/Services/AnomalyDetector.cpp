// =============================================================================
// Services/AnomalyDetector.cpp
// =============================================================================
#include "AnomalyDetector.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Core/Globals.h"
#include "AlarmRegistry.h"
#include "LogService.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace Services {

AnomalyDetector anomalyDetector;

void AnomalyDetector::begin() {
  _lastVoltage = 0.0f;
  _lastCurrent = 0.0f;
  _lastTemp = 0.0f;
  _lastHum = 0.0f;
  _lastSoc = 0.0f;
  _lastTickSec = 0;
  _lastTelemetrySeq = 0;
  for (uint8_t i = 0; i < STUCK_WINDOW; i++) _currentSamples[i] = 0;
  _stuckIdx = 0; _stuckFull = false;
}

void AnomalyDetector::tick(const AnomalyContext& ctx, uint32_t nowSec) {
  if (_lastTickSec == 0) {
    _lastVoltage = ctx.voltage;
    _lastCurrent = ctx.current;
    _lastTemp = ctx.temperatureC;
    _lastHum = ctx.humidityPct;
    _lastSoc = ctx.soc;
    _lastTickSec = nowSec;
    _lastTelemetrySeq = ctx.telemetrySeq;
    return;
  }
  uint32_t dt = nowSec - _lastTickSec;
  if (dt == 0 || dt > 600) { _lastTickSec = nowSec; return; }
  float dts = (float)dt;

  // Voltage impossible
  if (ctx.voltageQ == Core::MeasurementQuality::Valid) {
    if (ctx.voltage < Core::VBAT_MIN_PLAUSIBLE || ctx.voltage > Core::VBAT_MAX_PLAUSIBLE) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Voltage out of plausible: %.2fV", (double)ctx.voltage);
      alarms.raise("BATTERY_VOLTAGE_INVALID", Core::AlarmSeverity::Critical, buf);
    } else {
      alarms.clear("BATTERY_VOLTAGE_INVALID");
    }
    // Voltage jump
    if (_lastVoltage > 0) {
      float dV = std::fabs(ctx.voltage - _lastVoltage) / dts;
      if (dV > VOLTAGE_JUMP_V_PER_SEC) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Voltage jump: %.2f→%.2fV in %us",
                 (double)_lastVoltage, (double)ctx.voltage, dt);
        alarms.raise("BATTERY_VOLTAGE_INVALID", Core::AlarmSeverity::Warning, buf);
      }
    }
    if (ctx.voltage < Core::cfgLowVoltage) {
      alarms.raise("BATTERY_VOLTAGE_LOW", Core::AlarmSeverity::Warning,
                   "Battery voltage below low threshold");
    } else {
      alarms.clear("BATTERY_VOLTAGE_LOW");
    }
    if (ctx.voltage > Core::cfgFullVoltage * 1.02f) {  // 2% above full
      alarms.raise("BATTERY_VOLTAGE_HIGH", Core::AlarmSeverity::Warning,
                   "Battery voltage above full threshold");
    } else {
      alarms.clear("BATTERY_VOLTAGE_HIGH");
    }
  }

  // Current anomalies
  if (ctx.currentQ == Core::MeasurementQuality::Valid) {
    if (std::fabs(ctx.current) > Core::CURRENT_SPIKE_REJECT_A) {
      alarms.raise("BATTERY_CURRENT_SENSOR_ERROR", Core::AlarmSeverity::Critical,
                   "Battery current out of plausible range");
    }
    // Current spike (dI/dt)
    if (std::fabs(_lastCurrent) > 0 || std::fabs(ctx.current) > 0) {
      float dI = std::fabs(ctx.current - _lastCurrent) / dts;
      if (dI > CURRENT_SPIKE_A_PER_SEC) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Current spike: %.2f→%.2fA in %us",
                 (double)_lastCurrent, (double)ctx.current, dt);
        alarms.raise("BATTERY_CURRENT_SUSPECT", Core::AlarmSeverity::Warning, buf);
      }
    }
    // Overcurrent
    if (ctx.current > Core::OVERCURRENT_CHARGE_A) {
      alarms.raise("BATTERY_OVERCURRENT_CHARGE", Core::AlarmSeverity::Critical,
                   "Overcurrent during charge");
    } else { alarms.clear("BATTERY_OVERCURRENT_CHARGE"); }
    if (-ctx.current > Core::OVERCURRENT_DISCHARGE_A) {
      alarms.raise("BATTERY_OVERCURRENT_DISCHARGE", Core::AlarmSeverity::Critical,
                   "Overcurrent during discharge");
    } else { alarms.clear("BATTERY_OVERCURRENT_DISCHARGE"); }

    // Current stuck detection (over 8-sample window)
    _currentSamples[_stuckIdx] = ctx.current;
    _stuckIdx = (_stuckIdx + 1) % STUCK_WINDOW;
    if (_stuckIdx == 0) _stuckFull = true;
    if (_stuckFull) {
      float mn = _currentSamples[0], mx = _currentSamples[0];
      for (uint8_t i = 1; i < STUCK_WINDOW; i++) {
        mn = std::min(mn, _currentSamples[i]);
        mx = std::max(mx, _currentSamples[i]);
      }
      // If max-min is suspiciously small but current is large → stuck sensor
      if ((mx - mn) < 0.05f && std::fabs(ctx.current) > 1.0f) {
        alarms.raise("BATTERY_CURRENT_SUSPECT", Core::AlarmSeverity::Warning,
                     "Battery current reading stuck (no variation)");
      }
    }
  }

  // Temperature rapid rise
  if (_lastTemp > -40 && ctx.temperatureC > -40) {
    float dT_per_min = (ctx.temperatureC - _lastTemp) / dts * 60.0f;
    if (dT_per_min > TEMP_RISE_C_PER_MIN) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Temp rapid rise: %.1f→%.1fC in %us",
               (double)_lastTemp, (double)ctx.temperatureC, dt);
      alarms.raise("TEMPERATURE_HIGH", Core::AlarmSeverity::Warning, buf);
    }
  }
  if (ctx.temperatureC > Core::TEMP_CRIT_THRESHOLD_C) {
    alarms.raise("TEMPERATURE_CRITICAL", Core::AlarmSeverity::Critical,
                 "Ambient temperature critical");
  } else { alarms.clear("TEMPERATURE_CRITICAL"); }
  if (ctx.temperatureC > Core::TEMP_HIGH_THRESHOLD_C) {
    alarms.raise("TEMPERATURE_HIGH", Core::AlarmSeverity::Warning,
                 "Ambient temperature high");
  } else { alarms.clear("TEMPERATURE_HIGH"); }

  if (ctx.humidityPct > Core::HUMIDITY_HIGH_PCT) {
    alarms.raise("HUMIDITY_HIGH", Core::AlarmSeverity::Warning,
                 "Humidity above threshold");
  } else { alarms.clear("HUMIDITY_HIGH"); }

  // Telemetry sequence discontinuity
  if (_lastTelemetrySeq > 0 && ctx.telemetrySeq > _lastTelemetrySeq + 1) {
    alarms.raise("TELEMETRY_STALE", Core::AlarmSeverity::Warning,
                 "Telemetry sequence gap detected");
  }
  _lastTelemetrySeq = ctx.telemetrySeq;

  // SOC discontinuity (|dSOC/dt|)
  if (std::fabs(_lastSoc) > 0 && std::fabs(ctx.soc - _lastSoc) / dts > SOC_JUMP_PCT_PER_SEC) {
    alarms.raise("BATTERY_SOC_LOW", Core::AlarmSeverity::Warning,
                 "SOC discontinuity detected (rapid jump)");
  }

  _lastVoltage = ctx.voltage;
  _lastCurrent = ctx.current;
  _lastTemp = ctx.temperatureC;
  _lastHum = ctx.humidityPct;
  _lastSoc = ctx.soc;
  _lastTickSec = nowSec;
}

} // namespace Services
