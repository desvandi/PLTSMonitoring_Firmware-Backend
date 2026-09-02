// =============================================================================
// Services/AuthManager.cpp
// =============================================================================
// REMEDIATION 2026-08 (Audit #1+#2+#3 — P0-003, P0-004):
//   [P0-003] JWT signing secret is now the per-device CSPRNG secret generated
//            by ConfigStore::loadDeviceConfig() into Core::jwtSecret (NVS key
//            "plts"/"jwt", 32 bytes → 64 hex). The literal "jwt_secret" is
//            REMOVED. If the secret is unavailable (len != 64) the auth system
//            enters NOT_READY and fails closed (no token issue, no verify).
//   [P0-004] Rate limiter now binds failure state to the actual source IP
//            (packed IPv4). Each IP is tracked independently in its own slot
//            with a monotonic failure window: failures older than
//            RATE_LIMIT_WINDOW_MS no longer count; 5 fails → 60 s block,
//            10 fails → 300 s block. millis() rollover-safe unsigned math.
//            Memory-bounded: MAX_TRACKED_IPS slots, LRU eviction when full.
//            IPv4 only (ESP32 WebServer remoteIP is AF_INET); IPv6 is not
//            supported by this stack and is documented as such.
// =============================================================================
#include "AuthManager.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/Crypto.h"
#include "../Storage/ConfigStore.h"
#include "../Drivers/RtcDriver.h"
#include "LogService.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace Services {

AuthManager auth;

void AuthManager::begin() {
  rotateCsrfToken();
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) _attempts[i] = {};
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) _refreshTokens[i] = {};

  // [P0-003] Fail-closed readiness check: the per-device JWT secret must be a
  // 64-hex-char value loaded from NVS by ConfigStore. Without it, no token
  // may be signed or verified — AUTH SYSTEM = NOT_READY.
  _authReady = (strnlen(Core::jwtSecret, 65) == 64);
  if (!_authReady) {
    // Only reachable if NVS provisioning failed — never fall back to a
    // default secret (Audit directive §3.1: no fabricated trust).
    Log.append(Core::LogType::AuthFail,
               "AUTH NOT_READY: per-device JWT secret missing/invalid", 0);
  }
  Serial.print(F("[AUTH] init: "));
  Serial.print(_authReady ? "READY" : "NOT_READY");
  Serial.println(F(" | JWT 15min, refresh 7-day rotation, PBKDF2 10k, per-IP rate limit"));
}

bool AuthManager::isAuthReady() const { return _authReady; }

void AuthManager::rotateCsrfToken() {
  String t = Utils::generateToken(Core::CSRF_TOKEN_LEN);
  strncpy(_csrfToken, t.c_str(), Core::CSRF_TOKEN_LEN);
  _csrfToken[Core::CSRF_TOKEN_LEN] = '\0';
  _csrfTokenTime = millis();
}

bool AuthManager::checkCsrfToken(WebServer& server) {
  if (server.hasHeader("X-CSRF-Token")) {
    String given = server.header("X-CSRF-Token");
    if (given.length() == Core::CSRF_TOKEN_LEN &&
        Utils::constantTimeMemEquals((const volatile uint8_t*)given.c_str(),
                                      (const volatile uint8_t*)_csrfToken,
                                      Core::CSRF_TOKEN_LEN)) {
      return true;
    }
  }
  // Also accept from cookie
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    int idx = cookie.indexOf("csrf=");
    if (idx >= 0) {
      String t = cookie.substring(idx + 5, idx + 5 + Core::CSRF_TOKEN_LEN);
      // [WAVE-5 / FW-B1] Length check FIRST — constantTimeMemEquals with a
      // fixed length on a shorter String reads past its heap allocation.
      if (t.length() == Core::CSRF_TOKEN_LEN &&
          Utils::constantTimeMemEquals((const volatile uint8_t*)t.c_str(),
                                        (const volatile uint8_t*)_csrfToken,
                                        Core::CSRF_TOKEN_LEN)) {
        return true;
      }
    }
  }
  return false;
}

