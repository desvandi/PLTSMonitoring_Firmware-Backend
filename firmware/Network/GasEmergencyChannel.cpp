// =============================================================================
// Network/GasEmergencyChannel.cpp — E-WAVE GAS command/event channel (HMAC)
// -----------------------------------------------------------------------------
// See GasEmergencyChannel.h for the contract. TLS + envelope + canonical
// signing mirror AI::GasAdvisor (WAVE-1, byte-identical with Code.gs
// verifyHmac_): 'HMAC-SHA256' \n action \n timestamp \n nonce \n deviceId \n
// sha256hex(dataJson).
// =============================================================================
#include "GasEmergencyChannel.h"

#if PLTS_ENABLE_EMERGENCY

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include "../AI/GasRootCa.h"
#include "../Core/Globals.h"
#include "../Services/EmergencySupervisor.h"
#include "../Services/LogService.h"
#include "../Services/TimeManager.h"   // Services::timeManager (HMAC timestamp)
#include "../Utils/Crypto.h"

// Build-flag URL (empty = channel disabled — fail-closed). Same macro and
// endpoint as AI::GasAdvisor: one GAS deployment serves both actions.
#ifndef GAS_INGEST_URL
#define GAS_INGEST_URL ""
#endif

namespace Network {

GasEmergencyChannel gasEmergency;

// ---------------------------------------------------------------------------
void GasEmergencyChannel::begin() {
  String url(GAS_INGEST_URL);
  if (url.length() == 0) {
    _enabled = false;
    Services::Log.append(Core::LogType::Info,
        "GAS emergency channel DISABLED: GAS_INGEST_URL empty (fail-closed)");
    Serial.println("[EMG-GAS] disabled: no GAS_INGEST_URL");
    return;
  }
  if (strlen(Core::gasSecret) == 0) {
    _enabled = false;
    Services::Log.append(Core::LogType::Info,
        "GAS emergency channel DISABLED: device secret empty (fail-closed)");
    Serial.println("[EMG-GAS] disabled: no device secret");
    return;
  }
  if (strlen(Core::deviceId) == 0) {
    _enabled = false;
    Services::Log.append(Core::LogType::Info,
        "GAS emergency channel DISABLED: deviceId unset");
    Serial.println("[EMG-GAS] disabled: no deviceId");
    return;
  }
  _enabled = true;
  Serial.printf("[EMG-GAS] enabled: poll every %us, HMAC deviceId=%s\n",
                (unsigned)(EMERGENCY_POLL_INTERVAL_MS / 1000), Core::deviceId);
}

// ---------------------------------------------------------------------------
void GasEmergencyChannel::tick() {
  if (!_enabled) return;
  if (WiFi.status() != WL_CONNECTED) return;   // safety is local; commands can wait
  // HMAC replay window is +/-300 s on server time — a zero/unsynced clock
  // would sign garbage (and mbedTLS cannot validate notBefore/notAfter).
  // Events stay queued untouched (no retry budget consumed while waiting).
  if (Services::timeManager.getUnixTime() == 0) return;

  uint32_t now = millis();

  // Event flush first — a TRIP must reach the operator faster than the poll.
  _flushEvent();

  // Poll cadence with bounded failure backoff: 15 s -> 30 s -> 60 s (cap).
  uint32_t interval = EMERGENCY_POLL_INTERVAL_MS;
  if (_pollFails >= 4)      interval = 60000;
  else if (_pollFails >= 2) interval = 30000;
  if (now - _lastPollMs >= interval) {
    _lastPollMs = now;
    _pollPending();
  }
}

// ---------------------------------------------------------------------------
void GasEmergencyChannel::_pollPending() {
  // EMERGENCY_PENDING carries no data — the authenticated deviceId in the
  // envelope IS the queue selector (resolveDeviceKey_ -> auth.deviceKey).
  String body;
  int code = _postEnvelope("EMERGENCY_PENDING", "", body);
  if (code != 200) {
    _pollFails++;
    return;
  }
  _pollFails = 0;

  JsonDocument rdoc;
  DeserializationError err = deserializeJson(rdoc, body);
  if (err) {
    Services::Log.append(Core::LogType::Info,
        String("EMG-GAS: pending response not JSON (") + err.c_str() + ")");
    return;
  }
  const char* st = rdoc["status"] | "";
  if (strcmp(st, "SUCCESS") != 0) return;    // envelope must be SUCCESS
  JsonVariantConst data = rdoc["data"];       // null = "No pending emergency command"
  if (data.isNull()) return;

  const char* commandId = data["command_id"] | "";
  const char* command   = data["command"] | "";
  if (commandId[0] == '\0' || command[0] == '\0') return;

  _applyPendingCommand(data);
}

// ---------------------------------------------------------------------------
void GasEmergencyChannel::_applyPendingCommand(JsonVariantConst data) {
  String commandId = data["command_id"].as<String>();
  String command   = data["command"].as<String>();
  JsonVariantConst cfg = data["config"];

  String message;
  String result = Services::emergency.applyCommand(commandId, command, cfg, message);
  _lastCommandMs = millis();

  // ACK regardless of result — REJECTED rows must settle server-side too, or
  // the queue would re-deliver a rejected command forever.
  bool ok = _sendAck(commandId, result, message);
  _lastAckOk = ok ? 1 : 0;
  if (!ok) {
    // The row stays DELIVERED (still servable) — the next poll re-delivers
    // and the re-apply is idempotent, then the ACK retries. Safe by design.
    Services::Log.append(Core::LogType::Info,
        String("EMG-GAS: ACK delivery failed for ") + commandId +
        " — will re-ack on re-delivery");
  }
}

// ---------------------------------------------------------------------------
bool GasEmergencyChannel::_sendAck(const String& commandId, const String& result,
                                   const String& message) {
  JsonDocument d;
  d["command_id"] = commandId;
  d["result"]     = result;          // "APPLIED" | "REJECTED"
  d["message"]    = message;
  d["state"]      = Services::emergency.stateStr();
  String dataJson;
  serializeJson(d, dataJson);

  String body;
  int code = _postEnvelope("EMERGENCY_ACK", dataJson, body);
  return code == 200;
}

// ---------------------------------------------------------------------------
void GasEmergencyChannel::_flushEvent() {
  String type, reason;
  if (!Services::emergency.peekPendingEvent(type, reason)) return;

  uint32_t now = millis();
  // Rate limit (5 s) between event POSTs.
  if (_lastEventSentAtMs != 0 && now - _lastEventSentAtMs < EMERGENCY_EVENT_MIN_INTERVAL_MS) {
    return;
  }
  // Retry budget: after 20 failed attempts the event is dropped (GAS rows
  // are bounded + Telegram alert has its own cooldown; telemetry still
  // carries emg_state every 5 s).
  if (_eventTries >= EMERGENCY_EVENT_MAX_TRIES) {
    Services::emergency.consumePendingEvent();
    _eventTries = 0;
    Services::Log.append(Core::LogType::Info,
        "EMG-GAS: event dropped after 20 failed attempts (telemetry still carries state)");
    return;
  }

  JsonDocument d;
  d["type"]   = type;
  d["reason"] = reason;
  d["detail"] = "";
  d["state"]  = Services::emergency.stateStr();
  String dataJson;
  serializeJson(d, dataJson);

  _eventTries++;
  _lastEventSentAtMs = now;
  String body;
  int code = _postEnvelope("EMERGENCY_EVENT", dataJson, body);
  if (code == 200) {
    Services::emergency.consumePendingEvent();
    _eventTries = 0;
  }
}

// ---------------------------------------------------------------------------
int GasEmergencyChannel::_postEnvelope(const char* action, const String& dataJson,
                                       String& bodyOut) {
  String deviceId(Core::deviceId);
  String nonce = _generateNonce();
  uint32_t ts = Services::timeManager.getUnixTime();
  String signature = _signRequest(action, ts, nonce.c_str(), deviceId.c_str(), dataJson);
  if (signature.length() == 0) return 0;

  JsonDocument envelope;
  envelope["action"] = action;
  JsonObject auth = envelope["auth"].to<JsonObject>();
  auth["method"]    = "HMAC-SHA256";
  auth["timestamp"] = ts;
  auth["nonce"]     = nonce;
  auth["deviceId"]  = deviceId;
  auth["signature"] = signature;
  if (dataJson.length() > 0) envelope["data"] = dataJson;
  String body;
  serializeJson(envelope, body);

  if (body.length() == 0 || body.length() > Core::GAS_MAX_BODY_SIZE) {
    return 0;
  }

  WiFiClientSecure client;
  client.setTimeout(EMERGENCY_HTTP_TIMEOUT_MS / 1000);
  // TLS precedence — identical to AI::GasAdvisor (never a silent downgrade):
  // pinned CA macro > explicit insecure dev build > built-in GTS Root R4.
#ifdef GAS_ROOT_CA
  client.setCACert(GAS_ROOT_CA);
#elif defined(DEVELOPMENT_BUILD)
  client.setInsecure();   // development ONLY
#else
  client.setCACert(PLTS::GAS_ROOT_CA_GTS_R4);
#endif

  HTTPClient http;
  if (!http.begin(client, GAS_INGEST_URL)) return 0;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(EMERGENCY_HTTP_TIMEOUT_MS);

  // Feed the WDT around the blocking TLS+POST (this task is WDT-subscribed;
  // a stall here must never reboot the module mid-command).
  esp_task_wdt_reset();
  int code = http.POST((uint8_t*)body.c_str(), body.length());
  esp_task_wdt_reset();
  if (code == 200) bodyOut = http.getString();
  http.end();
  return code;
}

// ---------------------------------------------------------------------------
String GasEmergencyChannel::_signRequest(const char* action, uint32_t timestamp,
                                         const char* nonce, const char* deviceId,
                                         const String& dataJson) const {
  // WAVE-1 canonical string — byte-identical with Code.gs verifyHmac_.
  String dataDigest = Utils::sha256Hex(dataJson);
  if (dataDigest.length() == 0) return "";
  String canonical = String("HMAC-SHA256") + "\n" +
                     String(action) + "\n" +
                     String(timestamp) + "\n" +
                     String(nonce) + "\n" +
                     String(deviceId) + "\n" +
                     dataDigest;
  uint8_t hash[32];
  if (!Utils::hmacSha256((const uint8_t*)Core::gasSecret, strlen(Core::gasSecret),
                         (const uint8_t*)canonical.c_str(), canonical.length(), hash)) {
    return "";
  }
  char hex[65];
  Utils::bytesToHex(hash, 32, hex);
  return String(hex);
}

// ---------------------------------------------------------------------------
String GasEmergencyChannel::_generateNonce() const {
  uint8_t buf[16];
  Utils::generateRandomBytes(buf, 16);
  char hex[33];
  Utils::bytesToHex(buf, 16, hex);
  return String(hex);
}

} // namespace Network

#endif // PLTS_ENABLE_EMERGENCY
