// =============================================================================
// Network/MqttConfigReceiver.cpp — MQTT config/calibration command receiver
// =============================================================================
#include "MqttConfigReceiver.h"
#include "MqttTransport.h"
#include "MqttTelemetryPublisher.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Storage/ConfigStore.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/ConfigUpdater.h"
#include "../Services/LogService.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/SocStateMachine.h"
#include "../Services/EnergyCounters.h"
#include "../Services/AuthManager.h"
#include "../Services/FactoryReset.h"   // [audit p.427] shared factory-reset wipe
#include "../Drivers/Acs712Driver.h"
#include "../Drivers/Sht31Driver.h"   // [v1.6.3] live-apply calibration offsets
#include "../Comm/BatteryCommManager.h" // [PARITY-4] BMS comm live-reconfigure (REST parity)
#if PLTS_ENABLE_RELAYS
#include "../Services/RelayController.h"   // [v1.8.0] relay command dispatch
#endif
#include <ArduinoJson.h>
#include <cstring>
#include <cmath>

namespace Network {

MqttConfigReceiver mqttConfigReceiver;

// ---------------------------------------------------------------------------
// Schema for ACK body (brief §51):
//   { "transactionId": str,
//     "ok": bool,
//     "code": "ACCEPTED" | "DUPLICATE" | "CONFLICT" | "REJECTED" |
//             "BAD_SCHEMA" | "UNAUTHORIZED",
//     "message": str,
//     "source": "mqtt",
//     "appliedAt": unixSec }
// ---------------------------------------------------------------------------

void MqttConfigReceiver::begin() {
  _received = 0;
  _accepted = 0;
  _rejected = 0;
  _duplicates = 0;
  Services::Log.append(Core::LogType::ConfigurationChanged,
                        "MqttConfigReceiver initialized", -1);
}

void MqttConfigReceiver::handle(const char* topic, const uint8_t* payload, size_t len) {
  _received++;

  // --- 0. [GATE-3 / S1-02 REMEDIATION 2026-09] EXACT DEVICE-TOPIC BINDING ----
  // Second layer (the router already exact-matches): this handler processes
  // ONLY plts/<THIS deviceId>/config — byte-for-byte. Audit Phase 10 S1-02 /
  // Phase 3 P3-S1-04: the broker ACL is the PRIMARY authorization, but a
  // misconfigured ACL, wildcard permission, or over-broad credential must
  // never be enough to mutate THIS device with a foreign-device command.
  // Foreign topic → security log + reject, no parse, no mutation, no ACK.
  {
    const String expected = mqttTransport.getDeviceTopic("config");
    if (topic == nullptr || !String(topic).equals(expected)) {
      _rejected++;
      Services::Log.append(Core::LogType::Custom,
          "MQTT SECURITY: config command on FOREIGN topic rejected: " +
          String(topic ? topic : "(null)") + " (expected " + expected + ")", 0);
      mqttTransport.countForeignTopic();
      return;
    }
  }

  // --- 1. Deserialize + schema-validate envelope --------------------------------
  // Body must be valid JSON ≤ HTTP_MAX_BODY_SIZE (defense-in-depth even on MQTT).
  if (len == 0 || len > Core::HTTP_MAX_BODY_SIZE) {
    _publishAck("", false, "BAD_SCHEMA", "empty or oversized payload");
    _rejected++;
    return;
  }

  JsonDocument doc;
  {
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err != DeserializationError::Ok) {
      _publishAck("", false, "BAD_SCHEMA",
                  String("JSON parse error: ") + err.c_str());
      _rejected++;
      return;
    }
  }

  // Required envelope fields (canonical contract §3.3):
  //   type, action, transactionId, version, issuedAt, expiresAt
  // (requestId is OPTIONAL on MQTT — transactionId is the dedup key.)
  const char* typeC = doc["type"] | "";
  const char* actionC = doc["action"] | "";
  const char* tidC = doc["transactionId"] | "";
  int protocolVer = doc["version"] | 0;

  String type(typeC);
  String action(actionC);
  String tid(tidC);
  type.toLowerCase();
  action.toLowerCase();

  if (type.length() == 0 || action.length() == 0) {
    _publishAck(tid.c_str(), false, "BAD_SCHEMA", "missing type or action");
    _rejected++;
    return;
  }

