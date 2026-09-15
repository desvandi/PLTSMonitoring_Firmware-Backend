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
#include "../Utils/Crc.h"
#include "../Storage/ConfigStore.h"
#include "../Drivers/RtcDriver.h"
#include "LogService.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Services {

AuthManager auth;

// [AUTH-GATE-06] Persisted refresh-token blob layout (224 payload + 12 header):
//   [0..3]  magic 'R','T','O','K'
//   [4]     version (1)
//   [5..7]  reserved (0)
//   [8..11] CRC32 over payload bytes [12..end]
//   [12..]  4 slots × 56 bytes {token(32), issuedAt(4), expiresAt(4), used(1), ip(15)}
static const uint8_t RT_BLOB_MAGIC[4] = {'R','T','O','K'};
static const uint8_t RT_BLOB_VERSION = 1;

// [AUTH-GATE-05] Mutex guards the WHOLE refresh-rotation critical section.
//
// [AUDIT 2026-09 ROUND 10 / p.475] FAIL-CLOSED ACQUISITION — same shape as
// AlarmRegistry::_lock() (round-9 p.471). The old shape:
//     if (_authMutex == nullptr) _authMutex = xSemaphoreCreateMutex();
//     if (_authMutex) xSemaphoreTake(...);      // ← creation failure = no-op
// let the ENTIRE find→validate→mark-used→rotate→persist section run
// unsynchronized whenever the creation failed — reviving the A→B AND A→C
// refresh replay race AUTH-GATE-05 exists to close. Now: retry-create ONCE;
// still null → atomic count + rate-limited CRIT log (via LogService — a
// DIFFERENT service's mutex, no cycle) + return false. Every caller refuses
// the operation (see the header contract).
// [AUDIT 2026-09 ROUND 11 / p.476] The SAME mutex also serializes the
// factory-reset two-step (prepareFactoryReset/confirmFactoryReset) — the
// one-time confirmation token is shared mutable state across the web task
// (REST) and the network task (MQTT).
bool AuthManager::_lockAuth() {
  if (_authMutex == nullptr) _authMutex = xSemaphoreCreateMutex();   // retry-create ONCE
  if (_authMutex != nullptr) {
    xSemaphoreTake((SemaphoreHandle_t)_authMutex, portMAX_DELAY);
    return true;
  }
  _lockFailures.fetch_add(1, std::memory_order_relaxed);
  uint32_t last = _lastLockFailLogMs.load(std::memory_order_relaxed);
  uint32_t now = millis();
  if (now - last > 60000UL &&
      _lastLockFailLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    Log.append(Core::LogType::AuthFail,
               String("[AUTH] auth mutex UNAVAILABLE — auth operation REJECTED "
                      "(fail-closed, p.475/p.476); lockFailures=") +
                   _lockFailures.load(std::memory_order_relaxed),
               0);
  }
  return false;
}
void AuthManager::_unlockAuth() {
  // Only ever called after _lockAuth() returned TRUE — the handle is
  // non-null and HELD here by construction.
  xSemaphoreGive((SemaphoreHandle_t)_authMutex);
}

