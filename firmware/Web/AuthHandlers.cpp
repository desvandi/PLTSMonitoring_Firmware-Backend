// =============================================================================
// Web/AuthHandlers.cpp — JWT issue + refresh rotation + rate limit
// =============================================================================
#include "AuthHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Services/AuthManager.h"
#include "../Services/LogService.h"
#include "../Utils/Crypto.h"
#include <ArduinoJson.h>

namespace Web {
namespace AuthHandlers {

static uint32_t extractIp() {
  IPAddress ip = http.client().remoteIP();
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
}

void handleLogin() {
  uint32_t ip = extractIp();
  if (!Services::auth.checkRateLimit(ip)) {
    sendError(429, "Too many failed attempts — try later");
    return;
  }
  // [P0-003] Fail-closed: refuse login when the per-device JWT secret is
  // not provisioned (auth system NOT_READY) instead of signing with a
  // fallback secret.
  if (!Services::auth.isAuthReady()) {
    sendError(503, "Auth system not ready — device not provisioned");
    return;
  }
  if (!requireBody(1024)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  const char* user = doc["username"] | "";
  const char* pass = doc["password"] | "";
  if (strlen(user) == 0 || strlen(pass) == 0) {
    sendError(400, "Missing username or password"); return;
  }
  // Compute PBKDF2 hash of provided password using stored salt + iterations
  uint8_t hash[32];
  if (!Utils::pbkdf2HmacSha256(pass, strlen(pass),
                                Core::salt, Core::SALT_LEN,
                                Core::iterations, hash)) {
    sendError(500, "Hash computation failed"); return;
  }
  char hashHex[65];
  Utils::bytesToHex(hash, 32, hashHex);
  memset(hash, 0, sizeof(hash));
  bool ok = (strcmp(user, Core::wwwUser) == 0) &&
            Utils::constantTimeMemEquals(
              (const volatile uint8_t*)hashHex,
              (const volatile uint8_t*)Core::passHashHex, 64);
  if (!ok) {
    Services::auth.recordAuthFailure(ip);
    Services::Log.append(Core::LogType::AuthFail,
                          "Failed login from " + String(ip), 0);
    sendError(401, "Invalid credentials");
    return;
  }
  Services::auth.recordAuthSuccess(ip);
  String accessToken = Services::auth.issueAccessToken(user);
  String refreshToken = Services::auth.issueRefreshToken(http, user);
  // Rotate CSRF token after successful login
  Services::auth.rotateCsrfToken();
  // Set cookies (httpOnly for JWT, regular for CSRF + refresh)
  String jwtCookie = "jwt=" + accessToken + "; HttpOnly; Path=/; Max-Age=" +
                     String(Core::JWT_ACCESS_TTL_SEC);
  String refreshCookie = "refresh=" + refreshToken + "; HttpOnly; Path=/api/refresh; Max-Age=" +
                          String(Core::JWT_REFRESH_TTL_SEC);
  String csrfCookie = "csrf=" + Services::auth.getCsrfToken() +
                       "; Path=/; Max-Age=" + String(Core::JWT_ACCESS_TTL_SEC);
  sendSecurityHeaders();
  http.sendHeader("Set-Cookie", jwtCookie, false);
  http.sendHeader("Set-Cookie", refreshCookie, false);
  http.sendHeader("Set-Cookie", csrfCookie, false);
  String body = "{\"success\":true,\"message\":\"Login OK\","
                "\"data\":{\"csrfToken\":\"" + Services::auth.getCsrfToken() + "\"}}";
  http.send(200, "application/json; charset=utf-8", body);
  Services::Log.append(Core::LogType::Login, "User logged in", 0);
}

void handleLogout() {
  if (!requireCsrf()) return;
  Services::auth.rotateCsrfToken();
  sendSecurityHeaders();
  http.sendHeader("Set-Cookie", "jwt=; HttpOnly; Path=/; Max-Age=0", false);
  http.sendHeader("Set-Cookie", "refresh=; HttpOnly; Path=/api/refresh; Max-Age=0", false);
  http.sendHeader("Set-Cookie", "csrf=; Path=/; Max-Age=0", false);
  Services::Log.append(Core::LogType::Logout, "User logged out", 0);
  sendSuccess("Logged out", "{}");
}

void handleSession() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  String data = "{\"authenticated\":true,\"csrfToken\":\"" +
                 Services::auth.getCsrfToken() + "\"}";
  sendSuccess("OK", data);
}

void handleRefresh() {
  String refreshCookie;
  if (http.hasHeader("Cookie")) {
    String cookie = http.header("Cookie");
    int idx = cookie.indexOf("refresh=");
    if (idx >= 0) {
      int end = cookie.indexOf(';', idx);
      refreshCookie = (end < 0) ? cookie.substring(idx + 8)
                                  : cookie.substring(idx + 8, end);
    }
  }
  if (refreshCookie.length() != 32) { sendError(401, "No refresh token"); return; }
  String user;
  if (!Services::auth.verifyRefreshToken(refreshCookie, user)) {
    sendError(401, "Invalid or expired refresh token");
    return;
  }
  String newRefresh;
  if (!Services::auth.rotateRefreshToken(refreshCookie, newRefresh)) {
    sendError(500, "Refresh rotation failed");
    return;
  }
  String newAccess = Services::auth.issueAccessToken(user);
  Services::auth.rotateCsrfToken();
  String jwtCookie = "jwt=" + newAccess + "; HttpOnly; Path=/; Max-Age=" +
                     String(Core::JWT_ACCESS_TTL_SEC);
  String refreshCookieHdr = "refresh=" + newRefresh +
                             "; HttpOnly; Path=/api/refresh; Max-Age=" +
                             String(Core::JWT_REFRESH_TTL_SEC);
  String csrfCookie = "csrf=" + Services::auth.getCsrfToken() +
                       "; Path=/; Max-Age=" + String(Core::JWT_ACCESS_TTL_SEC);
  sendSecurityHeaders();
  http.sendHeader("Set-Cookie", jwtCookie, false);
  http.sendHeader("Set-Cookie", refreshCookieHdr, false);
  http.sendHeader("Set-Cookie", csrfCookie, false);
  String body = "{\"success\":true,\"message\":\"Refreshed\",\"data\":{\"csrfToken\":\"" +
                 Services::auth.getCsrfToken() + "\"}}";
  http.send(200, "application/json; charset=utf-8", body);
}

void registerRoutes() {
  http.on("/api/login", HTTP_POST, handleLogin);
  http.on("/api/logout", HTTP_POST, handleLogout);
  http.on("/api/session", HTTP_GET, handleSession);
  http.on("/api/refresh", HTTP_POST, handleRefresh);
}

} // namespace AuthHandlers
} // namespace Web
