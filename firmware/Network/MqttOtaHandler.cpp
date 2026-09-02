// =============================================================================
// Network/MqttOtaHandler.cpp — MQTT OTA command receiver
// =============================================================================
#include "MqttOtaHandler.h"
#include "MqttTransport.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Services/OtaManager.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>
#include <cstring>

namespace Network {

MqttOtaHandler mqttOtaHandler;

void MqttOtaHandler::begin() {
  _received = 0;
  _started = 0;
  _rejected = 0;
  _duplicates = 0;
  Services::Log.append(Core::LogType::OtaStarted,
                        "MqttOtaHandler initialized", -1);
}

void MqttOtaHandler::handle(const char* topic, const uint8_t* payload, size_t len) {
  (void)topic;  // topic is always plts/<deviceId>/ota
  _received++;

  // --- 1. Deserialize -----------------------------------------------------------
  if (len == 0 || len > Core::HTTP_MAX_BODY_SIZE) {
    _publishAck("", false, "BAD_SCHEMA", "empty or oversized payload");
    _rejected++;
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err != DeserializationError::Ok) {
    _publishAck("", false, "BAD_SCHEMA",
                String("JSON parse error: ") + err.c_str());
    _rejected++;
    return;
  }

  const char* typeC = doc["type"] | "";
  const char* actionC = doc["action"] | "";
  const char* tidC = doc["transactionId"] | "";
  int protocolVer = doc["version"] | 0;

  String type(typeC);
  String action(actionC);
  String tid(tidC);
  type.toLowerCase();
  action.toLowerCase();

  if (type != "ota" || (action != "start" && action != "check")) {
    _publishAck(tid.c_str(), false, "REJECTED",
                "expected ota.start or ota.check, got: " + type + "." + action);
    _rejected++;
    return;
  }

  // --- 2. Validate transactionId + protocol version ----------------------------
  {
    String errOut;
    if (!Services::CommandCanonicalizer::validateTransactionId(tid, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
    if (!Services::CommandCanonicalizer::validateProtocolVersion(protocolVer, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
  }

  // --- 3. Field whitelist -------------------------------------------------------
  {
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
      if (!Services::CommandCanonicalizer::isFieldAllowed(type, kv.key().c_str())) {
        _publishAck(tid.c_str(), false, "REJECTED",
                    "unknown field: " + String(kv.key().c_str()));
        _rejected++;
        return;
      }
    }
  }

  // --- 4. Canonicalize + hash ---------------------------------------------------
  doc["type"] = type;
  doc["action"] = action;
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) {
    _publishAck(tid.c_str(), false, "BAD_SCHEMA", canon.errorMessage);
    _rejected++;
    return;
  }

  // --- 5. Decide NEW / DUPLICATE / CONFLICT ------------------------------------
  String previousAck;
  Services::TransactionDecision decision =
    Services::journal.decide(canon.transactionId, canon.commandHash, previousAck);

  if (decision == Services::TransactionDecision::Duplicate) {
    _duplicates++;
    if (previousAck.length() > 0) {
      String ackTopic = mqttTransport.getDeviceTopic("ack");
      mqttTransport.publish(ackTopic.c_str(), previousAck.c_str(),
                             previousAck.length(), false);
    } else {
      _publishAck(canon.transactionId.c_str(), true, "DUPLICATE",
                  "OTA transaction already processed");
    }
    return;
  }
  if (decision == Services::TransactionDecision::Conflict) {
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "CONFLICT",
                "transactionId reused with different payload");
    return;
  }

  // --- 6. NEW — dispatch to OtaManager -----------------------------------------
  // Accept both "version" (legacy) and "fwVersion" (canonical) for firmware ver.
  const char* fwVersion = doc["fwVersion"] | "";
  if (fwVersion[0] == '\0') fwVersion = doc["version"] | "";  // legacy fallback
  // Note: protocol "version" field is also present (we read it above as int).
  // The canonical field is "fwVersion"; we accept "version" only as a fallback
  // for backwards compatibility with older senders.

  bool ok = false;
  String message;

  if (action == "start") {
    const char* url = doc["url"] | "";
    size_t size = (size_t)(doc["size"] | (uint32_t)0);
    const char* sha256 = doc["sha256"] | "";
    const char* signature = doc["signature"] | "";

    if (url[0] == '\0' || fwVersion[0] == '\0' || sha256[0] == '\0') {
      ok = false;
      message = "missing required fields (url, fwVersion, sha256)";
    } else {
      // Anti-downgrade + URL allowlist + HTTPS + Ed25519 fail-closed
      // are all enforced by OtaManager.beginDownload().
      ok = Services::ota.beginDownload(url, fwVersion, size, sha256, signature);
      if (ok) {
        message = "OTA download started: v=" + String(fwVersion);
        _started++;
      } else {
        message = Services::ota.getLastError();
      }
    }
  } else {  // action == "check"
    // [WAVE-6 / FW6-4] ota.check is IMPLEMENTED. Wave 5 turned the old lying
    // "scheduled" ACK into an honest reject; this wave builds the actual
    // feature: the manifest URL is validated against the SAME allowlist + CA
    // policy as ota.start, the fetch runs in OtaTask (non-blocking here), and
    // a strictly-newer signed manifest hands off to the identical
    // download→SHA-256→Ed25519→Update.end chain. "No update" and every
    // failure land in the log — nothing pretends to be scheduled.
    const char* manifestUrl = doc["url"] | "";
    if (manifestUrl[0] == '\0') {
      ok = false;
      message = "ota.check requires url (JSON manifest with fwVersion, url, sha256[, size, signature])";
    } else {
      ok = Services::ota.beginManifestCheck(manifestUrl);
      message = ok
        ? "OTA check scheduled: " + String(manifestUrl) +
          " (result in device log / ack channel)"
        : Services::ota.getLastError();
      if (ok) _started++;
    }
  }

  // --- 7. Store transaction + publish ACK -------------------------------------
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = canon.transactionId;
    ack["ok"] = ok;
    ack["code"] = ok ? "ACCEPTED" : "REJECTED";
    ack["message"] = message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }

  Services::journal.storeTransaction(canon.transactionId,
                                      canon.commandHash, ackJson);

  {
    String ackTopic = mqttTransport.getDeviceTopic("ack");
    mqttTransport.publish(ackTopic.c_str(), ackJson.c_str(),
                           ackJson.length(), false);
  }

  Services::Log.append(ok ? Core::LogType::OtaStarted : Core::LogType::OtaFailed,
                        "MQTT OTA " + action + ": " + message +
                        " tid=" + canon.transactionId, -1);

  if (!ok) _rejected++;
}

void MqttOtaHandler::_publishAck(const char* transactionId, bool ok,
                                   const char* code, const String& message) {
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = transactionId ? transactionId : "";
    ack["ok"] = ok;
    ack["code"] = code;
    ack["message"] = message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }
  String ackTopic = mqttTransport.getDeviceTopic("ack");
  mqttTransport.publish(ackTopic.c_str(), ackJson.c_str(),
                        ackJson.length(), false);
}

} // namespace Network
