// =============================================================================
// AI/GasAdvisor.h — HMAC-authenticated hourly POST + GET to Google Apps Script
// -----------------------------------------------------------------------------
// Brief §49, §94-95 — the ESP32 PROXIES AI insights for the PWA:
//
//   PWA ──► /api/insights ──► ESP32 ──HMAC──► GAS /insights ──► Gemini
//                                                          │
//                                                          ▼
//   PWA ◄── JSON insights ◄── ESP32 ◄──HMAC─── GAS ◄── Gemini
//
// NEVER does the PWA call GAS directly. The ESP32 signs every request with
// HMAC-SHA256 using the per-device gasSecret (stored in NVS, never in code).
//
// This module:
//   1. tick() — called from the AI/Automation task every second.
//      On the hourly interval (GAS_POST_INTERVAL_MS = 3600000ms), it:
//        a. Serializes the latest SystemStatus snapshot (NaN-safe) — this
//           string is the DATA PAYLOAD (dataJson).
//        b. Signs the WAVE-1 canonical string (byte-identical to Code.gs
//           verifyHmac_ — contract v2.1, fixes GAS-2-B/C):
//             'HMAC-SHA256' \n 'TELEMETRY' \n timestamp \n nonce \n
//             deviceId \n sha256hex(dataJson)
//        c. POSTs the envelope with credentials in the BODY (GAS cannot read
//           HTTP headers):
//             { action:'TELEMETRY',
//               auth:{method,timestamp,nonce,deviceId,signature},
//               data:'<dataJson — raw string, the exact signed bytes>' }
//           `data` rides as a RAW JSON STRING so Code.gs hashes the exact
//           bytes we signed — no cross-runtime re-serialization drift.
//        d. On 200: increments postCount, clears backoff.
//        e. On non-200 or network error: exponential backoff (1m → 30m).
//
//   2. fetchInsights() — called from the REST /api/insights handler when the
//      PWA requests the latest insights. Sends the same HMAC scheme with
//      credentials as QUERY PARAMETERS (GET has no body; headers are
//      unreadable by GAS). HONEST LIMITATION: Code.gs v2 has NO INSIGHTS
//      endpoint yet — doGet() will answer 400 until the server side exists.
//      This client is contract-ready, not claim-ready.
//      Rate-limited to 1 fetch / 30 s.
//
// Disciplines (canonical contract §3.12 + WAVE-1 v2.1 — GasAdvisor):
//   - HMAC: action-bound canonical
//     ('HMAC-SHA256' \n action \n ts \n nonce \n deviceId \n sha256hex(dataJson))
//     — action is signed (no action-confusion replay); dataJson is hashed as
//     the exact raw string (no re-serialization drift)
//   - Nonce: 16-byte CSPRNG hex (different from MQTT nonce space)
//   - Timestamp: ±300s window (5 min skew tolerance)
//   - Body: NaN-safe JSON (null for invalid measurements); `data` travels
//     as a raw JSON STRING inside the envelope
//   - Timeout: GAS_TIMEOUT_MS (30s)
//   - Fail-closed: empty gasSecret → advisor refuses to send (logs error)
//   - Advisory only: PWA displays insights with "ADVISORY ONLY" badge
//
// Unimplemented behaviors (deferred):
//   - Encrypted body (XOR with gasSecret-derived stream cipher) — Phase 13-K
//   - Local model fallback when GAS unreachable — Phase 13-L
// =============================================================================
#pragma once
#ifndef PLTS_AI_GAS_ADVISOR_H
#define PLTS_AI_GAS_ADVISOR_H

#include <Arduino.h>
#include <WiFiClientSecure.h>   // [WAVE-4 / GAS-2-D] _configureTls signature

namespace AI {

class GasAdvisor {
public:
  // begin() — call once at boot from setup(). Reads gasSecret from NVS
  // (populated by ConfigStore::loadDeviceConfig). Idempotent.
  void begin();

  // tick() — call every second from the AI/Automation task.
  // Returns immediately if not yet time to POST.
  void tick();

  // fetchInsights() — call from the REST /api/insights handler.
  // Returns the GAS response body as a String (may be JSON or empty on error).
  // Updates lastError_ on failure.
  String fetchInsights();

  // Stats for diagnostics
  uint32_t getPostCount() const { return _postCount; }
  uint32_t getFailureCount() const { return _failureCount; }
  uint32_t getLastPostAt() const { return _lastPostAt; }
  uint16_t getCurrentBackoffMin() const { return _currentBackoffMin; }
  String   getLastError() const { return _lastError; }
  bool     isEnabled() const { return _enabled; }

private:
  bool     _enabled = false;          // false if gasSecret empty (fail-closed)
  uint32_t _postCount = 0;
  uint32_t _failureCount = 0;
  uint32_t _lastPostAt = 0;            // unix-sec
  uint32_t _lastFetchAt = 0;           // unix-sec (rate-limit fetches)
  uint16_t _currentBackoffMin = 0;     // exponential backoff
  String   _lastError;

  // [WAVE-4 / GAS-2-D] TLS trust for GAS (script.google.com). Precedence:
  //   1. -DGAS_ROOT_CA="<pem>"  — operator-pinned anchor (rotation override)
  //   2. built-in GTS Root R4 (GasRootCa.h, public PKI, valid to 2036)
  //   3. DEVELOPMENT_BUILD may explicitly bypass (local mock GAS over
  //      self-signed TLS — the bypass is compile-time visible, NEVER a
  //      silent runtime fallback; production/staging never compile it)
  // Before Wave 4 both TLS sites called setInsecure() unconditionally — the
  // telemetry + HMAC-credential channel was MITM-exposed.
  void _configureTls(WiFiClientSecure& client);

  // Build the HMAC-SHA256 canonical request signature (WAVE-1 contract v2.1
  // — byte-identical to Code.gs verifyHmac_):
  //   canonical = 'HMAC-SHA256' + '\n' + action + '\n' + timestamp + '\n' +
  //               nonce + '\n' + deviceId + '\n' + sha256hex(dataJson)
  // dataJson = the RAW data payload string ('' when the action has no data).
  // Returns hex signature (64 chars, lowercase).
  String _signRequest(const char* action, uint32_t timestamp,
                       const char* nonce, const char* deviceId,
                       const String& dataJson);

  // Send one HMAC-authenticated TELEMETRY POST to GAS. dataJson is the raw
  // telemetry payload string; it is signed and transported verbatim as the
  // envelope's `data` field. Returns HTTP status code; 0 on network error.
  int _sendPost(const String& dataJson);

  // Generate a 16-byte hex nonce using esp_random().
  String _generateNonce();
};

extern GasAdvisor advisor;

} // namespace AI

#endif // PLTS_AI_GAS_ADVISOR_H