bool AuthManager::checkAuth(WebServer& server) {
  // [P0-003] Fail closed when the device secret is not provisioned.
  if (!_authReady) return false;
  if (server.hasHeader("Authorization")) {
    String h = server.header("Authorization");
    if (h.startsWith("Bearer ")) {
      String token = h.substring(7);
      String user;
      // [P0-003] per-device secret from Core::jwtSecret (NVS-provisioned).
      if (Utils::jwtVerify(token, String(Core::jwtSecret), user)) return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// [P0-004] Per-IP rate limiting — slot management
// ---------------------------------------------------------------------------

int AuthManager::_findIpSlot(uint32_t ip) {
  for (int i = 0; i < (int)Core::MAX_TRACKED_IPS; i++) {
    if (_attempts[i].ip == ip && _attempts[i].count > 0) return i;
  }
  return -1;
}

int AuthManager::_allocIpSlot(uint32_t ip) {
  // 1) prefer a free slot (count == 0)
  for (int i = 0; i < (int)Core::MAX_TRACKED_IPS; i++) {
    if (_attempts[i].count == 0) {
      _attempts[i] = {};
      _attempts[i].ip = ip;
      return i;
    }
  }
  // 2) memory-bounded: evict the slot with the OLDEST lastFailTime (LRU).
  //    Eviction is safe: an attacker cannot grow state unboundedly, and an
  //    idle slot's failure history has aged out of relevance.
  int oldest = 0;
  uint32_t oldestTs = 0xFFFFFFFF;
  for (int i = 0; i < (int)Core::MAX_TRACKED_IPS; i++) {
    if (_attempts[i].lastFailTime < oldestTs) {
      oldestTs = _attempts[i].lastFailTime;
      oldest = i;
    }
  }
  _attempts[oldest] = {};
  _attempts[oldest].ip = ip;
  return oldest;
}

bool AuthManager::checkRateLimit(uint32_t ip) {
  if (ip == 0) return true;  // no remote address (local console) — not tracked
  uint32_t now = millis();

  int slot = _findIpSlot(ip);
  if (slot < 0) return true;          // unknown IP — no failure state
  Core::AuthAttempt& a = _attempts[slot];

  // Active block?
  if (a.blockUntil != 0 &&
      (int32_t)(now - a.blockUntil) < 0) {
    return false;                     // still blocked
  }
  // Block expired → reset the window so the client gets a fresh start.
  if (a.blockUntil != 0) {
    a = {}; a.ip = ip;
    return true;
  }
  // Failure window expired → stale failures no longer count.
  if (a.count > 0 &&
      (uint32_t)(now - a.firstFailTime) > Core::RATE_LIMIT_WINDOW_MS) {
    a = {}; a.ip = ip;
  }
  return true;
}

void AuthManager::recordAuthFailure(uint32_t ip) {
  if (ip == 0) return;
  uint32_t now = millis();

  int slot = _findIpSlot(ip);
  if (slot < 0) slot = _allocIpSlot(ip);
  Core::AuthAttempt& a = _attempts[slot];

  // Window expired → start a new window with this failure.
  if (a.count > 0 &&
      (uint32_t)(now - a.firstFailTime) > Core::RATE_LIMIT_WINDOW_MS) {
    a.count = 0;
  }
  if (a.count == 0) a.firstFailTime = now;
  a.count++;
  a.lastFailTime = now;

  if (a.count >= Core::RATE_LIMIT_LONG_THRESHOLD) {
    a.blockUntil = now + Core::AUTH_BLOCK_LONG_MS;
    Log.append(Core::LogType::AuthFail,
               "Long block (" + String(Core::AUTH_BLOCK_LONG_MS / 1000) + "s) for IP " +
               _ipToString(ip) + " after " + String(a.count) + " failures", 0);
  } else if (a.count >= Core::RATE_LIMIT_SHORT_THRESHOLD) {
    a.blockUntil = now + Core::AUTH_BLOCK_SHORT_MS;
    Log.append(Core::LogType::AuthFail,
               "Short block (" + String(Core::AUTH_BLOCK_SHORT_MS / 1000) + "s) for IP " +
               _ipToString(ip) + " after " + String(a.count) + " failures", 0);
  }
}

void AuthManager::recordAuthSuccess(uint32_t ip) {
  int slot = _findIpSlot(ip);
  if (slot >= 0) _attempts[slot] = {};   // clear failure state for this IP only
}

String AuthManager::_ipToString(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
           (uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
           (uint8_t)(ip >> 8), (uint8_t)ip);
  return String(buf);
}

uint8_t AuthManager::trackedIpCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) {
    if (_attempts[i].count > 0) n++;
  }
  return n;
}

String AuthManager::issueAccessToken(const String& username) {
  if (!_authReady) return String();   // NOT_READY — never sign
  // [P0-003] per-device NVS-provisioned secret.
  return Utils::jwtSign(username, String(Core::jwtSecret), Core::JWT_ACCESS_TTL_SEC);
}

int AuthManager::_findRefreshSlot() {
  // LRU: find oldest unused slot or first empty
  int oldest = 0;
  uint32_t oldestTs = 0xFFFFFFFF;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS; i++) {
    if (!_refreshTokens[i].used && _refreshTokens[i].expiresAt < oldestTs) {
      oldestTs = _refreshTokens[i].expiresAt;
      oldest = i;
    }
    if (_refreshTokens[i].token[0] == '\0') return i;
  }
  return oldest;
}

