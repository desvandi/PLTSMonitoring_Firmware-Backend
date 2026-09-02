// =============================================================================
// Services/EnergyCounters.cpp — Energy Engine with quality-gated integration
// =============================================================================
#include "EnergyCounters.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "LogService.h"
#include <Preferences.h>
#include <cmath>
#include <cstdio>

namespace Services {

EnergyCounterService energyCounters;

void EnergyCounterService::begin() {
  _c = {};
  _lastMonotonicMs = 0;
  _lastDtMs = 0;
  _lastIntegrationMs = 0;
  _lastVoltage = 0.0f;
  _lastCurrent = 0.0f;
  _integrating = false;
  loadFromNVS();
  Serial.printf("[ENERGY] init: chargeAh=%.3f dischargeAh=%.3f chargeWh=%.1f dischargeWh=%.1f efc=%.3f\n",
                (double)_c.chargeAh, (double)_c.dischargeAh,
                (double)_c.chargeWh, (double)_c.dischargeWh, (double)_c.efc);
}

bool EnergyCounterService::tick(float voltage, float current,
                                 Core::MeasurementQuality vq,
                                 Core::MeasurementQuality iq,
                                 uint32_t monotonicMs) {
  _integrating = false;

  // ===========================================================================
  // INVARIANT 1: Quality gate — ONLY Valid quality is integrated
  // (directive §24, §69: sensor quality respected)
  // Stale, Suspect, Calibrating, SensorError, NotAvailable → BLOCK
  // ===========================================================================
  if (!qualityAllowsIntegration(vq) || !qualityAllowsIntegration(iq)) {
    // Phase 13-D: on quality failure, RESET lastMonotonicMs so next valid
    // measurement doesn't integrate across the gap (directive §19: gap policy).
    _lastMonotonicMs = monotonicMs;
    _lastVoltage = voltage;
    _lastCurrent = current;
    return false;
  }

  // ===========================================================================
  // INVARIANT 2: NaN/Inf rejection (directive §21, §69)
  // ===========================================================================
  if (!Core::isValidFloat(voltage) || !Core::isValidFloat(current)) {
    _lastMonotonicMs = monotonicMs;
    _lastVoltage = 0.0f;
    _lastCurrent = 0.0f;
    return false;
  }

  // ===========================================================================
  // RC-13: First sample — initialize, no integration
  // ===========================================================================
  if (_lastMonotonicMs == 0) {
    _lastMonotonicMs = monotonicMs;
    _lastVoltage = voltage;
    _lastCurrent = current;
    return false;
  }

  // ===========================================================================
  // RC-13: Compute dt from MONOTONIC millis (NOT wall-clock)
  // (directive §17: monotonic elapsed time mandatory)
  // ===========================================================================
  uint32_t dt_ms = monotonicMs - _lastMonotonicMs;  // unsigned: wraps correctly on rollover

  // ===========================================================================
  // INVARIANT 3: dt bounds enforcement (directive §18, §69)
  // dt == 0 → skip (same timestamp, no time elapsed)
  // dt > MAX → gap too large, skip (directive §19: gap policy)
  // NO fabricated dt fallback (directive: "dt = 1 second" FORBIDDEN)
  // ===========================================================================
  if (dt_ms == 0 || dt_ms > ENERGY_MAX_DT_MS) {
    _lastMonotonicMs = monotonicMs;
    _lastVoltage = voltage;
    _lastCurrent = current;
    _lastDtMs = dt_ms;
    return false;
  }

  // ===========================================================================
  // Energy integration: trapezoidal method
  // (directive §16-17, §69: sign convention, unit consistency)
  //
  // ΔAh = avgI × dt_hours = avgI × (dt_ms / 3600000.0)
  // ΔWh = avgV × avgI × dt_hours
  //
  // Sign convention (directive §16, D-9):
  //   I > 0 → charging → chargeAh += dAh, chargeWh += dWh
  //   I < 0 → discharging → dischargeAh += -dAh, dischargeWh += -dWh
  // ===========================================================================
  float dt_hours = (float)dt_ms / 3600000.0f;
  float avgI = (current + _lastCurrent) * 0.5f;
  float avgV = (voltage + _lastVoltage) * 0.5f;
  float dAh = avgI * dt_hours;
  float dWh = avgV * avgI * dt_hours;

  // ===========================================================================
  // INVARIANT 4: Separate charge/discharge accumulators (directive §20, §69)
  // chargeAh >= 0 ALWAYS, dischargeAh >= 0 ALWAYS
  // ===========================================================================
  if (dAh >= 0.0f) {
    _c.chargeAh    += dAh;
    _c.chargeWh    += dWh;
  } else {
    _c.dischargeAh += -dAh;   // -dAh is positive when dAh is negative
    _c.dischargeWh += -dWh;
  }

  _c.netAh = _c.chargeAh - _c.dischargeAh;
  _c.netWh = _c.chargeWh - _c.dischargeWh;

  float cap = Core::cfgBatteryCapacityAh > 0 ? Core::cfgBatteryCapacityAh : Core::BATTERY_CAPACITY_AH;
  _c.efc = _c.dischargeAh / cap;

  if (current > _c.peakChargeA)     _c.peakChargeA = current;
  if (-current > _c.peakDischargeA) _c.peakDischargeA = -current;

  _lastMonotonicMs = monotonicMs;
  _lastVoltage = voltage;
  _lastCurrent = current;
  _lastDtMs = dt_ms;
  _lastIntegrationMs = monotonicMs;
  _integrating = true;
  return true;
}

void EnergyCounterService::reset(const char* reason) {
  _c = {};
  char buf[80];
  snprintf(buf, sizeof(buf), "Energy counters reset: %s", reason ? reason : "unspecified");
  Log.append(Core::LogType::ConfigurationChanged, buf, 0);
  saveToNVS();
}

void EnergyCounterService::saveToNVS() {
  Preferences p;
  if (!p.begin("plts_energy", false)) return;
  p.putFloat("chAh", _c.chargeAh);
  p.putFloat("dchAh", _c.dischargeAh);
  p.putFloat("chWh", _c.chargeWh);
  p.putFloat("dchWh", _c.dischargeWh);
  p.putFloat("pkChA", _c.peakChargeA);
  p.putFloat("pkDchA", _c.peakDischargeA);
  p.end();
}

void EnergyCounterService::loadFromNVS() {
  Preferences p;
  if (!p.begin("plts_energy", true)) return;
  _c.chargeAh        = p.getFloat("chAh", 0.0f);
  _c.dischargeAh     = p.getFloat("dchAh", 0.0f);
  _c.chargeWh        = p.getFloat("chWh", 0.0f);
  _c.dischargeWh     = p.getFloat("dchWh", 0.0f);
  _c.peakChargeA     = p.getFloat("pkChA", 0.0f);
  _c.peakDischargeA  = p.getFloat("pkDchA", 0.0f);
  _c.netAh = _c.chargeAh - _c.dischargeAh;
  _c.netWh = _c.chargeWh - _c.dischargeWh;
  float cap = Core::cfgBatteryCapacityAh > 0 ? Core::cfgBatteryCapacityAh : Core::BATTERY_CAPACITY_AH;
  _c.efc = (cap > 0) ? _c.dischargeAh / cap : 0.0f;
  p.end();
}

} // namespace Services
