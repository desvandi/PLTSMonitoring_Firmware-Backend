// =============================================================================
// Web/DiagnosticsHandlers.h — GET /api/diagnostics (+ /api/diagnostics/ina219)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_DIAGNOSTICS_HANDLERS_H
#define PLTS_WEB_DIAGNOSTICS_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace DiagnosticsHandlers {
  void registerRoutes();
  void handleGet();
  // [AUDIT ROUND 4 / INA219 hardware acceptance — p.4-p.5 auditor items]
  // Raw register + derived-value + BMS cross-check snapshot for procedures
  // INA-001..INA-004 (docs/hardware-acceptance/).
  void handleGetIna219();
}
}
#endif // PLTS_WEB_DIAGNOSTICS_HANDLERS_H
