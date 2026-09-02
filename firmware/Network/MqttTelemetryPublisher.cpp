// =============================================================================
// Network/MqttTelemetryPublisher.cpp — telemetry publish + spool replay
// =============================================================================
// REMEDIATION 2026-08 (Audit #1+#2+#3):
//   FW-01: fake identity overrides ("PLTS-UNKNOWN", timestamp 0) removed —
//          the caller serializes the canonical envelope ONCE (shared with
//          the REST path); this class is now a transport-only helper.
//   FW-08: TelemetrySpool.setPublishCallback() is wired in begin() — the
//          spool replay path is functional for the first time.
//   FW-29: replay records are routed to plts/<deviceId>/status|log (not the
//          literal garbage topics "status"/"critical").
//   FW-32: callers spool on publish FAILURE while connected, not only when
//          disconnected (see firmware_v1.ino publishTelemetry()).
//   The blocking delay(500) in the old replay loop is removed — replay is
//   rate-limited by TelemetrySpool (MAX_REPLAY_PER_SEC) and driven by tick.
// =============================================================================
#include "MqttTelemetryPublisher.h"
#include "MqttTransport.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Core/Common.h"
#include "../Services/LogService.h"
#include "../Services/TelemetrySpool.h"
#include "../Services/TimeManager.h"
#include <cstring>

namespace Network {

MqttTelemetryPublisher mqttTelemetry;

// Static trampoline → instance-less routing (the spool uses a C function ptr).
bool MqttTelemetryPublisher::_spoolPublishTrampoline(uint8_t recordType,
                                                      const char* payload, size_t len) {
  // [FW-29] Route by record type to the canonical per-device topics.
  // QoS 1: PubSubClient blocks for PUBACK inside publish() — the return
  // value IS the delivery confirmation (P1-005 ACK-before-removal).
  const char* suffix =
    (recordType == (uint8_t)Services::SpoolRecordType::CriticalEvent) ? "log" : "status";
  String topic = mqttTransport.getDeviceTopic(suffix);
  return mqttTransport.publish(topic.c_str(), payload, len, false, 1);
}

void MqttTelemetryPublisher::begin() {
  _publishedCount = 0;
  _publishFailCount = 0;
  _criticalSeq = 0;
  // [FW-08] THE critical wiring that was missing: without this callback the
  // spool's replay() returned 0 forever and the queue never drained.
  Services::telemetrySpool.setPublishCallback(_spoolPublishTrampoline);
}

bool MqttTelemetryPublisher::publishStatus(const char* json, size_t len) {
  if (!json || len == 0) return false;
  if (!mqttTransport.isFullyOperational()) { _publishFailCount++; return false; }
  String topic = mqttTransport.getDeviceTopic("status");
  bool ok = mqttTransport.publish(topic.c_str(), json, len, false, 0);
  if (ok) _publishedCount++; else _publishFailCount++;
  return ok;
}

void MqttTelemetryPublisher::publishCriticalEvent(const char* type, const char* payload, size_t len) {
  if (!payload || len == 0) return;

  // [P1-005] Durable BEFORE delivery: persist to the NVS critical spool so a
  // power loss between reception and delivery cannot lose the event.
  uint32_t seq = 0x80000000u | (++_criticalSeq);   // critical namespace of the sequence space
  uint32_t ts = Services::timeManager.getUnixTime();
  Services::telemetrySpool.spoolCritical(seq, ts, payload, (uint16_t)len);

  // Best-effort immediate delivery (QoS 1); the record stays in the spool
  // until replay confirms delivery.
  if (mqttTransport.isFullyOperational()) {
    String topic = mqttTransport.getDeviceTopic("log");
    mqttTransport.publish(topic.c_str(), payload, len, false, 1);
  }
  (void)type;
}

void MqttTelemetryPublisher::replaySpool() {
  if (!mqttTransport.isFullyOperational()) return;
  // Oldest-first, rate-limited inside TelemetrySpool::replay() (2 records/s).
  // Non-blocking: one record per call; the telemetry/network tick drives it.
  Services::telemetrySpool.replay();
}

} // namespace Network
