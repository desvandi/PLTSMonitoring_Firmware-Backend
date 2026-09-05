#ifndef PLTS_GLOBALS_H
#define PLTS_GLOBALS_H

//=============================================================================
// PLTS Monitor — Global shared state declarations
// All shared state is mutex-protected. Brief §79: single source of truth.
//
// Phase 13-B changes:
//   - RC-8: cfg* runtime configuration globals declared as extern here.
//           Defined in Storage/ConfigStore.cpp. Populated by loadBatteryConfig().
//   - RC-11: Removed duplicate HealthSnapshot (canonical in Services/HealthSupervisor.h),
//            removed duplicate Alarm (canonical in Services/AlarmRegistry.h),
//            removed duplicate VoltageCalibrationPoint (canonical CalibrationPoint
//            in Core/Types.h). Globals.h now only contains shared task-communication
//            structs + extern declarations, not domain types.
//   - RC-9: Global instance externs now reference the canonical names defined in
//           each .cpp (ina219Battery, batteryAdc, acs712, sht31 — NOT the phantom
//           names declared in the old .ino).
//=============================================================================

#include "Core/Config.h"
#include "Core/Types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// Forward declarations of driver/service classes
namespace Drivers {
  class Ina219Driver;
  class AdcVoltageDriver;
  class Acs712Driver;
  class Sht31Driver;
  class RtcDriver;
#if PLTS_ENABLE_EMERGENCY
  class EmergencyRelayDriver;   // v1.7.0 E-WAVE port
#endif
#if PLTS_ENABLE_PZEM_AC
  class Pzem004tDriver;         // v1.7.0 — optional AC power meter
#endif
}
namespace Services {
  // Forward declare the Alarm struct (defined in Services/AlarmRegistry.h)
  // so that SystemStatus can hold a pointer to it.
  struct Alarm;
  class VoltageCalibration;
  class SocStateMachine;
  class EnergyCounterService;
  class AcMeasurement;
  class AnomalyDetector;
  class AlarmRegistry;
  class HealthSupervisor;
  class TelemetrySpool;
  class TransactionJournal;
  class CommandCanonicalizer;
  class AuthManager;
  class OtaManager;
  class LogService;
  class WifiManager;
  class TimeManager;
#if PLTS_ENABLE_EMERGENCY
  class EmergencySupervisor;    // v1.7.0 E-WAVE port
#endif
}
namespace Comm {
  class BatteryCommManager;   // v1.6.0 multi-protocol BMS/inverter comm
}
namespace Network {
  class MqttTransport;
  class MqttTelemetryPublisher;
  class MqttConfigReceiver;
  class MqttOtaHandler;
#if PLTS_ENABLE_EMERGENCY
  class GasEmergencyChannel;    // v1.7.0 E-WAVE port
#endif
}
namespace AI {
  class GasAdvisor;
}
namespace Web {
  class HttpServer;
}
namespace Storage {
  class ConfigStore;
}

