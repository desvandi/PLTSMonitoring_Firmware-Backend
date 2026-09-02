// =============================================================================
// Network/MqttOtaHandler.h — MQTT OTA command receiver (ota.start / ota.check)
// -----------------------------------------------------------------------------
// Brief §48, §49, §72 — subscribes to `plts/<deviceId>/ota` QoS 1 and routes
// inbound OTA commands through:
//
//     MqttTransport.onMessage(...) ─► mqttOtaHandler.handle(...)
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
//         OtaManager.beginDownload()  │     Reject + ACK conflict
//                       │             │
//                       ▼             │
//                  Publish ACK ◄──────┘  (replay previous ACK)
//
// Disciplines (canonical contract §3.12 + §3.13):
//   - Anti-downgrade: enforced by OtaManager (strict SemVer >).
//   - URL allowlist: enforced by OtaManager (OTA_ALLOWED_HOSTS suffix match).
//   - HTTPS mandatory: enforced by OtaManager.
//   - Ed25519 signature: MANDATORY in PRODUCTION_BUILD (fail-closed if empty).
//   - SHA-256 expected hash: MANDATORY (streaming verify during download).
//   - Same canonical hash as REST so dedup is cross-channel (PD-001).
//
// MQTT OTA command schema:
//   {
//     "type": "ota",
//     "action": "start",
//     "transactionId": "<uuid>",
//     "version": 1,
//     "issuedAt": <unixsec>,
//     "expiresAt": <unixsec>,
//     "url": "https://...",
//     "version": "1.2.3",         (firmware version, NOT protocol version)
//     "size": <bytes>,
//     "sha256": "<hex>",
//     "signature": "<ed25519-hex>"
//   }
//
// NOTE on naming conflict: the JSON field "version" appears twice in the
// OTA schema (protocol version in envelope vs firmware version in payload).
// To resolve, the canonical contract uses "fwVersion" for the firmware version.
// We accept BOTH "version" (legacy) and "fwVersion" (canonical) for backwards
// compatibility, but the canonical field is "fwVersion".
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_MQTT_OTA_HANDLER_H
#define PLTS_NETWORK_MQTT_OTA_HANDLER_H

#include <Arduino.h>

namespace Network {

class MqttOtaHandler {
public:
  // begin() is idempotent — called once at boot after mqttTransport.begin().
  void begin();

  // Entry point — called by MqttTransport's MessageCallback for messages
  // on the `plts/<deviceId>/ota` topic.
  void handle(const char* topic, const uint8_t* payload, size_t len);

  // Stats for diagnostics endpoint
  uint32_t getReceivedCount() const { return _received; }
  uint32_t getStartedCount() const { return _started; }
  uint32_t getRejectedCount() const { return _rejected; }
  uint32_t getDuplicateCount() const { return _duplicates; }

private:
  uint32_t _received = 0;
  uint32_t _started = 0;
  uint32_t _rejected = 0;
  uint32_t _duplicates = 0;

  void _publishAck(const char* transactionId, bool ok, const char* code,
                    const String& message);
};

extern MqttOtaHandler mqttOtaHandler;

} // namespace Network

#endif // PLTS_NETWORK_MQTT_OTA_HANDLER_H
