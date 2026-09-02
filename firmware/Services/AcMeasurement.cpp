// =============================================================================
// Services/AcMeasurement.cpp — AC RMS/peak/average + ESTIMATED power
// =============================================================================
#include "AcMeasurement.h"
#include "../Drivers/Acs712Driver.h"
#include "AlarmRegistry.h"
#include <cmath>
#include <cstdio>

namespace Services {

AcMeasurement acMeasurement;

void AcMeasurement::begin() {
  _assumedV = Core::ASSUMED_AC_VOLTAGE;
  _assumedPF = Core::ASSUMED_POWER_FACTOR;
  Serial.printf("[ACMEAS] init: assumedV=%.1f assumedPF=%.2f\n",
                (double)_assumedV, (double)_assumedPF);
}

AcMeasurementResult AcMeasurement::compute(uint32_t seq, uint32_t ts) {
  AcMeasurementResult r = {};
  Drivers::Acs712Reading rd = Drivers::acs712.getReading();

  Core::MeasurementQuality q;
  switch (rd.status) {
    case Drivers::Acs712Status::Ok:              q = Core::MeasurementQuality::Valid;       break;
    case Drivers::Acs712Status::NotInitialized:    q = Core::MeasurementQuality::NotAvailable; break;
    case Drivers::Acs712Status::OutOfRange:       q = Core::MeasurementQuality::OutOfRange;  break;
    default:                                      q = Core::MeasurementQuality::SensorError;  break;
  }

  if (q != Core::MeasurementQuality::Valid) {
    r.rmsCurrent     = Core::makeInvalidMeasurement(q, ts, seq);
    r.rmsCurrent.source = Core::MeasurementSource::Measured;
    r.peakCurrent    = Core::makeInvalidMeasurement(q, ts, seq);
    r.peakCurrent.source = Core::MeasurementSource::Measured;
    r.averageCurrent = Core::makeInvalidMeasurement(q, ts, seq);
    r.averageCurrent.source = Core::MeasurementSource::Measured;
    r.estimatedPower = Core::makeInvalidMeasurement(Core::MeasurementQuality::NotAvailable, ts, seq);
    r.estimatedPower.source = Core::MeasurementSource::Estimated;
    r.signalQuality = "UNAVAILABLE";
    return r;
  }

  r.rmsCurrent = Core::makeMeasurement(rd.rmsCurrentA,
                                        Core::MeasurementSource::Measured,
                                        Core::MeasurementQuality::Valid, ts, seq);
  r.peakCurrent = Core::makeMeasurement(rd.peakCurrentA,
                                         Core::MeasurementSource::Measured,
                                         Core::MeasurementQuality::Valid, ts, seq);
  r.averageCurrent = Core::makeMeasurement(rd.averageCurrentA,
                                             Core::MeasurementSource::Measured,
                                             Core::MeasurementQuality::Valid, ts, seq);
  // ESTIMATED power — brief §28: no AC voltage sensor, only assumed V × PF
  float pEst = _assumedV * rd.rmsCurrentA * _assumedPF;
  r.estimatedPower = Core::makeMeasurement(pEst,
                                             Core::MeasurementSource::Estimated,
                                             Core::MeasurementQuality::Estimated, ts, seq);
  r.signalQuality = Drivers::acs712SignalQualityStr(rd.signalQuality);

  // AC overcurrent alarm (brief §35)
  if (rd.rmsCurrentA > Core::AC_OVERCURRENT_THRESHOLD) {
    char buf[64];
    snprintf(buf, sizeof(buf), "AC overcurrent: %.2f A", (double)rd.rmsCurrentA);
    alarms.raise(Core::AlarmCode::AC_OVERCURRENT, Core::AlarmSeverity::Warning, buf);
  } else {
    alarms.clear(Core::AlarmCode::AC_OVERCURRENT);
  }
  return r;
}

} // namespace Services