namespace Core {

// ===========================================================================
// RUNTIME CONFIGURATION GLOBALS (RC-8)
// Populated by Storage::ConfigStore::loadBatteryConfig() at boot.
// These are in-RAM copies of persistent config for fast access by services.
// Compile-time defaults in Config.h are fallbacks only.
// ===========================================================================
extern float    cfgBatteryCapacityAh;
extern float    cfgBatteryNominalVoltage;
extern float    cfgFullVoltage;
extern float    cfgLowVoltage;
extern float    cfgIdleCurrentThreshold;
extern float    cfgFullChargeCurrentThreshold;
extern uint32_t cfgFullChargePersistenceSec;
extern uint32_t cfgTelemetryIntervalSec;
extern uint32_t cfgBmsPollIntervalMs;      // v1.6.0 BMS polling period
extern char     cfgBmsProtocol[16];        // "auto"|"none"|protocol id
extern uint8_t  cfgBmsModbusSlaveId;       // Modbus RTU/TCP unit id
extern char     cfgBmsModbusTcpHost[64];   // empty = Modbus TCP slot off
extern uint16_t cfgBmsModbusTcpPort;       // default 502
#if PLTS_ENABLE_EMERGENCY
// v1.7.0 — E-WAVE emergency trigger config (persisted in NVS "plts_emg"
// via Storage::ConfigStore; GAS EMERGENCY_CONFIG command rewrites it after
// range validation on BOTH sides — ranges mirror Code.gs EMERGENCY_CONFIG_
// FIELDS byte-for-byte).
extern float    cfgEmgVbatLowV;         // [30,60] default 42
extern float    cfgEmgVbatLowHystV;     // [0.1,5] default 1
extern float    cfgEmgVbatHighV;        // [48,60] default 55
extern float    cfgEmgVbatHighHystV;    // [0.1,5] default 1
extern float    cfgEmgIDcOverA;         // [10,120] default 110
extern float    cfgEmgIAcLoadOverA;     // [5,40] default 28
extern float    cfgEmgIAcGenOverA;      // [5,40] default 28 (RESERVED channel)
extern uint8_t  cfgEmgDebounceN;        // [1,10] default 3
extern uint32_t cfgEmgRecoverySec;      // [0,3600] default 60
extern uint8_t  cfgEmgRelayPin;         // [12,39] default 27
extern int8_t   cfgEmgEstopPin;         // [-1,39] default 14 (-1 = disabled)
extern uint8_t  cfgEmgEstopEnabled;     // {0,1} default 1
extern uint8_t  cfgEmgSensorFailPolicy; // {0,1} default 1 (fail-closed)
#endif
// Calibration + timezone globals — declared after Calibration struct below

// ===========================================================================
// TASK COMMUNICATION STRUCTS
// These are internal to the task pipeline and do not appear in telemetry.
// ===========================================================================

// Sensor samples passed from sensor → measurement task
struct SensorSample {
  float    batteryCurrent;        // A (signed, post-correction) from INA219
  bool     batteryCurrentValid;
  uint16_t batteryVoltageRaw;     // ADC counts
  bool     batteryVoltageValid;
  float    acCurrent;             // RMS A from ACS712
  bool     acCurrentValid;
  struct { float temperature; float humidity; } environment;
  bool     environmentValid;
  uint32_t timestamp;             // Unix epoch seconds (wall-clock, for telemetry)
  uint32_t monotonicMs;           // millis() at sample time (for integration dt — RC-13)
  uint32_t sequence;
};

// Snapshot passed from measurement → energy/telemetry task
struct MeasurementSnapshot {
  Measurement batteryVoltage;
  Measurement batteryCurrent;
  Measurement batteryPower;
  Direction   direction;
  Measurement temperature;
  Measurement humidity;
  Measurement dewPoint;
  bool        condensationRisk;
  struct {
    Measurement rmsCurrent;
    Measurement peakCurrent;
    Measurement averageCurrent;
    Measurement estimatedPower;
    AcSignalQuality signalQuality;
  } ac;
  uint32_t timestamp;             // wall-clock, for telemetry
  uint32_t monotonicMs;           // millis(), for integration dt (RC-13)
  uint32_t sequence;
};

// ===========================================================================
// CALIBRATION STATE (RC-11: single canonical definition)
// Used by ConfigStore + VoltageCalibration service.
// ===========================================================================
struct Calibration {
  uint8_t           version;
  CalibrationPoint  voltageLow;
  CalibrationPoint  voltageNominal;
  CalibrationPoint  voltageFull;
  float             acs712Offset;
  float             acs712Sensitivity;
  float             sht31TempOffset;
  float             sht31HumOffset;
  uint32_t          timestamp;
  char              source[24];
};

// Globals for calibration + timezone — declared after Calibration struct
extern Calibration calibration;
extern char cfgTimezone[40];

// User auth config globals — populated by ConfigStore::loadUserConfig()
extern char wwwUser[33];
extern uint8_t salt[16];
extern char passHashHex[65];
extern uint16_t iterations;
extern char jwtSecret[65];
extern char mqttPassword[17];
extern char gasSecret[65];
extern char devicePin[7];
extern char deviceName[64];
extern char siteName[64];
extern char apPassword[33];

// ===========================================================================
// DEVICE CONFIG (runtime, persisted)
// ===========================================================================
struct DeviceConfig {
  uint8_t  version;
  uint32_t revision;
  uint32_t timestamp;
  char     source[24];
  char     checksum[65];
  char     deviceName[64];
  char     siteName[64];
  char     timezone[32];
  float    batteryCapacityAh;
  float    batteryNominalVoltage;
  float    fullVoltage;
  float    lowVoltage;
  float    idleCurrentThreshold;
  float    fullChargeCurrentThreshold;
  uint32_t fullChargePersistenceSec;
  uint32_t telemetryIntervalSec;
};

// ===========================================================================
// ROOT TELEMETRY (the canonical snapshot published every 5s)
// BatteryTelemetry, AcTelemetry, EnvironmentTelemetry, HealthSnapshot are
// NOT defined here — they belong to their respective service headers
// (RC-11: single canonical owner per type).
// ===========================================================================
struct SystemStatus {
  uint8_t  protocolVersion;
  const char* firmwareVersion;
  const char* deviceId;
  const char* deviceName;
  uint32_t sequence;
  uint32_t timestamp;
  TimeQuality timeQuality;
  uint32_t uptimeSeconds;
  uint32_t bootCount;
  const char* resetReason;
  // Sub-objects are filled by their respective services.
  // For now, keep them inline here to minimize Phase 13-B churn.
  // Phase 13-C+ may extract them into service-owned types.
  struct {
    Measurement voltage;
    Measurement current;
    Measurement power;
    Direction   direction;
    struct {
      float value;
      MeasurementQuality quality;
      MeasurementSource source;
      const char* method;
      uint32_t lastSync;
      const char* confidence;
      SocProvenance provenance;    // v1.6.0: BMS_DIRECT | SHUNT_COULOMB | OCV_ESTIMATED | UNKNOWN
    } soc;
    float remainingAh;
    float chargeAh;
    float dischargeAh;
    float chargeWh;
    float dischargeWh;
    float netWh;
    float efc;
    float estimatedUsableCapacityAh;
    float peakChargeCurrent;
    float peakDischargeCurrent;
    // [v1.9.0 / DYNAMIC-GAIN] INA219 PGA mode for telemetry visibility.
    // String: "80mV" (high-resolution standby) or "160mV" (peak load).
    // Null when INA219 unavailable (backwards-compat: consumers treat absent as UNKNOWN).
    const char* pgaMode;
  } battery;
  // v1.6.0 — external BMS/inverter comm snapshot (NaN-safe like the rest).
  // Populated from Comm::batteryComm by the energy task under telemetryMutex.
  struct {
    bool     connected;                 // manager LOCKED and data fresh
    const char* protocol;               // active protocol id string
    const char* state;                  // manager state string
    float    voltage;                   // V (NaN = not provided)
    float    current;                   // A, +charging (NaN = not provided)
    float    temperature;               // °C
    float    soh;                       // %
    float    cellVoltageMin;            // V
    float    cellVoltageMax;            // V
    uint16_t cellCount;
    float    chargeCurrentLimit;        // A (CCL)
    float    dischargeCurrentLimit;     // A (DCL)
    uint32_t cycleCount;
    uint16_t faultFlags;
    uint16_t moduleCount;
    uint32_t lastSeenMs;                // uptime ms of last valid reading
    float    currentMismatchA;          // |I_bms − I_shunt| (NaN = n/a)
  } bms;
  struct {
    Measurement rmsCurrent;
    Measurement peakCurrent;
    Measurement averageCurrent;
    struct {
      float value;
      MeasurementQuality quality;
      struct { float voltage; float powerFactor; } assumptions;
    } estimatedPower;
    AcSignalQuality signalQuality;
#if PLTS_ENABLE_PZEM_AC
    // v1.7.0 — REAL AC meter block (PZEM-004T, OPTIONAL). When connected,
    // power here is MEASURED (replaces the ACS712 estimate in reporting);
    // when absent, connected=false + null — never a silent swap. energy is
    // the meter's own cumulative counter (resets on METER power loss — NOT
    // integrated into the DC shunt energy counters, which stay canonical).
    struct {
      bool  connected;
      float voltage;        // V
      float current;        // A
      float power;          // W active
      float energy;         // Wh (meter counter)
      float frequency;      // Hz
      float powerFactor;    // 0..1
    } meter;
#endif
  } ac;
  struct {
    Measurement temperature;
    Measurement humidity;
    Measurement dewPoint;
    const char* label;
    bool condensationRisk;
  } environment;
  struct {
    SystemState systemState;
    struct {
      SensorHealth ina219;
      SensorHealth batteryAdc;
      SensorHealth acs712;
      SensorHealth sht31;
      SensorHealth bmsComm;      // v1.6.0 external comm manager health
    } sensorHealth;
    uint32_t taskHeartbeats[TASK_COUNT];
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    int8_t   wifiRssi;
    uint32_t wifiReconnectCount;
    bool     mqttConnected;
    bool     ntpSynced;
    bool     storageOk;
    uint8_t  spoolSize;
    AlarmSeverity highestAlarmSeverity;
  } health;
#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE emergency layer snapshot (written by
  // Services::emergency.publishStatus() under telemetryMutex at 10 Hz).
  // Absent in <= v1.6.3 payloads — consumers must treat absent as DISABLED.
  struct {
    const char* state;          // "RUN" | "EMERGENCY"
    const char* reason;         // BOOT|VBAT_LOW|VBAT_HIGH|I_DC_OVER|I_AC_LOAD_OVER|
                                // I_AC_GEN_OVER|SENSOR_LOSS|ESTOP|OPERATOR|CRASHLOOP|""
    bool     estopOpen;         // physical E-stop line OPEN (latched)
    bool     relayEnergized;    // true = kontaktor path CLOSED (system RUN)
    uint32_t trips;             // lifetime counter (NVS)
    uint32_t tripAtMs;          // uptime ms of last transition into EMERGENCY
    uint8_t  crashChain;        // consecutive unhealthy reboots
  } emergency;
#endif
  // Alarms are owned by AlarmRegistry (Services::Alarm); SystemStatus holds a pointer
  const Services::Alarm* activeAlarms;
  uint8_t activeAlarmCount;
  Calibration calibration;
  DeviceConfig config;
};

// ===========================================================================
// GLOBAL INSTANCES — at global scope (NOT inside namespace Core)
// Each .cpp defines the instance in its own namespace (Drivers::, Services::, etc.)
// Globals.h externs at global scope match these definitions.
// The .ino uses `using namespace Drivers; using namespace Services;` etc.
// ===========================================================================

// Helper functions (implemented in firmware_v1.ino or a Common.cpp)
float computeDewPoint(float tempC, float humidity);
bool  checkCondensationRisk(float tempC, float humidity);

} // namespace Core

