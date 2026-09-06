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

// [PARITY-4 2026-09-06] Level-threshold hysteresis bands (engineering
// constants — the THRESHOLDS themselves are operator config, these offsets
// only prevent chatter around the clear line).
static constexpr float VOLTAGE_ALARM_HYST_V = 0.5f;
static constexpr float CURRENT_ALARM_HYST_A = 2.0f;
static constexpr float TEMP_ALARM_HYST_C    = 1.0f;
static constexpr float HUMIDITY_ALARM_HYST_PCT = 2.0f;
static constexpr float SOC_ALARM_HYST_PCT    = 2.0f;

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
    // [PARITY-4] Two-tier voltage alarms (operator config, NVS "plts_alarm").
    // AnomalyDetector is the SINGLE evaluator for these codes — the duplicate
    // hysteresis block in firmware_v1.ino was removed (dueling severities +
    // chattering clears). Severity upgrades on re-raise; the alarm clears
    // only beyond warn + hysteresis (brief §24).
    if (ctx.voltage < Core::cfgAlarmVoltageLowCriticalV) {
      alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_LOW, Core::AlarmSeverity::Critical,
                   "Battery voltage below critical threshold");
    } else if (ctx.voltage < Core::cfgAlarmVoltageLowWarnV) {
      alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_LOW, Core::AlarmSeverity::Warning,
                   "Battery voltage below warn threshold");
    } else if (ctx.voltage > Core::cfgAlarmVoltageLowWarnV + VOLTAGE_ALARM_HYST_V) {
      alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_LOW);
    }
    if (ctx.voltage > Core::cfgAlarmVoltageHighCriticalV) {
      alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_HIGH, Core::AlarmSeverity::Critical,
                   "Battery voltage above critical threshold");
    } else if (ctx.voltage > Core::cfgAlarmVoltageHighWarnV) {
      alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_HIGH, Core::AlarmSeverity::Warning,
                   "Battery voltage above warn threshold");
    } else if (ctx.voltage < Core::cfgAlarmVoltageHighWarnV - VOLTAGE_ALARM_HYST_V) {
      alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_HIGH);
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
    // [PARITY-4] Overcurrent — two tiers on |I| (operator config), direction
    // still selects the code (charge vs discharge). Legacy constants
    // OVERCURRENT_CHARGE_A/DISCHARGE_A are retired from evaluation.
    float absI = std::fabs(ctx.current);
    if (ctx.current > 0) {
      if (absI > Core::cfgAlarmCurrentHighCriticalA) {
        alarms.raise(Core::AlarmCode::BATTERY_OVERCURRENT_CHARGE, Core::AlarmSeverity::Critical,
                     "Overcurrent during charge (critical)");
      } else if (absI > Core::cfgAlarmCurrentHighWarnA) {
        alarms.raise(Core::AlarmCode::BATTERY_OVERCURRENT_CHARGE, Core::AlarmSeverity::Warning,
                     "Overcurrent during charge (warn)");
      } else if (absI < Core::cfgAlarmCurrentHighWarnA - CURRENT_ALARM_HYST_A) {
        alarms.clear(Core::AlarmCode::BATTERY_OVERCURRENT_CHARGE);
      }
    } else {
      if (absI > Core::cfgAlarmCurrentHighCriticalA) {
        alarms.raise(Core::AlarmCode::BATTERY_OVERCURRENT_DISCHARGE, Core::AlarmSeverity::Critical,
                     "Overcurrent during discharge (critical)");
      } else if (absI > Core::cfgAlarmCurrentHighWarnA) {
        alarms.raise(Core::AlarmCode::BATTERY_OVERCURRENT_DISCHARGE, Core::AlarmSeverity::Warning,
                     "Overcurrent during discharge (warn)");
      } else if (absI < Core::cfgAlarmCurrentHighWarnA - CURRENT_ALARM_HYST_A) {
        alarms.clear(Core::AlarmCode::BATTERY_OVERCURRENT_DISCHARGE);
      }
    }

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
  // [PARITY-4] Temperature + humidity — two-tier, operator config, with
  // clear-side hysteresis (legacy TEMPERATURE_CRITICAL/HIGH/HUMIDITY_HIGH
  // constants retired from evaluation).
  if (std::isfinite(ctx.temperatureC)) {
    if (ctx.temperatureC > Core::cfgAlarmTemperatureHighCriticalC) {
      alarms.raise(Core::AlarmCode::TEMPERATURE_CRITICAL, Core::AlarmSeverity::Critical,
                   "Ambient temperature critical");
    } else {
      alarms.clear(Core::AlarmCode::TEMPERATURE_CRITICAL);
    }
    if (ctx.temperatureC > Core::cfgAlarmTemperatureHighWarnC) {
      alarms.raise(Core::AlarmCode::TEMPERATURE_HIGH, Core::AlarmSeverity::Warning,
                   "Ambient temperature high");
    } else if (ctx.temperatureC < Core::cfgAlarmTemperatureHighWarnC - TEMP_ALARM_HYST_C) {
      alarms.clear(Core::AlarmCode::TEMPERATURE_HIGH);
    }
  }
  if (std::isfinite(ctx.humidityPct)) {
    if (ctx.humidityPct > Core::cfgAlarmHumidityHighWarnPct) {
      alarms.raise(Core::AlarmCode::HUMIDITY_HIGH, Core::AlarmSeverity::Warning,
                   "Humidity above threshold");
    } else if (ctx.humidityPct < Core::cfgAlarmHumidityHighWarnPct - HUMIDITY_ALARM_HYST_PCT) {
      alarms.clear(Core::AlarmCode::HUMIDITY_HIGH);
    }
  }

  // Telemetry sequence discontinuity
  if (_lastTelemetrySeq > 0 && ctx.telemetrySeq > _lastTelemetrySeq + 1) {
    alarms.raise("TELEMETRY_STALE", Core::AlarmSeverity::Warning,
                 "Telemetry sequence gap detected");
  }
  _lastTelemetrySeq = ctx.telemetrySeq;

  // SOC discontinuity (|dSOC/dt|) — a DATA-QUALITY signal, NOT a low-SOC
  // alarm. [PARITY-4] It previously reused the BATTERY_SOC_LOW code, which
  // collided with the real SOC-low evaluation below (two meanings, one code).
  if (std::fabs(_lastSoc) > 0 && std::fabs(ctx.soc - _lastSoc) / dts > SOC_JUMP_PCT_PER_SEC) {
    alarms.raise(Core::AlarmCode::BATTERY_SOC_DISCONTINUITY, Core::AlarmSeverity::Warning,
                 "SOC discontinuity detected (rapid jump)");
  }

  // [PARITY-4] REAL SOC-low alarm (brief §24) — two-tier, operator config.
  // Only evaluated when SOC is a KNOWN number (SocStateMachine serves NaN
  // while UNKNOWN — never a fabricated value that could raise a phantom
  // alarm). Severity upgrades on re-raise; clears above warn + hysteresis.
  if (std::isfinite(ctx.soc) && ctx.soc >= 0.0f && ctx.soc <= 100.0f) {
    if (ctx.soc < Core::cfgAlarmSocLowCriticalPct) {
      alarms.raise(Core::AlarmCode::BATTERY_SOC_LOW, Core::AlarmSeverity::Critical,
                   "Battery SOC below critical threshold");
    } else if (ctx.soc < Core::cfgAlarmSocLowWarnPct) {
      alarms.raise(Core::AlarmCode::BATTERY_SOC_LOW, Core::AlarmSeverity::Warning,
                   "Battery SOC below warn threshold");
    } else if (ctx.soc > Core::cfgAlarmSocLowWarnPct + SOC_ALARM_HYST_PCT) {
      alarms.clear(Core::AlarmCode::BATTERY_SOC_LOW);
    }
  }

  _lastVoltage = ctx.voltage;
  _lastCurrent = ctx.current;
  _lastTemp = ctx.temperatureC;
  _lastHum = ctx.humidityPct;
  _lastSoc = ctx.soc;
  _lastTickSec = nowSec;
}

} // namespace Services
