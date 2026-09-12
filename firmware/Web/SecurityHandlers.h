// =============================================================================
// Web/SecurityHandlers.h — GET /api/security
// =============================================================================
#pragma once
#ifndef PLTS_WEB_SECURITY_HANDLERS_H
#define PLTS_WEB_SECURITY_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace SecurityHandlers {
  void registerRoutes();
  void handleGet();
}
}
#endif // PLTS_WEB_SECURITY_HANDLERS_H
