// =============================================================================
// Services/AuthManager.h — JWT + refresh rotation + PBKDF2 + rate limiter
// -----------------------------------------------------------------------------
// Brief §11, §38:
//   - Access token: JWT HS256, 15-min TTL
//   - Refresh token: 7-day, NVS LRU 4 (one-time use)
//   - PBKDF2-SHA256 10k iters (BOUNDED — see AuthManager.cpp)
//   - Per-IP rate limiter: 5 fails → 1 min, 10 fails → 5 min
//   - Constant-time compares
//   - Two-step factory reset (60s TTL)
//
// [PRODUCTION-GRADE 2026-09] — audit p.165-173 remediation:
//   - consumeRefreshToken(): ONE atomic verify+mark-used+rotate operation
//     under a FreeRTOS mutex (AUTH-GATE-05) — the old separate
//     verifyRefreshToken() + rotateRefreshToken() pair allowed two
//     concurrent requests to both verify the same not-yet-used token and
//     both rotate it (A→B and A→C).
//   - revokeAllRefreshTokens(): logout now revokes SERVER-SIDE sessions
//     (AUTH-GATE-07) — previously logout only cleared browser cookies while
//     stolen refresh tokens stayed valid in NVS.
//   - The persisted refresh-token blob now carries a REAL CRC32 (AUTH-GATE-06)
//     — the old comment claimed a guard that was never written.
//   - PBKDF2 iterations are clamped to [MIN, MAX] on load (AUTH-GATE-04) — a
//     corrupted value of ~4 billion would turn one login into a CPU DoS.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_AUTH_MANAGER_H
#define PLTS_SERVICES_AUTH_MANAGER_H

#include <Arduino.h>
#include <WebServer.h>
#include <atomic>
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
  // [audit p.489 REMEDIATION] Authentication + AUTHORIZATION: verifies the
  // JWT AND its role claim against the required capability. Tokens without
  // a role claim resolve to "viewer" (least privilege, fail-closed) in
  // Utils::jwtVerify — mutation endpoints enforce "operator" here.
  bool checkAuthRole(WebServer& server, const char* requiredRole);
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
  // [AUTH-GATE-05] Atomic consume: find → validate → mark used → allocate new
  // → persist, all under the auth mutex. Replaces the racy
  // verify+rotate two-step. Returns false when oldToken is invalid/used/expired.
  bool consumeRefreshToken(const String& oldToken, String& outNewToken, String& outUsername);

  // Legacy split operations — RETAINED for read-only verification paths only
  // (never call verify+rotate sequentially for rotation; use consumeRefreshToken).
  bool verifyRefreshToken(const String& token, String& outUsername);

  // [AUTH-GATE-07] Revoke every refresh session server-side. Called on
  // logout — a stolen refresh token dies with the user's logout, not with
  // its 7-day TTL.
  // [p.475] Fail-closed: when the auth mutex is unavailable the revocation
  // is REFUSED (a logout that cannot prove revocation must not claim it);
  // the failure is logged + counted, never silently skipped.
  void revokeAllRefreshTokens();

  // [p.475] Lock-free atomic read — the ONE accessor working when the auth
  // mutex is unavailable (diagnostics path). Non-zero = at least one
  // refresh-critical-section operation was refused fail-closed since boot.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

  // CSRF
  String getCsrfToken() const { return String(_csrfToken); }
  void rotateCsrfToken();

  // Factory reset (two-step, 60s TTL).
  // [AUDIT 2026-09 ROUND 11 / p.476] ONE-TIME CONFIRMATION INVARIANT. Both
  // steps run under the SAME auth mutex as the refresh rotation, with the
  // WHOLE check → TTL-validate → constant-time-compare → consume chain
  // inside ONE critical section. The old shape ran both steps fully
  // unsynchronized while they ARE cross-task (REST = web task,
  // MQTT = network task): two concurrent confirms could BOTH pass the
  // compare before either cleared the token — a "one-time" authorization
  // confirmed twice, both callers free to enter the destructive wipe — and
  // a prepare racing a confirm could observe a torn token/time pair.
  //   prepareFactoryReset() → "" when the lock is unavailable (fail-closed:
  //       NO token is issued; the handlers treat the empty string as a
  //       refusal — REST 503 / MQTT REJECTED — never as a valid token).
  //   confirmFactoryReset() → false when the lock is unavailable
  //       (fail-closed, conservative: the token is NOT consumed, the reset
  //       is NOT authorized; counted in authLockFailures — a destructive
  //       operation is never guessed, only proven).
  String prepareFactoryReset();
  bool confirmFactoryReset(const String& token);

