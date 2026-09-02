// =============================================================================
// Services/VoltageCalibration.h — 3-point piecewise-linear voltage calibration
// -----------------------------------------------------------------------------
// Per brief §3, §35 (CALIBRATION_ERROR alarm):
//   - LOW (45V) / NOMINAL (~50V) / FULL (54V) reference points
//   - Each point: { reference (multimeter), raw (sensor reading), timestamp }
//   - Validation: reject if LOW>=NOMINAL or NOMINAL>=FULL or excessive deviation
//   - Source: FACTORY | FIELD | DEFAULT
//   - Restore previous + factory default supported
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_VOLTAGE_CALIBRATION_H
#define PLTS_SERVICES_VOLTAGE_CALIBRATION_H

#include <Arduino.h>
#include "../Core/Types.h"

namespace Services {

struct VoltageCalibrationState {
  Core::CalibrationPoint low;
  Core::CalibrationPoint nominal;
  Core::CalibrationPoint full;
  uint8_t version;
  uint32_t timestamp;
  char    source[24];
};

class VoltageCalibration {
public:
  void begin();
  // Apply calibration to AdcVoltageDriver
  void apply();
  // Update one calibration point (which: "low"|"nominal"|"full")
  // reference = known-good value (multimeter)
  // raw = sensor reading at that reference (captured at the same moment)
  bool setPoint(const char* which, float reference, float raw);
  // Restore last known good (revert a bad update)
  bool restorePrevious();
  // Factory defaults (no calibration — raw = calibrated)
  void resetToDefault();
  // Get current state
  VoltageCalibrationState getState() const { return _state; }
  // Validation — returns false if calibration is implausible
  bool validate(const VoltageCalibrationState& s, String& errOut) const;

private:
  VoltageCalibrationState _state = {};
  VoltageCalibrationState _previous = {};
  bool _hasPrevious = false;
};

extern VoltageCalibration voltageCalibration;

} // namespace Services

#endif // PLTS_SERVICES_VOLTAGE_CALIBRATION_H
