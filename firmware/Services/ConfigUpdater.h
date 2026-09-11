// =============================================================================
// Services/ConfigUpdater.h — [PRODUCTION-GRADE 2026-09] two-phase config mutation
// -----------------------------------------------------------------------------
// [audit p.274-279, BLOCKER B] Both the REST path (Web::ConfigHandlers) and the
// MQTT path (MqttConfigReceiver::_applyCommand) mutated Core::cfg* globals
// field-by-field as they validated. A request that failed validation on a
// LATER field (or a cross-field tier-order check) returned HTTP 400 / NACK
// while EARLIER fields were already live in RAM — a rejected request left the
// runtime config mutated ("misleading failure semantics": AnomalyDetector, SOC
// and BMS kept running on values the operator believed were rejected).
//
// This service implements the audit's two-phase contract:
//   PHASE 1 — parse + range-validate EVERY present field into a CANDIDATE
//             struct (zero RAM mutation), then cross-field-validate the
//             candidate (absent fields default to current RAM values).
//   PHASE 2 — only a fully-valid candidate is committed: Core:: globals,
//             NVS persistence, live-apply flags.
//
// REST and MQTT both route through applyUpdate() — one validation law for
// both ingresses (audit p.129: single ConfigValidator).
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_CONFIG_UPDATER_H
#define PLTS_SERVICES_CONFIG_UPDATER_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace Services {

class ConfigUpdater {
public:
  struct Result {
    bool ok = false;
    String message;
    bool bmsConfigChanged = false;   // caller must Comm::batteryComm.reconfigure()
    bool alarmConfigChanged = false; // caller must saveAlarmConfig() (done here)
  };

  /// Two-phase mutation of the runtime battery/BMS/alarm config from a
  /// canonicalized config.update document. Zero RAM mutation until every
  /// present field AND every cross-field invariant has passed.
  static Result applyUpdate(JsonDocument& doc);
};

} // namespace Services

#endif // PLTS_SERVICES_CONFIG_UPDATER_H
