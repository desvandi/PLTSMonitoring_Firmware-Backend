// =============================================================================
// Network/GasOtaReporter.h — [PARITY-3 AUDIT 2026-09-06] GAS OTA_STATUS bridge
// -----------------------------------------------------------------------------
// The modular tree's OTA lifecycle used to be observable ONLY via MQTT
// plts/<id>/ota/event + the in-RAM /api/ota/history ring — the PWA's
// "Authoritative — GAS OTA_LOG" panel stayed empty forever because nothing
// bridged lifecycle events into the GAS OtaEvents sheet. OtaManager.h
// documented this hole explicitly: "the modular tree reports locally until a
// GAS OTA_STATUS bridge exists". firmware-generic has had its OTA_STATUS
// reporter since WAVE-6; this is the modular equivalent, closing the
// three-layer gap (backend stores it, PWA displays it, firmware now feeds it).
// [audit p.432] ota/event is a PubSubClient QoS-0 publish (socket write,
// not broker-acknowledged); this HMAC GAS bridge is the durable record.
//
// Contract (HMAC envelope byte-identical with GasAdvisor /
// GasEmergencyChannel — contract v2.1, verified by Code.gs verifyHmac_):
//   POST {"action":"OTA_STATUS","auth":{"method":"HMAC-SHA256",
//         "timestamp":T,"nonce":N,"deviceId":D,"signature":S},
//         "data":"{\"event\":\"...\",\"version\":\"...\",\"message\":\"...\"}"}
//   <-    {"status":"SUCCESS","code":200,...}
// Code.gs otaLogStatus_ accepts the full modular lifecycle vocabulary
// (ACCEPTED/DOWNLOADING/VERIFIED/FLASHED/ACTIVATED/ROLLBACK/FAILED) since
// PARITY-3 — one word list on both sides of the wire.
//
// Threading: events are pushed from MULTIPLE tasks (web server REST OTA
// handlers, MQTT ota callback, otaTask download pump, loopTask boot
// confirmation) — the ring is protected by a portMUX critical section and
// report() NEVER blocks. The blocking TLS flush runs ONLY inside otaTask
// (2 Hz pump, 7 s HTTP cap — same isolation rationale as gasEmergencyTask:
// a stalled POST must never break MQTT keepalive or the web server).
//
// Honesty: this channel is best-effort observability, NEVER a safety or
// release gate. Events dropped after MAX_ATTEMPTS are logged to the device
// log (the same record MqttTelemetryPublisher keeps when MQTT is down).
// OTA itself (download, verify, flash, rollback) does not depend on it.
// =============================================================================
#pragma once
#ifndef PLTS_NETWORK_GAS_OTA_REPORTER_H
#define PLTS_NETWORK_GAS_OTA_REPORTER_H

// [W12-fix class] Config.h comes BEFORE everything: include order must never
// decide whether a feature exists.
#include "../Core/Config.h"

#include <Arduino.h>
#include <cstdint>

namespace Network {

class GasOtaReporter {
public:
  GasOtaReporter() = default;

  // Fail-closed (logged once) when GAS_INGEST_URL is empty, the device
  // secret is empty, or deviceId is unset — mirroring GasAdvisor and
  // GasEmergencyChannel gate conditions.
  void begin();

  // Non-blocking enqueue from ANY task. Fixed-size slots (no heap inside
  // the critical section). When the ring is full the OLDEST entry is
  // dropped with a device-log line — terminal states arrive last, so a
  // saturated ring still surfaces the final outcome.
  void report(const char* state, const char* version, const char* detail);

  // otaTask pump (2 Hz). WiFi-gated; HMAC needs a synced clock (unsynced
  // time signs garbage), so events stay queued until NTP converges. Flushes
  // ONE event per call, MIN_FLUSH_INTERVAL_MS apart, with failure backoff.
  void tick();

  bool    isEnabled() const { return _enabled; }
  // NOTE: not const — portENTER_CRITICAL requires a non-const spinlock*
  // (ESP-IDF portMUX semantics).
  uint8_t pendingCount();

  // [PRODUCTION-GRADE 2026-09 / audit p.26, p.115 — WDT vs blocking TLS]
  // INVARIANT (must remain true): worst-case flushOne() runtime
  //   = TLS handshake timeout (HTTP_TIMEOUT_MS)
  //   + HTTP request timeout (HTTP_TIMEOUT_MS)
  //   = 2 × HTTP_TIMEOUT_MS
  // must stay STRICTLY BELOW the task-WDT window (esp_task_wdt_init(10 s)
  // in firmware_v1.ino). 4000 ms × 2 = 8 s < 10 s. The single
  // esp_task_wdt_reset() before http.POST() can only buy ONE WDT interval —
  // with the old 7 s timeouts the worst case (14 s) could outrun the
  // watchdog mid-call and reset the device during an OTA report. Changing
  // this value requires re-verifying the invariant against the TWDT config.
  static constexpr uint32_t HTTP_TIMEOUT_MS        = 4000;
  static constexpr uint32_t MIN_FLUSH_INTERVAL_MS  = 5000;
  static constexpr uint8_t  MAX_ATTEMPTS_PER_EVENT = 20;
  static constexpr uint8_t  RING_SIZE              = 6;

private:
  struct Event {
    char state[20];    // "ACCEPTED"..."ROLLBACK" (longest: "VERIFICATION_FAILED" is NOT sent; GAS-side mapping)
    char version[24];  // semver + slack
    char detail[112];  // honest reason string (truncated)
  };

  // Blocking TLS POST of ONE event. Returns true when GAS accepted it
  // (HTTP 200 + status SUCCESS). GAS-side permanent rejection (non-success
  // body) also returns true — the event is consumed, the reason is logged.
  bool flushOne();

  bool     _enabled = false;
  bool     _disableLogged = false;

  // Ring state — guarded by a critical section (multi-task producers).
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  Event    _ring[RING_SIZE];
  uint8_t  _head = 0;         // next write slot
  uint8_t  _count = 0;
  uint8_t  _attempts = 0;     // attempts spent on the current head event
  uint32_t _lastFlushMs = 0;
  uint8_t  _flushFails = 0;   // cadence backoff 5 -> 15 -> 45 s
};

extern GasOtaReporter gasOtaReporter;

} // namespace Network

#endif // PLTS_NETWORK_GAS_OTA_REPORTER_H