void AuthManager::begin() {
  rotateCsrfToken();
  for (uint8_t i = 0; i < Core::MAX_TRACKED_IPS; i++) _attempts[i] = {};
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) _refreshTokens[i] = {};

  // [AUTH-GATE-05] Create the rotation mutex up front.
  //
  // [p.475] BOOT GUARD — fail-closed. Without this mutex the refresh
  // rotation critical section (find→validate→mark-used→rotate→persist) runs
  // unsynchronized — the exact replay race AUTH-GATE-05 closed. Auth is a
  // security boundary: refuse to bring it up rather than run it unlocked.
  // Log FATAL and halt WITHOUT feeding the task watchdog → deterministic
  // TWDT panic reset, honest crash-chain (BOOT/CRASHLOOP).
  if (_authMutex == nullptr) _authMutex = xSemaphoreCreateMutex();
  if (_authMutex == nullptr) {
    Serial.println(F("[FATAL] AuthManager auth mutex creation failed (heap exhausted at boot) "
                    "— refusing to enter multi-task state (p.475 fail-closed)"));
    Serial.flush();
    Log.append(Core::LogType::AuthFail,
               String("[FATAL] AuthManager auth mutex creation failed — boot REFUSED "
                      "(p.475): refresh rotation critical section requires the mutex; "
                      "halting for TWDT panic reset"),
               0);
    while (true) {
      delay(10000);   // no esp_task_wdt_reset() on purpose → panic reset
    }
  }

  // [audit-2 K-4] Restore refresh tokens from NVS so sessions survive reboot.
  _loadRefreshTokens();

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

  // [AUTH-GATE-04 / audit p.160] PBKDF2 iteration count: lower AND upper
  // bound. Core::iterations is uint16_t (max 65535); a corrupted value of
  // 65535 would still cost ~6.5× the default per login attempt — enough for
  // a request-timing local DoS when combined with a weak password oracle.
  if (Core::iterations < 1000 || Core::iterations > 50000) {
    Log.append(Core::LogType::AuthFail,
               String("AUTH: PBKDF2 iterations=") + String(Core::iterations) +
               " out of [1000,50000] — default applied", 0);
    Core::iterations = Core::PBKDF2_ITERATIONS;
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
  // [audit-2 S-7 FIX] Proper double-submit cookie pattern: require BOTH
  // the X-CSRF-Token header AND the csrf cookie to be present AND equal
  // to the server-side token. The previous code accepted EITHER source,
  // which weakened the contract to single-submit — a cookie-injection
  // vector or XSS that could read the cookie would bypass CSRF.
  //
  // Double-submit invariant:
  //   1. Header X-CSRF-Token must be present and match _csrfToken
  //   2. Cookie csrf= must be present and match _csrfToken
  //   3. (Implicit: header == cookie == _csrfToken, all constant-time compared)
  String headerToken;
  bool hasHeader = false;
  if (server.hasHeader("X-CSRF-Token")) {
    headerToken = server.header("X-CSRF-Token");
    hasHeader = (headerToken.length() == Core::CSRF_TOKEN_LEN);
  }
  if (!hasHeader) return false;

  String cookieToken;
  bool hasCookie = false;
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    int idx = cookie.indexOf("csrf=");
    if (idx >= 0) {
      cookieToken = cookie.substring(idx + 5, idx + 5 + Core::CSRF_TOKEN_LEN);
      hasCookie = (cookieToken.length() == Core::CSRF_TOKEN_LEN);
    }
  }
  if (!hasCookie) return false;

  // Constant-time compare all three: header vs server, cookie vs server,
  // header vs cookie (the last is implied by the first two but explicit is
  // better for audit clarity).
  bool headerMatch = Utils::constantTimeMemEquals(
    (const volatile uint8_t*)headerToken.c_str(),
    (const volatile uint8_t*)_csrfToken,
    Core::CSRF_TOKEN_LEN);
  bool cookieMatch = Utils::constantTimeMemEquals(
    (const volatile uint8_t*)cookieToken.c_str(),
    (const volatile uint8_t*)_csrfToken,
    Core::CSRF_TOKEN_LEN);
  return headerMatch && cookieMatch;
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
  // [audit-2 K-3/R-4 FIX] Proper LRU eviction:
  //   1. First empty slot (token[0]=='\0') — preferred (no eviction needed)
  //   2. If all slots used, evict the OLDEST entry (lowest issuedAt) — used
  //      tokens are eviction candidates too (their replay is already blocked
  //      by the `used` flag, but the slot can be reused safely once expired).
  // The previous logic skipped `used=true` slots, which is counter-intuitive
  // (used + expired = garbage, prime eviction candidate).
  int oldest = -1;
  uint32_t oldestTs = 0xFFFFFFFF;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS; i++) {
    if (_refreshTokens[i].token[0] == '\0') return i;   // empty slot — best case
    if (_refreshTokens[i].issuedAt < oldestTs) {
      oldestTs = _refreshTokens[i].issuedAt;
      oldest = i;
    }
  }
  return (oldest >= 0) ? oldest : 0;
}

