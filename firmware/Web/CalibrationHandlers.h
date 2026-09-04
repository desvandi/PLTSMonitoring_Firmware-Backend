// =============================================================================
// Web/CalibrationHandlers.h — voltage 3-point + ACS712 zero
// =============================================================================
#pragma once
#ifndef PLTS_WEB_CALIBRATION_HANDLERS_H
#define PLTS_WEB_CALIBRATION_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace CalibrationHandlers {
  void registerRoutes();
  void handleGet();
  void handlePost();
  void handlePoint(const String& which);
  void handleAcs712Zero();
}
}
#endif
