// =============================================================================
// Services/AcMeasurement.h — AC RMS/peak/average current + ESTIMATED power
// -----------------------------------------------------------------------------
// P_est = V_assumed × I_rms × PF_assumed  (always ESTIMATED, brief §83)
// No direct AC voltage measurement — V_assumed = 220V, PF_assumed = 0.9.
// Reserve protocol for future PZEM/Modbus AC meter (extended schema).
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_AC_MEASUREMENT_H
#define PLTS_SERVICES_AC_MEASUREMENT_H

#include <Arduino.h>
#include "../Core/Types.h"
#include "../Drivers/Acs712Driver.h"

namespace Services {

struct AcMeasurementResult {
  Core::Measurement rmsCurrent;     // A, MEASURED
  Core::Measurement peakCurrent;    // A, MEASURED
  Core::Measurement averageCurrent;  // A, MEASURED
  Core::Measurement estimatedPower; // W, ESTIMATED
  const char* signalQuality;
};

class AcMeasurement {
public:
  void begin();
  // Called by MeasurementTask each cycle — reads from Acs712Driver + computes
  AcMeasurementResult compute(uint32_t sequence, uint32_t timestamp);
  // Allow override of assumptions (e.g. known inverter PF)
  void setAssumedVoltage(float v) { _assumedV = v; }
  void setAssumedPowerFactor(float pf) { _assumedPF = pf; }

private:
  float _assumedV = Core::ASSUMED_AC_VOLTAGE;
  float _assumedPF = Core::ASSUMED_POWER_FACTOR;
};

extern AcMeasurement acMeasurement;

} // namespace Services

#endif // PLTS_SERVICES_AC_MEASUREMENT_H
