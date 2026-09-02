// =============================================================================
// Network/GasEmergencyChannel.h — E-WAVE GAS command/event channel (HMAC)
// -----------------------------------------------------------------------------
// PORT of firmware-generic's checkEmergencyPending/emgAck/emgPostEventNow to
// the modular firmware, using the STRONGER WAVE-1 HMAC envelope (contract
// v2.1) instead of the legacy shared AUTH_TOKEN body:
//
//   POST {"action":"EMERGENCY_PENDING","auth":{"method":"HMAC-SHA256",
//         "timestamp":T,"nonce":N,"deviceId":D,"signature":S},"data":""}
//   <-    {"status":"SUCCESS","data":{"command_id","command","note","config"}}
//   POST EMERGENCY_ACK   data={"command_id","result","message","state"}
//   POST EMERGENCY_EVENT data={"type","reason","detail","state"}
//
// GAS-side auth: device secret from the DEVICES sheet + nonce replay cache +
// +/-300 s timestamp window; TLS pinned to GTS Root R4 (same precedence as
// AI::GasAdvisor). Code.gs accepts both token and HMAC clients for these
// actions ("accept both, exactly like OTA_STATUS") — verified in code.gs/
// Code.gs doPost() EMERGENCY_* dispatch.
//
// SECURITY vs firmware-generic (documented deltas):
//   * Per-device HMAC + nonce instead of one fleet-wide AUTH_TOKEN — a leaked
//     device secret cannot impersonate the fleet.
//   * Commands are idempotent by construction (ARM re-apply -> "already RUN",
//     DISARM re-apply -> still isolated, CONFIG re-apply -> same values), so
//     the GAS DELIVERED -> re-delivery window needs no device-side journal.
//   * GAS-side TTL (EMERGENCY_QUEUE_TTL_MIN, default 10 min) bounds staleness
//     of un-ACKed commands; expired rows are settled EXPIRED server-side.
//   * No TELEMETRY-response piggyback consumption (generic's second channel):
//     this firmware's dedicated 15 s poll already bounds command latency, and
//     the hourly GAS telemetry post makes piggyback nearly useless here.
//
// Runs in its own FreeRTOS task (gasEmergencyTask): a blocking TLS POST
// (7 s timeout) must never stall networkTask (MQTT keepalive, web server) or
// emergencyTask (E-stop polling, local triggers).
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_GAS_EMERGENCY_CHANNEL_H
#define PLTS_NETWORK_GAS_EMERGENCY_CHANNEL_H

#if PLTS_ENABLE_EMERGENCY

#include <Arduino.h>
#include <cstdint>
#include "../Core/Config.h"

namespace Network {

class GasEmergencyChannel {
public:
  GasEmergencyChannel() = default;

  // Fail-closed: disabled (and logged once) when GAS_INGEST_URL is empty, the
  // device secret is empty, or deviceId is unset. The supervisor keeps
  // running locally regardless — safety never depends on this channel.
  void begin();

  // 10 Hz pump (own task). WiFi-gated internally. Cadence:
  //   * poll EMERGENCY_PENDING every EMERGENCY_POLL_INTERVAL_MS (15 s)
  //   * flush queued EMERGENCY_EVENT, rate-limited 5 s, max 20 attempts
  void tick();

  bool isEnabled() const { return _enabled; }
  uint32_t lastPollMs() const { return _lastPollMs; }
  uint32_t lastCommandMs() const { return _lastCommandMs; }
  uint8_t  lastCommandResult() const { return _lastAckOk; }   // 1=APPLIED ACK sent

  static constexpr uint32_t EMERGENCY_POLL_INTERVAL_MS = 15000;
  static constexpr uint32_t EMERGENCY_HTTP_TIMEOUT_MS  = 7000;
  static constexpr uint32_t EMERGENCY_EVENT_MIN_INTERVAL_MS = 5000;
  static constexpr uint8_t  EMERGENCY_EVENT_MAX_TRIES  = 20;

private:
  String _signRequest(const char* action, uint32_t timestamp, const char* nonce,
                      const char* deviceId, const String& dataJson) const;
  String _generateNonce() const;
  int    _postEnvelope(const char* action, const String& dataJson, String& bodyOut);
  void   _pollPending();
  void   _flushEvent();
  bool   _sendAck(const String& commandId, const String& result,
                  const String& message);
  void   _applyPendingCommand(JsonVariantConst data);

  bool     _enabled       = false;
  bool     _disableLogged = false;
  uint32_t _lastPollMs    = 0;
  uint32_t _lastCommandMs = 0;
  uint8_t  _lastAckOk     = 0;
  uint8_t  _pollFails     = 0;      // bounded backoff 15->30->60 s
  // Event flush state (retry budget + rate limit — generic's dead 20-try
  // budget is REAL here: consume only after an HTTP 200).
  uint32_t _lastEventSentAtMs = 0;
  uint8_t  _eventTries        = 0;
  String   _eventInFlightType;
  String   _eventInFlightReason;
};

extern GasEmergencyChannel gasEmergency;

} // namespace Network

#endif // PLTS_ENABLE_EMERGENCY
#endif // PLTS_NETWORK_GAS_EMERGENCY_CHANNEL_H
