// =============================================================================
#include "../Drivers/Sht31Driver.h"
// Web/CalibrationHandlers.cpp
// =============================================================================
#include "CalibrationHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Storage/ConfigStore.h"
#include "../Services/VoltageCalibration.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include "../Drivers/Acs712Driver.h"
// [PARITY-3 2026-09-06] Live raw voltage exposure for the PWA calibration
// capture button (was: "the operator must enter raw manually OR the firmware
// must expose it" — calibration-center.tsx).
#include "../Drivers/AdcVoltageDriver.h"
#include "../Core/Common.h"
#include <ArduinoJson.h>

namespace Web {
namespace CalibrationHandlers {

void handleGet() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  StaticJsonDocument<2048> doc;
  doc["version"] = Core::calibration.version;
  JsonObject vl = doc.createNestedObject("voltageLow");
  vl["reference"] = Core::calibration.voltageLow.reference;
  vl["raw"] = Core::calibration.voltageLow.raw;
  vl["timestamp"] = Core::calibration.voltageLow.timestamp;
  JsonObject vn = doc.createNestedObject("voltageNominal");
  vn["reference"] = Core::calibration.voltageNominal.reference;
  vn["raw"] = Core::calibration.voltageNominal.raw;
  vn["timestamp"] = Core::calibration.voltageNominal.timestamp;
  JsonObject vf = doc.createNestedObject("voltageFull");
  vf["reference"] = Core::calibration.voltageFull.reference;
  vf["raw"] = Core::calibration.voltageFull.raw;
  vf["timestamp"] = Core::calibration.voltageFull.timestamp;
  doc["acs712Offset"] = Core::calibration.acs712Offset;
  doc["acs712Sensitivity"] = Core::calibration.acs712Sensitivity;
  doc["sht31TempOffset"] = Core::calibration.sht31TempOffset;
  doc["sht31HumOffset"] = Core::calibration.sht31HumOffset;
  doc["timestamp"] = Core::calibration.timestamp;
  doc["source"] = Core::calibration.source;
  // [PARITY-3 2026-09-06] voltageRaw — the CURRENT live reading BEFORE
  // 3-point calibration is applied (AdcVoltageDriver::getRawV(), volts).
  // This is exactly the `raw` value the point routes expect, so the PWA
  // capture button can prefill it instead of asking the operator to read a
  // serial console. Honest null when the driver has no valid reading.
  {
    float rawV = Drivers::batteryAdc.getRawV();
    if (Core::isValidFloat(rawV)) doc["voltageRaw"] = rawV;
    else                          doc["voltageRaw"] = nullptr;
  }
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void handlePost() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(Core::HTTP_MAX_BODY_SIZE)) return;
  String raw = http.arg("plain");
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }
  doc["type"] = "calibration"; doc["action"] = "update";
  // [P2-1 REMEDIATION 2026-09] Freshness gate (REST/MQTT parity — see
  // ConfigHandlers.cpp for the full rationale).
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(doc, expiryErr)) {
      sendError(400, expiryErr);
      return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(doc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  // Apply
  // [WAVE-5 / FW-E1] Range validation identical to the MQTT path — REST must
  // not be the weak sibling. acs712Sensitivity is a divisor (0 → inf, negative
  // → sign-inverted current), offsets are bounded physical corrections.
  if (doc.containsKey("acs712Offset")) {
    float v = doc["acs712Offset"] | NAN;
    if (!isfinite(v) || v < 0.0f || v > 3300.0f) {
      sendError(400, "acs712Offset out of range [0,3300]"); return;
    }
    Core::calibration.acs712Offset = v;
  }
  if (doc.containsKey("acs712Sensitivity")) {
    float v = doc["acs712Sensitivity"] | NAN;
    if (!isfinite(v) || v < 10.0f || v > 400.0f) {
      sendError(400, "acs712Sensitivity out of range [10,400] mV/A"); return;
    }
    Core::calibration.acs712Sensitivity = v;
    Drivers::acs712.setSensitivity(Core::calibration.acs712Sensitivity);
  }
  if (doc.containsKey("sht31TempOffset")) {
    float v = doc["sht31TempOffset"] | NAN;
    if (!isfinite(v) || v < -50.0f || v > 50.0f) {
      sendError(400, "sht31TempOffset out of range [-50,50] C"); return;
    }
    Core::calibration.sht31TempOffset = v;
    Drivers::sht31.setTempOffset(Core::calibration.sht31TempOffset);
  }
  if (doc.containsKey("sht31HumOffset")) {
    float v = doc["sht31HumOffset"] | NAN;
    if (!isfinite(v) || v < -50.0f || v > 50.0f) {
      sendError(400, "sht31HumOffset out of range [-50,50] %RH"); return;
    }
    Core::calibration.sht31HumOffset = v;
    Drivers::sht31.setHumOffset(Core::calibration.sht31HumOffset);
  }
  Storage::config.markCalibrationDirty();
  Storage::config.saveCalibration(true);
  String ack = "{\"success\":true,\"message\":\"Calibration updated\"}";
  Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack);
  sendSuccess("Calibration updated", "{}");
}

