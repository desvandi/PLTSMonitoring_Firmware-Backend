// =============================================================================
// AI/GasAdvisor.cpp — HMAC-authenticated hourly POST + GET to Google Apps Script
// =============================================================================
#include "GasAdvisor.h"
#include "GasRootCa.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Utils/Crypto.h"
#include "../Services/LogService.h"
#include "../Services/TimeManager.h"
#include "../Web/BatteryStatusSerializer.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <esp_task_wdt.h>   // [v1.6.3] WDT feed around the blocking GAS POST
#include <cctype>
#include <cstring>
#include <cstdio>

namespace AI {

// ---------------------------------------------------------------------------
// [WAVE-4 / GAS-2-D] TLS trust configuration — see GasAdvisor.h for the
// precedence contract. Mirrors the house convention of MqttTransport/
// OtaManager: pin when an anchor exists, allow an EXPLICIT compile-time dev
// bypass, and NEVER silently downgrade. Fail state: with a pinned CA and a
// server that cannot be validated, the TLS handshake FAILS — honest, closed.
// ---------------------------------------------------------------------------
void GasAdvisor::_configureTls(WiFiClientSecure& client) {
#ifdef GAS_ROOT_CA
  client.setCACert(GAS_ROOT_CA);
#elif defined(DEVELOPMENT_BUILD)
  // EXPLICIT DEV BYPASS — production/staging builds do NOT compile this path
  client.setInsecure();
#else
  client.setCACert(PLTS::GAS_ROOT_CA_GTS_R4);
#endif
}

GasAdvisor advisor;

// Forward declaration (definition at the bottom of this file).
static String urlEncodeComponent(const String& s);

// GAS URL endpoints (set via build flags -DGAS_INGEST_URL and -DGAS_INSIGHTS_URL,
// or empty in development to disable).
#ifndef GAS_INGEST_URL
  #define GAS_INGEST_URL ""
#endif
#ifndef GAS_INSIGHTS_URL
  #define GAS_INSIGHTS_URL ""
#endif

// Backoff schedule (minutes): 1, 5, 15, 30, 30, ...
static const uint16_t BACKOFF_STEPS[] = { 1, 5, 15, 30 };
static const uint8_t BACKOFF_STEPS_COUNT = sizeof(BACKOFF_STEPS) / sizeof(BACKOFF_STEPS[0]);

// ---------------------------------------------------------------------------
// begin() — verify gasSecret is non-empty (fail-closed)
// ---------------------------------------------------------------------------
void GasAdvisor::begin() {
  if (Core::gasSecret[0] == '\0') {
    _enabled = false;
    _lastError = "gasSecret empty — GasAdvisor disabled (fail-closed)";
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "GasAdvisor disabled (no gasSecret)", -1);
    return;
  }
  if (strlen(GAS_INGEST_URL) == 0 || strlen(GAS_INSIGHTS_URL) == 0) {
    _enabled = false;
    _lastError = "GAS URL not configured (define GAS_INGEST_URL + GAS_INSIGHTS_URL)";
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "GasAdvisor disabled (no GAS URL)", -1);
    return;
  }
  _enabled = true;
  _postCount = 0;
  _failureCount = 0;
  _lastPostAt = 0;
  _currentBackoffMin = 0;
  _lastError = "";
  Services::Log.append(Core::LogType::ConfigurationChanged,
                          "GasAdvisor initialized", -1);
}

// ---------------------------------------------------------------------------
// tick() — called every second from AI/Automation task
// ---------------------------------------------------------------------------
void GasAdvisor::tick() {
  if (!_enabled) return;

  uint32_t now = Services::timeManager.getUnixTime();
  if (now == 0) return;  // NTP not synced — wait

  // First post: scheduled at GAS_POST_INTERVAL_MS after boot (not at boot,
  // to allow NTP sync + first telemetry sample).
  uint32_t elapsed = now - _lastPostAt;
  uint32_t requiredSec = Core::GAS_POST_INTERVAL_MS / 1000;

  // Apply exponential backoff if previous post failed
  if (_currentBackoffMin > 0) {
    requiredSec = (uint32_t)_currentBackoffMin * 60;
  }

  // Not yet time
  if (_lastPostAt > 0 && elapsed < requiredSec) return;
  // For first post, wait at least 60s after boot for sensor stabilization
  if (_lastPostAt == 0 && Services::timeManager.getUptimeSec() < 60) return;

  // Build telemetry body
  Core::SystemStatus snap;
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snap = latestStatus;
    xSemaphoreGive(telemetryMutex);
  } else {
    return;  // try again next tick
  }

  // Build telemetry DATA PAYLOAD — the raw string that will be signed and
  // transported verbatim as the envelope's `data` field (WAVE-1 contract).
  // Snap must be NaN-safe (serializer handles null/NaN → null in JSON).
  String dataJson = Web::serialize(snap);
  if (dataJson.length() == 0 || dataJson.length() > Core::GAS_MAX_BODY_SIZE) {
    _lastError = "telemetry body empty or oversized";
    return;
  }

  // Rate limit: GAS_MAX_POSTS_PER_HOUR
  if (_postCount >= Core::GAS_MAX_POSTS_PER_HOUR) {
    // Check if any posts in the last hour exceeded the limit
    // (Simple approximation: reset every hour.)
    static uint32_t lastHourReset = 0;
    if (now - lastHourReset >= 3600) {
      _postCount = 0;
      lastHourReset = now;
    } else {
      _lastError = "rate limit: max posts per hour reached";
      return;
    }
  }

  int status = _sendPost(dataJson);
  _lastPostAt = now;

  if (status == 200) {
    _postCount++;
    _failureCount = 0;
    _currentBackoffMin = 0;
    _lastError = "";
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "GAS ingest POST ok", -1);
  } else {
    _failureCount++;
    // Exponential backoff
    uint8_t idx = (_failureCount - 1) < BACKOFF_STEPS_COUNT
                    ? (_failureCount - 1)
                    : (BACKOFF_STEPS_COUNT - 1);
    _currentBackoffMin = BACKOFF_STEPS[idx];
    _lastError = "GAS POST failed status=" + String(status) +
                 " backoff=" + String(_currentBackoffMin) + "m";
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "GAS ingest POST failed: " + _lastError, -1);
  }
}

