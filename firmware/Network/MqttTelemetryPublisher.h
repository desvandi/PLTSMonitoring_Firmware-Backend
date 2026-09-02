// =============================================================================
// Network/MqttTelemetryPublisher.h — telemetry publish + spool replay
// -----------------------------------------------------------------------------
// REMEDIATION 2026-08 (Audit #1+#2+#3 — FW-01, FW-08, FW-29, FW-30, FW-32):
//   - The publisher no longer builds its own snapshot with a fake identity
//     (deviceId "PLTS-UNKNOWN", timestamp 0). Serialization happens ONCE in
//     publishTelemetry() with the canonical identity, shared by REST + MQTT.
//   - begin() wires TelemetrySpool's publish callback (previously NEVER set —
//     replay() was dead code). The callback routes by record type to
//     plts/<deviceId>/status or .../log at QoS 1 and returns the broker-ACK
//     result; spooled records are removed ONLY on confirmed delivery.
//   - publishCriticalEvent() persists to the NVS critical spool AND attempts
//     immediate delivery — critical events survive power loss.
//   - replaySpool() drives the oldest-first replay with no blocking delays.
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_MQTT_TELEMETRY_PUBLISHER_H
#define PLTS_NETWORK_MQTT_TELEMETRY_PUBLISHER_H

#include <Arduino.h>

namespace Network {

class MqttTelemetryPublisher {
public:
  void begin();

  // Publish one serialized telemetry envelope at QoS 0.
  // Returns true when the broker accepted the PUBLISH.
  bool publishStatus(const char* json, size_t len);

  // Publish a critical event (BOOT/ALARM/FAULT/SAFETY):
  //   1. persist to NVS critical spool (survives power loss)
  //   2. attempt immediate delivery at QoS 1
  //   3. retained in spool until delivery is confirmed
  void publishCriticalEvent(const char* type, const char* payload, size_t len);

  // Replay spooled records (oldest-first, rate-limited by TelemetrySpool).
  // Call when the transport is fully operational.
  void replaySpool();

  uint32_t publishedCount() const { return _publishedCount; }
  uint32_t publishFailCount() const { return _publishFailCount; }

private:
  uint32_t _publishedCount = 0;
  uint32_t _publishFailCount = 0;
  uint32_t _criticalSeq = 0;

  static bool _spoolPublishTrampoline(uint8_t recordType, const char* payload, size_t len);
};

extern MqttTelemetryPublisher mqttTelemetry;

} // namespace Network

#endif // PLTS_NETWORK_MQTT_TELEMETRY_PUBLISHER_H
