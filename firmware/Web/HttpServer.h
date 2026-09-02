// =============================================================================
// Web/HttpServer.h — ESP32 WebServer, ~20 routes, security headers, CORS
// =============================================================================
#pragma once
#ifndef PLTS_WEB_HTTP_SERVER_H
#define PLTS_WEB_HTTP_SERVER_H

#include <Arduino.h>
#include <WebServer.h>

namespace Web {

class HttpServer {
public:
  void begin();
  void handleClient();

  // Public accessor for handlers (CORS + auth helpers)
  WebServer& raw() { return _server; }

private:
  WebServer _server;
};

extern HttpServer server;
extern WebServer& http;  // alias for handler convenience

} // namespace Web

#endif // PLTS_WEB_HTTP_SERVER_H
