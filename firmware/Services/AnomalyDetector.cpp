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
  _haveVoltage = false;
  _haveCurrent = false;
  _haveTemp = false;
  _haveHum = false;
  _haveSoc = false;
  _lastVoltageSec = 0;
  _lastCurrentSec = 0;
  _lastTempSec = 0;
  _lastSocSec = 0;
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

// [AUDIT 2026-09 ROUND 6 / p.460] Clear-if-active helper. alarms.clear()
// persists to NVS unconditionally (operator-action contract), so an
// unconditional clear on every ineligible tick would burn flash for a
// no-op; this guard makes the persistence write happen exactly once, on
// the Active → not-assessable transition. Returns true when a live alarm
// was actually cleared (callers aggregate this for a single log line).
bool AnomalyDetector::_clearIfActive(const char* code) {
  const Alarm* a = alarms.find(code);
  if (a && a->lifecycle != Core::AlarmLifecycle::Cleared) {
    alarms.clear(code);
    return true;
  }
  return false;
}

void AnomalyDetector::tick(const AnomalyContext& ctx, uint32_t nowSec) {
  if (_lastTickSec == 0) {
    // [p.459] Prime each baseline ONLY from an eligible sample — an
    // ineligible first sample (SensorError/Stale/Estimated stand-in) must
    // not seed a rate baseline it was never allowed to be compared against.
    if (envEligible(ctx.voltageQ) && std::isfinite(ctx.voltage)) {
      _lastVoltage = ctx.voltage; _lastVoltageSec = nowSec; _haveVoltage = true;
    }
    if (envEligible(ctx.currentQ) && std::isfinite(ctx.current)) {
      _lastCurrent = ctx.current; _lastCurrentSec = nowSec; _haveCurrent = true;
    }
    if (envEligible(ctx.temperatureQ) && std::isfinite(ctx.temperatureC)) {
      _lastTemp = ctx.temperatureC; _haveTemp = true;
    }
    if (envEligible(ctx.humidityQ) && std::isfinite(ctx.humidityPct)) {
      _lastHum = ctx.humidityPct; _haveHum = true;
    }
    if (socEligible(ctx.socQ) && std::isfinite(ctx.soc)) {
      _lastSoc = ctx.soc; _lastSocSec = nowSec; _haveSoc = true;
    }
    _lastTickSec = nowSec;
    _lastTelemetrySeq = ctx.telemetrySeq;
    return;
  }
  uint32_t dt = nowSec - _lastTickSec;
  if (dt == 0 || dt > 600) { _lastTickSec = nowSec; return; }
  (void)dt;   // rate denominators are per-quantity since their last ELIGIBLE sample

  // =========================================================================
  // VOLTAGE family — eligible = Valid quality only
  // =========================================================================
  if (envEligible(ctx.voltageQ)) {
    if (std::isfinite(ctx.voltage)) {
      // Voltage impossible
      if (ctx.voltage < Core::VBAT_MIN_PLAUSIBLE || ctx.voltage > Core::VBAT_MAX_PLAUSIBLE) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Voltage out of plausible: %.2fV", (double)ctx.voltage);
        alarms.raise("BATTERY_VOLTAGE_INVALID", Core::AlarmSeverity::Critical, buf);
      } else {
        alarms.clear("BATTERY_VOLTAGE_INVALID");
      }
      // Voltage jump — [p.459] compared against the LAST ELIGIBLE sample
      // over the time span separating the two eligible samples. The old
      // code compared against ctx.voltage of the PREVIOUS tick regardless
      // of its quality, so a SensorError sample poisoned the baseline and
      // the next valid sample raised a phantom |dV/dt| alarm.
      if (_haveVoltage) {
        uint32_t vElapsed = nowSec - _lastVoltageSec;
        if (vElapsed > 0 && vElapsed <= 600) {
          float dV = std::fabs(ctx.voltage - _lastVoltage) / (float)vElapsed;
          if (dV > VOLTAGE_JUMP_V_PER_SEC) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Voltage jump: %.2f→%.2fV in %us",
                     (double)_lastVoltage, (double)ctx.voltage, vElapsed);
            alarms.raise("BATTERY_VOLTAGE_INVALID", Core::AlarmSeverity::Warning, buf);
          }
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
    // [p.459] baseline: ELIGIBLE samples only
    _lastVoltage = ctx.voltage;
    _lastVoltageSec = nowSec;
    _haveVoltage = true;
  } else {
    // [AUDIT 2026-09 ROUND 6 / p.460] Not assessable: the voltage number
    // is unknown/stale/estimated — a numerical low/high verdict would be a
    // claim about data we do not have. Clear the numerical alarms (their
    // condition is no longer being evaluated) and let the producer's
    // sensor-failure alarm (BATTERY_VOLTAGE_INVALID / sensor health block)
    // carry the truth. The emergency layer independently fail-closes via
    // SENSOR_LOSS, so no protection function is lost — this is display
    // honesty, not a safety regression.
    bool clearedAny = _clearIfActive(Core::AlarmCode::BATTERY_VOLTAGE_LOW);
    clearedAny = _clearIfActive(Core::AlarmCode::BATTERY_VOLTAGE_HIGH) || clearedAny;
    if (clearedAny) {
      Log.append(Core::LogType::Info,
                 String("[ANOMALY] voltage quality not assessable (") +
                 Core::qualityToStr(ctx.voltageQ) +
                 ") — numerical voltage alarms suspended until a valid measurement returns", -1);
    }
  }

  // =========================================================================
  // CURRENT family — eligible = Valid quality only
  // =========================================================================
  if (envEligible(ctx.currentQ)) {
    if (std::isfinite(ctx.current)) {
      if (std::fabs(ctx.current) > Core::CURRENT_SPIKE_REJECT_A) {
        alarms.raise("BATTERY_CURRENT_SENSOR_ERROR", Core::AlarmSeverity::Critical,
                     "Battery current out of plausible range");
      }
      // Current spike (dI/dt) — [p.459] vs LAST ELIGIBLE sample
      if (_haveCurrent) {
        uint32_t iElapsed = nowSec - _lastCurrentSec;
        if (iElapsed > 0 && iElapsed <= 600) {
          float dI = std::fabs(ctx.current - _lastCurrent) / (float)iElapsed;
          if (dI > CURRENT_SPIKE_A_PER_SEC) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Current spike: %.2f→%.2fA in %us",
                     (double)_lastCurrent, (double)ctx.current, iElapsed);
            alarms.raise("BATTERY_CURRENT_SUSPECT", Core::AlarmSeverity::Warning, buf);
          }
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

      // Current stuck detection (over 8-sample window) — [p.459] the window
      // contains ELIGIBLE samples only; a quality break resets it below so
      // samples from different sensor eras never mix into one verdict.
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
    // [p.459] baseline: ELIGIBLE samples only
    _lastCurrent = ctx.current;
    _lastCurrentSec = nowSec;
    _haveCurrent = true;
  } else {
    // [p.460] Not assessable — same honesty contract as the voltage family:
    // OVERCURRENT_xxx no longer claims a verdict it cannot compute, the
    // producer's BATTERY_CURRENT_SENSOR_ERROR stays the live truth, and the
    // emergency layer's I_DC_OVER / SENSOR_LOSS protection never depended
    // on this alarm.
    bool clearedAny = _clearIfActive(Core::AlarmCode::BATTERY_OVERCURRENT_CHARGE);
    clearedAny = _clearIfActive(Core::AlarmCode::BATTERY_OVERCURRENT_DISCHARGE) || clearedAny;
    clearedAny = _clearIfActive("BATTERY_CURRENT_SUSPECT") || clearedAny;
    if (clearedAny) {
      Log.append(Core::LogType::Info,
                 String("[ANOMALY] current quality not assessable (") +
                 Core::qualityToStr(ctx.currentQ) +
                 ") — numerical current alarms suspended until a valid measurement returns", -1);
    }
    // [p.459] window never spans a quality break
    _stuckIdx = 0;
    _stuckFull = false;
  }

  // =========================================================================
  // ENVIRONMENT family — [p.458] quality-gated. The old code only checked
  // isfinite(value): a SHT31 that stopped updating (driver still publishing
  // a finite last value, or a caller passing a raw cached number) was
  // evaluated as a live reading. Ambient T/H alarms now require the
  // measurement's own quality to be Valid.
  // =========================================================================
  if (envEligible(ctx.temperatureQ) && std::isfinite(ctx.temperatureC)) {
    // Temperature rapid rise — [p.459] vs LAST ELIGIBLE sample
    if (_haveTemp && _lastTemp > -40 && ctx.temperatureC > -40) {
      uint32_t tElapsed = nowSec - _lastTempSec;
      if (tElapsed > 0 && tElapsed <= 600) {
        float dT_per_min = (ctx.temperatureC - _lastTemp) / (float)tElapsed * 60.0f;
        if (dT_per_min > TEMP_RISE_C_PER_MIN) {
          char buf[64];
          snprintf(buf, sizeof(buf), "Temp rapid rise: %.1f→%.1fC in %us",
                   (double)_lastTemp, (double)ctx.temperatureC, tElapsed);
          alarms.raise("TEMPERATURE_HIGH", Core::AlarmSeverity::Warning, buf);
        }
      }
    }
    // [PARITY-4] Temperature — two-tier, operator config, with clear-side
    // hysteresis (legacy TEMPERATURE_CRITICAL/HIGH constants retired).
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
    _lastTemp = ctx.temperatureC;   // [p.459] eligible baseline only
    _lastTempSec = nowSec;
    _haveTemp = true;
  } else {
    // [p.460] not assessable
    _clearIfActive(Core::AlarmCode::TEMPERATURE_HIGH);
    _clearIfActive(Core::AlarmCode::TEMPERATURE_CRITICAL);
  }
  if (envEligible(ctx.humidityQ) && std::isfinite(ctx.humidityPct)) {
    if (ctx.humidityPct > Core::cfgAlarmHumidityHighWarnPct) {
      alarms.raise(Core::AlarmCode::HUMIDITY_HIGH, Core::AlarmSeverity::Warning,
                   "Humidity above threshold");
    } else if (ctx.humidityPct < Core::cfgAlarmHumidityHighWarnPct - HUMIDITY_ALARM_HYST_PCT) {
      alarms.clear(Core::AlarmCode::HUMIDITY_HIGH);
    }
    _lastHum = ctx.humidityPct;    // [p.459] eligible baseline only
    _haveHum = true;
  } else {
    _clearIfActive(Core::AlarmCode::HUMIDITY_HIGH);   // [p.460]
  }

  // =========================================================================
  // TELEMETRY sequence — gap + healthy-flow clear
  // [p.461] This detector only ticks when a sample ARRIVES, so a stalled
  // producer (no new samples at all) is detected by the measurement task's
  // queue-timeout branch, not here. What is detectable here: sequence
  // discontinuity (dropped samples) — raised; and consecutive +1 advance
  // (healthy flow resumed) — clears the alarm honestly instead of letting
  // a one-time gap linger as a permanent zombie.
  // =========================================================================
  if (_lastTelemetrySeq > 0) {
    if (ctx.telemetrySeq > _lastTelemetrySeq + 1) {
      alarms.raise("TELEMETRY_STALE", Core::AlarmSeverity::Warning,
                   "Telemetry sequence gap detected");
    } else if (ctx.telemetrySeq == _lastTelemetrySeq + 1) {
      _clearIfActive("TELEMETRY_STALE");   // [p.461]
    }
  }
  _lastTelemetrySeq = ctx.telemetrySeq;

  // =========================================================================
  // SOC family — [p.458] socEligible() (KNOWN, not Valid: the coulomb
  // engine legitimately reports Estimated — requiring Valid here would
  // silence SOC alarms on the normal shunt path).
  // =========================================================================
  if (socEligible(ctx.socQ) && std::isfinite(ctx.soc)) {
    // SOC discontinuity (|dSOC/dt|) — a DATA-QUALITY signal, NOT a low-SOC
    // alarm. [PARITY-4] It previously reused the BATTERY_SOC_LOW code, which
    // collided with the real SOC-low evaluation below (two meanings, one
    // code). [p.459] compared against the LAST ELIGIBLE SOC sample.
    if (_haveSoc && std::fabs(_lastSoc) > 0) {
      uint32_t sElapsed = nowSec - _lastSocSec;
      if (sElapsed > 0 && sElapsed <= 600) {
        if (std::fabs(ctx.soc - _lastSoc) / (float)sElapsed > SOC_JUMP_PCT_PER_SEC) {
          alarms.raise(Core::AlarmCode::BATTERY_SOC_DISCONTINUITY, Core::AlarmSeverity::Warning,
                       "SOC discontinuity detected (rapid jump)");
        }
      }
    }
    // [PARITY-4] REAL SOC-low alarm (brief §24) — two-tier, operator config.
    // Only evaluated when SOC is a KNOWN number (SocStateMachine serves NaN
    // while UNKNOWN — never a fabricated value that could raise a phantom
    // alarm). Severity upgrades on re-raise; clears above warn + hysteresis.
    if (ctx.soc >= 0.0f && ctx.soc <= 100.0f) {
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
    _lastSoc = ctx.soc;           // [p.459] eligible baseline only
    _lastSocSec = nowSec;
    _haveSoc = true;
  } else {
    // [p.460] SOC unknown — numerical SOC alarms are not assessable.
    _clearIfActive(Core::AlarmCode::BATTERY_SOC_LOW);
    _clearIfActive(Core::AlarmCode::BATTERY_SOC_DISCONTINUITY);
  }

  _lastTickSec = nowSec;
}

} // namespace Services