// ---------------------------------------------------------------------------
// fetchInsights() — called from REST /api/insights handler
// ---------------------------------------------------------------------------
String GasAdvisor::fetchInsights() {
  if (!_enabled) {
    return "{\"success\":false,\"error\":\"GasAdvisor disabled\","
           "\"message\":\"gasSecret or GAS URL not configured\"}";
  }

  uint32_t now = Services::timeManager.getUnixTime();
  if (now == 0) {
    return "{\"success\":false,\"error\":\"NTP not synced\"}";
  }

  // Rate-limit: max 1 fetch per 30s (PWA polls every 5min, so this is generous)
  if (now - _lastFetchAt < 30) {
    return "{\"success\":false,\"error\":\"rate limit\","
           "\"message\":\"fetch Insights too frequently\"}";
  }
  _lastFetchAt = now;

  // Build HMAC GET request. [WAVE-1 / GAS-2-B] GAS cannot read HTTP headers,
  // and GET has no body — credentials ride as QUERY PARAMETERS. Canonical
  // uses the same scheme with an empty data payload (sha256hex("")).
  // HONEST LIMITATION: Code.gs v2 has NO INSIGHTS endpoint yet — doGet()
  // answers 400 "Please use POST for API calls" until the server side
  // exists. This client is contract-ready, not claim-ready.
  String deviceId(Core::deviceId);
  String nonce = _generateNonce();
  uint32_t ts = now;
  String signature = _signRequest("INSIGHTS", ts, nonce.c_str(),
                                    deviceId.c_str(), String(""));

  String url = String(GAS_INSIGHTS_URL);
  url += (url.indexOf('?') >= 0) ? '&' : '?';
  url += "action=INSIGHTS";
  url += "&auth_method=HMAC-SHA256";
  url += "&auth_timestamp=" + String(ts);
  url += "&auth_nonce=" + urlEncodeComponent(nonce);
  url += "&auth_device_id=" + urlEncodeComponent(deviceId);
  url += "&auth_signature=" + urlEncodeComponent(signature);

  WiFiClientSecure client;
  client.setTimeout(Core::GAS_TIMEOUT_MS / 1000);
  _configureTls(client);   // [WAVE-4 / GAS-2-D] was setInsecure() — MITM hole

  HTTPClient http;
  if (!http.begin(client, url)) {
    _lastError = "http.begin failed";
    return "{\"success\":false,\"error\":\"http begin failed\"}";
  }
  http.addHeader("Accept", "application/json");
  http.setTimeout(Core::GAS_TIMEOUT_MS);

  int status = http.GET();
  String response;
  if (status > 0) {
    response = http.getString();
  } else {
    _lastError = "GET failed: " + String(status);
    response = "{\"success\":false,\"error\":\"network error: "
               + String(status) + "\"}";
  }
  http.end();
  return response;
}