  // --- 2. Validate transactionId format (PD-001) -------------------------------
  {
    String errOut;
    if (!Services::CommandCanonicalizer::validateTransactionId(tid, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
  }

  // --- 3. Validate protocol version ---------------------------------------------
  {
    String errOut;
    if (!Services::CommandCanonicalizer::validateProtocolVersion(protocolVer, errOut)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", errOut);
      _rejected++;
      return;
    }
  }

  // --- 4. [CORE-02] Mandatory mutation envelope -------------------------------
  // version + transactionId + issuedAt + expiresAt must ALL be present so
  // replay protection never silently degrades to journal-retention-only
  // (same gate as every REST handler — REST/MQTT equivalence).
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(doc, envErr)) {
      _publishAck(tid.c_str(), false, "BAD_SCHEMA", envErr);
      _rejected++;
      return;
    }
  }

  // --- 4b. Expiry check (defense-in-depth) --------------------------------------
  // [P2-1 REMEDIATION 2026-09] Now routed through the SHARED gate so every
  // ingress (REST + MQTT) enforces identical freshness semantics — see
  // CommandCanonicalizer::isCommandExpired + TransactionJournal.h for the
  // full retention contract.
  // [GATE-1 / PH8-03] Energizing relay commands (on/pulse) pass the STRICT
  // flag: an unusable clock REJECTS the command (CLOCK_INVALID) instead of
  // silently bypassing freshness. Safe-direction (off/all_off) and config
  // commands keep the legacy semantics.
  {
    const bool energizing = (type == "relay") && (action == "on" || action == "pulse");
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr, energizing)) {
      _publishAck(tid.c_str(), false, "REJECTED", expiryErr);
      _rejected++;
      return;
    }
  }

  // --- 5. Whitelist (type, action) — fail-closed -------------------------------
  if (!Services::CommandCanonicalizer::isKnownCommandType(type, action)) {
    _publishAck(tid.c_str(), false, "REJECTED",
                "unknown (type, action): " + type + "." + action);
    _rejected++;
    return;
  }

  // --- 6. Field whitelist — reject unknown fields BEFORE hashing --------------
  // This prevents injection of fields the receiver doesn't understand
  // (canonical contract §3.3 — fail-closed schema).
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

  // --- 7. Canonicalize + hash (shared with REST — PD-001) -----------------------
  // Tag with (type, action) in case REST sender omitted them.
  doc["type"] = type;
  doc["action"] = action;
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) {
    _publishAck(tid.c_str(), false, "BAD_SCHEMA", canon.errorMessage);
    _rejected++;
    return;
  }

  // --- 8. Decide NEW / DUPLICATE / CONFLICT via TransactionJournal -------------
  String previousAck;
  Services::TransactionDecision decision =
    Services::journal.decide(canon.transactionId, canon.commandHash, previousAck);

  // [p.473] Journal lock unavailable — REJECT (fail-closed): executing a
  // config mutation without a dedup check could double-apply it on retry.
  // Honest degradation ack, never a silent NEW.
  if (decision == Services::TransactionDecision::Unavailable) {
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "JOURNAL_UNAVAILABLE",
                "transaction journal lock unavailable — command NOT executed (fail-closed), retry");
    return;
  }
  if (decision == Services::TransactionDecision::Duplicate) {
    // Idempotent replay — re-publish the previous ACK verbatim.
    // This is the contract guarantee: clients may retry safely.
    // [audit p.418] The retry's transport identity is logged (NOT merged
    // into the stored ack) so the audit trail can count transport
    // attempts per logical transaction.
    _duplicates++;
    Services::Log.append(Core::LogType::Custom,
        "MQTT: duplicate submission TX=" + canon.transactionId +
        " attempt requestId=" + canon.requestId +
        " — replaying original ACK", 0);
    if (previousAck.length() > 0) {
      String ackTopic = mqttTransport.getDeviceTopic("ack");
      mqttTransport.publishBestEffortQoS0(ackTopic.c_str(), previousAck.c_str(),
                             previousAck.length(), false);
    } else {
      // No stored ACK — synthesize a generic DUPLICATE ack.
      _publishAck(canon.transactionId.c_str(), true, "DUPLICATE",
                  "transaction already processed (ACK not in journal)");
    }
    return;
  }

  if (decision == Services::TransactionDecision::Conflict) {
    // Same transactionId but different hash — reject (canonical contract §3.3).
    _rejected++;
    _publishAck(canon.transactionId.c_str(), false, "CONFLICT",
                "transactionId reused with different payload");
    return;
  }

  // --- 9. NEW — apply the command via canonical path ----------------------------
  ApplyResult r = _applyCommand(type, action, doc, canon.transactionId, canon.commandHash);

  // --- 10. Store transaction + ACK in journal (2-phase commit) -----------------
  // ACK body — same schema regardless of ok=true/false (per §51)
  // [audit p.418] requestId = the TRANSPORT attempt identity (echoes back so
  // the audit trail can correlate this ACK with the submission attempt).
  String ackJson;
  {
    JsonDocument ack;
    ack["transactionId"] = canon.transactionId;
    ack["requestId"] = canon.requestId;
    ack["ok"] = r.ok;
    ack["code"] = r.code;
    ack["message"] = r.message;
    ack["source"] = "mqtt";
    ack["appliedAt"] = (uint32_t)::time(nullptr);
    serializeJson(ack, ackJson);
  }

  // Store in journal BEFORE publishing ACK (so a crash between store + publish
  // is recoverable — client retries, journal says DUPLICATE, replays ACK).
  // [TXN-02/CORE-05] A failed durable store must NOT produce a success ACK —
  // the command may already be applied, but the client is told (via the
  // DURABILITY_FAILURE code) to reconcile rather than blindly retry.
  if (!Services::journal.storeTransaction(canon.transactionId,
                                          canon.commandHash, ackJson)) {
    _publishAck(canon.transactionId.c_str(), false, "DURABILITY_FAILURE",
                "transaction journal persistence failed — reconcile before retrying");
    _rejected++;
    return;
  }

  // Publish ACK
  {
    String ackTopic = mqttTransport.getDeviceTopic("ack");
    mqttTransport.publishBestEffortQoS0(ackTopic.c_str(), ackJson.c_str(),
                           ackJson.length(), false);
  }

  if (r.ok) {
    _accepted++;
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT command accepted: " + type + "." + action +
                          " tid=" + canon.transactionId, -1);
  } else {
    _rejected++;
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT command rejected: " + type + "." + action +
                          " tid=" + canon.transactionId + " code=" + r.code, -1);
  }
}