// [PRODUCTION-GRADE 2026-09 / audit p.266-269, BLOCKER C] Both calibration
// point and ACS712-zero mutations now flow through the SAME canonical
// transaction pipeline as the MQTT path: envelope → freshness →
// canonicalize → journal → apply → ACK. Previously these REST endpoints
// called VoltageCalibration::setPoint()/captureZeroOffset() DIRECTLY — a
// network timeout followed by an operator retry could apply the capture
// TWICE with no durable transaction identity (idempotency gap).
static void handlePointImpl(const String& which) {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(1024)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> body;
  if (deserializeJson(body, raw)) { sendError(400, "Invalid JSON"); return; }

  // Build the canonical command envelope
  StaticJsonDocument<512> cmdDoc;
  cmdDoc["type"] = "calibration";
  cmdDoc["action"] = "point";
  cmdDoc["which"] = which;
  cmdDoc["reference"] = body["reference"] | 0.0f;
  cmdDoc["raw"] = body["raw"] | 0.0f;
  cmdDoc["requestId"] = body["requestId"] | "";
  cmdDoc["transactionId"] = body["transactionId"] | "";
  cmdDoc["version"] = body["version"] | 0;
  cmdDoc["issuedAt"] = body["issuedAt"] | 0;
  cmdDoc["expiresAt"] = body["expiresAt"] | 0;

  // [CORE-02] Envelope gate (same as relay/config mutations)
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(cmdDoc, envErr)) {
      sendError(400, envErr.c_str());
      return;
    }
  }
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(cmdDoc, expiryErr)) {
      sendError(400, expiryErr);
      return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command");
    return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }

  // Execute (two-phase: setPoint validates internally BEFORE mutating)
  float ref = cmdDoc["reference"] | 0.0f;
  float rawV = cmdDoc["raw"] | 0.0f;
  if (!Services::voltageCalibration.setPoint(which.c_str(), ref, rawV)) {
    sendError(400, "Calibration point rejected");
    return;
  }

  String ack = "{\"success\":true,\"message\":\"Calibration point " + which + " set\"}";
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack)) {
    sendError(503, "Calibration applied but journal persistence failed");
    return;
  }
  sendSuccess("Calibration point set", "{}");
}

static void handleAcs712ZeroImpl() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  if (!requireBody(512)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> body;
  if (deserializeJson(body, raw)) { sendError(400, "Invalid JSON"); return; }

  // Build the canonical command envelope (action=acs712_zero)
  StaticJsonDocument<512> cmdDoc;
  cmdDoc["type"] = "calibration";
  cmdDoc["action"] = "acs712_zero";
  cmdDoc["requestId"] = body["requestId"] | "";
  cmdDoc["transactionId"] = body["transactionId"] | "";
  cmdDoc["version"] = body["version"] | 0;
  cmdDoc["issuedAt"] = body["issuedAt"] | 0;
  cmdDoc["expiresAt"] = body["expiresAt"] | 0;

  // [CORE-02] Envelope gate — the ACS712 capture is a physical measurement
  // mutation; a retried POST MUST dedupe to the first capture result.
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(cmdDoc, envErr)) {
      sendError(400, envErr.c_str());
      return;
    }
  }
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(cmdDoc, expiryErr)) {
      sendError(400, expiryErr);
      return;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
  if (!canon.ok) { sendError(400, canon.errorMessage); return; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  if (d.decision == Services::TransactionDecision::Conflict) {
    sendError(409, "requestId reuse with different command");
    return;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    sendSecurityHeaders();
    http.send(200, "application/json; charset=utf-8", d.previousAckJson);
    return;
  }

  // Execute — capture zero offset from current sensor (must be at zero current)
  float offset = Drivers::acs712.captureZeroOffset();
  Core::calibration.acs712Offset = offset;
  Drivers::acs712.setZeroOffset(offset);
  Storage::config.markCalibrationDirty();
  Storage::config.saveCalibration(true);

  String ack = "{\"success\":true,\"offset\":" + String(offset, 2) + "}";
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ack)) {
    sendError(503, "Zero-cal applied but journal persistence failed");
    return;
  }
  sendSuccess("ACS712 zero offset captured", ack);
}

void handlePoint(const String& which) { handlePointImpl(which); }
void handleAcs712Zero() { handleAcs712ZeroImpl(); }

void registerRoutes() {
  http.on("/api/calibration", HTTP_GET, handleGet);
  http.on("/api/calibration", HTTP_POST, handlePost);
  http.on("/api/calibration/voltage/point/low", HTTP_POST, []() { handlePoint("low"); });
  http.on("/api/calibration/voltage/point/nominal", HTTP_POST, []() { handlePoint("nominal"); });
  http.on("/api/calibration/voltage/point/full", HTTP_POST, []() { handlePoint("full"); });
  http.on("/api/calibration/acs712/zero", HTTP_POST, handleAcs712Zero);
}

} // namespace CalibrationHandlers
} // namespace Web
