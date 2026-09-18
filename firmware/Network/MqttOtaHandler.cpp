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
  _received++;

  // --- 0. [GATE-3 / S1-02 REMEDIATION 2026-09] EXACT DEVICE-TOPIC BINDING ----
  // Second layer (the router already exact-matches): this handler processes
  // ONLY plts/<THIS deviceId>/ota — byte-for-byte. Audit Phase 10 S1-02 /
  // Phase 4 F4-06: the broker ACL is the PRIMARY authorization, but a
  // misconfigured ACL, wildcard permission, or over-broad credential must
  // never be enough to flash THIS device with a foreign-device OTA command.
  // Foreign topic → security log + reject, no parse, no mutation, no ACK.
  {
    const String expected = mqttTransport.getDeviceTopic("ota");
    if (topic == nullptr || !String(topic).equals(expected)) {
      _rejected++;
      Services::Log.append(Core::LogType::Custom,
          "MQTT SECURITY: OTA command on FOREIGN topic rejected: " +
          String(topic ? topic : "(null)") + " (expected " + expected + ")", 0);
      mqttTransport.countForeignTopic();
      return;
    }
  }

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

  // --- 2a. [CORE-02] Mandatory mutation envelope --------------------------------
  // version + transactionId + issuedAt + expiresAt must ALL be present (same
  // gate as every other ingress — OTA commands are mutations too).
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(doc, envErr)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", envErr);
      _rejected++;
      return;
    }
  }

  // --- 2b. [P2-1 REMEDIATION 2026-09] Freshness gate (RETENTION CONTRACT) ------
  // An expired OTA command can be neither applied nor safely deduplicated
  // once its journal ring slot is gone (see TransactionJournal.h).
  // [GATE-1 / PH8-03] ota.start is an ENERGIZING mutation (it flashes the
  // running image) — the STRICT flag applies: an unusable clock REJECTS the
  // command (CLOCK_INVALID) instead of silently bypassing freshness.
  {
    String errOut;
    const bool energizing = (action == "start");
    if (Services::CommandCanonicalizer::isCommandExpired(doc, errOut, energizing)) {
      _publishAck(tid.c_str(), false, "REJECTED", errOut);
      _rejected++;
      return;
    }
  }

  // --- 3. Field whitelist -------------------------------------------------------
  {
    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
      if (!Services::CommandCanonicalizer::isFieldAllowed(type, action, kv.key().c_str())) {
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

  // [p.473] Journal lock unavailable — REJECT (fail-closed): executing an
  // OTA without a dedup check could re-flash on retry. Honest degradation
  // ack, never a silent NEW.
  if (decision == Services::TransactionDecision::Unavailable) {
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "JOURNAL_UNAVAILABLE",
                "transaction journal lock unavailable — OTA NOT executed (fail-closed), retry");
    return;
  }
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

  // --- 6. NEW — DURABLE RESERVATION BEFORE the OTA side effect ----------------
  // [GATE-4 / S1-03 + F4-06 REMEDIATION 2026-09 — TRANSACTION ORDERING]
  // audit Phase 10 S1-03 / Phase 4 F4-06: the OLD shape ran
  //   decide() -> beginDownload() -> storeTransaction()   (return ignored)
  // — the OTA download/flash side effect STARTED before the transaction was
  // durable, and a journal write failure was silently swallowed. A crash in
  // that window left an active OTA job with NO durable record; a retry was
  // re-DECIDED as NEW and could re-flash.
  //
  // New ordering (audit's REQUIRED shape):
  //   decide NEW
  //     -> reserveTransaction (durable, MUST succeed)
  //     -> only then beginDownload() / beginManifestCheck()
  //     -> updateAck with the terminal command-settlement outcome.
  // Reservation failure → DURABILITY_FAILURE ack, OTA NOT started, honest
  // retry with the SAME transactionId is safe (idempotent).
  String reservedAckJson;
  {
    JsonDocument resAck;
    resAck["transactionId"] = canon.transactionId;
    resAck["requestId"] = canon.requestId;
    resAck["ok"] = true;
    resAck["code"] = "RESERVED";
    resAck["phase"] = "RESERVED";   // durable reservation — job not yet started
    resAck["message"] = "OTA transaction reserved (durable) — starting job";
    resAck["source"] = "mqtt";
    resAck["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(resAck, reservedAckJson);
  }
  if (!Services::journal.storeTransaction(canon.transactionId,
                                          canon.commandHash, reservedAckJson)) {
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "DURABILITY_FAILURE",
                "OTA transaction could not be made durable — job NOT started, "
                "retry with the SAME transactionId is safe");
    Services::Log.append(Core::LogType::OtaFailed,
        "MQTT OTA REFUSED: durable reservation failed tid=" + canon.transactionId, -1);
    return;
  }

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
    // [W13-2] Mixed-fleet self-check: an ota.start command explicitly
    // targeted at the OTHER firmware tree ('generic') must never flash here.
    // '' / absent = fleet-wide (pre-W13 behavior). GAS filters by
    // DEVICES!firmware_type too — this is the device-side second layer.
    String target(doc["target"] | "");
    target.trim();
    target.toLowerCase();
    if (target.length() > 0 && target != "modular") {
      ok = false;
      message = "manifest target '" + target + "' does not match this device (modular)";
      _rejected++;
      Services::Log.append(Core::LogType::OtaFailed,
                            "MQTT OTA start refused: " + message, -1);
    } else if (url[0] == '\0' || fwVersion[0] == '\0' || sha256[0] == '\0') {
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

  // --- 7. Terminal settlement — update the DURABLE record (never overwrite
  // identity) + publish ACK. [GATE-4 / S1-03] The journal entry was RESERVED
  // before the side effect; updateAck() evolves it to the terminal phase
  // (ACCEPTED/REJECTED). commandHash identity is immutable in the journal, so
  // an idempotent retry still replays the FINAL ack, never the reservation.
  // ACK state semantics made EXPLICIT (P2-2):
  //   "phase": "ACCEPTED"  — the OTA JOB was accepted and the download was
  //                          STARTED. It does NOT mean the image flashed.
  //   "phase": "REJECTED"  — the job was refused (policy/validation failure).
  // The FINAL flash outcome is reported out-of-band via OTA_STATUS events:
  //   DOWNLOAD_FAILED | VERIFICATION_FAILED | ROLLBACK | ACTIVATED
  // A command-sender that treats ACK as "flashed" is misreading the contract
  // — the ACK is a transport-level settle for the journal (idempotent
  // replay returns this same ACK), never a lifecycle completion signal.
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = canon.transactionId;
    ack["requestId"] = canon.requestId;
    ack["ok"] = ok;
    ack["code"] = ok ? "ACCEPTED" : "REJECTED";
    ack["phase"] = ok ? "ACCEPTED" : "REJECTED";   // job-level, not flash-level
    ack["message"] = message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }

  // [GATE-4 / S1-03] updateAck return checked: a failed terminal update is
  // OBSERVED (log + counter) — the durable RESERVED record remains the
  // truth (a later retry replays it), and the command still settles on the
  // wire. Never silently swallow a journal failure again.
  if (!Services::journal.updateAck(canon.transactionId,
                                    canon.commandHash, ackJson)) {
    Services::Log.append(Core::LogType::OtaFailed,
        "MQTT OTA: terminal journal update FAILED (reserved record stands) tid=" +
        canon.transactionId, -1);
  }

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
