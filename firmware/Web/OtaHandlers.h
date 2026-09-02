// =============================================================================
// Web/OtaHandlers.h — POST /api/ota (multipart upload), POST /api/ota/check
// =============================================================================
#pragma once
#ifndef PLTS_WEB_OTA_HANDLERS_H
#define PLTS_WEB_OTA_HANDLERS_H
#include <Arduino.h>
namespace Web {
namespace OtaHandlers {
  void registerRoutes();
  void handleUpload();
  void handleCheck();
  // Handler called by WebServer upload callback for each chunk
  void handleUploadStep();
}
}
#endif
