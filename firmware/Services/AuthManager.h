// =============================================================================
// Services/AuthManager.h — JWT + refresh rotation + PBKDF2 + rate limiter
// -----------------------------------------------------------------------------
// Brief §11, §38:
//   - Access token: JWT HS256, 15-min TTL
//   - Refresh token: 7-day, NVS LRU 4 (one-time use)
//   - PBKDF2-SHA256 10k iters
//   - Per-IP rate limiter: 5 fails → 1 min, 10 fails → 5 min
//   - Constant-time compares
//   - Two-step factory reset (60s TTL)
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_AUTH_MANAGER_H
#define PLTS_SERVICES_AUTH_MANAGER_H

#include <Arduino.h>
#include <WebServer.h>
#include "../Core/Config.h"
#include "../Core/Types.h"

namespace Services {

struct RefreshToken {
  char token[33];          // 32 hex chars + null
  uint32_t issuedAt;
  uint32_t expiresAt;
  char     ip[16];
  bool     used;
};

class AuthManager {
public:
  void begin();
  bool checkAuth(WebServer& server);
  bool checkCsrfToken(WebServer& server);

  // [P0-004] Per-IP rate limiting (packed IPv4, monotonic window).
  // checkRateLimit: false when this IP is currently blocked.
  bool checkRateLimit(uint32_t ip);
  void recordAuthFailure(uint32_t ip);
  void recordAuthSuccess(uint32_t ip);
  uint8_t trackedIpCount() const;

  // [P0-003] True when the per-device JWT secret is provisioned.
  bool isAuthReady() const;

  // JWT issue + refresh
  String issueAccessToken(const String& username);
  String issueRefreshToken(WebServer& server, const String& username);
  bool verifyRefreshToken(const String& token, String& outUsername);
  bool rotateRefreshToken(const String& oldToken, String& outNewToken);

  // CSRF
  String getCsrfToken() const { return String(_csrfToken); }
  void rotateCsrfToken();

  // Factory reset (two-step)
  String prepareFactoryReset();
  bool confirmFactoryReset(const String& token);

private:
  char _csrfToken[Core::CSRF_TOKEN_LEN + 1] = {0};
  unsigned long _csrfTokenTime = 0;
  Core::AuthAttempt _attempts[Core::MAX_TRACKED_IPS] = {};
  RefreshToken _refreshTokens[Core::MAX_REFRESH_TOKENS] = {};
  uint8_t _refreshIdx = 0;
  char _factoryResetToken[33] = {0};
  unsigned long _factoryResetTokenTime = 0;
  bool _authReady = false;          // [P0-003] fail-closed readiness

  int _findIpSlot(uint32_t ip);      // [P0-004]
  int _allocIpSlot(uint32_t ip);     // [P0-004] LRU eviction, memory-bounded
  static String _ipToString(uint32_t ip);

  int _findRefreshSlot();
  bool verifyJwtOrRefresh(const String& authHeader);

  // [audit-2 K-4] Persist refresh tokens across reboot (header contract
  // "NVS LRU 4" was previously a lie — no persistence existed).
  void _persistRefreshTokens();
  void _loadRefreshTokens();
};

extern AuthManager auth;

} // namespace Services

#endif // PLTS_SERVICES_AUTH_MANAGER_H
