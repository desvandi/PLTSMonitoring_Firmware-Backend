// =============================================================================
// Network/MqttTransport.h — TLS connection, reconnect, per-device ACL
// -----------------------------------------------------------------------------
// Brief §38, §39 + REMEDIATION 2026-08 (Audit #1+#2+#3 — FW-02, FW-03, FW-21):
//   - TLS port from MQTT_BROKER_PORT (was hardcoded 8883, ignoring the env)
//   - MQTT_ROOT_CA loaded into WiFiClientSecure when provided; explicit,
//     clearly-marked setInsecure() ONLY in DEVELOPMENT_BUILD (P0-005 style
//     single trust boundary — no silent insecure TLS in production)
//   - Subscription persistence: topics registered via subscribe() are stored
//     and RE-SUBSCRIBED on every successful (re)connect — the command
//     channel previously died permanently after the first reconnect
//   - Command/config/OTA topics at QoS 1 (at-least-once + broker journaling);
//     telemetry at QoS 0
//   - LWT: plts/<deviceId>/online = "0" (retained, QoS 1) announced at
//     CONNECT time; "1" published only AFTER subscriptions are verified
//   - ONLINE gating: isFullyOperational() == connected && subscriptionsOK
//     (transport no longer declared operational merely because TCP/TLS is up)
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_MQTT_TRANSPORT_H
#define PLTS_NETWORK_MQTT_TRANSPORT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <functional>
#include "../Core/Config.h"

namespace Network {

// Connection state machine exposed for diagnostics (P1-020-style idempotent
// reconnect: state transitions are logged once, never spammed).
enum class MqttConnState : uint8_t {
  Disabled = 0,
  Disconnected,
  Connecting,
  Connected,     // TCP/TLS + CONNACK ok — subscriptions not yet verified
  Online,        // subscriptions verified — safe to publish/consume commands
};

class MqttTransport {
public:
  typedef std::function<void(const char* topic, const uint8_t* payload, size_t len)> MessageCallback;

  static constexpr uint8_t MAX_SUBSCRIPTIONS = 8;

  MqttTransport() : _client(_tls) {}

  void begin();
  void tick();  // called from NetworkTask
  bool isConnected() const { return _client.connected(); }
  bool isFullyOperational() const { return _state == MqttConnState::Online; }
  MqttConnState getState() const { return _state; }
  const char* getStateStr() const;
  void setMessageCallback(MessageCallback cb) { _msgCb = cb; }

  // QoS-aware publish. qos: 0 (telemetry/fire-and-forget) or 1 (commands/acks).
  bool publish(const char* topic, const char* payload, size_t len,
               bool retained = false, uint8_t qos = 0);
  // Register a topic for persistent subscription (re-established on every
  // reconnect). qos 1 for command topics. Returns false when the table is full.
  bool subscribe(const char* topic, uint8_t qos = 1);
  void setDeviceId(const char* id) { _deviceId = id; }
  String getDeviceTopic(const char* suffix) const;
  void onMessage(char* topic, uint8_t* payload, unsigned int len);

  // Diagnostics (P2 observability): counters are real runtime state.
  uint32_t reconnectCount() const { return _reconnectCount; }
  uint32_t publishFailCount() const { return _publishFailCount; }
  uint8_t  subscriptionCount() const { return _subCount; }
  bool     subscriptionsVerified() const { return _subsVerified; }

private:
  WiFiClientSecure _tls;
  mutable PubSubClient _client;
  String _deviceId;
  unsigned long _lastReconnectMs = 0;
  uint16_t _reconnectDelayMs = Core::MQTT_RECONNECT_MIN_MS;
  bool _initialized = false;
  MessageCallback _msgCb;
  MqttConnState _state = MqttConnState::Disabled;

  // Persistent subscription table (FW-03)
  char _subTopics[MAX_SUBSCRIPTIONS][96] = {};
  uint8_t _subQos[MAX_SUBSCRIPTIONS] = {};
  uint8_t _subCount = 0;
  bool _subsVerified = false;

  uint32_t _reconnectCount = 0;
  uint32_t _publishFailCount = 0;

  void _reconnect();
  bool _resubscribeAll();
};

extern MqttTransport mqttTransport;

} // namespace Network

#endif // PLTS_NETWORK_MQTT_TRANSPORT_H
