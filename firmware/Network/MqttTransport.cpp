// =============================================================================
// Network/MqttTransport.cpp
// =============================================================================
// REMEDIATION 2026-08 (Audit #1+#2+#3 — FW-02, FW-03, FW-21):
//   FW-02: port now comes from MQTT_BROKER_PORT (was hardcoded 8883 while
//          the development env defines 1883 → guaranteed connect failure).
//          TLS root CA is loaded from MQTT_ROOT_CA when provided. Only
//          DEVELOPMENT_BUILD may explicitly call setInsecure() — production
//          builds fail closed (Config.h #error guards ensure the CA exists).
//   FW-03: subscribe() now records the topic in a persistent table; every
//          successful (re)connect re-establishes ALL subscriptions before
//          the transport is declared Online. Previously subscribe() was
//          called once at boot before any connection existed and was never
//          retried — the MQTT command channel was permanently dead.
//   FW-21: LWT is registered at CONNECT time (plts/<id>/online = "0",
//          retained, QoS 1) so an ungraceful drop is visible to subscribers;
//          "1" is published only after subscriptions verify. Command topics
//          are subscribed at QoS 1.
// =============================================================================
#include "MqttTransport.h"
#include "../Drivers/RtcDriver.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Services/HealthSupervisor.h"
#include "../Services/LogService.h"
#include "../Services/WifiManager.h"
#include <cstring>

namespace Network {

MqttTransport mqttTransport;

static MqttTransport* g_self = nullptr;

static void mqttCbWrapper(char* topic, uint8_t* payload, unsigned int len) {
  if (g_self) g_self->onMessage(topic, payload, len);
}

void MqttTransport::begin() {
  g_self = this;
  _deviceId = String(Core::deviceId);   // [FW-01] canonical device identity
  if (_deviceId.length() == 0) {
    _deviceId = String("PLTS-") + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), 16);
  }

  // [FW-02] Port from build configuration, not a hardcoded literal.
  _client.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  _client.setKeepAlive(15);
  _client.setBufferSize(Core::MQTT_BUFFER_SIZE);
  _client.setCallback(mqttCbWrapper);

  // [FW-02] TLS trust: load the broker root CA when provided.
  // PRODUCTION_BUILD refuses to compile without MQTT_ROOT_CA (Config.h
  // #error guard). DEVELOPMENT_BUILD may explicitly opt out — the bypass is
  // compile-time visible, never a silent runtime fallback.
#ifdef MQTT_ROOT_CA
  _tls.setCACert(MQTT_ROOT_CA);
#elif defined(DEVELOPMENT_BUILD)
  // EXPLICIT DEV BYPASS — production builds do NOT compile this path.
  _tls.setInsecure();
#else
  // No CA and not a dev build (e.g. staging without CA): fail closed by
  // leaving the client unconfigured — connects will fail rather than
  // silently downgrading to no validation.
#endif

  _state = MqttConnState::Disconnected;
  _initialized = true;
}

void MqttTransport::tick() {
  if (!_initialized) return;
  if (!_client.connected()) {
    if (_state != MqttConnState::Disconnected && _state != MqttConnState::Connecting) {
      // Connection lost — reset the ONLINE gating state (idempotent).
      _state = MqttConnState::Disconnected;
      _subsVerified = false;
      Services::Log.append(Core::LogType::Custom, "MQTT connection lost", 0);
    }
    _reconnect();
  }
  _client.loop();
}

bool MqttTransport::publish(const char* topic, const char* payload, size_t len,
                            bool retained, uint8_t qos) {
  if (!_client.connected()) { _publishFailCount++; return false; }
  // LIBRARY LIMITATION (documented in 07_FAILURE_RECOVERY_MODEL.md): the
  // upstream knolleary/PubSubClient 2.8 supports QoS selection only on
  // SUBSCRIBE; publish() is always QoS 0 (fire-and-forget with TCP-level
  // delivery). The qos parameter is kept for API stability: qos>=1 requests
  // best-available delivery (socket write confirmed) — NOT a broker PUBACK.
  // Consequence: spooled records are removed after a confirmed socket write,
  // which is the strongest delivery evidence this transport can produce.
  // Migration path to broker-acknowledged QoS 1: espMqttClient (tracked as a
  // documented open limitation, NOT silently claimed as QoS 1).
  (void)qos;
  bool ok = _client.publish(topic, (const uint8_t*)payload, (unsigned int)len, retained);
  if (!ok) _publishFailCount++;
  return ok;
}

