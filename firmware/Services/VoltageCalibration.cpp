// =============================================================================
// Services/VoltageCalibration.cpp
// =============================================================================
#include "VoltageCalibration.h"
#include "../Drivers/AdcVoltageDriver.h"
#include "../Storage/ConfigStore.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "AlarmRegistry.h"
#include "../Drivers/RtcDriver.h"
#include "LogService.h"
#include <cstring>
#include <cmath>

namespace Services {

VoltageCalibration voltageCalibration;

void VoltageCalibration::begin() {
  _state.low      = Core::calibration.voltageLow;
  _state.nominal  = Core::calibration.voltageNominal;
  _state.full     = Core::calibration.voltageFull;
  _state.version  = Core::calibration.version;
  _state.timestamp = Core::calibration.timestamp;
  strncpy(_state.source, Core::calibration.source, sizeof(_state.source)-1);
  _state.source[sizeof(_state.source)-1] = '\0';
  apply();
}

void VoltageCalibration::apply() {
  Drivers::batteryAdc.setCalibration(
    _state.low.reference, _state.low.raw,
    _state.nominal.reference, _state.nominal.raw,
    _state.full.reference, _state.full.raw);
}

bool VoltageCalibration::validate(const VoltageCalibrationState& s, String& errOut) const {
  if (s.low.reference >= s.nominal.reference) {
    errOut = "LOW reference must be < NOMINAL reference";
    return false;
  }
  if (s.nominal.reference >= s.full.reference) {
    errOut = "NOMINAL reference must be < FULL reference";
    return false;
  }
  if (s.low.raw >= s.nominal.raw) {
    errOut = "LOW raw must be < NOMINAL raw";
    return false;
  }
  if (s.nominal.raw >= s.full.raw) {
    errOut = "NOMINAL raw must be < FULL raw";
    return false;
  }
  // Excessive deviation check: each raw must be within ±10% of reference
  auto within10 = [](float a, float b) -> bool {
    if (a <= 0) return false;
    float dev = std::fabs(a - b) / a;
    return dev <= 0.10f;
  };
  if (!within10(s.low.reference, s.low.raw) ||
      !within10(s.nominal.reference, s.nominal.raw) ||
      !within10(s.full.reference, s.full.raw)) {
    errOut = "raw deviation > 10% — sensor or capture error";
    return false;
  }
  // Plausibility bounds
  if (s.low.reference < Core::VBAT_MIN_PLAUSIBLE ||
      s.full.reference > Core::VBAT_MAX_PLAUSIBLE) {
    errOut = "reference outside plausible battery voltage range";
    return false;
  }
  return true;
}

bool VoltageCalibration::setPoint(const char* which, float reference, float raw) {
  if (!which) return false;
  VoltageCalibrationState s = _state;
  uint32_t now = Drivers::rtc.getUnixTime();  // 0 if unsynced (acceptable)
  if (strcmp(which, "low") == 0) {
    s.low = { reference, raw, now };
  } else if (strcmp(which, "nominal") == 0) {
    s.nominal = { reference, raw, now };
  } else if (strcmp(which, "full") == 0) {
    s.full = { reference, raw, now };
  } else {
    return false;
  }
  String err;
  if (!validate(s, err)) {
    String msg = String("Voltage calibration rejected: ") + err;
    alarms.raise("CALIBRATION_ERROR", Core::AlarmSeverity::Warning, msg.c_str());
    return false;
  }
  // Save previous for restore
  _previous = _state;
  _hasPrevious = true;
  _state = s;
  _state.timestamp = now;
  strncpy(_state.source, "FIELD", sizeof(_state.source)-1);
  _state.source[sizeof(_state.source)-1] = '\0';
  // Sync to Core::calibration
  Core::calibration.voltageLow = _state.low;
  Core::calibration.voltageNominal = _state.nominal;
  Core::calibration.voltageFull = _state.full;
  Core::calibration.timestamp = _state.timestamp;
  strncpy(Core::calibration.source, _state.source, sizeof(Core::calibration.source)-1);
  apply();
  Storage::config.markCalibrationDirty();
  Storage::config.saveCalibration(true);
  Log.audit("calibration.voltage.point", which, "-", String(reference, 3).c_str(),
            _state.source, _state.version);
  alarms.clear("CALIBRATION_ERROR");
  return true;
}

bool VoltageCalibration::restorePrevious() {
  if (!_hasPrevious) return false;
  _state = _previous;
  _hasPrevious = false;
  Core::calibration.voltageLow = _state.low;
  Core::calibration.voltageNominal = _state.nominal;
  Core::calibration.voltageFull = _state.full;
  apply();
  Storage::config.markCalibrationDirty();
  Storage::config.saveCalibration(true);
  Log.append(Core::LogType::CalibrationChanged,
             "Voltage calibration restored to previous", 0);
  return true;
}

void VoltageCalibration::resetToDefault() {
  _previous = _state;
  _hasPrevious = true;
  _state.low      = { 45.0f, 45.0f, 0 };
  _state.nominal  = { 50.0f, 50.0f, 0 };
  _state.full     = { 54.0f, 54.0f, 0 };
  _state.version  = 1;  // calibration schema version 1
  _state.timestamp = 0;
  strncpy(_state.source, "DEFAULT", sizeof(_state.source)-1);
  _state.source[sizeof(_state.source)-1] = '\0';
  Core::calibration.voltageLow = _state.low;
  Core::calibration.voltageNominal = _state.nominal;
  Core::calibration.voltageFull = _state.full;
  Core::calibration.timestamp = 0;
  strncpy(Core::calibration.source, "DEFAULT", sizeof(Core::calibration.source)-1);
  Drivers::batteryAdc.clearCalibration();
  Storage::config.markCalibrationDirty();
  Storage::config.saveCalibration(true);
  Log.append(Core::LogType::CalibrationChanged,
             "Voltage calibration reset to factory default", 0);
}

} // namespace Services