String AuthManager::issueRefreshToken(WebServer& server, const String& username) {
  (void)username;
  int slot = _findRefreshSlot();
  RefreshToken& rt = _refreshTokens[slot];
  String t = Utils::generateToken(32);
  strncpy(rt.token, t.c_str(), 32); rt.token[32] = '\0';
  rt.issuedAt = Drivers::rtc.getUnixTime();
  rt.expiresAt = rt.issuedAt + Core::JWT_REFRESH_TTL_SEC;
  // Best-effort IP capture
  String ip = server.client().remoteIP().toString();
  strncpy(rt.ip, ip.c_str(), 15); rt.ip[15] = '\0';
  rt.used = false;
  return t;
}

bool AuthManager::verifyRefreshToken(const String& token, String& outUsername) {
  // [WAVE-5 / FW-B1] Fixed-length compare needs an equal-length input —
  // a shorter String would be overread past its allocation.
  if (token.length() != 32) return false;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS; i++) {
    if (_refreshTokens[i].token[0] == '\0') continue;
    if (Utils::constantTimeMemEquals((const volatile uint8_t*)_refreshTokens[i].token,
                                      (const volatile uint8_t*)token.c_str(), 32)) {
      if (_refreshTokens[i].used) return false;
      uint32_t now = Drivers::rtc.getUnixTime();
      if (now > _refreshTokens[i].expiresAt) return false;
      outUsername = Core::wwwUser;  // single-user system
      return true;
    }
  }
  return false;
}

bool AuthManager::rotateRefreshToken(const String& oldToken, String& outNewToken) {
  // [WAVE-5 / FW-B1] Same length guard as verifyRefreshToken.
  if (oldToken.length() != 32) return false;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS; i++) {
    if (_refreshTokens[i].token[0] == '\0') continue;
    if (Utils::constantTimeMemEquals((const volatile uint8_t*)_refreshTokens[i].token,
                                      (const volatile uint8_t*)oldToken.c_str(), 32)) {
      _refreshTokens[i].used = true;  // one-time use
      outNewToken = Utils::generateToken(32);
      // Reuse slot for new token
      strncpy(_refreshTokens[i].token, outNewToken.c_str(), 32);
      _refreshTokens[i].token[32] = '\0';
      _refreshTokens[i].issuedAt = Drivers::rtc.getUnixTime();
      _refreshTokens[i].expiresAt = _refreshTokens[i].issuedAt + Core::JWT_REFRESH_TTL_SEC;
      _refreshTokens[i].used = false;
      return true;
    }
  }
  return false;
}

String AuthManager::prepareFactoryReset() {
  String t = Utils::generateToken(32);
  strncpy(_factoryResetToken, t.c_str(), 32);
  _factoryResetToken[32] = '\0';
  _factoryResetTokenTime = millis();
  Log.append(Core::LogType::ConfigurationChanged,
             "Factory reset prepared (60s TTL)", 0);
  return t;
}

bool AuthManager::confirmFactoryReset(const String& token) {
  if (_factoryResetToken[0] == '\0') return false;
  // [WAVE-5 / FW-B1] Length guard before fixed-length constant-time compare
  // (token arrives from an MQTT JSON field — arbitrary length possible).
  if (token.length() != 32) return false;
  if (millis() - _factoryResetTokenTime > Core::FACTORY_RESET_TOKEN_TTL_MS) {
    _factoryResetToken[0] = '\0';
    return false;
  }
  if (!Utils::constantTimeMemEquals((const volatile uint8_t*)_factoryResetToken,
                                      (const volatile uint8_t*)token.c_str(), 32)) {
    return false;
  }
  _factoryResetToken[0] = '\0';
  return true;
}

} // namespace Services