// ---------------------------------------------------------------------------
// _applyCommand — dispatch to the same ConfigStore / AlarmRegistry APIs used
// by the REST handlers. NEVER bypass canonicalization (already done above)
// or auth (broker ACL already enforced).
//
// IMPORTANT: this method MUST NOT re-validate the canonical hash or expiry —
// that has already been done. It only performs the type-specific mutation.
// ---------------------------------------------------------------------------
MqttConfigReceiver::ApplyResult
MqttConfigReceiver::_applyCommand(const String& type, const String& action,
                                    JsonDocument& doc, const String& tid,
                                    const String& commandHash) {
  (void)commandHash;  // used by the relay branch below
  // ---------- config.update ----------
  // [FW-22 REMEDIATION 2026-08] Range validation now IDENTICAL to the REST
  // path (Web::ConfigHandlers). Previously MQTT config.update accepted ANY
  // value (negative capacity, 0 V thresholds) and persisted it — a divergent
  // validation surface violating the single-command-model requirement.
  if (type == "config" && action == "update") {
    // [PRODUCTION-GRADE 2026-09 / audit p.274-279, BLOCKER B] Two-phase
    // mutation via the SHARED ConfigUpdater (same service the REST path
    // uses). The old code mutated Core::cfg* field-by-field, so a REJECTED
    // request (later field invalid, or a tier-order violation) left earlier
    // fields already live in RAM. Validation law is now identical for both
    // ingresses and no RAM mutation happens before the candidate is fully
    // valid.
    Services::ConfigUpdater::Result r = Services::ConfigUpdater::applyUpdate(doc);
    if (!r.ok) {
      return { false, "REJECTED", r.message };
    }
    if (r.bmsConfigChanged) {
      Comm::batteryComm.reconfigure();
      Services::Log.append(Core::LogType::ConfigurationChanged,
                            String("MQTT: BMS comm reconfigured proto=") + Core::cfgBmsProtocol, -1);
    }
    return { true, "ACCEPTED", r.message };
  }

  // ---------- calibration.update ----------
  // [WAVE-5 / FW-E1] Range validation — previously this path accepted ANY
  // float. acs712Sensitivity is a DIVISOR in the current computation: 0 →
  // inf/NaN, negative → silently sign-inverted current (data lies).
  // config.update got this treatment in FW-22; calibration now matches.
  if (type == "calibration" && action == "update") {
    if (doc.containsKey("version"))
      Core::calibration.version = doc["version"] | Core::calibration.version;
    if (doc.containsKey("acs712Offset")) {
      float v = doc["acs712Offset"] | NAN;
      if (!isfinite(v) || v < 0.0f || v > 3300.0f)
        return { false, "REJECTED", "acs712Offset out of range [0,3300] mV-ish ADC counts" };
      Core::calibration.acs712Offset = v;
      // [v1.6.3] Apply DIRECTLY to the driver — previously the value landed
      // in Core::calibration (persisted, applied at NEXT BOOT) but the live
      // driver kept the old value, so the operator calibrated, saw no
      // change, and concluded the sensor was broken.
      Drivers::acs712.setZeroOffset(v);
    }
    if (doc.containsKey("acs712Sensitivity")) {
      float v = doc["acs712Sensitivity"] | NAN;
      if (!isfinite(v) || v < 10.0f || v > 400.0f)
        return { false, "REJECTED", "acs712Sensitivity out of range [10,400] mV/A" };
      Core::calibration.acs712Sensitivity = v;
      Drivers::acs712.setSensitivity(v);   // [v1.6.3] live-apply (see above)
    }
    if (doc.containsKey("sht31TempOffset")) {
      float v = doc["sht31TempOffset"] | NAN;
      if (!isfinite(v) || v < -50.0f || v > 50.0f)
        return { false, "REJECTED", "sht31TempOffset out of range [-50,50] C" };
      Core::calibration.sht31TempOffset = v;
      Drivers::sht31.setTempOffset(v);     // [v1.6.3] live-apply
    }
    if (doc.containsKey("sht31HumOffset")) {
      float v = doc["sht31HumOffset"] | NAN;
      if (!isfinite(v) || v < -50.0f || v > 50.0f)
        return { false, "REJECTED", "sht31HumOffset out of range [-50,50] %RH" };
      Core::calibration.sht31HumOffset = v;
      Drivers::sht31.setHumOffset(v);      // [v1.6.3] live-apply
    }
    if (doc.containsKey("source")) {
      const char* src = doc["source"];
      if (src) strncpy(Core::calibration.source, src, 15);
      Core::calibration.source[15] = '\0';
    }
    Storage::config.saveCalibration(true);
    return { true, "ACCEPTED", "calibration updated" };
  }

  // ---------- calibration.point ----------
  if (type == "calibration" && action == "point") {
    const char* which = doc["which"] | "";
    float reference = doc["reference"] | 0.0f;
    float raw = doc["raw"] | 0.0f;
    String w(which); w.toLowerCase();
    if (w == "low" || w == "nominal" || w == "full") {
      // Defer to VoltageCalibration service for the actual point capture —
      // same API as Web::CalibrationHandlers.
      // (VoltageCalibration exposes setPoint(which, reference, raw).)
      // For now, mutate calibration struct directly + persist.
      uint32_t now = (uint32_t)::time(nullptr);
      if (w == "low") {
        Core::calibration.voltageLow.reference = reference;
        Core::calibration.voltageLow.raw = raw;
        Core::calibration.voltageLow.timestamp = now;
      } else if (w == "nominal") {
        Core::calibration.voltageNominal.reference = reference;
        Core::calibration.voltageNominal.raw = raw;
        Core::calibration.voltageNominal.timestamp = now;
      } else {  // full
        Core::calibration.voltageFull.reference = reference;
        Core::calibration.voltageFull.raw = raw;
        Core::calibration.voltageFull.timestamp = now;
      }
      Storage::config.saveCalibration(true);
      return { true, "ACCEPTED", "calibration point " + w + " updated" };
    }
    return { false, "REJECTED", "invalid calibration point: " + w };
  }

  // ---------- calibration.acs712_zero ----------
  if (type == "calibration" && action == "acs712_zero") {
    // [FW-22 CLOSED 2026-08] Real execution: capture the zero-current offset
    // from the driver NOW (64-sample average, ~1 s window at zero current)
    // and persist it. Previously this stub logged + returned ACCEPTED without
    // performing ANY mutation — an ACK that lied.
    float captured = Drivers::acs712.captureZeroOffset();
    Core::calibration.acs712Offset = captured;
    Storage::config.saveCalibration(true);
    Services::Log.append(Core::LogType::CalibrationChanged,
                          "MQTT: ACS712 zero-cal captured", -1);
    return { true, "ACCEPTED", "acs712 zero-cal applied" };
  }

  // ---------- alarm.acknowledge ----------
  if (type == "alarm" && action == "acknowledge") {
    const char* code = doc["code"] | "";
    if (strlen(code) == 0) return { false, "REJECTED", "missing code" };
    // [FW-22 CLOSED 2026-08] Real execution via the canonical AlarmRegistry
    // API — previously a log-only stub that ACKed without acknowledging.
    // [p.467] find() is copy-out: no interior pointer into the registry.
    Services::Alarm a;
    if (!Services::alarms.find(code, a)) return { false, "REJECTED", "alarm not found" };
    Services::alarms.acknowledge(code);   // persists immediately (FW-23)
    Services::Log.append(Core::LogType::AlarmAcknowledged,
                          String("MQTT: ack alarm ") + code, -1);
    return { true, "ACCEPTED", "alarm acknowledged" };
  }

  // ---------- alarm.acknowledgeAll ----------
  if (type == "alarm" && action == "acknowledgeAll") {
    // [FW-22 CLOSED 2026-08] Real execution — acknowledge every ACTIVE alarm.
    Services::alarms.acknowledgeAll();    // persists immediately (FW-23)
    Services::Log.append(Core::LogType::AlarmAcknowledged,
                          "MQTT: ack all alarms", -1);
    return { true, "ACCEPTED", "all alarms acknowledged" };
  }

  // ---------- system.reboot ----------
  if (type == "system" && action == "reboot") {
    // [FW-22 CLOSED 2026-08] Real execution: persist state NOW (SOC, energy,
    // sequence high-water mark) so the reboot is a clean checkpoint, then
    // defer the actual esp_restart() so this ACK is delivered first.
    Services::socStateMachine.saveToNVS();
    Services::energyCounters.saveToNVS();
    Storage::config.saveTelemetrySequence(telemetrySequence + Core::SEQ_REBOOT_MARGIN);
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: reboot requested — scheduling in 500ms", -1);
    Network::mqttConfigReceiver.requestDeferredReboot(500);
    return { true, "ACCEPTED", "reboot scheduled" };
  }

  // ---------- system.factory_reset_prepare / confirm ----------
  if (type == "system" && action == "factory_reset_prepare") {
    // [FW-22 CLOSED 2026-08] Real execution: issue a 60 s one-time token via
    // AuthManager (same two-step flow as REST).
    // [AUDIT 2026-09 ROUND 11 / p.476] Fail-closed: empty token = refusal
    // (no serialization → no token). The ACK must not claim a token was
    // issued when none was.
    String token = Services::auth.prepareFactoryReset();
    if (token.length() != 32) {
      return { false, "REJECTED",
               "factory reset unavailable (auth lock) — token not issued" };
    }
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: factory_reset_prepare (60s TTL)", -1);
    return { true, "ACCEPTED", "factory reset token issued (60s TTL)" };
  }
  if (type == "system" && action == "factory_reset_confirm") {
    const char* token = doc["token"] | "";
    if (strlen(token) == 0) return { false, "REJECTED", "missing token" };
    // [FW-22 CLOSED 2026-08] Real execution: verify the one-time token, then
    // erase persisted state and reboot into first-boot provisioning.
    if (!Services::auth.confirmFactoryReset(token)) {
      // [p.476] False covers an invalid/expired token AND the fail-closed
      // lock refusal (no serialization → token NOT consumed, wipe NOT
      // authorized) — both refuse conservatively.
      return { false, "REJECTED", "invalid or expired factory reset token" };
    }
    Services::Log.append(Core::LogType::ConfigurationChanged,
                          "MQTT: factory_reset_confirm — erasing state", -1);
    // [audit p.427] The MQTT path previously kept its OWN (9-of-13)
    // namespace list — a factory reset issued over MQTT left plts_time,
    // plts_emg, plts_auth, plts_relays ALIVE and skipped the LittleFS
    // format. It now sweeps the SAME shared, single-source set as REST
    // (Services/FactoryReset.cpp) including the audit-log preservation.
    Services::executeFactoryResetWipe();
    Network::mqttConfigReceiver.requestDeferredReboot(500);
    return { true, "ACCEPTED", "factory reset applied — rebooting" };
  }

  // ---------- relay.* (v1.8.0) ----------
  #if PLTS_ENABLE_RELAYS
  if (type == "relay") {
    // [audit p.434] Executable-action whitelist — the relay queue accepts
    // ONLY runtime mutations. "config" is registered in the canonicalizer
    // schema but has NO runtime ingress (maxOnTime/minOnTime/interlock are
    // provisioned, not commanded); queueing it would ACK a silent no-op.
    // Fail closed with an honest NACK instead of a misleading success.
    if (action != "on" && action != "off" && action != "pulse" &&
        action != "all_off" && action != "acknowledge" && action != "clear") {
      return { false, "REJECTED",
               "relay action '" + action +
               "' is not an executable runtime mutation (config is not a "
               "runtime path)" };
    }
    uint8_t channel = doc["channel"] | 0;
    String source = doc["source"] | "MANUAL";
    uint32_t pulseMs = doc["durationMs"] | 0;

    // [PRODUCTION-GRADE 2026-09 / audit p.284-286] MQTT relay commands MUST
    // go through the SAME single physical mutation authority as REST — the
    // FreeRTOS queue consumed by relayTask. The old direct
    // relaysController.applyCommand() from the networkTask context created a
    // second hardware mutation path (PATH B) with non-deterministic
    // ordering vs. queued REST commands and cross-core I²C access.
    Services::QueuedRelayCommand qc = {};
    strncpy(qc.transactionId, tid.c_str(), sizeof(qc.transactionId) - 1);
    // [audit p.418] Transport identity — MAY differ from the logical
    // transactionId (a retry presents a fresh requestId with the SAME
    // transactionId); falls back to tid when the client sends none.
    {
      const char* ridC = doc["requestId"] | "";
      String rid = (strlen(ridC) > 0) ? String(ridC) : tid;
      strncpy(qc.requestId, rid.c_str(), sizeof(qc.requestId) - 1);
    }
    strncpy(qc.commandHash, commandHash.c_str(), sizeof(qc.commandHash) - 1);
    strncpy(qc.command, action.c_str(), sizeof(qc.command) - 1);
    qc.channel = channel;
    qc.desiredState = (action == "on" || action == "pulse");
    qc.pulseDurationMs = pulseMs;
    strncpy(qc.source, source.c_str(), sizeof(qc.source) - 1);
    qc.issuedAt = doc["issuedAt"] | 0U;
    qc.expiresAt = doc["expiresAt"] | 0U;
    qc.safetyGeneration = Services::relaysController.safetyGeneration();
    qc.enqueuedAtMs = millis();

    if (!Services::relaysController.queueCommand(qc)) {
      return { false, "REJECTED", "relay command queue full — retry with the same transactionId" };
    }

    // Asynchronous submission semantics (audit p.73-75): the ACK says the
    // command was QUEUED, not executed. The final outcome is retrievable
    // via GET /api/relays/transactions/{transactionId} on REST, or observed
    // through telemetry (reportedState + stateSequence).
    // [audit p.432] Delivery-guarantee honesty: this ACK is published with
    // PubSubClient QoS 0 (fire-and-forget socket write) — NOT a broker
    // PUBACK. The DURABLE journal + REST reconciliation endpoint is the
    // authoritative outcome source; a lost MQTT ACK is recoverable by
    // re-issuing with the SAME transactionId (idempotent replay).
    return { true, "QUEUED", "relay command queued for execution" };
  }
  #endif

  // Unreachable — whitelist check above already rejected unknown types.
  return { false, "REJECTED", "unreachable" };
}

// ---------------------------------------------------------------------------
// _publishAck — helper to publish a synthesized ACK when we don't have a
// stored one (e.g., on bad schema before canonicalization).
// ---------------------------------------------------------------------------
void MqttConfigReceiver::_publishAck(const char* transactionId, bool ok,
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
  mqttTransport.publishBestEffortQoS0(ackTopic.c_str(), ackJson.c_str(),
                        ackJson.length(), false);
}

// [FW-22] Deferred reboot driver — called from networkTask's 100 Hz loop.
void MqttConfigReceiver::tick() {
  if (_rebootAtMs == 0) return;
  if ((int32_t)(millis() - _rebootAtMs) >= 0) {
    Services::Log.append(Core::LogType::ConfigurationChanged,
                         "Deferred reboot firing now", -1);
    delay(100);            // let the socket flush the ACK
    ESP.restart();
  }
}

} // namespace Network
