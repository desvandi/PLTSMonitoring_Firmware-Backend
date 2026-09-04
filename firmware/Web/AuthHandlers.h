// =============================================================================
// Web/AuthHandlers.h — POST /api/login, /api/logout, /api/session, /api/refresh
// =============================================================================
#pragma once
#ifndef PLTS_WEB_AUTH_HANDLERS_H
#define PLTS_WEB_AUTH_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace AuthHandlers {
  void registerRoutes();
  void handleLogin();
  void handleLogout();
  void handleSession();
  void handleRefresh();
}
}
#endif
