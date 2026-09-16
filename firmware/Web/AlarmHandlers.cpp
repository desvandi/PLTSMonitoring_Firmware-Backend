// =============================================================================
// Web/AlarmHandlers.cpp
// =============================================================================
#include "AlarmHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/CommandCanonicalizer.h"
#include "../Services/TransactionJournal.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>

namespace Web {
namespace AlarmHandlers {

// [PRODUCTION-GRADE 2026-09 / audit p.270-271, BLOCKER D] Alarm ACK is not
// just a UI operation — it persists ACTIVE→ACKNOWLEDGED and must be traceable
// (who/what/when/transactionId). Both the per-alarm and acknowledge-all REST
// endpoints now flow through the canonical transaction pipeline (envelope →
// freshness → canonicalize → journal → apply → ACK), matching the MQTT
// alarm.acknowledge / alarm.acknowledgeAll paths.
static bool runAlarmAckPipeline(const String& action, const String& code,
                                String& ackOut, String& errMsgOut, int& httpStatusOut) {
  httpStatusOut = 400;
  String raw = http.arg("plain");
  StaticJsonDocument<512> body;
  if (deserializeJson(body, raw)) { errMsgOut = "Invalid JSON"; return false; }

  StaticJsonDocument<512> cmdDoc;
  cmdDoc["type"] = "alarm";
  cmdDoc["action"] = action;
  if (code.length() > 0) cmdDoc["code"] = code;
  cmdDoc["requestId"] = body["requestId"] | "";
  cmdDoc["transactionId"] = body["transactionId"] | "";
  cmdDoc["version"] = body["version"] | 0;
  cmdDoc["issuedAt"] = body["issuedAt"] | 0;
  cmdDoc["expiresAt"] = body["expiresAt"] | 0;

  // [CORE-02] Envelope gate
  {
    String envErr;
    if (!Services::CommandCanonicalizer::validateCommandEnvelope(cmdDoc, envErr)) {
      errMsgOut = envErr;
      return false;
    }
  }
  {
    String expiryErr;
    if (Services::CommandCanonicalizer::isCommandExpired(cmdDoc, expiryErr)) {
      errMsgOut = expiryErr;
      return false;
    }
  }
  Services::CanonicalResult canon = Services::CommandCanonicalizer::canonicalizeAndHash(cmdDoc);
  if (!canon.ok) { errMsgOut = canon.errorMessage; return false; }
  Services::DecisionResult d =
    Services::CommandCanonicalizer::decideTransaction(canon.transactionId, canon.commandHash);
  // [p.473] Journal lock unavailable — REJECT (fail-closed): executing
  // without a dedup check could double-apply the mutation on retry.
  if (d.decision == Services::TransactionDecision::Unavailable) {
    errMsgOut = "transaction journal lock unavailable — command NOT executed (fail-closed), retry";
    return false;
  }
  if (d.decision == Services::TransactionDecision::Conflict) {
    errMsgOut = "requestId reuse with different command";
    return false;
  }
  if (d.decision == Services::TransactionDecision::Duplicate) {
    // Idempotent replay — re-deliver the stored ACK verbatim.
    ackOut = d.previousAckJson;
    return true;
  }

  // Execute
  bool ok;
  if (action == "acknowledge") {
    // [p.467] find() is copy-out: no interior pointer into the registry.
    Services::Alarm a;
    if (!Services::alarms.find(code.c_str(), a)) {
      // P1-3 canonical contract: 404 when alarm code not found (was: silently OK)
      httpStatusOut = 404;
      errMsgOut = "Alarm not found";
      return false;
    }
    Services::alarms.acknowledge(code.c_str());
    ok = true;
  } else {  // acknowledgeAll
    Services::alarms.acknowledgeAll();
    ok = true;
  }

  ackOut = String("{\"success\":") + (ok ? "true" : "false") +
           ",\"message\":\"Alarm(s) acknowledged\"}";
  if (!Services::journal.storeTransaction(canon.transactionId, canon.commandHash, ackOut)) {
    errMsgOut = "Alarm acknowledged but journal persistence failed";
    return false;
  }
  return true;
}

void handleGetAlarms() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  // v1.6.1 — canonical contract compliance (02_CANONICAL_API_CONTRACT.md):
  // GET /api/alarms → {active, history}. Bentuk lama {alarms:[...]} tidak
  // pernah cocok dengan kontrak maupun konsumen PWA (Alarm Center crash
  // `.map is not a function` saat dilayani langsung oleh firmware).
  StaticJsonDocument<4096> doc;
  JsonArray active = doc.createNestedArray("active");
  JsonArray history = doc.createNestedArray("history");
  // [AUDIT 2026-09 ROUND 7 / p.469] ONE coherent snapshot — the old
  // countAll()+getAlarm(i) loop re-entered the registry once per entry,
  // and a concurrent raise/clear/eviction between iterations served a JSON
  // body stitched from two different registry states (or nullptr holes when
  // _count shrank mid-loop). Heap-allocated: a Snapshot is ~3.3 KB, too
  // large for the async-web task stack.
  Services::AlarmRegistry::Snapshot* snap =
      (Services::AlarmRegistry::Snapshot*)malloc(sizeof(Services::AlarmRegistry::Snapshot));
  if (!snap) { sendError(500, "Out of memory"); return; }
  Services::alarms.snapshotInto(*snap);
  for (uint8_t i = 0; i < snap->count; i++) {
    const Services::Alarm* a = &snap->alarms[i];
    if (!a) continue;
    JsonArray& dst = (a->lifecycle == Core::AlarmLifecycle::Active) ? active : history;
    JsonObject o = dst.createNestedObject();
    o["code"] = a->code;
    o["severity"] = Core::severityToStr(a->severity);
    o["lifecycle"] = Core::lifecycleToStr(a->lifecycle);
    o["raisedAt"] = a->raisedAt;
    o["acknowledgedAt"] = a->acknowledgedAt;
    o["clearedAt"] = a->clearedAt;
    o["message"] = a->message;
  }
  // [AUDIT 2026-09 ROUND 5 / p.451] Registry saturation observability — a
  // non-zero overflowCount means at least one alarm was REJECTED because all
  // MAX_ALARMS slots held non-cleared entries (no active safety alarm is ever
  // evicted to make room). Additive field; consumers treat absent as 0.
  // [p.469] Taken from the SAME snapshot — the counters and the list can
  // no longer describe two different instants.
  doc["overflowCount"] = snap->overflowCount;
  // [AUDIT 2026-09 ROUND 9 / p.471] Fail-closed observability — non-zero
  // means at least one registry operation was REFUSED since boot because the
  // registry mutex was unavailable (degraded mode; CRIT log fires
  // rate-limited). The alarm list above is only authoritative when this is
  // 0. Additive field; consumers treat absent as 0.
  doc["lockFailures"] = snap->lockFailures;
  free(snap);
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

static void handleAcknowledgeImpl() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
  if (!requireCsrf()) return;
  if (!requireBody(512)) return;
  // Extract code from path: /api/alarms/{code}/acknowledge
  String uri = http.uri();
  int s1 = uri.indexOf("/alarms/");
  if (s1 < 0) { sendError(400, "Missing alarm code"); return; }
  s1 += 8;
  int s2 = uri.indexOf("/acknowledge", s1);
  if (s2 < 0) { sendError(400, "Bad path"); return; }
  String code = uri.substring(s1, s2);
  if (code.length() == 0) { sendError(400, "Empty alarm code"); return; }