extern Drivers::Ina219Driver              ina219Battery;
extern Drivers::AdcVoltageDriver           batteryAdc;
extern Drivers::Acs712Driver               acs712;
extern Drivers::Sht31Driver                sht31;
extern Drivers::RtcDriver                  rtc;
extern Services::VoltageCalibration       voltageCalibration;
extern Services::AcMeasurement             acMeasurement;
extern Services::SocStateMachine         socStateMachine;
extern Services::EnergyCounterService    energyCounters;
extern Services::AnomalyDetector&         anomalyDetector;
extern Services::AlarmRegistry&           alarms;
extern Services::HealthSupervisor&        health;
extern Storage::ConfigStore&               config;
extern Services::TelemetrySpool         telemetrySpool;
extern Services::TransactionJournal&      journal;
extern Services::CommandCanonicalizer&    canonicalizer;
extern Services::AuthManager&             auth;
extern Services::OtaManager&              ota;
extern Services::LogService&              Log;
extern Services::WifiManager&             wifi;
extern Services::TimeManager&             timeManager;
extern Network::MqttTransport&           mqttTransport;
extern Network::MqttTelemetryPublisher&   mqttTelemetry;
extern Network::MqttConfigReceiver&       mqttConfigReceiver;
extern Network::MqttOtaHandler&           mqttOtaHandler;
extern Comm::BatteryCommManager&          batteryComm;
extern AI::GasAdvisor&                    advisor;
extern Web::HttpServer&                   server;
#if PLTS_ENABLE_EMERGENCY
extern Drivers::EmergencyRelayDriver      emergencyRelay;   // v1.7.0 E-WAVE
extern Services::EmergencySupervisor      emergency;        // v1.7.0 E-WAVE
extern Network::GasEmergencyChannel       gasEmergency;     // v1.7.0 E-WAVE
#endif
#if PLTS_ENABLE_PZEM_AC
extern Drivers::Pzem004tDriver            pzemAc;           // v1.7.0 optional AC meter
#endif

