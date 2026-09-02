// =============================================================================
// Web/LogHandlers.h — GET /api/logs?limit=&type=
// =============================================================================
#pragma once
#ifndef PLTS_WEB_LOG_HANDLERS_H
#define PLTS_WEB_LOG_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace LogHandlers {
  void registerRoutes();
  void handleGetLogs();
}
}
#endif