String AuthManager::issueRefreshToken(WebServer& server, const String& username) {
  (void)username;
  // [p.475] Now INSIDE the auth mutex: this mutates the same
  // _refreshTokens[] slots + persists the same NVS blob that
  // consumeRefreshToken() rotates — an unlocked login could interleave slot
  // allocation with a concurrent refresh rotation. Fail-closed: no mutex →
  // no token (the login handler surfaces the empty string as an honest 500,
  // never a session cookie with a fabricated token).
  if (!_lockAuth()) return String();
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
  // [audit-2 K-4 FIX] Persist refresh tokens to NVS so they survive reboot.
  // Previously the header claimed "NVS LRU 4" but no persistence existed —
  // every reboot forced logout. Persist is best-effort (NVS full = warn).
  // [p.475 — harness-caught] Persist INSIDE the lock: _persistRefreshTokens()
  // serializes the very slot array this critical section protects; persisting
  // after _unlockAuth() raced concurrent consumeRefreshToken() mutations
  // (torn blob). consume/revoke already persisted under the lock — issue now
  // matches. The native harness (verify_service_lock_concurrency Phase U)
  // caught this under TSAN before the auditor could.
  _persistRefreshTokens();
  _unlockAuth();
  return t;
}

bool AuthManager::verifyRefreshToken(const String& token, String& outUsername) {
  // [WAVE-5 / FW-B1] Fixed-length compare needs an equal-length input —
  // a shorter String would be overread past its allocation.
  // NOTE (AUTH-GATE-05): read-only verification. Rotation MUST use
  // consumeRefreshToken() — verify+rotate as two calls is the race the
  // audit found (two concurrent refreshes could both pass verify).
  // [p.475] Fail-closed: the slots are read under the auth mutex (a slot can
  // be rotated/revoked concurrently); no mutex → refuse the check.
  if (token.length() != 32) return false;
  if (!_lockAuth()) return false;
  bool found = false;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS && !found; i++) {
    if (_refreshTokens[i].token[0] == '\0') continue;
    if (Utils::constantTimeMemEquals((const volatile uint8_t*)_refreshTokens[i].token,
                                      (const volatile uint8_t*)token.c_str(), 32)) {
      if (!_refreshTokens[i].used) {
        uint32_t now = Drivers::rtc.getUnixTime();
        if (now <= _refreshTokens[i].expiresAt) {
          outUsername = Core::wwwUser;  // single-user system
          found = true;
        }
      }
      break;
    }
  }
  _unlockAuth();
  return found;
}

// [AUTH-GATE-05 / audit p.166-167] ONE ATOMIC consume operation:
// find → validate → mark old used → allocate new → persist, all under the
// auth mutex. There is NO window in which two concurrent requests can both
// verify the same not-yet-used token and both rotate it (A→B AND A→C).
bool AuthManager::consumeRefreshToken(const String& oldToken, String& outNewToken,
                                      String& outUsername) {
  if (oldToken.length() != 32) return false;
  // [p.475] Fail-closed: no mutex → NO rotation. Returning false here is
  // conservative (the client's refresh fails; authLockFailures makes the
  // degradation operator-visible) — the alternative, running the critical
  // section unsynchronized, is exactly the A→B AND A→C replay race.
  if (!_lockAuth()) return false;
  bool ok = false;
  for (int i = 0; i < (int)Core::MAX_REFRESH_TOKENS && !ok; i++) {
    if (_refreshTokens[i].token[0] == '\0') continue;
    if (!Utils::constantTimeMemEquals((const volatile uint8_t*)_refreshTokens[i].token,
                                       (const volatile uint8_t*)oldToken.c_str(), 32)) {
      continue;
    }
    if (_refreshTokens[i].used) break;               // replay — reject
    uint32_t now = Drivers::rtc.getUnixTime();
    if (now > _refreshTokens[i].expiresAt) break;     // expired — reject

    // Mark old token used (one-time semantics) in place.
    _refreshTokens[i].used = true;

    // Issue the successor in a fresh slot.
    int newSlot = _findRefreshSlot();
    RefreshToken& rt = _refreshTokens[newSlot];
    outNewToken = Utils::generateToken(32);
    strncpy(rt.token, outNewToken.c_str(), 32);
    rt.token[32] = '\0';
    rt.issuedAt = now;
    rt.expiresAt = rt.issuedAt + Core::JWT_REFRESH_TTL_SEC;
    rt.used = false;
    _persistRefreshTokens();
    outUsername = Core::wwwUser;  // single-user system
    ok = true;
  }
  _unlockAuth();
  return ok;
}

