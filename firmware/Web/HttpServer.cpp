// =============================================================================
// Web/HttpServer.cpp — route registration + security headers
// =============================================================================
#include "HttpServer.h"
#include "../Core/Config.h"
#include "../Services/AuthManager.h"
#include "../Services/LogService.h"

// Handler headers
#include "StatusHandlers.h"
#include "ConfigHandlers.h"
#include "CalibrationHandlers.h"
#include "LogHandlers.h"
#include "AlarmHandlers.h"
#include "DiagnosticsHandlers.h"
#include "EventHandlers.h"
#include "AuthHandlers.h"
#include "VersionHandlers.h"
#include "OtaHandlers.h"
#include "SystemHandlers.h"
#include "ExtraHandlers.h"
#include "ProvisionHandlers.h"

namespace Web {

HttpServer server;
WebServer& http = server.raw();

void HttpServer::begin() {
  // CORS + auth + OPTIONS handlers
  _server.onNotFound([]() {
    if (http.method() == HTTP_OPTIONS) {
      // CORS preflight
      String origin = F(ALLOWED_CORS_ORIGINS);
      if (strcmp(origin.c_str(), "*") == 0) {
        http.sendHeader("Access-Control-Allow-Origin", "*");
      } else {
        // Echo if matches allowed list
        if (http.hasHeader("Origin")) {
          String reqOrigin = http.header("Origin");
          String allowed = origin;
          int start = 0;
          while (start < (int)allowed.length()) {
            int comma = allowed.indexOf(',', start);
            String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
            one.trim();
            if (one.length() > 0 && reqOrigin == one) {
              http.sendHeader("Access-Control-Allow-Origin", reqOrigin);
              http.sendHeader("Vary", "Origin");
              break;
            }
            if (comma < 0) break;
            start = comma + 1;
          }
        }
      }
      http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
      http.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
      http.sendHeader("Access-Control-Allow-Credentials", "true");
      http.send(204, "text/plain", "");
      return;
    }
    http.send(404, "application/json", "{\"success\":false,\"message\":\"Not Found\"}");
  });

  // Register routes from each handler module
  StatusHandlers::registerRoutes();
  ConfigHandlers::registerRoutes();
  CalibrationHandlers::registerRoutes();
  LogHandlers::registerRoutes();
  AlarmHandlers::registerRoutes();
  DiagnosticsHandlers::registerRoutes();
  EventHandlers::registerRoutes();
  AuthHandlers::registerRoutes();
  VersionHandlers::registerRoutes();
  OtaHandlers::registerRoutes();
  SystemHandlers::registerRoutes();
  ExtraHandlers::registerRoutes();   // [FW-20] PWA-contract route parity
  // [AUDIT 2026-08-28 F-FW1] First-boot WiFi provisioning (AP setup mode
  // only — closes the dead-end where nothing ever wrote wifi_ssid/wifi_pass).
  ProvisionHandlers::registerRoutes();

  // Collect headers we need
  const char* headers[] = {
    "Origin", "Authorization", "X-CSRF-Token", "Content-Type", "Content-Length"
  };
  _server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));

  _server.begin();
  Serial.println(F("[WEB] HTTP server started on port 80"));
}

void HttpServer::handleClient() {
  _server.handleClient();
}

} // namespace Web