// ---------------------------------------------------------------------------
// _sendPost — one HMAC-authenticated TELEMETRY POST to GAS
// [WAVE-1 / GAS-2-B+C] Envelope (contract v2.1, synchronized with Code.gs):
//   { "action":"TELEMETRY",
//     "auth":{"method":"HMAC-SHA256","timestamp":T,"nonce":N,
//              "deviceId":D,"signature":S},
//     "data":"<dataJson — RAW STRING, the exact bytes covered by S>" }
// `data` rides as a raw JSON STRING so Code.gs hashes byte-identical input
// (no ArduinoJson↔V8 re-serialization drift). Credentials are in the BODY
// because GAS cannot read HTTP headers (the old X-Auth-* headers were dead
// weight — GAS-2-B lapis 2).
// ---------------------------------------------------------------------------
int GasAdvisor::_sendPost(const String& dataJson) {
  String deviceId(Core::deviceId);
  String nonce = _generateNonce();
  uint32_t ts = Services::timeManager.getUnixTime();
  String signature = _signRequest("TELEMETRY", ts, nonce.c_str(),
                                    deviceId.c_str(), dataJson);
  if (signature.length() == 0) {
    _lastError = "HMAC signing failed";
    return 0;
  }

  // Envelope with `data` embedded as an escaped JSON string value.
  // ArduinoJson v7 (JsonDocument) escapes the string on serialize — the
  // receiver's JSON.parse() recovers the exact original bytes.
  JsonDocument envelope;
  envelope["action"] = "TELEMETRY";
  JsonObject auth = envelope["auth"].to<JsonObject>();
  auth["method"]    = "HMAC-SHA256";
  auth["timestamp"] = ts;
  auth["nonce"]     = nonce;
  auth["deviceId"]  = deviceId;
  auth["signature"] = signature;
  envelope["data"]  = dataJson;
  String body;
  serializeJson(envelope, body);

  // Escaping can inflate the payload — check the FINAL wire size.
  if (body.length() == 0 || body.length() > Core::GAS_MAX_BODY_SIZE) {
    _lastError = "HMAC envelope empty or oversized";
    return 0;
  }

  WiFiClientSecure client;
  client.setTimeout(Core::GAS_TIMEOUT_MS / 1000);
  _configureTls(client);   // [WAVE-4 / GAS-2-D] was setInsecure() — MITM hole

  HTTPClient http;
  if (!http.begin(client, GAS_INGEST_URL)) return 0;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(Core::GAS_TIMEOUT_MS);

  // [v1.6.3 / audit-noise] The TLS handshake + POST can block for seconds
  // on a noisy WiFi rail. This task is WDT-subscribed — an unlucky stall
  // here rebooted the module mid-cycle. Feed the watchdog around the
  // blocking call (defense identical to the OTA download loop).
  esp_task_wdt_reset();
  int status = http.POST((uint8_t*)body.c_str(), body.length());
  esp_task_wdt_reset();
  http.end();
  return status;
}

// ---------------------------------------------------------------------------
// _signRequest — HMAC-SHA256 over the WAVE-1 canonical string (contract v2.1
// — MUST stay byte-identical with Code.gs verifyHmac_):
//   'HMAC-SHA256' + '\n' + action + '\n' + timestamp + '\n' + nonce + '\n'
//   + deviceId + '\n' + sha256hex(dataJson)
// dataJson = the RAW data payload string ('' when the action has no data).
// Returns 64-char lowercase hex.
// ---------------------------------------------------------------------------
String GasAdvisor::_signRequest(const char* action, uint32_t timestamp,
                                  const char* nonce, const char* deviceId,
                                  const String& dataJson) {
  String dataDigest = Utils::sha256Hex(dataJson);
  if (dataDigest.length() == 0) return "";  // hash failure

  String canonical = String("HMAC-SHA256") + "\n" +
                     String(action) + "\n" +
                     String(timestamp) + "\n" +
                     String(nonce) + "\n" +
                     String(deviceId) + "\n" +
                     dataDigest;
  uint8_t hash[32];
  if (!Utils::hmacSha256((const uint8_t*)Core::gasSecret,
                          strlen(Core::gasSecret),
                          (const uint8_t*)canonical.c_str(),
                          canonical.length(),
                          hash)) {
    return "";  // error
  }
  char hex[65];
  Utils::bytesToHex(hash, 32, hex);
  return String(hex);
}

// ---------------------------------------------------------------------------
// urlEncodeComponent — percent-encode everything outside RFC 3986 unreserved
// (used for HMAC credentials in query strings on the GET path).
// ---------------------------------------------------------------------------
static String urlEncodeComponent(const String& s) {
  // Nama sengaja HEX_CHARS, bukan HEX — Arduino Print.h mendefinisikan
  // makro `#define HEX 16` yang akan merewrite identifier `HEX` apa pun
  // (ditemukan saat build staging nyata 2026-08-28 — Gel-3).
  static const char* HEX_CHARS = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += HEX_CHARS[(c >> 4) & 0x0F];
      out += HEX_CHARS[c & 0x0F];
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// _generateNonce — 16-byte CSPRNG hex (32 chars)
// ---------------------------------------------------------------------------
String GasAdvisor::_generateNonce() {
  uint8_t buf[16];
  Utils::generateRandomBytes(buf, 16);
  char hex[33];
  Utils::bytesToHex(buf, 16, hex);
  return String(hex);
}

} // namespace AI