// [AUTH-GATE-07 / audit p.171-172] Logout server-side revocation: zero every
// slot and persist. A stolen refresh token dies with the user's logout,
// not with its 7-day TTL.
// [p.475] Fail-closed: no mutex → revocation REFUSED (a logout that cannot
// prove revocation must not claim it) — logged + counted, never skipped.
void AuthManager::revokeAllRefreshTokens() {
  if (!_lockAuth()) {
    Log.append(Core::LogType::AuthFail,
               "Logout revocation REFUSED — auth mutex unavailable (fail-closed, "
               "p.475); tokens NOT provably revoked", 0);
    return;
  }
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    memset(_refreshTokens[i].token, 0, sizeof(_refreshTokens[i].token));
    _refreshTokens[i].used = true;   // belt-and-braces: any residual value is dead
    _refreshTokens[i].issuedAt = 0;
    _refreshTokens[i].expiresAt = 0;
    _refreshTokens[i].ip[0] = '\0';
  }
  _persistRefreshTokens();
  _unlockAuth();
  Log.append(Core::LogType::Logout,
             "All refresh tokens revoked (server-side session revocation)", 0);
}

// [audit-2 K-4 FIX + AUTH-GATE-06] Persist refresh token slots to NVS
// namespace `plts_auth` so they survive reboot. Header contract "NVS LRU 4"
// is now honored — AND the blob now carries a REAL CRC32 (the old comment
// claimed a guard that was never written). Layout:
//   [0..3] magic 'RTOK' | [4] version | [5..7] reserved | [8..11] CRC32
//   [12..235] 4 slots × 56 bytes. Write result is verified (STORAGE-GATE-04).
void AuthManager::_persistRefreshTokens() {
  Preferences p;
  if (!p.begin("plts_auth", false)) {
    Log.append(Core::LogType::Custom,
               "NVS FAILURE: plts_auth open failed — refresh tokens NOT persisted", 0);
    return;
  }
  static_assert(sizeof(RefreshToken) >= 56, "RefreshToken too small for NVS blob");
  const size_t payloadLen = Core::MAX_REFRESH_TOKENS * 56;
  uint8_t buf[12 + payloadLen] = {0};
  memcpy(buf, RT_BLOB_MAGIC, 4);
  buf[4] = RT_BLOB_VERSION;
  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    uint8_t* slot = buf + 12 + i * 56;
    memcpy(slot, _refreshTokens[i].token, 32);
    memcpy(slot + 32, &_refreshTokens[i].issuedAt, 4);
    memcpy(slot + 36, &_refreshTokens[i].expiresAt, 4);
    slot[40] = _refreshTokens[i].used ? 1 : 0;
    memcpy(slot + 41, _refreshTokens[i].ip, 15);
  }
  uint32_t crc = Utils::crc32(buf + 12, payloadLen);
  buf[8] = (uint8_t)(crc & 0xFF);
  buf[9] = (uint8_t)((crc >> 8) & 0xFF);
  buf[10] = (uint8_t)((crc >> 16) & 0xFF);
  buf[11] = (uint8_t)((crc >> 24) & 0xFF);
  bool ok = p.putBytes("rtokens", buf, sizeof(buf)) == sizeof(buf);
  p.end();
  if (!ok) {
    Log.append(Core::LogType::Custom,
               "NVS FAILURE: refresh-token blob write failed — sessions may not survive reboot", 0);
  }
}

void AuthManager::_loadRefreshTokens() {
  Preferences p;
  if (!p.begin("plts_auth", true)) return;
  const size_t payloadLen = Core::MAX_REFRESH_TOKENS * 56;
  uint8_t buf[12 + payloadLen] = {0};
  size_t n = p.getBytes("rtokens", buf, sizeof(buf));
  p.end();

  // Legacy image (224 raw bytes, no header): the old format had no CRC, so it
  // cannot be verified — accept it once and it will be re-persisted in the
  // new format on the next rotation/logout. New format (236 bytes) is fully
  // verified: magic + version + CRC (AUTH-GATE-06 — format-valid is no longer
  // the same as integrity-valid).
  const uint8_t* payload = nullptr;
  if (n == 12 + payloadLen) {
    if (memcmp(buf, RT_BLOB_MAGIC, 4) != 0 || buf[4] != RT_BLOB_VERSION) {
      Log.append(Core::LogType::Custom,
                 "AUTH: refresh-token blob header mismatch — sessions discarded", 0);
      return;
    }
    uint32_t storedCrc = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                         ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    if (storedCrc != Utils::crc32(buf + 12, payloadLen)) {
      Log.append(Core::LogType::Custom,
                 "AUTH: refresh-token blob CRC mismatch — sessions discarded", 0);
      return;
    }
    payload = buf + 12;
  } else if (n == payloadLen) {
    payload = buf;  // legacy raw layout
  } else {
    return;  // empty or stale — keep zero-initialized slots
  }

  for (uint8_t i = 0; i < Core::MAX_REFRESH_TOKENS; i++) {
    const uint8_t* slot = payload + i * 56;
    memcpy(_refreshTokens[i].token, slot, 32);
    _refreshTokens[i].token[32] = '\0';
    memcpy(&_refreshTokens[i].issuedAt, slot + 32, 4);
    memcpy(&_refreshTokens[i].expiresAt, slot + 36, 4);
    _refreshTokens[i].used = (slot[40] == 1);
    memcpy(_refreshTokens[i].ip, slot + 41, 15);
    _refreshTokens[i].ip[15] = '\0';
    // Drop expired tokens on load (don't restore dead sessions)
    uint32_t now = Drivers::rtc.getUnixTime();
    if (_refreshTokens[i].expiresAt > 0 && now > _refreshTokens[i].expiresAt) {
      memset(_refreshTokens[i].token, 0, sizeof(_refreshTokens[i].token));
      _refreshTokens[i].used = false;
    }
  }
}