  String ack, err;
  int status = 400;
  if (!runAlarmAckPipeline("acknowledge", code, ack, err, status)) {
    sendError(status, err);
    return;
  }
  sendSuccess("Alarm acknowledged", ack);
}

void handleAcknowledge() { handleAcknowledgeImpl(); }

// P1-3 — pattern router for /api/alarms/{code}/acknowledge
// The Arduino WebServer library does not support path parameters natively,
// so we use a single catch-all handler mounted at "/api/alarms/" (note the
// trailing slash) and dispatch by suffix.
void handleAlarmWildcard() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
  String uri = http.uri();
  // Only handle POST /api/alarms/{code}/acknowledge here. Other sub-paths
  // fall through to 404 (the previous behavior).
  if (http.method() == HTTP_POST && uri.endsWith("/acknowledge")) {
    handleAcknowledge();
    return;
  }
  sendError(404, "Not Found");
}

void registerRoutes() {
  http.on("/api/alarms", HTTP_GET, handleGetAlarms);
  // P1-3 canonical contract: POST /api/alarms/{alarmId}/acknowledge
  // Mounted as a sub-path catch-all (Arduino WebServer has no path params).
  http.on("/api/alarms/", HTTP_POST, handleAlarmWildcard);
  // Bulk acknowledge-all (not the canonical per-alarm path) — same
  // transaction pipeline (audit BLOCKER D).
  http.on("/api/alarms/acknowledge-all", HTTP_POST, []() {
    if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
    if (!requireRole("operator")) return;   // [audit p.489] role-gated mutation
    if (!requireCsrf()) return;
    if (!requireBody(512)) return;
    String ack, err;
    int status = 400;
    if (!runAlarmAckPipeline("acknowledgeAll", "", ack, err, status)) {
      sendError(status, err);
      return;
    }
    sendSuccess("All alarms acknowledged", ack);
  });
}

} // namespace AlarmHandlers
} // namespace Web
