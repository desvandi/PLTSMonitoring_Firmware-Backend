// =============================================================================
// Network/GasOtaReporter.cpp — [PARITY-3 AUDIT 2026-09-06] GAS OTA_STATUS
// bridge. See GasOtaReporter.h for the contract and threading model.
// -----------------------------------------------------------------------------
// TLS + HMAC envelope + canonical signing mirror AI::GasAdvisor and
// Network::GasEmergencyChannel (WAVE-1, byte-identical with Code.gs
// verifyHmac_): 'HMAC-SHA256' \n action \n timestamp \n nonce \n deviceId \n
// sha256hex(dataJson).
// =============================================================================
#include "GasOtaReporter.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include "../AI/GasRootCa.h"
#include "../Core/Globals.h"
#include "../Services/LogService.h"
#include "../Services/TimeManager.h"   // HMAC timestamp (replay window)
#include "../Utils/Crypto.h"

#include <cstring>

// Build-flag URL (empty = channel disabled — fail-closed). Same macro and
// endpoint as AI::GasAdvisor / GasEmergencyChannel: ONE GAS deployment
// serves TELEMETRY, EMERGENCY_*, and now OTA_STATUS.
#ifndef GAS_INGEST_URL
#define GAS_INGEST_URL ""
#endif

namespace Network {

GasOtaReporter gasOtaReporter;

// ---------------------------------------------------------------------------
void GasOtaReporter::begin() {
  String url(GAS_INGEST_URL);
  if (url.length() == 0) {
    _enabled = false;
    if (!_disableLogged) {
      _disableLogged = true;
      Services::Log.append(Core::LogType::Info,
          "GAS OTA reporter DISABLED: GAS_INGEST_URL empty (fail-closed)");
    }
    return;
  }
  if (strlen(Core::gasSecret) == 0) {
    _enabled = false;
    if (!_disableLogged) {
      _disableLogged = true;
      Services::Log.append(Core::LogType::Info,
          "GAS OTA reporter DISABLED: device secret empty (fail-closed)");
    }
    return;
  }
  if (strlen(Core::deviceId) == 0) {
    _enabled = false;
    if (!_disableLogged) {
      _disableLogged = true;
      Services::Log.append(Core::LogType::Info,
          "GAS OTA reporter DISABLED: deviceId unset");
    }
    return;
  }
  _enabled = true;
}

// ---------------------------------------------------------------------------
void GasOtaReporter::report(const char* state, const char* version,
                            const char* detail) {
  if (!_enabled || !state || !state[0]) return;

  Event ev;
  memset(&ev, 0, sizeof(ev));
  strncpy(ev.state,   state,   sizeof(ev.state)   - 1);
  strncpy(ev.version, version ? version : "", sizeof(ev.version) - 1);
  strncpy(ev.detail,  detail  ? detail  : "", sizeof(ev.detail)  - 1);

  bool droppedOldest = false;
  portENTER_CRITICAL(&_mux);
  if (_count < RING_SIZE) {
    _ring[_head] = ev;
    _head = (_head + 1) % RING_SIZE;
    _count++;
  } else {
    // Ring full: overwrite the OLDEST slot (read index = _head when full).
    // Terminal states arrive last — keeping the newest preserves the final
    // outcome; the dropped one is named in the device log (honest loss).
    uint8_t oldest = _head;                    // _count == RING_SIZE
    _ring[oldest] = ev;
    _head = (_head + 1) % RING_SIZE;
    droppedOldest = true;
    _attempts = 0;                             // fresh event, fresh budget
  }
  portEXIT_CRITICAL(&_mux);

  if (droppedOldest) {
    Services::Log.append(Core::LogType::OtaFailed,
      String("GAS OTA ring full — dropped oldest, kept state=") + ev.state, 0);
  }
}

// ---------------------------------------------------------------------------
uint8_t GasOtaReporter::pendingCount() const {
  portENTER_CRITICAL(&_mux);
  uint8_t n = _count;
  portEXIT_CRITICAL(&_mux);
  return n;
}

// ---------------------------------------------------------------------------
void GasOtaReporter::tick() {
  if (!_enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;   // event stays queued
  // HMAC replay window is ±300 s on server time — an unsynced clock signs
  // garbage (same gate as GasEmergencyChannel::tick).
  if (Services::timeManager.getUnixTime() == 0) return;

  uint32_t now = millis();

  // Read head event presence WITHOUT popping (peek).
  portENTER_CRITICAL(&_mux);
  bool hasPending = (_count > 0);
  portEXIT_CRITICAL(&_mux);
  if (!hasPending) {
    _flushFails = 0;   // idle — reset cadence backoff
    return;
  }

  // Cadence with bounded backoff: 5 s -> 15 s -> 45 s (cap).
  uint32_t interval = MIN_FLUSH_INTERVAL_MS;
  if (_flushFails >= 4)      interval = 45000;
  else if (_flushFails >= 2) interval = 15000;
  if (now - _lastFlushMs < interval) return;

  _lastFlushMs = now;
  flushOne();
}

// ---------------------------------------------------------------------------
bool GasOtaReporter::flushOne() {
  // Peek the head event under the lock, then work on a local copy — the
  // TLS POST (up to 7 s) must never hold a critical section.
  Event ev;
  portENTER_CRITICAL(&_mux);
  if (_count == 0) { portEXIT_CRITICAL(&_mux); return true; }
  uint8_t readIdx = (_head + RING_SIZE - _count) % RING_SIZE;
  ev = _ring[readIdx];
  portEXIT_CRITICAL(&_mux);

  _attempts++;

  // --- Build the signed OTA_STATUS envelope (contract v2.1) ---
  // data rides as a RAW JSON STRING so Code.gs hashes byte-identical input.
  JsonDocument dataDoc;
  dataDoc["event"]   = ev.state;
  dataDoc["version"] = ev.version;
  dataDoc["message"] = ev.detail;
  String dataJson;
  serializeJson(dataDoc, dataJson);

  String deviceId(Core::deviceId);
  uint8_t nonceBuf[16];
  Utils::generateRandomBytes(nonceBuf, 16);
  char nonceHex[33];
  Utils::bytesToHex(nonceBuf, 16, nonceHex);
  uint32_t ts = Services::timeManager.getUnixTime();

  String dataDigest = Utils::sha256Hex(dataJson);
  if (dataDigest.length() == 0) {
    _flushFails++;
    return false;
  }
  String canonical = String("HMAC-SHA256") + "\n" +
                     String("OTA_STATUS") + "\n" +
                     String(ts) + "\n" +
                     String(nonceHex) + "\n" +
                     deviceId + "\n" +
                     dataDigest;
  uint8_t hash[32];
  if (!Utils::hmacSha256((const uint8_t*)Core::gasSecret, strlen(Core::gasSecret),
                         (const uint8_t*)canonical.c_str(), canonical.length(), hash)) {
    _flushFails++;
    return false;
  }
  char sigHex[65];
  Utils::bytesToHex(hash, 32, sigHex);

  JsonDocument envelope;
  envelope["action"] = "OTA_STATUS";
  JsonObject auth = envelope["auth"].to<JsonObject>();
  auth["method"]    = "HMAC-SHA256";
  auth["timestamp"] = ts;
  auth["nonce"]     = String(nonceHex);
  auth["deviceId"]  = deviceId;
  auth["signature"] = String(sigHex);
  envelope["data"]  = dataJson;
  String body;
  serializeJson(envelope, body);

  if (body.length() == 0 || body.length() > Core::GAS_MAX_BODY_SIZE) {
    _flushFails++;
    return false;
  }

  // --- TLS POST (same precedence chain as GasEmergencyChannel) ---
  WiFiClientSecure client;
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);
#ifdef GAS_ROOT_CA
  client.setCACert(GAS_ROOT_CA);
#elif defined(DEVELOPMENT_BUILD)
  client.setInsecure();   // development ONLY
#else
  client.setCACert(PLTS::GAS_ROOT_CA_GTS_R4);
#endif

  HTTPClient http;
  if (!http.begin(client, String(GAS_INGEST_URL))) {
    _flushFails++;
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);

  // Feed the WDT around the blocking TLS+POST (otaTask is WDT-subscribed).
  esp_task_wdt_reset();
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  esp_task_wdt_reset();
  String response;
  if (code > 0) response = http.getString();
  http.end();

  if (code != 200) {
    // Transport failure — retryable. Consume the attempt budget only.
    _flushFails++;
    if (_attempts >= MAX_ATTEMPTS_PER_EVENT) {
      // Honest drop: log exactly what was lost (same guarantee MQTT keeps).
      portENTER_CRITICAL(&_mux);
      if (_count > 0) {
        uint8_t dropIdx = (_head + RING_SIZE - _count) % RING_SIZE;
        _ring[dropIdx] = Event{};   // clear the slot
        _count--;
        if (_count == 0) _head = 0;
      }
      _attempts = 0;
      portEXIT_CRITICAL(&_mux);
      Services::Log.append(Core::LogType::OtaFailed,
        String("GAS OTA_STATUS unsent after ") + String(MAX_ATTEMPTS_PER_EVENT) +
        " tries: " + ev.state + " v" + ev.version, 0);
    }
    return false;
  }

  // HTTP 200 — GAS always answers 200 with the verdict INSIDE the body.
  JsonDocument respDoc;
  DeserializationError err = deserializeJson(respDoc, response);
  String status = (err == DeserializationError::Ok)
                  ? String((const char*)(respDoc["status"] | "")) : String("");
  const bool accepted = (status == "SUCCESS");

  // Consume the event in BOTH cases: an HTTP-200 verdict (SUCCESS or a
  // permanent server-side rejection — e.g. unregistered device / vocab
  // mismatch) will not change on retry. Re-queueing a rejected event would
  // spin the cadence for nothing.
  portENTER_CRITICAL(&_mux);
  if (_count > 0) {
    uint8_t dropIdx = (_head + RING_SIZE - _count) % RING_SIZE;
    _ring[dropIdx] = Event{};
    _count--;
    if (_count == 0) _head = 0;
  }
  _attempts = 0;
  portEXIT_CRITICAL(&_mux);

  if (!accepted) {
    // Permanent GAS-side rejection — log the reason verbatim (honest).
    String reason = (err == DeserializationError::Ok)
      ? String((const char*)(respDoc["message"] | "unknown GAS error"))
      : String("invalid GAS response body");
    Services::Log.append(Core::LogType::OtaFailed,
      String("GAS OTA_STATUS rejected: ") + ev.state + " — " + reason, 0);
    _flushFails = 0;   // server answered — cadence back to base
    return true;
  }

  _flushFails = 0;
  return true;
}

} // namespace Network
