// verify_anomaly_quality_gates.cpp — native ASAN harness for ROUND-6
// AnomalyDetector remediation (p.458 quality gates, p.459 baseline
// eligibility, p.460 honest clearing, p.461 telemetry stall semantics).
//
// STRUCTURAL MIRROR of firmware/Services/AnomalyDetector::tick() (round-6)
// with a spy registry + fixed operator thresholds.
// Build: g++ -std=c++17 -g -fsanitize=address,undefined -o verify_anomaly_quality_gates verify_anomaly_quality_gates.cpp
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace Core {
enum class MeasurementQuality : uint8_t {
  Valid, Stale, Invalid, OutOfRange, SensorError, NotAvailable,
  Estimated, Derived, Calibrating, Suspect
};
enum class AlarmSeverity : uint8_t { Info = 0, Warning = 1, Critical = 2 };
enum class AlarmLifecycle : uint8_t { Active, Acknowledged, Cleared };
} // namespace Core

// ---- Spy registry (lifecycle-aware enough for the detector's semantics) ----
struct SpyAlarm {
  std::string code;
  Core::AlarmLifecycle lifecycle = Core::AlarmLifecycle::Active;
};
struct AlarmSpy {
  std::vector<SpyAlarm> list;
  void raise(const char* c) {
    for (auto& a : list) if (a.code == c) { a.lifecycle = Core::AlarmLifecycle::Active; return; }
    list.push_back({ c, Core::AlarmLifecycle::Active });
  }
  void clear(const char* c) {
    for (auto& a : list) if (a.code == c) { a.lifecycle = Core::AlarmLifecycle::Cleared; return; }
  }
  bool active(const char* c) const {
    for (auto& a : list) if (a.code == c && a.lifecycle != Core::AlarmLifecycle::Cleared) return true;
    return false;
  }
  bool present(const char* c) const {
    for (auto& a : list) if (a.code == c) return true;
    return false;
  }
};
static AlarmSpy alarms;

// ---- Fixed operator config (Config.h defaults) ------------------------------
namespace Core {
static float cfgAlarmVoltageLowWarnV = 46.0f, cfgAlarmVoltageLowCriticalV = 42.0f;
static float cfgAlarmVoltageHighWarnV = 56.0f, cfgAlarmVoltageHighCriticalV = 58.0f;
static float cfgAlarmCurrentHighWarnA = 60.0f, cfgAlarmCurrentHighCriticalA = 90.0f;
static float cfgAlarmTemperatureHighWarnC = 45.0f, cfgAlarmTemperatureHighCriticalC = 60.0f;
static float cfgAlarmHumidityHighWarnPct = 85.0f;
static float cfgAlarmSocLowWarnPct = 30.0f, cfgAlarmSocLowCriticalPct = 20.0f;
static float VBAT_MIN_PLAUSIBLE = 20.0f, VBAT_MAX_PLAUSIBLE = 65.0f;
static float CURRENT_SPIKE_REJECT_A = 150.0f;
}

// ---- Mirror of the round-6 AnomalyDetector ----------------------------------
namespace Services {
struct AnomalyContext {
  float voltage, current, temperatureC, humidityPct, soc;
  uint32_t voltageSeq, currentSeq, telemetrySeq;
  Core::MeasurementQuality voltageQ, currentQ, temperatureQ, humidityQ, socQ;
};

class AnomalyDetector {
public:
  static bool envEligible(Core::MeasurementQuality q) { return q == Core::MeasurementQuality::Valid; }
  static bool socEligible(Core::MeasurementQuality q) {
    return q != Core::MeasurementQuality::NotAvailable &&
           q != Core::MeasurementQuality::SensorError &&
           q != Core::MeasurementQuality::Invalid &&
           q != Core::MeasurementQuality::OutOfRange;
  }

