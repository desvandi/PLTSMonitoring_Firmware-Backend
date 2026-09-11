// =============================================================================
// Storage/ConfigStore.h — Atomic A/B + CRC32 persistence for config.json,
//                          calibration.json, energy counters (NVS),
//                          telemetry sequence (NVS).
// =============================================================================
#pragma once
#ifndef PLTS_STORAGE_CONFIG_STORE_H
#define PLTS_STORAGE_CONFIG_STORE_H

#include <Arduino.h>
#include "../Core/Types.h"
#include "../Core/Config.h"   // PLTS_ENABLE_* feature flags (v1.7.0 E-WAVE)

namespace Storage {

class ConfigStore {
public:
  // ---------- USER CONFIG (auth credentials) ----------
  void loadUserConfig();
  void saveUserConfig();
  void initDefaultUserConfig();  // random 16-char password via CSPRNG

  // ---------- DEVICE CONFIG (name, timezone, secrets) ----------
  void loadDeviceConfig();
  void saveDeviceConfig();

  // ---------- RUNTIME BATTERY CONFIG (capacity, thresholds, intervals) ----------
  void loadBatteryConfig();   // populates Core::cfg* globals
  // [STORAGE-GATE-04] bool = persistence verified; false must be surfaced by
  // callers (REST/MQTT ACK, ConfigUpdater) — never a silent success.
  bool saveBatteryConfig();

  // ---------- [PARITY-4] OPERATOR ALARM THRESHOLD CONFIG (NVS "plts_alarm") ----
  // Two-tier alarm thresholds (PWA AlarmThresholds schema names). Defaults-on-
  // read + range sanitize — old NVS images upgrade silently, same pattern as
  // the emergency config. Consumed live by Services::AnomalyDetector.
  void loadAlarmConfig();     // populates Core::cfgAlarm* globals
  void saveAlarmConfig();

#if PLTS_ENABLE_EMERGENCY
  // ---------- v1.7.0 E-WAVE EMERGENCY TRIGGER CONFIG (NVS "plts_emg") ----------
  // 13 fields, ranges mirror Code.gs EMERGENCY_CONFIG_FIELDS. Defaults-on-read
  // means old NVS images upgrade silently — no migration needed.
  void loadEmergencyConfig();   // populates Core::cfgEmg* globals
  void saveEmergencyConfig();
#endif

  // ---------- CALIBRATION ----------
  void loadCalibration();
  void saveCalibration(bool force = false);
  void initDefaultCalibration();
  void markCalibrationDirty();
  void clearCalibrationDirty();

  // ---------- ENERGY COUNTERS (NVS) ----------
  void loadEnergyFromNVS();
  void saveEnergyToNVS();

  // ---------- TELEMETRY SEQUENCE (NVS) ----------
  uint32_t loadTelemetrySequence();
  void saveTelemetrySequence(uint32_t seq);

  // ---------- EXPORT / IMPORT ----------
  String exportAll();
  bool importAll(const String& json);
};

extern ConfigStore config;

} // namespace Storage

#endif // PLTS_STORAGE_CONFIG_STORE_H
