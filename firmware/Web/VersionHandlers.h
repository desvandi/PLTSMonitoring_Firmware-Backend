// =============================================================================
// Web/VersionHandlers.h — GET /api/version
// =============================================================================
#pragma once
#ifndef PLTS_WEB_VERSION_HANDLERS_H
#define PLTS_WEB_VERSION_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace VersionHandlers {
  void registerRoutes();
  void handleGet();
}
}
#endif