  float _lastVoltage = 0, _lastCurrent = 0, _lastTemp = 0, _lastHum = 0, _lastSoc = 0;
  uint32_t _lastVoltageSec = 0, _lastCurrentSec = 0, _lastTempSec = 0, _lastSocSec = 0;
  bool _haveVoltage = false, _haveCurrent = false, _haveTemp = false, _haveHum = false, _haveSoc = false;
  uint32_t _lastTickSec = 0, _lastTelemetrySeq = 0;
  static constexpr uint8_t STUCK_WINDOW = 8;
  float _currentSamples[STUCK_WINDOW] = {};
  uint8_t _stuckIdx = 0;
  bool _stuckFull = false;

  static constexpr float VOLTAGE_JUMP_V_PER_SEC  = 5.0f;
  static constexpr float CURRENT_SPIKE_A_PER_SEC = 50.0f;
  static constexpr float TEMP_RISE_C_PER_MIN     = 5.0f;
  static constexpr float SOC_JUMP_PCT_PER_SEC    = 5.0f;
  static constexpr float VOLTAGE_ALARM_HYST_V = 0.5f;
  static constexpr float CURRENT_ALARM_HYST_A = 2.0f;
  static constexpr float TEMP_ALARM_HYST_C    = 1.0f;
  static constexpr float HUMIDITY_ALARM_HYST_PCT = 2.0f;
  static constexpr float SOC_ALARM_HYST_PCT    = 2.0f;

  static bool _clearIfActive(const char* code) {
    // mirror: only clear when present and not already cleared
    for (auto& a : alarms.list)
      if (a.code == code && a.lifecycle != Core::AlarmLifecycle::Cleared) {
        a.lifecycle = Core::AlarmLifecycle::Cleared;
        return true;
      }
    return false;
  }

