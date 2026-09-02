// =============================================================================
// Web/EventHandlers.h — GET /api/events?from=&to=
// =============================================================================
#pragma once
#ifndef PLTS_WEB_EVENT_HANDLERS_H
#define PLTS_WEB_EVENT_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace EventHandlers {
  void registerRoutes();
  void handleGet();
}
}
#endif
