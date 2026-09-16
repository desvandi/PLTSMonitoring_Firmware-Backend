// =============================================================================
// Web/Common.h — Shared web helpers (CORS, security headers, success/error)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_COMMON_H
#define PLTS_WEB_COMMON_H

#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "HttpServer.h"
#include "../Core/Config.h"
#include "../Core/Types.h"
#include "../Services/AuthManager.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include "../Utils/Json.h"

namespace Web {

inline String getAllowedOrigin() {
  if (strcmp(ALLOWED_CORS_ORIGINS, "*") == 0) return "*";
  if (http.hasHeader("Origin")) {
    String origin = http.header("Origin");
    String allowed = ALLOWED_CORS_ORIGINS;
    int start = 0;
    while (start < (int)allowed.length()) {
      int comma = allowed.indexOf(',', start);
      String one = (comma < 0) ? allowed.substring(start) : allowed.substring(start, comma);
      one.trim();
      if (one.length() > 0 && origin == one) return origin;
      if (comma < 0) break;
      start = comma + 1;
    }
  }
  return "";
}

inline void sendSecurityHeaders() {
  String origin = getAllowedOrigin();
  if (origin.length() > 0) {
    http.sendHeader("Access-Control-Allow-Origin", origin);
    http.sendHeader("Access-Control-Allow-Credentials", "true");
    http.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    http.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-CSRF-Token");
    if (origin != "*") http.sendHeader("Vary", "Origin");
  }
  http.sendHeader("X-Frame-Options", "DENY");
  http.sendHeader("X-Content-Type-Options", "nosniff");
  http.sendHeader("Cache-Control", "no-store");
  http.sendHeader("Referrer-Policy", "no-referrer");
#ifdef PRODUCTION_BUILD
  // [audit p.490 REMEDIATION] HSTS for the documented production transport:
  // the device API is reached through a TLS gateway (Cloudflare Tunnel /
  // nginx TLS proxy) that terminates TLS and forwards to :80. The header
  // passes through the gateway to the browser and pins HTTPS for the
  // exposed hostname. (Ignored by browsers when received over plain HTTP —
  // harmless on the isolated LAN path.)
  http.sendHeader("Strict-Transport-Security", "max-age=31536000");
#endif
}

inline void sendSuccess(const String& message, const String& dataJson = "{}") {
  String body = "{\"success\":true,\"message\":\"" + message + "\",\"data\":" + dataJson + "}";
  sendSecurityHeaders();
  http.send(200, "application/json; charset=utf-8", body);
}

inline void sendError(int code, const String& message) {
  String body = "{\"success\":false,\"message\":\"" + message + "\",\"data\":null}";
  sendSecurityHeaders();
  http.send(code, "application/json; charset=utf-8", body);
}

inline bool requireAuth() { return Services::auth.checkAuth(http); }
// [audit p.489 REMEDIATION] Role-gated auth for EVERY mutation endpoint:
// 401 when not authenticated, 403 when authenticated but the token's role
// claim does not carry the required capability. Tokens without a role claim
// resolve to viewer (least privilege) — see Utils::jwtVerify.
inline bool requireRole(const char* role) {
  if (Services::auth.checkAuthRole(http, role)) return true;
  if (!Services::auth.checkAuth(http)) {
    sendError(401, "Unauthorized");
  } else {
    sendError(403, String("Forbidden — this endpoint requires the '") + role + "' role");
  }
  return false;
}
inline bool requireCsrf() {
  if (!Services::auth.checkCsrfToken(http)) {
    sendError(403, "Invalid CSRF token");
    return false;
  }
  return true;
}
inline bool requireBody(size_t maxSize) {
  if (http.hasHeader("Content-Length")) {
    size_t len = (size_t)http.header("Content-Length").toInt();
    if (len > maxSize) { sendError(413, "Body too large"); return false; }
  }
  return true;
}

} // namespace Web

#endif // PLTS_WEB_COMMON_H
