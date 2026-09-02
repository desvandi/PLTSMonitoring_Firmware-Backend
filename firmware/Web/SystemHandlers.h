// =============================================================================
// Web/SystemHandlers.h — /api/reboot, /api/factory_reset/{prepare,confirm}
// =============================================================================
#pragma once
#ifndef PLTS_WEB_SYSTEM_HANDLERS_H
#define PLTS_WEB_SYSTEM_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace SystemHandlers {
  void registerRoutes();
  void handleReboot();
  void handleFactoryResetPrepare();
  void handleFactoryResetConfirm();
}
}
#endif
