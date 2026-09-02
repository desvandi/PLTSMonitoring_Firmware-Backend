// =============================================================================
// Web/StatusHandlers.h — GET /api/status (full telemetry snapshot)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_STATUS_HANDLERS_H
#define PLTS_WEB_STATUS_HANDLERS_H

#include <Arduino.h>

namespace Web {
namespace StatusHandlers {
  void registerRoutes();
  void handleStatus();  // GET /api/status
}
}

#endif
