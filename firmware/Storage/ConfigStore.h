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
  void saveBatteryConfig();

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