// Core state globals — at global scope
extern SemaphoreHandle_t telemetryMutex;
extern Core::SystemStatus       latestStatus;
extern uint32_t                  telemetrySequence;
extern QueueHandle_t      sensorQueue;
extern QueueHandle_t      measurementQueue;
extern uint32_t                  bootCount;
extern uint8_t                   lastResetReason;

// Runtime config globals — inside namespace Core (to match .cpp references)
namespace Core {
  extern float    cfgBatteryCapacityAh;
  extern float    cfgBatteryNominalVoltage;
  extern float    cfgFullVoltage;
  extern float    cfgLowVoltage;
  extern float    cfgIdleCurrentThreshold;
  extern float    cfgFullChargeCurrentThreshold;
  extern uint32_t cfgFullChargePersistenceSec;
  extern uint32_t cfgTelemetryIntervalSec;
  extern Calibration calibration;
  extern char cfgTimezone[40];
  extern char wwwUser[33];
  extern uint8_t salt[16];
  extern char passHashHex[65];
  extern uint16_t iterations;
  extern char jwtSecret[65];
  extern char mqttPassword[17];
  extern char gasSecret[65];
  extern char devicePin[7];
  extern char deviceName[64];
  extern char siteName[64];
  extern char apPassword[33];
  extern bool calibrationDirty;
  extern char deviceId[17];
#if PLTS_ENABLE_EMERGENCY
  extern float    cfgEmgVbatLowV;
  extern float    cfgEmgVbatLowHystV;
  extern float    cfgEmgVbatHighV;
  extern float    cfgEmgVbatHighHystV;
  extern float    cfgEmgIDcOverA;
  extern float    cfgEmgIAcLoadOverA;
  extern float    cfgEmgIAcGenOverA;
  extern uint8_t  cfgEmgDebounceN;
  extern uint32_t cfgEmgRecoverySec;
  extern uint8_t  cfgEmgRelayPin;
  extern int8_t   cfgEmgEstopPin;
  extern uint8_t  cfgEmgEstopEnabled;
  extern uint8_t  cfgEmgSensorFailPolicy;
#endif
}

#endif // PLTS_GLOBALS_H