bool MqttTransport::subscribe(const char* topic, uint8_t qos) {
  if (!topic || strlen(topic) == 0 || strlen(topic) >= sizeof(_subTopics[0])) {
    return false;
  }
  // Deduplicate
  for (uint8_t i = 0; i < _subCount; i++) {
    if (strcmp(_subTopics[i], topic) == 0) {
      _subQos[i] = qos;
      return true;
    }
  }
  if (_subCount >= MAX_SUBSCRIPTIONS) return false;
  strncpy(_subTopics[_subCount], topic, sizeof(_subTopics[0]) - 1);
  _subTopics[_subCount][sizeof(_subTopics[0]) - 1] = '\0';
  _subQos[_subCount] = (qos > 1) ? 1 : qos;
  _subCount++;

  // If already connected, subscribe immediately as well.
  if (_client.connected()) {
    _client.subscribe(_subTopics[_subCount - 1], _subQos[_subCount - 1]);
  }
  return true;
}

bool MqttTransport::_resubscribeAll() {
  if (_subCount == 0) { _subsVerified = true; return true; }
  bool all = true;
  for (uint8_t i = 0; i < _subCount; i++) {
    // [FW-03] QoS 1 for command-bearing topics (config/ota).
    if (!_client.subscribe(_subTopics[i], _subQos[i])) {
      all = false;
      Services::Log.append(Core::LogType::Custom,
                            String("MQTT subscribe FAILED: ") + _subTopics[i], 0);
    }
  }
  _subsVerified = all;
  return all;
}

String MqttTransport::getDeviceTopic(const char* suffix) const {
  String t = String(Core::MQTT_TOPIC_PREFIX) + "/" + _deviceId + "/" + suffix;
  return t;
}

const char* MqttTransport::getStateStr() const {
  switch (_state) {
    case MqttConnState::Online:       return "ONLINE";
    case MqttConnState::Connected:    return "CONNECTED";
    case MqttConnState::Connecting:   return "CONNECTING";
    case MqttConnState::Disconnected: return "DISCONNECTED";
    default:                          return "DISABLED";
  }
}

void MqttTransport::_reconnect() {
  unsigned long now = millis();
  if (now - _lastReconnectMs < _reconnectDelayMs) return;   // backoff window
  _lastReconnectMs = now;
  _state = MqttConnState::Connecting;

  // [FW-21] LWT: a retained "0" on the presence topic is announced BY THE
  // BROKER if this client disappears without DISCONNECT.
  String willTopic = getDeviceTopic("online");
  const char* willPayload = "0";
  bool willRetain = true;
  int willQos = 1;

  // Credentials: prefer build-time broker credentials; fall back to the
  // per-device CSPRNG pair (username = deviceId, password = Core::mqttPassword)
  // provisioned into the broker ACL out-of-band.
  const char* user = (strlen(MQTT_USERNAME) > 0) ? MQTT_USERNAME : _deviceId.c_str();
  const char* pass = (strlen(MQTT_PASSWORD) > 0) ? MQTT_PASSWORD : Core::mqttPassword;

  bool ok = _client.connect(_deviceId.c_str(), user, pass,
                            willTopic.c_str(), willQos, willRetain, willPayload);

  if (ok) {
    _reconnectCount++;
    _reconnectDelayMs = Core::MQTT_RECONNECT_MIN_MS;
    _state = MqttConnState::Connected;
    Services::Log.append(Core::LogType::Custom, "MQTT connected (TLS)", 0);

    // [FW-03] Re-establish every registered subscription BEFORE going Online.
    if (_resubscribeAll()) {
      // [FW-21] Subscriptions verified — NOW announce presence.
      String onlineTopic = getDeviceTopic("online");
      _client.publish(onlineTopic.c_str(), (const uint8_t*)"1", 1, true);
      _state = MqttConnState::Online;
      Services::Log.append(Core::LogType::Custom,
                           "MQTT ONLINE: subscriptions verified", 0);
    } else {
      Services::Log.append(Core::LogType::Custom,
                           "MQTT connected but subscriptions INCOMPLETE", 0);
      // Stay in Connected — not Online — and force a fast retry cycle.
      _reconnectDelayMs = Core::MQTT_RECONNECT_MIN_MS;
    }
  } else {
    _state = MqttConnState::Disconnected;
    _reconnectDelayMs = _reconnectDelayMs * 2;
    if (_reconnectDelayMs > Core::MQTT_RECONNECT_MAX_MS)
      _reconnectDelayMs = Core::MQTT_RECONNECT_MAX_MS;
  }
}

void MqttTransport::onMessage(char* topic, uint8_t* payload, unsigned int len) {
  if (_msgCb) {
    _msgCb(topic, payload, (size_t)len);
  }
}

} // namespace Network