private:
  char _csrfToken[Core::CSRF_TOKEN_LEN + 1] = {0};
  unsigned long _csrfTokenTime = 0;
  Core::AuthAttempt _attempts[Core::MAX_TRACKED_IPS] = {};
  RefreshToken _refreshTokens[Core::MAX_REFRESH_TOKENS] = {};
  uint8_t _refreshIdx = 0;
  // [p.476] Guarded by _authMutex (written by prepareFactoryReset, read +
  // cleared by confirmFactoryReset — cross-task: web/REST vs network/MQTT).
  char _factoryResetToken[33] = {0};
  unsigned long _factoryResetTokenTime = 0;
  bool _authReady = false;          // [P0-003] fail-closed readiness

  // [AUDIT 2026-09 ROUND 10 / p.475] FAIL-CLOSED AUTH MUTEX. The old
  // _lockAuth() created the mutex lazily and then only took it
  // `if (_authMutex)` — a failed creation let consumeRefreshToken() run its
  // whole find→validate→mark-used→rotate→persist critical section WITHOUT
  // serialization, reviving exactly the A→B AND A→C refresh replay race
  // AUTH-GATE-05 was built to close. Now:
  //   - begin() treats creation failure as FATAL (boot guard — Serial + Log
  //     FATAL, halt WITHOUT feeding the task watchdog → TWDT panic reset,
  //     honest crash-chain); auth never runs unsynchronized.
  //   - _lockAuth() retries creation ONCE; still null → atomic count +
  //     rate-limited CRIT log via LogService (different mutex, no cycle) +
  //     return false — every caller REFUSES the operation:
  //       consumeRefreshToken() → false (no rotation without serialization)
  //       issueRefreshToken()  → "" (login degrades honestly — handler 500s)
  //       verifyRefreshToken() → false (read-only check refuses)
  //       revokeAllRefreshTokens() → refused + logged (no false "revoked")
  //       prepareFactoryReset() → "" (no one-time token without serialization)
  //       confirmFactoryReset() → false (token NOT consumed, reset NOT
  //                                authorized — destructive ops are proven,
  //                                never guessed)
  //   - issueRefreshToken() NOW ALSO TAKES THE LOCK: it mutates the same
  //     _refreshTokens[] slots + persists the same NVS blob that
  //     consumeRefreshToken() rotates — leaving it unlocked would make the
  //     "atomic critical section" claim only partial (login vs refresh
  //     could interleave slot allocation with rotation).
  //   - [ROUND 11 / p.476] prepareFactoryReset()/confirmFactoryReset() share
  //     this SAME mutex: the factory-reset token + timestamp are shared
  //     mutable state across the web task (REST) and the network task
  //     (MQTT). prepare → "" and confirm → false when unavailable; the
  //     one-time check→compare→consume chain is ONE critical section.
  void* _authMutex = nullptr;       // SemaphoreHandle_t (AUTH-GATE-05)
  bool _lockAuth();
  void _unlockAuth();
  std::atomic<uint32_t> _lockFailures{0};
  std::atomic<uint32_t> _lastLockFailLogMs{0};   // rate-limit bookkeeping

  int _findIpSlot(uint32_t ip);      // [P0-004]
  int _allocIpSlot(uint32_t ip);     // [P0-004] LRU eviction, memory-bounded
  static String _ipToString(uint32_t ip);

  int _findRefreshSlot();
  bool verifyJwtOrRefresh(const String& authHeader);

  // [audit-2 K-4] Persist refresh tokens across reboot (header contract
  // "NVS LRU 4" was previously a lie — no persistence existed).
  // [AUTH-GATE-06] The blob now REALLY carries a CRC32 over the packed
  // slots + a magic/version header; load rejects a CRC mismatch.
  void _persistRefreshTokens();
  void _loadRefreshTokens();
  // [p.475] _lockAuth()/_unlockAuth() are declared with the fail-closed
  // block above (bool _lockAuth() — the old void pair was replaced).
};

extern AuthManager auth;

} // namespace Services

#endif // PLTS_SERVICES_AUTH_MANAGER_H