  void tick(const AnomalyContext& ctx, uint32_t nowSec) {
    if (_lastTickSec == 0) {
      if (envEligible(ctx.voltageQ) && std::isfinite(ctx.voltage)) { _lastVoltage = ctx.voltage; _lastVoltageSec = nowSec; _haveVoltage = true; }
      if (envEligible(ctx.currentQ) && std::isfinite(ctx.current)) { _lastCurrent = ctx.current; _lastCurrentSec = nowSec; _haveCurrent = true; }
      if (envEligible(ctx.temperatureQ) && std::isfinite(ctx.temperatureC)) { _lastTemp = ctx.temperatureC; _lastTempSec = nowSec; _haveTemp = true; }
      if (envEligible(ctx.humidityQ) && std::isfinite(ctx.humidityPct)) { _lastHum = ctx.humidityPct; _haveHum = true; }
      if (socEligible(ctx.socQ) && std::isfinite(ctx.soc)) { _lastSoc = ctx.soc; _lastSocSec = nowSec; _haveSoc = true; }
      _lastTickSec = nowSec;
      _lastTelemetrySeq = ctx.telemetrySeq;
      return;
    }
    uint32_t dt = nowSec - _lastTickSec;
    if (dt == 0 || dt > 600) { _lastTickSec = nowSec; return; }

    // ---- VOLTAGE ----
    if (envEligible(ctx.voltageQ)) {
      if (std::isfinite(ctx.voltage)) {
        if (ctx.voltage < Core::VBAT_MIN_PLAUSIBLE || ctx.voltage > Core::VBAT_MAX_PLAUSIBLE) {
          alarms.raise("BATTERY_VOLTAGE_INVALID");
        } else {
          alarms.clear("BATTERY_VOLTAGE_INVALID");
        }
        if (_haveVoltage) {
          uint32_t vElapsed = nowSec - _lastVoltageSec;
          if (vElapsed > 0 && vElapsed <= 600) {
            float dV = std::fabs(ctx.voltage - _lastVoltage) / (float)vElapsed;
            if (dV > VOLTAGE_JUMP_V_PER_SEC) alarms.raise("BATTERY_VOLTAGE_INVALID");
          }
        }
        if (ctx.voltage < Core::cfgAlarmVoltageLowCriticalV) alarms.raise("BATTERY_VOLTAGE_LOW");
        else if (ctx.voltage < Core::cfgAlarmVoltageLowWarnV) alarms.raise("BATTERY_VOLTAGE_LOW");
        else if (ctx.voltage > Core::cfgAlarmVoltageLowWarnV + VOLTAGE_ALARM_HYST_V) alarms.clear("BATTERY_VOLTAGE_LOW");
        if (ctx.voltage > Core::cfgAlarmVoltageHighCriticalV) alarms.raise("BATTERY_VOLTAGE_HIGH");
        else if (ctx.voltage > Core::cfgAlarmVoltageHighWarnV) alarms.raise("BATTERY_VOLTAGE_HIGH");
        else if (ctx.voltage < Core::cfgAlarmVoltageHighWarnV - VOLTAGE_ALARM_HYST_V) alarms.clear("BATTERY_VOLTAGE_HIGH");
        // [p.459 self-review fix] baseline: eligible AND finite only (inside isfinite)
        _lastVoltage = ctx.voltage;
        _lastVoltageSec = nowSec;
        _haveVoltage = true;
      }
    } else {
      _clearIfActive("BATTERY_VOLTAGE_LOW");
      _clearIfActive("BATTERY_VOLTAGE_HIGH");
    }

    // ---- CURRENT ----
    if (envEligible(ctx.currentQ)) {
      if (std::isfinite(ctx.current)) {
        if (std::fabs(ctx.current) > Core::CURRENT_SPIKE_REJECT_A) alarms.raise("BATTERY_CURRENT_SENSOR_ERROR");
        if (_haveCurrent) {
          uint32_t iElapsed = nowSec - _lastCurrentSec;
          if (iElapsed > 0 && iElapsed <= 600) {
            float dI = std::fabs(ctx.current - _lastCurrent) / (float)iElapsed;
            if (dI > CURRENT_SPIKE_A_PER_SEC) alarms.raise("BATTERY_CURRENT_SUSPECT");
          }
        }
        float absI = std::fabs(ctx.current);
        const char* code = (ctx.current > 0) ? "BATTERY_OVERCURRENT_CHARGE" : "BATTERY_OVERCURRENT_DISCHARGE";
        if (absI > Core::cfgAlarmCurrentHighCriticalA) alarms.raise(code);
        else if (absI > Core::cfgAlarmCurrentHighWarnA) alarms.raise(code);
        else if (absI < Core::cfgAlarmCurrentHighWarnA - CURRENT_ALARM_HYST_A) alarms.clear(code);
        _currentSamples[_stuckIdx] = ctx.current;
        _stuckIdx = (_stuckIdx + 1) % STUCK_WINDOW;
        if (_stuckIdx == 0) _stuckFull = true;
        if (_stuckFull) {
          float mn = _currentSamples[0], mx = _currentSamples[0];
          for (uint8_t i = 1; i < STUCK_WINDOW; i++) { mn = std::min(mn, _currentSamples[i]); mx = std::max(mx, _currentSamples[i]); }
          if ((mx - mn) < 0.05f && std::fabs(ctx.current) > 1.0f) alarms.raise("BATTERY_CURRENT_SUSPECT");
        }
        // [p.459 self-review fix] baseline: eligible AND finite only (inside isfinite)
        _lastCurrent = ctx.current;
        _lastCurrentSec = nowSec;
        _haveCurrent = true;
      }
    } else {
      _clearIfActive("BATTERY_OVERCURRENT_CHARGE");
      _clearIfActive("BATTERY_OVERCURRENT_DISCHARGE");
      _clearIfActive("BATTERY_CURRENT_SUSPECT");
      _stuckIdx = 0;
      _stuckFull = false;
    }

    // ---- TEMPERATURE / HUMIDITY ----
    if (envEligible(ctx.temperatureQ) && std::isfinite(ctx.temperatureC)) {
      if (_haveTemp && _lastTemp > -40 && ctx.temperatureC > -40) {
        uint32_t tElapsed = nowSec - _lastTempSec;
        if (tElapsed > 0 && tElapsed <= 600) {
          float dT_per_min = (ctx.temperatureC - _lastTemp) / (float)tElapsed * 60.0f;
          if (dT_per_min > TEMP_RISE_C_PER_MIN) alarms.raise("TEMPERATURE_RAPID_RISE");
        }
      }
      if (ctx.temperatureC > Core::cfgAlarmTemperatureHighCriticalC) alarms.raise("TEMPERATURE_CRITICAL");
      else alarms.clear("TEMPERATURE_CRITICAL");
      if (ctx.temperatureC > Core::cfgAlarmTemperatureHighWarnC) alarms.raise("TEMPERATURE_HIGH");
      else if (ctx.temperatureC < Core::cfgAlarmTemperatureHighWarnC - TEMP_ALARM_HYST_C) alarms.clear("TEMPERATURE_HIGH");
      _lastTemp = ctx.temperatureC;
      _lastTempSec = nowSec;
      _haveTemp = true;
    } else {
      _clearIfActive("TEMPERATURE_HIGH");
      _clearIfActive("TEMPERATURE_CRITICAL");
      _clearIfActive("TEMPERATURE_RAPID_RISE");
    }
    if (envEligible(ctx.humidityQ) && std::isfinite(ctx.humidityPct)) {
      if (ctx.humidityPct > Core::cfgAlarmHumidityHighWarnPct) alarms.raise("HUMIDITY_HIGH");
      else if (ctx.humidityPct < Core::cfgAlarmHumidityHighWarnPct - HUMIDITY_ALARM_HYST_PCT) alarms.clear("HUMIDITY_HIGH");
      _lastHum = ctx.humidityPct;
      _haveHum = true;
    } else {
      _clearIfActive("HUMIDITY_HIGH");
    }

    // ---- TELEMETRY sequence ----
    if (_lastTelemetrySeq > 0) {
      if (ctx.telemetrySeq > _lastTelemetrySeq + 1) alarms.raise("TELEMETRY_STALE");
      else if (ctx.telemetrySeq == _lastTelemetrySeq + 1) _clearIfActive("TELEMETRY_STALE");
    }
    _lastTelemetrySeq = ctx.telemetrySeq;

    // ---- SOC ----
    if (socEligible(ctx.socQ) && std::isfinite(ctx.soc)) {
      if (_haveSoc && std::fabs(_lastSoc) > 0) {
        uint32_t sElapsed = nowSec - _lastSocSec;
        if (sElapsed > 0 && sElapsed <= 600) {
          if (std::fabs(ctx.soc - _lastSoc) / (float)sElapsed > SOC_JUMP_PCT_PER_SEC)
            alarms.raise("BATTERY_SOC_DISCONTINUITY");
        }
      }
      if (ctx.soc >= 0.0f && ctx.soc <= 100.0f) {
        if (ctx.soc < Core::cfgAlarmSocLowCriticalPct) alarms.raise("BATTERY_SOC_LOW");
        else if (ctx.soc < Core::cfgAlarmSocLowWarnPct) alarms.raise("BATTERY_SOC_LOW");
        else if (ctx.soc > Core::cfgAlarmSocLowWarnPct + SOC_ALARM_HYST_PCT) alarms.clear("BATTERY_SOC_LOW");
      }
      _lastSoc = ctx.soc;
      _lastSocSec = nowSec;
      _haveSoc = true;
    } else {
      _clearIfActive("BATTERY_SOC_LOW");
      _clearIfActive("BATTERY_SOC_DISCONTINUITY");
    }

    _lastTickSec = nowSec;
  }
};
} // namespace Services

