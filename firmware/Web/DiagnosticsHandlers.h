// =============================================================================
// Web/DiagnosticsHandlers.h — GET /api/diagnostics
// =============================================================================
#pragma once
#ifndef PLTS_WEB_DIAGNOSTICS_HANDLERS_H
#define PLTS_WEB_DIAGNOSTICS_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace DiagnosticsHandlers {
  void registerRoutes();
  void handleGet();
}
}
#endif
