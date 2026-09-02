// =============================================================================
// Web/ConfigHandlers.h — GET/POST /api/config (CSRF + requestId + TXN)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_CONFIG_HANDLERS_H
#define PLTS_WEB_CONFIG_HANDLERS_H

#include <Arduino.h>
namespace Web {
namespace ConfigHandlers {
  void registerRoutes();
  void handleGetConfig();
  void handlePostConfig();
}
}
#endif