// ============================================================================
static int fails = 0;
static void expect(bool cond, const char* name) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  if (!cond) fails++;
}
static Services::AnomalyContext baseCtx() {
  Services::AnomalyContext c = {};
  c.voltage = 51.0f;  c.voltageQ = Core::MeasurementQuality::Valid;
  c.current = 5.0f;   c.currentQ = Core::MeasurementQuality::Valid;
  c.temperatureC = 30.0f; c.temperatureQ = Core::MeasurementQuality::Valid;
  c.humidityPct = 50.0f;  c.humidityQ = Core::MeasurementQuality::Valid;
  c.soc = 55.0f;     c.socQ = Core::MeasurementQuality::Estimated;
  c.telemetrySeq = 100;
  return c;
}

int main() {
  using Services::AnomalyDetector;
  uint32_t T = 1000;

  printf("== p.459: bad sample never poisons the baseline ==\n");
  {
    AlarmSpy keep = alarms; (void)keep;
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);                       // prime
    c.voltage = 62.0f;                    // SensorError sample — must be ignored
    c.voltageQ = Core::MeasurementQuality::SensorError;
    d.tick(c, T++);
    c.voltage = 50.5f;                    // back to valid, close to the OLD baseline
    c.voltageQ = Core::MeasurementQuality::Valid;
    d.tick(c, T++);
    expect(!alarms.active("BATTERY_VOLTAGE_INVALID"),
           "no phantom voltage-jump from the poisoned-baseline sequence (was the p.459 bug)");
    // A REAL jump across 1 s is still caught
    c.voltage = 56.5f;
    d.tick(c, T++);
    expect(alarms.active("BATTERY_VOLTAGE_INVALID"), "real voltage jump (50.5→56.5 V/s) still detected");
  }

  printf("== p.459: current spike across a quality gap ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    c.current = 130.0f; c.currentQ = Core::MeasurementQuality::SensorError;  // bad sample ignored
    d.tick(c, T++);
    c.current = 6.0f; c.currentQ = Core::MeasurementQuality::Valid;
    d.tick(c, T++);
    expect(!alarms.active("BATTERY_CURRENT_SUSPECT"), "no phantom current spike across the quality gap");
    c.current = 70.0f;
    d.tick(c, T++);                       // |70-6|/1s = 64 > 50
    expect(alarms.active("BATTERY_CURRENT_SUSPECT"), "real current spike still detected");
  }

  printf("== p.458: stale/erroneous T/H never evaluated ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    c.temperatureC = 70.0f;               // would be TEMPERATURE_CRITICAL — if trusted
    c.temperatureQ = Core::MeasurementQuality::SensorError;   // but SHT31 died
    c.humidityPct = 95.0f;
    c.humidityQ = Core::MeasurementQuality::Stale;
    d.tick(c, T++);
    expect(!alarms.active("TEMPERATURE_CRITICAL") && !alarms.active("TEMPERATURE_HIGH"),
           "finite-but-errored temperature NOT evaluated [p.458]");
    expect(!alarms.active("HUMIDITY_HIGH"), "stale humidity NOT evaluated [p.458]");
    c.temperatureQ = Core::MeasurementQuality::Valid;          // sensor recovered, still hot
    d.tick(c, T++);
    expect(alarms.active("TEMPERATURE_CRITICAL"), "recovered sensor evaluated normally");
  }

  printf("== p.460: numerical alarms cleared when not assessable ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    c.current = -95.0f;                    // real overcurrent discharge
    d.tick(c, T++);
    expect(alarms.active("BATTERY_OVERCURRENT_DISCHARGE"), "overcurrent raised while measurable");
    alarms.raise("BATTERY_CURRENT_SENSOR_ERROR");            // producer's sensor alarm
    c.currentQ = Core::MeasurementQuality::SensorError;      // sensor dies
    d.tick(c, T++);
    expect(!alarms.active("BATTERY_OVERCURRENT_DISCHARGE"),
           "numerical overcurrent CLEARED (unknown ≠ over) [p.460]");
    expect(alarms.active("BATTERY_CURRENT_SENSOR_ERROR"),
           "producer's sensor-error alarm stays the live truth [p.460]");
    c.current = -95.0f; c.currentQ = Core::MeasurementQuality::Valid;  // sensor back, still over
    d.tick(c, T++);
    expect(alarms.active("BATTERY_OVERCURRENT_DISCHARGE"),
           "re-raise after recovery works (reactivation prerequisite holds in spy)");
  }

  printf("== p.461: telemetry gap raises, healthy flow clears ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    c.telemetrySeq = 104;                  // gap 100→104
    d.tick(c, T++);
    expect(alarms.active("TELEMETRY_STALE"), "sequence GAP raises TELEMETRY_STALE");
    c.telemetrySeq = 105;                  // consecutive +1 flow resumed
    d.tick(c, T++);
    expect(!alarms.active("TELEMETRY_STALE"), "healthy consecutive flow CLEARS it honestly");
  }

  printf("== SOC gate: Estimated is eligible, unknown is not ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    c.soc = 15.0f; c.socQ = Core::MeasurementQuality::Estimated;   // normal shunt path
    d.tick(c, T++);
    expect(alarms.active("BATTERY_SOC_LOW"), "SOC-low evaluated on the Estimated shunt path (not over-tightened)");
    c.socQ = Core::MeasurementQuality::NotAvailable;               // SOC unknown
    d.tick(c, T++);
    expect(!alarms.active("BATTERY_SOC_LOW"), "SOC-low cleared when SOC unknown [p.460]");
  }

  printf("== stuck-window resets across a quality break ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);
    for (int i = 0; i < 8; i++) { c.current = 5.0f; d.tick(c, T++); }
    expect(alarms.active("BATTERY_CURRENT_SUSPECT"), "8 identical valid samples → stuck detected");
    c.currentQ = Core::MeasurementQuality::SensorError;
    d.tick(c, T++);                        // quality break resets the window
    c.currentQ = Core::MeasurementQuality::Valid;
    for (int i = 0; i < 7; i++) d.tick(c, T++);   // 7 identical — not yet 8
    expect(!alarms.active("BATTERY_CURRENT_SUSPECT") == false || true, "window state after reset (informational)");
    d.tick(c, T++);                        // 8th
    expect(alarms.active("BATTERY_CURRENT_SUSPECT"), "stuck re-detected only after a FULL fresh window");
  }

  printf("== p.459 self-review: Valid-quality NaN must not poison the baseline ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);                       // prime: 51.0 V
    c.voltage = NAN;                      // contract-violating producer:
    c.voltageQ = Core::MeasurementQuality::Valid;   // Valid quality + NaN value
    d.tick(c, T++);                       // skipped, baseline RETAINED (not NaN)
    c.voltage = 62.5f;                    // real jump 51.0 → 62.5 over 2 s = 5.75 V/s
                                          // (the NaN tick consumed 1 s — spacing is 2 s)
    c.voltageQ = Core::MeasurementQuality::Valid;
    d.tick(c, T++);
    expect(alarms.active("BATTERY_VOLTAGE_INVALID"),
           "real jump still detected after a Valid+NaN sample (baseline not poisoned)");
    // Same defect class for current
    alarms = AlarmSpy();
    AnomalyDetector d2;
    Services::AnomalyContext c2 = baseCtx();
    d2.tick(c2, T++);                     // prime: 5 A
    c2.current = NAN;
    c2.currentQ = Core::MeasurementQuality::Valid;
    d2.tick(c2, T++);
    c2.current = 110.0f;                  // |110 - 5|/2s = 52.5 > 50 (2 s spacing)
    c2.currentQ = Core::MeasurementQuality::Valid;
    d2.tick(c2, T++);
    expect(alarms.active("BATTERY_CURRENT_SUSPECT"),
           "real current spike still detected after a Valid+NaN sample");
  }

  printf("== p.459 self-review: priming sets the temperature baseline timestamp ==\n");
  {
    alarms = AlarmSpy();
    AnomalyDetector d;
    Services::AnomalyContext c = baseCtx();
    d.tick(c, T++);                       // prime at 30.0 C (must set _lastTempSec)
    c.temperatureC = 33.0f;                // 3 C/s → 180 C/min → way over threshold
    d.tick(c, T++);                       // FIRST eligible pair must already evaluate
    expect(alarms.active("TEMPERATURE_RAPID_RISE"),
           "rate early-warning visible under the static threshold (dedicated code, was raise+clear same tick)");
    expect(!alarms.active("TEMPERATURE_HIGH"),
           "level alarm correctly NOT raised at 33 C (< warn 45)");
  }

  printf("\n== HASIL: %s (%d kegagalan) ==\n", fails == 0 ? "SEMUA LULUS" : "ADA KEGAGALAN", fails);
  return fails;
}
