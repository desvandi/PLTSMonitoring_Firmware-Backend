// =============================================================================
// Network/MqttConfigReceiver.h — MQTT config/calibration command receiver
// -----------------------------------------------------------------------------
// Brief §48, §49, §51 — subscribes to `plts/<deviceId>/config` QoS 1 and routes
// inbound commands through the SAME canonical pipeline used by REST:
//
//     MqttTransport.onMessage(...) ─► mqttConfigReceiver.handle(...)
//                                     │
//                                     ▼
//                Services::CommandCanonicalizer::canonicalizeAndHash(...)
//                                     │
//                                     ▼
//                Services::TransactionJournal::decide(...)
//                                     │
//                       ┌─────────────┼─────────────┐
//                       ▼             ▼             ▼
//                    New          Duplicate       Conflict
//                       │             │             │
//                       ▼             │             ▼
//         Apply via ConfigStore      │     Reject + ACK conflict
//         (same path as REST)        │
//                       │             │
//                       ▼             │
//                  Publish ACK ◄──────┘  (replay previous ACK)
//
// Disciplines (canonical contract §3.12):
//   - Fail-closed: unknown (type, action) or unknown field → REJECT + NACK.
//   - Same canonical hash as REST so dedup is cross-channel (PD-001).
//   - Never bypass CSRF/auth — MQTT ACL provides per-device auth at broker
//     layer (per-device username/password from ConfigStore), and canonical
//     hash + transaction journal provides command-level idempotency.
//   - Never log secrets. ACK body contains only {transactionId, ok, code}.
//
// Whitelist (fail-closed — same as REST, see CommandCanonicalizer.cpp):
//   config.update, calibration.update, calibration.point,
//   calibration.acs712_zero, alarm.acknowledge, alarm.acknowledgeAll,
//   system.reboot, system.factory_reset_prepare, system.factory_reset_confirm
//
// OTA commands (ota.start / ota.check) are routed by MqttOtaHandler, NOT here.
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_MQTT_CONFIG_RECEIVER_H
#define PLTS_NETWORK_MQTT_CONFIG_RECEIVER_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace Network {

class MqttConfigReceiver {
public:
  // begin() is idempotent — called once at boot from setup() after
  // mqttTransport.begin() and configStore.loadBatteryConfig().
  void begin();

  // Entry point — called by MqttTransport's MessageCallback when a message
  // arrives on the `plts/<deviceId>/config` topic.
  // Performs: schema validation → canonicalize → decide → apply or replay.
  // Always publishes an ACK to `plts/<deviceId>/ack` (success or failure).
  void handle(const char* topic, const uint8_t* payload, size_t len);

  // Stats for diagnostics endpoint
  uint32_t getReceivedCount() const { return _received; }
  uint32_t getAcceptedCount() const { return _accepted; }
  uint32_t getRejectedCount() const { return _rejected; }
  uint32_t getDuplicateCount() const { return _duplicates; }

  // [FW-22] Deferred reboot — system.reboot / factory_reset_confirm persist
  // state, publish their ACK, then reboot after the delay so the ACK is
  // actually delivered. tick() (called from networkTask) fires the restart.
  void requestDeferredReboot(uint32_t delayMs) {
    _rebootAtMs = millis() + delayMs;
  }
  void tick();   // drives deferred reboot

private:
  uint32_t _received = 0;
  uint32_t _accepted = 0;
  uint32_t _rejected = 0;
  uint32_t _duplicates = 0;
  uint32_t _rebootAtMs = 0;   // [FW-22] 0 = no reboot pending

  // Internal dispatch — given a parsed (type, action) + JsonDocument,
  // applies the command via the canonical ConfigStore path.
  // Returns {ok, code, message} for ACK body.
  struct ApplyResult { bool ok; const char* code; String message; };
  ApplyResult _applyCommand(const String& type, const String& action,
                            JsonDocument& doc);

  // Publish ACK to `plts/<deviceId>/ack` topic.
  // ACK body schema (brief §51):
  //   {"transactionId":"...","ok":true|false,"code":"ACCEPTED|DUPLICATE|CONFLICT|REJECTED","message":"...","source":"mqtt"}
  void _publishAck(const char* transactionId, bool ok, const char* code,
                    const String& message);
};

extern MqttConfigReceiver mqttConfigReceiver;

} // namespace Network

#endif // PLTS_NETWORK_MQTT_CONFIG_RECEIVER_H
