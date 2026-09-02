// =============================================================================
// Web/AlarmHandlers.h — GET /api/alarms + POST /api/alarms/{code}/acknowledge
// =============================================================================
#pragma once
#ifndef PLTS_WEB_ALARM_HANDLERS_H
#define PLTS_WEB_ALARM_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace AlarmHandlers {
  void registerRoutes();
  void handleGetAlarms();
  void handleAcknowledge();
}
}
#endif