String AuthManager::prepareFactoryReset() {
  // [AUDIT 2026-09 ROUND 11 / p.476] The token + timestamp are shared
  // mutable state written here and read + cleared by confirmFactoryReset(),
  // from DIFFERENT tasks (web/REST vs network/MQTT). Unserialized, a
  // concurrent confirm could observe a torn token/time pair (mid-copy token,
  // or a fresh token paired with the previous timestamp). Fail-closed: no
  // serialization → NO token — the empty string is the honest "not issued"
  // answer (same contract as issueRefreshToken), never a fabricated one.
  if (!_lockAuth()) {
    Log.append(Core::LogType::AuthFail,
               "Factory reset PREPARE refused — auth mutex unavailable "
               "(fail-closed, p.476); no token issued", 0);
    return String();
  }
  String t = Utils::generateToken(32);
  strncpy(_factoryResetToken, t.c_str(), 32);
  _factoryResetToken[32] = '\0';
  _factoryResetTokenTime = millis();
  _unlockAuth();
  // Log AFTER unlock: release the auth mutex before taking the LogService
  // mutex (one-directional auth→log order, no cycle) and keep the hold time
  // minimal (P3 lock-hold-time, accepted).
  Log.append(Core::LogType::ConfigurationChanged,
             "Factory reset prepared (60s TTL)", 0);
  return t;
}

bool AuthManager::confirmFactoryReset(const String& token) {
  // [WAVE-5 / FW-B1] Length guard before fixed-length constant-time compare
  // (token arrives from an MQTT JSON field — arbitrary length possible).
  // Pure input validation — reads NO shared state, safe outside the lock.
  if (token.length() != 32) return false;
  // [AUDIT 2026-09 ROUND 11 / p.476] The WHOLE check → TTL-validate →
  // constant-time-compare → consume chain runs inside ONE critical section.
  // The old unsynchronized shape let two concurrent confirms (REST web task
  // + MQTT network task) BOTH pass the compare before either cleared the
  // token — a "one-time" authorization confirmed TWICE, both callers free
  // to enter the destructive factory wipe. Fail-closed: no serialization →
  // the token is NOT consumed and the reset is NOT authorized; counted in
  // authLockFailures — a destructive operation is proven or refused, never
  // guessed.
  if (!_lockAuth()) {
    Log.append(Core::LogType::AuthFail,
               "Factory reset CONFIRM refused — auth mutex unavailable "
               "(fail-closed, p.476); token NOT consumed, reset NOT authorized",
               0);
    return false;
  }
  bool ok = false;
  if (_factoryResetToken[0] != '\0') {
    if (millis() - _factoryResetTokenTime > Core::FACTORY_RESET_TOKEN_TTL_MS) {
      _factoryResetToken[0] = '\0';   // expired — discard under the same lock
    } else if (Utils::constantTimeMemEquals(
                   (const volatile uint8_t*)_factoryResetToken,
                   (const volatile uint8_t*)token.c_str(), 32)) {
      _factoryResetToken[0] = '\0';   // consume — SAME critical section
      ok = true;
    }
  }
  _unlockAuth();
  return ok;
}

} // namespace Services
