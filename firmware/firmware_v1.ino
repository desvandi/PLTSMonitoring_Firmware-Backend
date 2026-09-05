/*
 * PLTS Monitor — Production-Grade 48V LiFePO4 PLTS Monitoring System
 * Firmware v1.0.0 — Protocol v1
 *
 * Phase 13-B: Controlled Firmware Build Recovery
 *   - RC-1 (structural): .ino is now orchestration only, not God Object
 *   - RC-2: MqttConfigReceiver, MqttOtaHandler, GasAdvisor NOT referenced
 *           (deferred to Phase 13-F). No fake stubs created.
 *   - RC-3: Include paths corrected (Network/ not Services/ for MQTT)
 *   - RC-9: Uses canonical extern instances (Drivers::ina219Battery, Drivers::batteryAdc, etc.)
 *           No local instance declarations.
 *
 * Architecture (brief §78): FreeRTOS task-based, local-first, monitoring-only.
 *   - SensorTask      : samples INA219, ADC, ACS712, SHT31 at sensor-specific rates
 *   - MeasurementTask : voltage calibration, power calc, AC RMS, dew point
 *   - EnergyTask      : Ah/Wh integration, SOC update, EFC
 *                       TODO(PHASE-13-C): RC-13 — replace wall-clock dt with monotonic
 *   - TelemetryTask   : 5s publish + spool management
 *   - NetworkTask     : WiFi/MQTT/GAS/NTP
 *   - PersistenceTask : periodic NVS save (config, energy, SOC)
 *   - HealthTask      : Services::health supervision, alarm evaluation, anomaly detection
 *   - EmergencyTask   : [v1.7.0 E-WAVE] emergency supervisor, LOCAL-FIRST 10 Hz —
 *                       relay/E-stop/trigger evaluation run regardless of WiFi
 *   - GasEmergencyTask: [v1.7.0 E-WAVE] EMERGENCY_PENDING poll + ACK + events (HMAC)
 *
 * Brief §1: "Never fabricate certainty." Every measurement has value/unit/quality/source/timestamp/sequence.
 * Brief §75: Local-first — ESP32 remains operational when Internet/MQTT/GAS/PWA all OFF.
 * Brief §102 (v1.7.0 amendment): the firmware drives exactly ONE actuator — the
 *   E-WAVE safety relay (PLTS_ENABLE_EMERGENCY, default ON) — whose fail-safe
 *   direction is ISOLATED (GPIO Hi-Z at boot/crash = de-energized). Every other
 *   subsystem remains monitoring-only. With the flag OFF, this build is
 *   byte-equivalent in behavior to v1.6.3 (pure monitoring).
 */

#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <Preferences.h>
// [FW-20] ExtraHandlers — PWA-contract route parity (noteBootEvent)
#include "Web/ExtraHandlers.h"
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>

#include "Core/Config.h"
#include "Core/Types.h"
#include "Core/Common.h"

// Drivers MUST be included before Globals.h (forward-declared there)
#include "Drivers/Ina219Driver.h"
#include "Drivers/AdcVoltageDriver.h"
#include "Drivers/Acs712Driver.h"
#include "Drivers/Sht31Driver.h"
#include "Drivers/RtcDriver.h"
// v1.7.0 — E-WAVE emergency layer (ported from firmware-generic)
#if PLTS_ENABLE_EMERGENCY
#include "Drivers/EmergencyRelayDriver.h"
#endif
#if PLTS_ENABLE_PZEM_AC
#include "Drivers/Pzem004tDriver.h"   // v1.7.0 — optional real AC power meter
#endif

// Services MUST be included before Globals.h (forward-declared there)
#include "Services/VoltageCalibration.h"
#include "Services/SocStateMachine.h"
#include "Services/EnergyCounters.h"
#include "Services/AcMeasurement.h"
#include "Services/AnomalyDetector.h"
#include "Services/AlarmRegistry.h"
#include "Services/HealthSupervisor.h"
#include "Services/TelemetrySpool.h"
#include "Services/TransactionJournal.h"
#include "Services/CommandCanonicalizer.h"
#include "Services/AuthManager.h"
#include "Services/OtaManager.h"
#include "Services/LogService.h"
#include "Services/WifiManager.h"
#include "Services/TimeManager.h"
#if PLTS_ENABLE_EMERGENCY
#include "Services/EmergencySupervisor.h"
#endif

#include "Storage/FileSystem.h"
#include "Storage/ConfigStore.h"

// v1.6.0 — multi-protocol BMS/inverter communication layer
#include "Comm/BatteryProtocol.h"
#include "Comm/BatteryCommManager.h"
#if PLTS_ENABLE_RS485_CONSOLE
#include "Comm/Rs485Console.h"   // v1.7.0 — passive vendor-frame capture (bench)
#endif
#if PLTS_ENABLE_PYLONTECH_CAN
#include "Comm/PylontechCanClient.h"
#endif
#if PLTS_ENABLE_MODBUS_RTU
#include "Comm/ModbusRtuClient.h"
#endif
#if PLTS_ENABLE_MODBUS_TCP
#include "Comm/ModbusTcpClient.h"
#endif

#include "Utils/Crypto.h"
#include "Utils/Crc.h"
#include "Utils/Json.h"

#include "Network/MqttTransport.h"
#include "Network/MqttTelemetryPublisher.h"
#include "Network/MqttConfigReceiver.h"
#include "Network/MqttOtaHandler.h"
#if PLTS_ENABLE_EMERGENCY
#include "Network/GasEmergencyChannel.h"
#endif
#include "AI/GasAdvisor.h"

#include "Web/HttpServer.h"
#include "Web/BatteryStatusSerializer.h"

// Globals.h LAST — after all forward-declared classes are fully defined
#include "Core/Globals.h"

// Bring driver/service/network/web instance names into scope

// ---------------------------------------------------------------------------
// Shared state definitions (Globals.h declares externs at global scope)
// These are defined at global scope to match the .cpp file definitions.
// ---------------------------------------------------------------------------
SemaphoreHandle_t telemetryMutex = nullptr;
Core::SystemStatus       latestStatus = {};
uint32_t           telemetrySequence = 0;
QueueHandle_t      sensorQueue = nullptr;
QueueHandle_t      measurementQueue = nullptr;
uint32_t           bootCount = 0;
uint8_t            lastResetReason = 0;

// Runtime config globals — inside namespace Core to match .cpp references
namespace Core {
  float    cfgBatteryCapacityAh          = BATTERY_CAPACITY_AH;
  float    cfgBatteryNominalVoltage      = BATTERY_NOMINAL_V;
  float    cfgFullVoltage                 = BATTERY_FULL_V;
  float    cfgLowVoltage                   = BATTERY_LOW_V;
  float    cfgIdleCurrentThreshold        = IDLE_CURRENT_THRESHOLD_A;
  float    cfgFullChargeCurrentThreshold  = FULL_CHARGE_CURRENT_THRESHOLD_A;
  uint32_t cfgFullChargePersistenceSec    = FULL_CHARGE_PERSISTENCE_SEC;
  uint32_t cfgTelemetryIntervalSec        = TELEMETRY_INTERVAL_MS / 1000;
  // v1.6.0 — BMS/inverter comm config (persisted in NVS "plts_batt")
  uint32_t cfgBmsPollIntervalMs          = BMS_POLL_INTERVAL_MS;
  char     cfgBmsProtocol[16]            = "auto";   // BMS_PROTOCOL_DEFAULT
  uint8_t  cfgBmsModbusSlaveId           = BMS_MODBUS_SLAVE_ID;
  char     cfgBmsModbusTcpHost[64]       = "";       // empty = Modbus TCP off
  uint16_t cfgBmsModbusTcpPort           = BMS_MODBUS_TCP_PORT;
  // v1.7.0 — E-WAVE emergency trigger config (NVS "plts_emg", loaded by
  // Storage::config.loadEmergencyConfig(); GAS CONFIG command rewrites it)
#if PLTS_ENABLE_EMERGENCY
  float    cfgEmgVbatLowV        = EMG_VBAT_LOW_V;
  float    cfgEmgVbatLowHystV    = EMG_VBAT_LOW_HYST_V;
  float    cfgEmgVbatHighV       = EMG_VBAT_HIGH_V;
  float    cfgEmgVbatHighHystV   = EMG_VBAT_HIGH_HYST_V;
  float    cfgEmgIDcOverA        = EMG_IDC_OVER_A;
  float    cfgEmgIAcLoadOverA    = EMG_IAC_LOAD_OVER_A;
  float    cfgEmgIAcGenOverA     = EMG_IAC_GEN_OVER_A;
  uint8_t  cfgEmgDebounceN      = EMG_DEBOUNCE_N;
  uint32_t cfgEmgRecoverySec    = EMG_RECOVERY_SEC;
  uint8_t  cfgEmgRelayPin       = PIN_EMERGENCY_RELAY;
  int8_t   cfgEmgEstopPin       = PIN_EMERGENCY_ESTOP;
  uint8_t  cfgEmgEstopEnabled   = 1;
  uint8_t  cfgEmgSensorFailPolicy = EMG_SENSOR_FAIL_POLICY;
#endif
  Calibration calibration = {};
  char deviceId[17] = "";  // Generated from MAC at boot
  char cfgTimezone[40] = "Asia/Jakarta";
  char wwwUser[33] = "admin";
  uint8_t salt[16] = {0};
  char passHashHex[65] = {0};
  uint16_t iterations = PBKDF2_ITERATIONS;
  char jwtSecret[65] = {0};
  char mqttPassword[17] = {0};
  char gasSecret[65] = {0};
  char devicePin[7] = {0};
  char deviceName[64] = "PLTS Monitor";
  char siteName[64] = "Site A";
  char apPassword[33] = "PLTS-AP-PASSWORD";
  bool calibrationDirty = false;
}

// ---------------------------------------------------------------------------
// Forward declarations of task functions
// ---------------------------------------------------------------------------
void sensorTask(void* pv);
void measurementTask(void* pv);
void energyTask(void* pv);
void telemetryTask(void* pv);
void networkTask(void* pv);
void persistenceTask(void* pv);
void healthTask(void* pv);
void bmsCommTask(void* pv);   // v1.6.0 — BMS/inverter protocol manager
#if PLTS_ENABLE_EMERGENCY
void emergencyTask(void* pv);     // v1.7.0 — E-WAVE supervisor (local-first)
void gasEmergencyTask(void* pv);  // v1.7.0 — GAS command poll/ACK/events
#endif
#if PLTS_ENABLE_RELAYS
void relayTask(void* pv);         // v1.8.0 — 8-channel relay controller (5 Hz)
#endif
void publishTelemetry();
void printBootBanner();

// otaTask drives MQTT OTA download progress.
// (Phase 13-F: MqttOtaHandler now exists; otaTask pumps ota.tickDownload()
//  while OTA is in Downloading state.)
void otaTask(void* pv);

//=============================================================================
// SETUP — orchestration only (RC-1: .ino is not a God Object)
//=============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  printBootBanner();

  // Watchdog — 10s timeout, panic on timeout (brief §76)
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  // v1.7.0 [E-WAVE] — FAIL-SAFE FIRST: drive the emergency relay pin HIGH
  // (ISOLATED on the active-LOW module) BEFORE LittleFS/WiFi/anything that
  // can hang. ESP32 GPIOs are Hi-Z at reset -> module opto OFF -> isolated
  // anyway; this makes it EXPLICIT and covers the window until user code
  // runs (boot-isolation ordering is asserted by the mechanical test suite,
  // same as firmware-generic's E1).
#if PLTS_ENABLE_EMERGENCY
  Drivers::emergencyRelay.begin();
#endif

  // I2C bus — initialized ONCE here, shared by INA219 + SHT31
  Wire.begin(Core::PIN_I2C_SDA, Core::PIN_I2C_SCL, Core::I2C_FREQUENCY);

  // LittleFS (brief §44)
  if (!LittleFS.begin(true)) {
    Serial.println("[FATAL] LittleFS mount failed — formatting");
    LittleFS.format();
    LittleFS.begin(true);
  }
  esp_task_wdt_reset();

  // Storage — sole owner of persistence (RC-12, D-6)
  // ConfigStore doesn't have begin(); load each subsystem explicitly.
  Storage::config.loadUserConfig();
  Storage::config.loadDeviceConfig();
  Storage::config.loadBatteryConfig();  // populates cfg* runtime globals
#if PLTS_ENABLE_EMERGENCY
  Storage::config.loadEmergencyConfig(); // v1.7.0 — E-WAVE trigger config
#endif
  Storage::config.loadCalibration();
  Storage::config.loadEnergyFromNVS();
  telemetrySequence = Storage::config.loadTelemetrySequence();
  // bootCount is tracked by HealthSupervisor (NVS) — no ConfigStore.getBootCount()
  lastResetReason = (uint8_t)esp_reset_reason();

  // Generate deviceId from MAC address
  {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(Core::deviceId, sizeof(Core::deviceId), "PLTS-%06X", (uint32_t)(mac & 0xFFFFFF));
  }

  esp_task_wdt_reset();

  // Logging
  Services::Log.begin();
  Services::Log.append(Core::LogType::Boot, String("DEVICE_BOOT reason=") + Core::resetReasonStr(lastResetReason) + " bootCount=unknown");

  // Sensors — each driver owns its full pipeline (RC-1, D-1)
  Drivers::ina219Battery.begin();
  Drivers::batteryAdc.begin();
  Drivers::acs712.begin();
  Drivers::sht31.begin();
  Drivers::rtc.begin();
#if PLTS_ENABLE_PZEM_AC
  Drivers::pzemAc.begin();   // optional — presence proven by the first valid frame
#endif

  // Voltage calibration — inject into ADC driver
  // [FW-15 REMEDIATION 2026-08] Persisted calibration is now APPLIED AT BOOT.
  // Previously the values were loaded into Core::calibration but never
  // injected into the drivers until a manual REST POST — every reboot ran
  // on uncalibrated sensors while reporting calibrated-looking data.
  Services::voltageCalibration.begin();                          // 3-point → ADC
  Drivers::acs712.setZeroOffset(Core::calibration.acs712Offset); // zero-offset → ACS712
  // [v1.6.3 / audit-noise] SENSITIVITY now applied at boot too. The driver
  // class defaults to 100 mV/A (20A variant) while this system's module is
  // 185 mV/A — until this line, every reboot ran the AC channel at
  // 185/100 = 1.85x systematic error while Core::calibration held the
  // correct value (the zero-offset had this same bug class and was fixed
  // in 2026-08; sensitivity was the survivor).
  Drivers::acs712.setSensitivity(Core::calibration.acs712Sensitivity);
  Drivers::sht31.setTempOffset(Core::calibration.sht31TempOffset);
  Drivers::sht31.setHumOffset(Core::calibration.sht31HumOffset);

  // Core::Measurement services
  Services::socStateMachine.begin();
  Services::energyCounters.begin();
  Services::anomalyDetector.begin();

  // Health & Services::alarms
  Services::alarms.begin();
  Services::health.begin();
  Services::health.recordBoot();

#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE supervisor: crash-chain NVS accounting, BOOT/CRASHLOOP
  // event, relay pins from persisted config, LED init. Every boot re-enters
  // EMERGENCY — ARM is operator-only (never automatic).
  Services::emergency.begin();
#endif
#if PLTS_ENABLE_RELAYS
  // [v1.8.0] 8-channel relay controller — init AFTER emergency (so E-WAVE
  // isolation is guaranteed regardless of relay init outcome).
  Services::relaysController.begin();
#endif

  // Spool & Services::journal
  Services::telemetrySpool.begin();
  Services::journal.begin();
  // Services::canonicalizer doesn't have begin() — it's stateless
  // Services::journal.begin() already called above
  // Services::canonicalizer.begin();  // REMOVED — no such method

  // Auth & OTA
  Services::auth.begin();
  Services::ota.begin();

  // WiFi + time
  Services::wifi.begin();
  Services::timeManager.begin();   // NTP sync runs internally via tick() (no separate syncNtp call)

  // MQTT transport — telemetry publisher + config receiver + OTA handler
  // (RC-2 closure, Phase 13-F: all 3 modules now implemented)
  Network::mqttTransport.begin();
  Network::mqttTelemetry.begin();
  Network::mqttConfigReceiver.begin();
  Network::mqttOtaHandler.begin();

  // Install message router — fans out inbound messages by topic suffix.
  // (MqttTransport calls _msgCb for every received message; we route by suffix.)
  Network::mqttTransport.setMessageCallback(
    [](const char* topic, const uint8_t* payload, size_t len) {
      String t(topic);
      // Topic format: plts/<deviceId>/<suffix>
      int lastSlash = t.lastIndexOf('/');
      if (lastSlash < 0) return;
      String suffix = t.substring(lastSlash + 1);
      if (suffix == "config") {
        Network::mqttConfigReceiver.handle(topic, payload, len);
      } else if (suffix == "ota") {
        Network::mqttOtaHandler.handle(topic, payload, len);
      }
      // Other suffixes (status/log/ack/online) are publish-only — ignore.
    }
  );

  // Subscribe the config + OTA receivers to their topics.
  // [FW-03 REMEDIATION 2026-08] subscribe() records topics in MqttTransport's
  // persistent subscription table; every successful (re)connect re-establishes
  // ALL subscriptions (QoS 1) before the transport is declared Online. The old
  // TODO Phase 13-G (subscription persistence) is closed.
  {
    String configTopic = Network::mqttTransport.getDeviceTopic("config");
    String otaTopic = Network::mqttTransport.getDeviceTopic("ota");
    Network::mqttTransport.subscribe(configTopic.c_str(), 1);  // QoS 1 — commands
    Network::mqttTransport.subscribe(otaTopic.c_str(), 1);     // QoS 1 — OTA
  }

  // GAS advisor (AI insights proxy — HMAC to GAS)
  AI::advisor.begin();

#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE GAS channel (EMERGENCY_PENDING poll, 15 s). Fail-closed
  // when GAS_INGEST_URL / device secret are absent — the supervisor keeps
  // running locally regardless.
  Network::gasEmergency.begin();
#endif

  // HTTP server
  Web::server.begin();
  // [FW-20] Seed the in-RAM OTA history with this boot's record
  Web::ExtraHandlers::noteBootEvent();

  // [W13-1] OTA boot-health confirmation is driven from the 1 s main loop
  // (markBootHealthyIfPending): a fresh PENDING_VERIFY image is confirmed via
  // esp_ota_mark_app_valid_cancel_rollback() only after a 60 s stable window.
  // Confirming here in setup() would mark a crash-looping image healthy.

  // Mutexes & queues
  telemetryMutex = xSemaphoreCreateMutex();
  sensorQueue = xQueueCreate(16, sizeof(Core::SensorSample));
  measurementQueue = xQueueCreate(8, sizeof(Core::MeasurementSnapshot));

  // v1.6.0 — multi-protocol BMS/inverter communication manager.
  // Reads its NVS config (protocol override, slave id, TCP host) inside begin().
  // When cfgBmsProtocol = "none" the manager stays DISABLED and the system
  // behaves exactly like v1.5.0 (INA219/ACS712 shunt path only).
  Comm::batteryComm.begin();
#if PLTS_ENABLE_RS485_CONSOLE
  // v1.7.0 — passive RS485 capture console. Self-gated: only takes the UART
  // when cfgBmsProtocol == "rs485_console" (bench capture mode).
  Comm::rs485Console.begin();
#endif
  // [AUDIT v1.6.0] Zero-init makes floats 0.0 — the serializer would emit a
  // fake "0 V / 0 A BMS" in the window before the first energyTask cycle.
  // Initialize to NaN so the serializer emits null (honest "no data yet").
  latestStatus.bms.connected = false;
  latestStatus.bms.protocol = "NONE";
  latestStatus.bms.state = Comm::batteryComm.stateStr();
  latestStatus.bms.voltage = NAN;
  latestStatus.bms.current = NAN;
  latestStatus.bms.temperature = NAN;
  latestStatus.bms.soh = NAN;
  latestStatus.bms.cellVoltageMin = NAN;
  latestStatus.bms.cellVoltageMax = NAN;
  latestStatus.bms.cellCount = 0;
  latestStatus.bms.chargeCurrentLimit = NAN;
  latestStatus.bms.dischargeCurrentLimit = NAN;
  latestStatus.bms.cycleCount = 0;
  latestStatus.bms.faultFlags = 0;
  latestStatus.bms.moduleCount = 0;
  latestStatus.bms.lastSeenMs = 0;
  latestStatus.bms.currentMismatchA = NAN;
  latestStatus.battery.soc.provenance = Core::SocProvenance::Unknown;

#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE: seed the emergency block BEFORE tasks start so the
  // first REST/MQTT serialization is honest (publishStatus below refreshes
  // it now that telemetryMutex exists, then emergencyTask at 10 Hz).
  latestStatus.emergency.state          = "EMERGENCY";
  latestStatus.emergency.reason         = "BOOT";
  latestStatus.emergency.estopOpen      = Drivers::emergencyRelay.isEstopOpen();
  latestStatus.emergency.relayEnergized = false;
  latestStatus.emergency.trips          = Services::emergency.trips();
  latestStatus.emergency.tripAtMs       = 0;
  latestStatus.emergency.crashChain     = Services::emergency.crashChain();
  Services::emergency.publishStatus();
#endif

  // FreeRTOS tasks (brief §78 — 9 tasks: 8 system + 1 OTA driver)
  xTaskCreatePinnedToCore(sensorTask,       "sensor",      4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(measurementTask,  "measure",     4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(energyTask,       "energy",      4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(telemetryTask,    "telemetry",   6144, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(networkTask,      "network",     6144, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(persistenceTask,  "persist",      4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(healthTask,       "Services::health",       4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(otaTask,          "ota",         6144, NULL, 1, NULL, 0);  // [v1.6.3] 4K→6K: TLS + flash-write in one stack frame chain
  xTaskCreatePinnedToCore(bmsCommTask,      "bmscomm",     4096, NULL, 2, NULL, 0);
#if PLTS_ENABLE_EMERGENCY
  // v1.7.0 — E-WAVE: safety-critical tick at 10 Hz on core 0 (same core as
  // sensors, priority 3 = sensor tier); GAS network I/O on core 1 so a
  // blocking TLS POST (7 s cap) can never stall MQTT/web or the supervisor.
  xTaskCreatePinnedToCore(emergencyTask,    "emg",         4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(gasEmergencyTask, "gasemg",      6144, NULL, 2, NULL, 1);
#endif
#if PLTS_ENABLE_RELAYS
  xTaskCreatePinnedToCore(relayTask,        "relay",       4096, NULL, 2, NULL, 0);
#endif

  Serial.println("[BOOT] All tasks started. System ready.");
  Services::Log.append(Core::LogType::Info, String("SYSTEM_READY version=") + Core::FIRMWARE_VERSION);
}

//=============================================================================
// LOOP — minimal; work is done in tasks
//=============================================================================
void loop() {
  esp_task_wdt_reset();
  // [W13-1] OTA image confirmation: no-op (two compares) until the 60 s
  // window elapses, then confirms PENDING_VERIFY images exactly once per boot.
  Services::ota.markBootHealthyIfPending();
  vTaskDelay(pdMS_TO_TICKS(1000));
}

//=============================================================================
// SENSOR TASK — samples all sensors at sensor-specific rates (brief §39)
// RC-1: Uses canonical getReading() API, NOT readRaw()/convertToVolts()/filter()
//=============================================================================
void sensorTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t samplePeriod = pdMS_TO_TICKS(Core::SENSOR_SAMPLE_INTERVAL_MS);

  while (true) {
    esp_task_wdt_reset();
    Core::SensorSample sample = {};

    // INA219 — battery current (canonical: Drivers::ina219Battery.getReading())
    Drivers::Ina219Reading inaReading = Drivers::ina219Battery.getReading();
    sample.batteryCurrent = inaReading.currentA;
    sample.batteryCurrentValid = Drivers::ina219Battery.isAvailable() &&
                                  inaReading.status == Drivers::Ina219Status::Ok;
    Drivers::ina219Battery.tick();  // advance internal state machine for next read

    // ADC — battery voltage (canonical: Drivers::batteryAdc.getReading())
    Drivers::batteryAdc.tick();
    Drivers::AdcVoltageReading adcReading = Drivers::batteryAdc.getReading();
    sample.batteryVoltageRaw = adcReading.rawAdc;
    sample.batteryVoltageValid = Drivers::batteryAdc.isAvailable() &&
                                  adcReading.status == Drivers::AdcVoltageStatus::Ok;

    // SHT31 — temperature/humidity (1 Hz, throttled)
    static TickType_t lastSht31 = 0;
    if ((xTaskGetTickCount() - lastSht31) >= pdMS_TO_TICKS(Core::SHT31_SAMPLE_INTERVAL_MS)) {
      Drivers::sht31.tick();
      Drivers::Sht31Reading envReading = Drivers::sht31.getReading();
      sample.environment.temperature = envReading.temperatureC;
      sample.environment.humidity = envReading.humidityPct;
      sample.environmentValid = Drivers::sht31.isAvailable() &&
                                 envReading.status == Drivers::Sht31Status::Ok;
      lastSht31 = xTaskGetTickCount();
    }

    // ACS712 — AC current RMS (canonical: Drivers::acs712.getReading())
    Drivers::acs712.tick();
    Drivers::Acs712Reading acsReading = Drivers::acs712.getReading();
    sample.acCurrent = acsReading.rmsCurrentA;
    sample.acCurrentValid = Drivers::acs712.isAvailable() &&
                             acsReading.status == Drivers::Acs712Status::Ok;

    // RC-13: Capture BOTH wall-clock (for telemetry) AND monotonic (for integration)
    sample.timestamp = Services::timeManager.getUnixTime();   // wall-clock, for telemetry display
    sample.monotonicMs = millis();             // monotonic, for energy/SOC integration dt
    sample.sequence = telemetrySequence++;

    xQueueSendToBack(sensorQueue, &sample, 0);
    Services::health.recordHeartbeat(Core::TaskId::Ina219Battery);
    Services::health.recordHeartbeat(Core::TaskId::AdcVoltage);
    if (sample.environmentValid) Services::health.recordHeartbeat(Core::TaskId::Sht31);
    Services::health.recordHeartbeat(Core::TaskId::Acs712);

    vTaskDelayUntil(&lastWake, samplePeriod);
  }
}

//=============================================================================
// MEASUREMENT TASK — calibrate, compute derived values (brief §8, §10)
// RC-1: Uses driver Reading structs (already calibrated+filtered by driver)
//=============================================================================
void measurementTask(void* pv) {
  esp_task_wdt_add(NULL);
  Core::SensorSample sample;
  Core::MeasurementSnapshot snap;

  // Phase 13-D.1: Quality state tracking for runtime producers
  static uint32_t lastVoltageMonotonicMs = 0;
  static uint32_t lastCurrentMonotonicMs = 0;
  static float lastValidVoltage = 0.0f;
  static float lastValidCurrent = 0.0f;
  static bool calibratingVoltage = false;
  static bool calibratingAcs712 = false;
  static constexpr uint32_t STALE_THRESHOLD_MS = 15000;  // directive §6.1
  static constexpr float SUSPECT_VOLTAGE_JUMP = 10.0f;   // >10V jump = suspect
  static constexpr float SUSPECT_CURRENT_JUMP = 100.0f;  // >100A jump = suspect
  static constexpr uint8_t SUSPECT_RECOVERY_COUNT = 3;   // 3 clean samples to recover

  // Suspect recovery counters
  static uint8_t voltageSuspectRecovery = 0;
  static uint8_t currentSuspectRecovery = 0;
  static Core::MeasurementQuality voltageQualityState = Core::MeasurementQuality::NotAvailable;
  static Core::MeasurementQuality currentQualityState = Core::MeasurementQuality::NotAvailable;

  while (true) {
    esp_task_wdt_reset();
    if (xQueueReceive(sensorQueue, &sample, pdMS_TO_TICKS(1000)) == pdTRUE) {

      // =====================================================================
      // Phase 13-D.1: QUALITY RUNTIME WIRING — Battery Voltage
      // Producer: measurementTask (runtime firmware, NOT test simulation)
      // =====================================================================
      Core::Measurement vMeas;
      vMeas.timestamp = sample.timestamp;
      vMeas.sequence = sample.sequence;
      vMeas.source = Core::MeasurementSource::Measured;
      float filteredV = Drivers::batteryAdc.getFilteredV();

      if (!sample.batteryVoltageValid) {
        // Producer: SensorError — sensor communication failure
        vMeas.quality = Core::MeasurementQuality::SensorError;
        vMeas.value = NAN;
        voltageQualityState = Core::MeasurementQuality::SensorError;
        voltageSuspectRecovery = 0;
        Services::alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_INVALID, Core::AlarmSeverity::Critical,
                     "Battery voltage sensor error");
      } else if (std::isnan(filteredV) || std::isinf(filteredV)) {
        // Producer: SensorError — NaN/Inf from driver
        vMeas.quality = Core::MeasurementQuality::SensorError;
        vMeas.value = NAN;
        voltageQualityState = Core::MeasurementQuality::SensorError;
        voltageSuspectRecovery = 0;
      } else if (calibratingVoltage) {
        // Producer: Calibrating — calibration is active
        vMeas.quality = Core::MeasurementQuality::Calibrating;
        vMeas.value = NAN;  // Do NOT use value during calibration
        voltageQualityState = Core::MeasurementQuality::Calibrating;
        voltageSuspectRecovery = 0;
      } else if (filteredV < Core::VBAT_MIN_PLAUSIBLE || filteredV > Core::VBAT_MAX_PLAUSIBLE) {
        // Producer: OutOfRange — outside plausible range
        vMeas.quality = Core::MeasurementQuality::OutOfRange;
        vMeas.value = NAN;
        voltageQualityState = Core::MeasurementQuality::OutOfRange;
        voltageSuspectRecovery = 0;
        Services::alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_INVALID, Core::AlarmSeverity::Warning,
                     "Battery voltage out of plausible range");
      } else if (lastValidVoltage > 0.0f && std::fabs(filteredV - lastValidVoltage) > SUSPECT_VOLTAGE_JUMP) {
        // Producer: Suspect — excessive voltage jump detected
        vMeas.quality = Core::MeasurementQuality::Suspect;
        vMeas.value = filteredV;  // Retain value but mark as Suspect
        voltageQualityState = Core::MeasurementQuality::Suspect;
        voltageSuspectRecovery = 0;
      } else if (voltageQualityState == Core::MeasurementQuality::Suspect) {
        // Recovery: need SUSPECT_RECOVERY_COUNT clean samples
        voltageSuspectRecovery++;
        if (voltageSuspectRecovery >= SUSPECT_RECOVERY_COUNT) {
          vMeas.quality = Core::MeasurementQuality::Valid;
          vMeas.value = filteredV;
          voltageQualityState = Core::MeasurementQuality::Valid;
          lastValidVoltage = filteredV;
          lastVoltageMonotonicMs = sample.monotonicMs;
          Services::alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_INVALID);
        } else {
          vMeas.quality = Core::MeasurementQuality::Suspect;
          vMeas.value = filteredV;
        }
      } else if (lastVoltageMonotonicMs > 0 &&
                 (sample.monotonicMs - lastVoltageMonotonicMs) > STALE_THRESHOLD_MS) {
        // Producer: Stale — measurement is old (monotonic time, NOT wall-clock)
        // This check is on the CURRENT sample being fresh vs last valid sample.
        // If the current sample IS valid but a long time has passed since the
        // last valid sample, the previous state was Stale. Since we're receiving
        // a fresh sample now, transition to Valid.
        vMeas.quality = Core::MeasurementQuality::Valid;
        vMeas.value = filteredV;
        voltageQualityState = Core::MeasurementQuality::Valid;
        lastValidVoltage = filteredV;
        lastVoltageMonotonicMs = sample.monotonicMs;
        Services::alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_INVALID);
      } else {
        // Producer: Valid — normal valid measurement
        vMeas.quality = Core::MeasurementQuality::Valid;
        vMeas.value = filteredV;
        voltageQualityState = Core::MeasurementQuality::Valid;
        lastValidVoltage = filteredV;
        lastVoltageMonotonicMs = sample.monotonicMs;
        Services::alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_INVALID);
      }
      snap.batteryVoltage = vMeas;

      // =====================================================================
      // Phase 13-D.1: QUALITY RUNTIME WIRING — Battery Current
      // =====================================================================
      Core::Measurement iMeas;
      iMeas.value = sample.batteryCurrent;
      iMeas.timestamp = sample.timestamp;
      iMeas.sequence = sample.sequence;
      iMeas.source = Core::MeasurementSource::Measured;

      if (!sample.batteryCurrentValid || std::isnan(sample.batteryCurrent) || std::isinf(sample.batteryCurrent)) {
        // Producer: SensorError
        iMeas.quality = Core::MeasurementQuality::SensorError;
        iMeas.value = NAN;
        currentQualityState = Core::MeasurementQuality::SensorError;
        currentSuspectRecovery = 0;
        Services::alarms.raise(Core::AlarmCode::BATTERY_CURRENT_SENSOR_ERROR, Core::AlarmSeverity::Critical,
                     "INA219 battery current sensor error");
      } else if (calibratingAcs712) {
        // Producer: Calibrating — calibration is active
        iMeas.quality = Core::MeasurementQuality::Calibrating;
        iMeas.value = NAN;
        currentQualityState = Core::MeasurementQuality::Calibrating;
        currentSuspectRecovery = 0;
      } else if (std::fabs(sample.batteryCurrent) > Core::CURRENT_SPIKE_REJECT_A) {
        // Producer: OutOfRange — current exceeds shunt rating
        iMeas.quality = Core::MeasurementQuality::OutOfRange;
        iMeas.value = NAN;
        currentQualityState = Core::MeasurementQuality::OutOfRange;
        currentSuspectRecovery = 0;
      } else if (lastValidCurrent != 0.0f &&
                 std::fabs(sample.batteryCurrent - lastValidCurrent) > SUSPECT_CURRENT_JUMP) {
        // Producer: Suspect — excessive current jump
        iMeas.quality = Core::MeasurementQuality::Suspect;
        iMeas.value = sample.batteryCurrent;
        currentQualityState = Core::MeasurementQuality::Suspect;
        currentSuspectRecovery = 0;
      } else if (currentQualityState == Core::MeasurementQuality::Suspect) {
        // Recovery: need SUSPECT_RECOVERY_COUNT clean samples
        currentSuspectRecovery++;
        if (currentSuspectRecovery >= SUSPECT_RECOVERY_COUNT) {
          iMeas.quality = Core::MeasurementQuality::Valid;
          currentQualityState = Core::MeasurementQuality::Valid;
          lastValidCurrent = sample.batteryCurrent;
          lastCurrentMonotonicMs = sample.monotonicMs;
          Services::alarms.clear(Core::AlarmCode::BATTERY_CURRENT_SENSOR_ERROR);
        } else {
          iMeas.quality = Core::MeasurementQuality::Suspect;
        }
      } else if (lastCurrentMonotonicMs > 0 &&
                 (sample.monotonicMs - lastCurrentMonotonicMs) > STALE_THRESHOLD_MS) {
        // Producer: Stale — current measurement is old
        // Fresh sample arriving → transition to Valid
        iMeas.quality = Core::MeasurementQuality::Valid;
        currentQualityState = Core::MeasurementQuality::Valid;
        lastValidCurrent = sample.batteryCurrent;
        lastCurrentMonotonicMs = sample.monotonicMs;
        Services::alarms.clear(Core::AlarmCode::BATTERY_CURRENT_SENSOR_ERROR);
      } else {
        // Producer: Valid
        iMeas.quality = Core::MeasurementQuality::Valid;
        currentQualityState = Core::MeasurementQuality::Valid;
        lastValidCurrent = sample.batteryCurrent;
        lastCurrentMonotonicMs = sample.monotonicMs;
        Services::alarms.clear(Core::AlarmCode::BATTERY_CURRENT_SENSOR_ERROR);
      }
      snap.batteryCurrent = iMeas;
      snap.direction = Core::classifyDirection(sample.batteryCurrent,
                                          Core::cfgIdleCurrentThreshold);

      // Battery power (derived — brief §15)
      Core::Measurement pMeas;
      pMeas.source = Core::MeasurementSource::Derived;
      pMeas.timestamp = sample.timestamp;
      pMeas.sequence = sample.sequence;
      if (vMeas.isValid() && iMeas.isValid()) {
        pMeas.value = vMeas.value * iMeas.value;
        pMeas.quality = Core::MeasurementQuality::Derived;
        // [P1-010 REMEDIATION 2026-08] Cross-sensor plausibility P ≈ V × I:
        // the INA219 computes its own power from an INDEPENDENT bus-voltage
        // ADC. When that reading is unsaturated and meaningful (10–25 V —
        // INA219 FSR is 26 V, so a 48 V pack saturates it and the check is
        // HONESTLY SKIPPED: no independent voltage source exists on this
        // wiring), a >20% disagreement marks the derived power SUSPECT
        // instead of silently trusting the ADC-divider × shunt product.
        float inaBusV = Drivers::ina219Battery.getBusVoltage();
        if (Core::isValidFloat(inaBusV) && inaBusV >= 10.0f && inaBusV <= 25.0f) {
          float inaPower = Drivers::ina219Battery.getBusVoltage() * iMeas.value;
          // INA219 bus tap ratio unknown; compare per-ampere voltage products
          // only when the INA bus is in the same order as the pack voltage
          // divided range — otherwise the deployment wires VBUS elsewhere.
          float expected = vMeas.value * iMeas.value;
          if (std::fabs(expected) > 100.0f && std::fabs(inaPower) > 100.0f) {
            float relDev = std::fabs(inaPower - expected) /
                           (std::fabs(expected) > std::fabs(inaPower) ? std::fabs(expected) : std::fabs(inaPower));
            if (relDev > 0.20f) {
              pMeas.quality = Core::MeasurementQuality::Suspect;
              Services::alarms.raise("SENSOR_DISAGREEMENT", Core::AlarmSeverity::Warning,
                           "P(V×I) vs INA219 power disagree > 20%");
            } else {
              Services::alarms.clear("SENSOR_DISAGREEMENT");
            }
          }
        }
      } else {
        pMeas.value = NAN;
        pMeas.quality = Core::MeasurementQuality::NotAvailable;
      }
      snap.batteryPower = pMeas;

      // Environment (brief §29)
      if (sample.environmentValid) {
        snap.temperature.value = sample.environment.temperature;
        snap.temperature.quality = Core::MeasurementQuality::Valid;
        snap.temperature.source = Core::MeasurementSource::Measured;
        snap.humidity.value = sample.environment.humidity;
        snap.humidity.quality = Core::MeasurementQuality::Valid;
        snap.humidity.source = Core::MeasurementSource::Measured;
        snap.dewPoint.value = Core::computeDewPoint(sample.environment.temperature,
                                              sample.environment.humidity);
        snap.dewPoint.quality = Core::MeasurementQuality::Derived;
        snap.dewPoint.source = Core::MeasurementSource::Derived;
        Services::alarms.clear(Core::AlarmCode::SHT31_FAILURE);
      } else {
        snap.temperature.value = NAN;
        snap.temperature.quality = Core::MeasurementQuality::SensorError;
        snap.humidity.value = NAN;
        snap.humidity.quality = Core::MeasurementQuality::SensorError;
        snap.dewPoint.value = NAN;
        snap.dewPoint.quality = Core::MeasurementQuality::NotAvailable;
        Services::alarms.raise(Core::AlarmCode::SHT31_FAILURE, Core::AlarmSeverity::Warning,
                     "SHT31 sensor communication failure");
      }

      // AC measurement (brief §26-28)
      Services::AcMeasurementResult acResult = Services::acMeasurement.compute(sample.sequence, sample.timestamp);
      snap.ac.rmsCurrent = acResult.rmsCurrent;
      snap.ac.peakCurrent = acResult.peakCurrent;
      snap.ac.averageCurrent = acResult.averageCurrent;
      snap.ac.estimatedPower = acResult.estimatedPower;
      // [FW-25 REMEDIATION 2026-08] signalQuality was hardcoded Good — now
      // derived from actual sensor validity (NoSignal when the AC sensor is
      // unavailable, Good when a valid window was measured).
      snap.ac.signalQuality = sample.acCurrentValid
          ? Core::AcSignalQuality::Good
          : Core::AcSignalQuality::Invalid;

#if PLTS_ENABLE_PZEM_AC
      // v1.7.0 — PZEM-004T AC meter (OPTIONAL, flag OFF until bench
      // validation). Real V/P/f/PF replaces the ACS712 estimate IN
      // REPORTING when healthy; an absent meter reports connected=false +
      // null — never a silent estimate swap (the estimate block stays
      // labeled estimatedPower either way, so the operator always knows
      // which instrument produced the number).
      Drivers::pzemAc.tick();
      {
        Drivers::PzemReading r = Drivers::pzemAc.getReading();
        bool meterOk = Drivers::pzemAc.isAvailable() &&
                       r.status == Drivers::PzemStatus::Ok;
        if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          latestStatus.ac.meter.connected   = meterOk;
          latestStatus.ac.meter.voltage     = meterOk ? r.voltageV    : NAN;
          latestStatus.ac.meter.current     = meterOk ? r.currentA    : NAN;
          latestStatus.ac.meter.power       = meterOk ? r.powerW      : NAN;
          latestStatus.ac.meter.energy      = meterOk ? r.energyWh    : NAN;
          latestStatus.ac.meter.frequency   = meterOk ? r.frequencyHz : NAN;
          latestStatus.ac.meter.powerFactor = meterOk ? r.powerFactor : NAN;
          xSemaphoreGive(telemetryMutex);
        }
      }
#endif

      // [FW-25 REMEDIATION 2026-08] condensationRisk was copied into telemetry
      // but NEVER computed — it read uninitialized stack memory. Now derived
      // from the valid temperature/humidity pair via the Magnus dew point.
      snap.condensationRisk = Core::checkCondensationRisk(
          sample.environment.temperature, sample.environment.humidity);

      // [FW-16 REMEDIATION 2026-08] Sensor health wiring: HealthSupervisor's
      // setSensorHealth() was never called, so internal health stayed Offline
      // forever while telemetry showed Online (the enum zero value).
      Services::health.setSensorHealth("ina219", sample.batteryCurrentValid
          ? Core::SensorHealth::Online : Core::SensorHealth::Error);
      Services::health.setSensorHealth("batteryAdc", sample.batteryVoltageValid
          ? Core::SensorHealth::Online : Core::SensorHealth::Error);
      Services::health.setSensorHealth("acs712", sample.acCurrentValid
          ? Core::SensorHealth::Online : Core::SensorHealth::Error);
      Services::health.setSensorHealth("sht31", sample.environmentValid
          ? Core::SensorHealth::Online : Core::SensorHealth::Offline);

      snap.timestamp = sample.timestamp;
      snap.monotonicMs = sample.monotonicMs;
      snap.sequence = sample.sequence;

      xQueueSendToBack(measurementQueue, &snap, 0);
    } else {
      // No sample received within 1s timeout — check for Stale transition
      // Phase 13-D.1: Producer: Stale via timeout (monotonic time)
      uint32_t nowMs = millis();
      if (lastVoltageMonotonicMs > 0 && (nowMs - lastVoltageMonotonicMs) > STALE_THRESHOLD_MS) {
        if (voltageQualityState == Core::MeasurementQuality::Valid) {
          voltageQualityState = Core::MeasurementQuality::Stale;
          // Mark latest snapshot voltage as Stale
          if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            latestStatus.battery.voltage.quality = Core::MeasurementQuality::Stale;
            xSemaphoreGive(telemetryMutex);
          }
        }
      }
      if (lastCurrentMonotonicMs > 0 && (nowMs - lastCurrentMonotonicMs) > STALE_THRESHOLD_MS) {
        if (currentQualityState == Core::MeasurementQuality::Valid) {
          currentQualityState = Core::MeasurementQuality::Stale;
          if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            latestStatus.battery.current.quality = Core::MeasurementQuality::Stale;
            xSemaphoreGive(telemetryMutex);
          }
        }
      }
    }
  }
}

//=============================================================================
// ENERGY TASK — Ah/Wh integration, SOC, EFC (brief §16-23)
// RC-4: Uses canonical tick()+get() API, NOT addX()/getX()
// Phase 13-D: RC-13 RESOLVED — tick() accepts monotonicMs, dt computed internally
//=============================================================================
void energyTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(Core::ENERGY_CALC_INTERVAL_MS);
  Core::MeasurementSnapshot snap;
  // v1.6.0 — BMS provenance bookkeeping
  static uint32_t lastBmsBaselineSyncMs = 0;       // periodic coulomb-baseline re-sync
  static Core::SocProvenance lastProvenance = Core::SocProvenance::Unknown;
  static bool bmsProtocolLostAlarm = false;
  static bool bmsMismatchAlarm = false;
  static bool bmsImbalanceAlarm = false;
  static bool bmsFaultAlarm = false;

  while (true) {
    esp_task_wdt_reset();
    if (xQueueReceive(measurementQueue, &snap, pdMS_TO_TICKS(500)) == pdTRUE) {
      // Phase 13-D: tick() now accepts monotonicMs and computes dt internally.
      // Services enforce quality gate (only Valid integrates) and dt bounds.
      // No wall-clock used for integration. No fabricated dt fallback.
      Services::energyCounters.tick(snap.batteryVoltage.value, snap.batteryCurrent.value,
                          snap.batteryVoltage.quality, snap.batteryCurrent.quality,
                          snap.monotonicMs);
      Services::socStateMachine.tick(snap.batteryVoltage.value, snap.batteryCurrent.value,
                            snap.batteryVoltage.quality, snap.batteryCurrent.quality,
                            snap.monotonicMs);

      // -----------------------------------------------------------------
      // v1.6.0 — BMS/inverter merge + SOC provenance cascade.
      // BMS (LOCKED + fresh + plausible SOC) is AUTHORITATIVE for SOC;
      // the coulomb engine keeps running underneath as the warm fallback and
      // is re-baselined to the BMS SOC every 60 s so a later BMS dropout
      // hands over seamlessly (never a jump, never a silent switch).
      // -----------------------------------------------------------------
      Comm::BmsData bms = Comm::batteryComm.getData();
      uint32_t nowMs = snap.monotonicMs;
      bool bmsAuthoritative = Comm::batteryComm.socAuthoritative();

      // Cross-check BMS current vs INA219 shunt (redundancy — catches wrong
      // sign conventions and failing shunts within one poll cycle).
      float mismatchA = Comm::batteryComm.crossCheckShunt(snap.batteryCurrent.value, nowMs);

      if (bmsAuthoritative) {
        // Periodic re-baseline of the coulomb engine to BMS truth.
        if (nowMs - lastBmsBaselineSyncMs >= 60000UL) {
          Services::socStateMachine.setSoc(bms.soc, "BMS_SYNC");
          lastBmsBaselineSyncMs = nowMs;
        }
      }

      // Update latest snapshot (mutex-protected)
      if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latestStatus.battery.voltage = snap.batteryVoltage;
        latestStatus.battery.current = snap.batteryCurrent;
        latestStatus.battery.power = snap.batteryPower;
        latestStatus.battery.direction = snap.direction;
        // [v1.9.0 / DYNAMIC-GAIN] Expose INA219 PGA mode for telemetry visibility
        latestStatus.battery.pgaMode = Drivers::ina219Battery.isAvailable()
            ? Drivers::ina219Battery.getPgaModeStr()
            : nullptr;
        // SOC snapshot — [P1-011/P1-012] NaN when UNKNOWN; provenance-marked.
        if (bmsAuthoritative) {
          // BMS SOC: measured by the battery's own gauge. Quality is Valid
          // UNLESS the redundancy cross-check is flagging a mismatch — then
          // the value is honest but SUSPECT (two instruments disagree).
          latestStatus.battery.soc.value = bms.soc;
          latestStatus.battery.soc.quality = Comm::batteryComm.isMismatchActive()
              ? Core::MeasurementQuality::Suspect
              : Core::MeasurementQuality::Valid;
          latestStatus.battery.soc.source = Core::MeasurementSource::Measured;
          latestStatus.battery.soc.method = "BMS_DIRECT";
          latestStatus.battery.soc.confidence = "HIGH";
          latestStatus.battery.soc.provenance = Core::SocProvenance::BmsDirect;
        } else {
          // Shunt path (INA219 coulomb counting / OCV at boot) — the honest
          // fallback, provenance-marked so the operator always knows.
          latestStatus.battery.soc.value = Services::socStateMachine.getSoc();
          latestStatus.battery.soc.quality = Services::socStateMachine.getSocQuality();
          latestStatus.battery.soc.source = Core::MeasurementSource::Estimated;
          latestStatus.battery.soc.method = "ESTIMATED";
          latestStatus.battery.soc.confidence = "MEDIUM";
          latestStatus.battery.soc.provenance = Services::socStateMachine.isSocValid()
              ? Core::SocProvenance::ShuntCoulomb
              : Core::SocProvenance::Unknown;
        }
        // Boot OCV resolution refines the provenance label (coarse estimate).
        if (!bmsAuthoritative && Services::socStateMachine.isSocValid() &&
            Services::socStateMachine.getLastSyncTs() != 0 &&
            strcmp(Services::socStateMachine.getStateStrSafe(), "SYNCHRONIZED") == 0 &&
            Services::socStateMachine.socCameFromOcv()) {
          latestStatus.battery.soc.provenance = Core::SocProvenance::OcvEstimated;
        }
        latestStatus.battery.soc.lastSync = Services::socStateMachine.getLastSyncTs();
        // Provenance transitions are events — log them (no silent fallback).
        if (latestStatus.battery.soc.provenance != lastProvenance) {
          Services::Log.append(Core::LogType::SocBaselineCorrected,
              String("SOC_PROVENANCE_CHANGED to=") +
              Core::socProvenanceToStr(latestStatus.battery.soc.provenance));
          lastProvenance = latestStatus.battery.soc.provenance;
        }
        // Energy counters snapshot from service (canonical get() — RC-4)
        Services::EnergyCounters ec = Services::energyCounters.get();
        latestStatus.battery.remainingAh = (Services::socStateMachine.getSoc() / 100.0f) * Core::cfgBatteryCapacityAh;
        latestStatus.battery.chargeAh = ec.chargeAh;
        latestStatus.battery.dischargeAh = ec.dischargeAh;
        latestStatus.battery.chargeWh = ec.chargeWh;
        latestStatus.battery.dischargeWh = ec.dischargeWh;
        latestStatus.battery.netWh = ec.chargeWh - ec.dischargeWh;
        latestStatus.battery.efc = ec.efc;
        latestStatus.battery.estimatedUsableCapacityAh = Services::socStateMachine.getEstimatedUsableCapacityAh();
        latestStatus.battery.peakChargeCurrent = ec.peakChargeA;
        latestStatus.battery.peakDischargeCurrent = ec.peakDischargeA;
        latestStatus.ac.rmsCurrent = snap.ac.rmsCurrent;
        latestStatus.ac.peakCurrent = snap.ac.peakCurrent;
        latestStatus.ac.averageCurrent = snap.ac.averageCurrent;
        latestStatus.ac.estimatedPower.value = snap.ac.estimatedPower.value;
        latestStatus.ac.estimatedPower.quality = snap.ac.estimatedPower.quality;
        latestStatus.ac.signalQuality = snap.ac.signalQuality;
        latestStatus.environment.temperature = snap.temperature;
        latestStatus.environment.humidity = snap.humidity;
        latestStatus.environment.dewPoint = snap.dewPoint;
        latestStatus.environment.condensationRisk = snap.condensationRisk;
        // v1.6.0 — BMS snapshot into telemetry status (NaN-safe fields).
        latestStatus.bms.connected = bmsAuthoritative;
        latestStatus.bms.protocol = Comm::batteryComm.activeProtocolStr();
        latestStatus.bms.state = Comm::batteryComm.stateStr();
        latestStatus.bms.voltage = bms.voltage;
        latestStatus.bms.current = bms.current;
        latestStatus.bms.temperature = bms.temperature;
        latestStatus.bms.soh = bms.soh;
        latestStatus.bms.cellVoltageMin = bms.cellVoltageMin;
        latestStatus.bms.cellVoltageMax = bms.cellVoltageMax;
        latestStatus.bms.cellCount = bms.cellCount;
        latestStatus.bms.chargeCurrentLimit = bms.chargeCurrentLimit;
        latestStatus.bms.dischargeCurrentLimit = bms.dischargeCurrentLimit;
        latestStatus.bms.cycleCount = bms.cycleCount;
        latestStatus.bms.faultFlags = bms.faultFlags;
        latestStatus.bms.moduleCount = bms.moduleCount;
        latestStatus.bms.lastSeenMs = bms.lastUpdateMs;
        latestStatus.bms.currentMismatchA = mismatchA;
        latestStatus.timestamp = snap.timestamp;
        latestStatus.sequence = snap.sequence;
        xSemaphoreGive(telemetryMutex);
      }

      // -----------------------------------------------------------------
      // v1.6.0 — BMS-derived alarms (single owner: energyTask).
      // -----------------------------------------------------------------
      // Protocol lost after being locked → warn (fallback happened, logged).
      bool managerLost = (Comm::batteryComm.getState() == Comm::BatteryCommManager::State::Lost);
      if (managerLost && !bmsProtocolLostAlarm) {
        Services::alarms.raise(Core::AlarmCode::BMS_PROTOCOL_LOST, Core::AlarmSeverity::Warning,
                     "BMS protocol connection lost — falling back to shunt estimation");
        bmsProtocolLostAlarm = true;
      } else if (!managerLost && bmsProtocolLostAlarm) {
        Services::alarms.clear(Core::AlarmCode::BMS_PROTOCOL_LOST);
        bmsProtocolLostAlarm = false;
      }
      // Sustained current mismatch BMS vs shunt.
      if (Comm::batteryComm.isMismatchActive() && !bmsMismatchAlarm) {
        Services::alarms.raise(Core::AlarmCode::BMS_CURRENT_MISMATCH, Core::AlarmSeverity::Warning,
                     "BMS current disagrees with INA219 shunt beyond tolerance");
        bmsMismatchAlarm = true;
      } else if (!Comm::batteryComm.isMismatchActive() && bmsMismatchAlarm) {
        Services::alarms.clear(Core::AlarmCode::BMS_CURRENT_MISMATCH);
        bmsMismatchAlarm = false;
      }
      // Cell imbalance (only when cell voltages are actually provided).
      if (Core::isValidFloat(bms.cellVoltageMin) && Core::isValidFloat(bms.cellVoltageMax)) {
        float delta = bms.cellVoltageMax - bms.cellVoltageMin;
        if (delta > Core::BMS_CELL_IMBALANCE_V && !bmsImbalanceAlarm) {
          Services::alarms.raise(Core::AlarmCode::BMS_CELL_IMBALANCE, Core::AlarmSeverity::Warning,
                       "BMS cell voltage imbalance above threshold");
          bmsImbalanceAlarm = true;
        } else if (delta <= Core::BMS_CELL_IMBALANCE_V && bmsImbalanceAlarm) {
          Services::alarms.clear(Core::AlarmCode::BMS_CELL_IMBALANCE);
          bmsImbalanceAlarm = false;
        }
      }
      // BMS-reported fault flags (protocol-specific bitfield).
      if (bmsAuthoritative && bms.faultFlags != 0 && !bmsFaultAlarm) {
        Services::alarms.raise(Core::AlarmCode::BMS_FAULT, Core::AlarmSeverity::Critical,
                     "BMS reported fault flags (see documentation bit map)");
        bmsFaultAlarm = true;
      } else if ((!bmsAuthoritative || bms.faultFlags == 0) && bmsFaultAlarm) {
        Services::alarms.clear(Core::AlarmCode::BMS_FAULT);
        bmsFaultAlarm = false;
      }

      // Anomaly detection (brief §69)
      // [FW-14 REMEDIATION 2026-08] ENABLED — tick() was commented out, so
      // stuck-sensor, spike, temperature-rise and SOC-jump detection were
      // all dead code while the docs claimed them.
      {
        Services::AnomalyContext actx = {};
        actx.voltage   = snap.batteryVoltage.isValid() ? snap.batteryVoltage.value : NAN;
        actx.current   = snap.batteryCurrent.isValid() ? snap.batteryCurrent.value : NAN;
        actx.temperatureC = snap.temperature.isValid() ? snap.temperature.value : NAN;
        actx.humidityPct  = snap.humidity.isValid() ? snap.humidity.value : NAN;
        actx.soc       = Services::socStateMachine.getSoc();
        actx.voltageSeq = snap.batteryVoltage.sequence;
        actx.currentSeq = snap.batteryCurrent.sequence;
        actx.telemetrySeq = snap.sequence;
        actx.voltageQ  = snap.batteryVoltage.quality;
        actx.currentQ  = snap.batteryCurrent.quality;
        Services::anomalyDetector.tick(actx, snap.timestamp);
      }

      // Voltage Services::alarms (brief §24 — hysteresis)
      if (snap.batteryVoltage.isValid()) {
        static bool lowAlarmActive = false;
        static bool highAlarmActive = false;
        float v = snap.batteryVoltage.value;
        if (!lowAlarmActive && v < Core::cfgLowVoltage) {
          Services::alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_LOW, Core::AlarmSeverity::Critical,
                       "Battery voltage below low threshold");
          lowAlarmActive = true;
        } else if (lowAlarmActive && v > Core::BATTERY_LOW_CLEAR_V) {
          Services::alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_LOW);
          lowAlarmActive = false;
        }
        if (!highAlarmActive && v > Core::BATTERY_HIGH_V) {
          Services::alarms.raise(Core::AlarmCode::BATTERY_VOLTAGE_HIGH, Core::AlarmSeverity::Warning,
                       "Battery voltage above high threshold");
          highAlarmActive = true;
        } else if (highAlarmActive && v < Core::BATTERY_HIGH_CLEAR_V) {
          Services::alarms.clear(Core::AlarmCode::BATTERY_VOLTAGE_HIGH);
          highAlarmActive = false;
        }
      }
    }
    vTaskDelayUntil(&lastWake, period);
  }
}

//=============================================================================
// TELEMETRY TASK — 5s publish + spool management (brief §41-43)
//=============================================================================
void telemetryTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(Core::TELEMETRY_INTERVAL_MS);

  while (true) {
    esp_task_wdt_reset();
    publishTelemetry();
    Services::health.recordHeartbeat(Core::TaskId::Telemetry);
    vTaskDelayUntil(&lastWake, period);
  }
}

void publishTelemetry() {
  Core::SystemStatus snapshot;
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // [FW-01 REMEDIATION 2026-08] Canonical identity is written INTO
    // latestStatus (single source of truth) so the REST /api/status path and
    // the MQTT path serialize the SAME envelope. Previously these fields were
    // only ever filled in a local copy — REST served empty identity and MQTT
    // served "PLTS-UNKNOWN"/timestamp 0.
    latestStatus.protocolVersion = 1;
    latestStatus.firmwareVersion = Core::FIRMWARE_VERSION;
    latestStatus.deviceId = Core::deviceId;
    latestStatus.deviceName = Core::deviceName;
    latestStatus.timestamp = Services::timeManager.getUnixTime();
    latestStatus.timeQuality = Services::timeManager.isSynced()
        ? Core::TimeQuality::Valid : Core::TimeQuality::Unsynced;
    latestStatus.uptimeSeconds = millis() / 1000;
    Services::HealthSnapshot h = Services::health.getSnapshot();
    latestStatus.bootCount = h.bootCount;               // [FW-13] real boot count
    latestStatus.resetReason = Core::resetReasonStr(lastResetReason);
    // Health snapshot — copy fields from Services::HealthSnapshot
    latestStatus.health.systemState = h.systemState;
    latestStatus.health.freeHeap = h.freeHeap;
    latestStatus.health.minFreeHeap = h.minFreeHeap;
    latestStatus.health.wifiRssi = h.wifiRssi;
    latestStatus.health.wifiReconnectCount = h.wifiReconnectCount;
    latestStatus.health.mqttConnected = Network::mqttTransport.isFullyOperational();
    latestStatus.health.ntpSynced = (h.timeQuality == Core::TimeQuality::Valid);
    latestStatus.health.storageOk = h.nvsOk;
    // [P2-009] REAL spool occupancy — was hardcoded 0 (a fabricated "all ok").
    latestStatus.health.spoolSize = Services::telemetrySpool.pendingCount() +
                                    Services::telemetrySpool.criticalPendingCount();
    latestStatus.health.highestAlarmSeverity = Services::alarms.highestActiveSeverity();
    // [FW-23 REMEDIATION 2026-08] Alarms — ACTIVE ONLY (Active + Acknowledged,
    // never Cleared). The old pointer + count pair exposed the full registry
    // array INCLUDING cleared alarms in every telemetry envelope.
    static Services::Alarm s_activeAlarmsBuf[Services::AlarmRegistry::MAX_ALARMS];
    latestStatus.activeAlarmCount = Services::alarms.copyActiveAlarms(
        s_activeAlarmsBuf, Services::AlarmRegistry::MAX_ALARMS);
    latestStatus.activeAlarms = s_activeAlarmsBuf;
    snapshot = latestStatus;
    xSemaphoreGive(telemetryMutex);
  } else {
    return;
  }

  // [FW-17 REMEDIATION 2026-08] Per-MESSAGE sequence: the telemetry envelope
  // gets its own monotonically increasing identity (previously it inherited
  // the 5 Hz sensor-sample counter, and the NVS checkpoint lagged by up to
  // ~1500 counts → post-reboot sequences REGRESSED, breaking backend dedupe).
  // Persistence task stores a high-water mark (sequence + margin) so a reboot
  // resumes ABOVE every pre-reboot value: gaps may appear (honest — the
  // interval is unknown), never regressions.
  snapshot.sequence = telemetrySequence++;

  // Serialize (NaN-safe — null for invalid, never 0)
  String json = Web::serialize(snapshot);

  // [FW-32] Publish; spool when delivery is NOT confirmed — including a
  // failed publish while connected (previously only the disconnected state
  // spooled, so transient broker failures silently dropped telemetry).
  bool delivered = Network::mqttTelemetry.publishStatus(json.c_str(), json.length());
  if (!delivered) {
    Services::telemetrySpool.spool(snapshot.sequence, snapshot.timestamp,
                                   json.c_str(), (uint16_t)json.length());
  }
  // Drain the spool oldest-first when the transport is fully operational.
  Network::mqttTelemetry.replaySpool();
}

//=============================================================================
// NETWORK TASK — WiFi/MQTT/NTP maintenance
//=============================================================================
void networkTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    esp_task_wdt_reset();

    if (WiFi.status() != WL_CONNECTED) {
      Services::wifi.tick();  // WiFi reconnection handled internally
      Services::alarms.raise(Core::AlarmCode::NETWORK_DEGRADED, Core::AlarmSeverity::Warning,
                   "WiFi disconnected");
    } else {
      Services::alarms.clear(Core::AlarmCode::NETWORK_DEGRADED);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Network::mqttTransport.tick();  // handles reconnect + publish + onMessage dispatch internally
      // [FW-22] Deferred reboot driver (system.reboot / factory_reset_confirm)
      Network::mqttConfigReceiver.tick();

      // GAS advisor — hourly HMAC POST + on-demand fetch
      AI::advisor.tick();
    }
    Services::health.recordHeartbeat(Core::TaskId::Mqtt);

    Services::timeManager.tick();

    Web::server.handleClient();

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(100));
  }
}

//=============================================================================
// PERSISTENCE TASK — periodic NVS save (brief §77 — wear management)
//=============================================================================
void persistenceTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(Core::PERSIST_INTERVAL_MS);

  while (true) {
    esp_task_wdt_reset();
    // Save energy counters + SOC state (brief §45)
    // [FW-12 CLOSED 2026-08] SOC persistence implemented in SocStateMachine.
    Services::energyCounters.saveToNVS();
    Services::socStateMachine.saveToNVS();
    // [FW-23] Alarm checkpoint — persist when dirty (raise() marks dirty;
    // operator clear/ack already saved immediately).
    if (Services::alarms.isDirty()) Services::alarms.saveToNVS();
    // [FW-17] Sequence high-water mark: persist current counter PLUS a safety
    // margin covering the maximum possible increments between checkpoints
    // (PERSIST_INTERVAL_MS / SENSOR_SAMPLE_INTERVAL_MS = 1500). Post-reboot
    // sequences resume above every pre-reboot value — monotonic across reboot.
    Storage::config.saveTelemetrySequence(telemetrySequence + Core::SEQ_REBOOT_MARGIN);
    Services::health.recordHeartbeat(Core::TaskId::Persistence);

    vTaskDelayUntil(&lastWake, period);
  }
}

//=============================================================================
// HEALTH TASK — Services::health supervision, alarm evaluation (brief §33)
//=============================================================================
void healthTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    esp_task_wdt_reset();
    Services::health.tick();
    Services::health.recordHeartbeat(Core::TaskId::HealthMonitor);

    // [AUDIT 2026-08 v1.6.0] sensorHealth was NEVER populated — the API always
    // reported every sensor ONLINE (zero-initialized enum), even during a
    // hardware failure. Derived now from the latest measurement quality:
    //   Valid/Derived/Estimated → Online, Suspect/Calibrating → Recovering,
    //   Stale → Offline, Invalid/SensorError/NotAvailable/OutOfRange → Error
    if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      auto qToHealth = [](Core::MeasurementQuality q) -> Core::SensorHealth {
        switch (q) {
          case Core::MeasurementQuality::Valid:
          case Core::MeasurementQuality::Derived:
          case Core::MeasurementQuality::Estimated:
            return Core::SensorHealth::Online;
          case Core::MeasurementQuality::Suspect:
          case Core::MeasurementQuality::Calibrating:
            return Core::SensorHealth::Recovering;
          case Core::MeasurementQuality::Stale:
            return Core::SensorHealth::Offline;
          case Core::MeasurementQuality::Invalid:
          case Core::MeasurementQuality::OutOfRange:
          case Core::MeasurementQuality::SensorError:
          case Core::MeasurementQuality::NotAvailable:
          default:
            return Core::SensorHealth::Error;
        }
      };
      latestStatus.health.sensorHealth.ina219    = qToHealth(latestStatus.battery.current.quality);
      latestStatus.health.sensorHealth.batteryAdc = qToHealth(latestStatus.battery.voltage.quality);
      latestStatus.health.sensorHealth.acs712    = qToHealth(latestStatus.ac.rmsCurrent.quality);
      latestStatus.health.sensorHealth.sht31     = qToHealth(latestStatus.environment.temperature.quality);
      // v1.6.0 — BMS comm health from the manager state machine.
      switch (Comm::batteryComm.getState()) {
        case Comm::BatteryCommManager::State::Locked:    latestStatus.health.sensorHealth.bmsComm = Core::SensorHealth::Online;     break;
        case Comm::BatteryCommManager::State::Probing:   latestStatus.health.sensorHealth.bmsComm = Core::SensorHealth::Recovering; break;
        case Comm::BatteryCommManager::State::Lost:      latestStatus.health.sensorHealth.bmsComm = Core::SensorHealth::Error;      break;
        case Comm::BatteryCommManager::State::Disabled:
        case Comm::BatteryCommManager::State::IdleNoBms:
        default:                                        latestStatus.health.sensorHealth.bmsComm = Core::SensorHealth::Offline;    break;
      }
      xSemaphoreGive(telemetryMutex);
    }

    if (!Services::timeManager.isSynced()) {
      Services::alarms.raise(Core::AlarmCode::TIME_UNSYNCED, Core::AlarmSeverity::Warning,
                   "System time not synchronized");
    } else {
      Services::alarms.clear(Core::AlarmCode::TIME_UNSYNCED);
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

//=============================================================================
// BMS COMM TASK — v1.6.0 multi-protocol battery/inverter manager
// Drives the auto-detect state machine (probing/lock/loss/fallback). All
// client I/O is non-blocking; a slow or silent BMS can never stall this task
// beyond its 100 ms tick (HealthSupervisor WDT guards it).
//=============================================================================
void bmsCommTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    esp_task_wdt_reset();
    Comm::batteryComm.tick(millis());
#if PLTS_ENABLE_RS485_CONSOLE
    Comm::rs485Console.tick(millis());   // no-op unless capture mode active
#endif
    Services::health.recordHeartbeat(Core::TaskId::BmsComm);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(100));
  }
}

#if PLTS_ENABLE_EMERGENCY
//=============================================================================
// EMERGENCY TASK — v1.7.0 E-WAVE supervisor (LOCAL-FIRST, 10 Hz).
// E-stop poll, trigger evaluation, ARM gating bookkeeping, LED, status
// publish. NO network I/O ever — a safety function must never depend on
// WiFi/MQTT/GAS being up (brief §75). The relay state itself is latched in
// the GPIO/driver, so even a total task stall leaves the system ISOLATED
// (fail-safe direction) — the WDT panic path resets the GPIO to Hi-Z which
// ALSO isolates. Both failure directions point to safety.
//=============================================================================
void emergencyTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    esp_task_wdt_reset();
    Services::emergency.tick();
    Services::health.recordHeartbeat(Core::TaskId::Emergency);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(100));
  }
}

//=============================================================================
// GAS EMERGENCY TASK — v1.7.0 E-WAVE remote command channel (10 Hz pump).
// Own task because the TLS POST blocks up to 7 s: stalling networkTask would
// break MQTT keepalive + web server, stalling emergencyTask would delay
// E-stop polling. Here it can only delay the next poll cadence (15 s base,
// backoff to 60 s on failures — bounded, never the safety path).
//=============================================================================
void gasEmergencyTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();

  while (true) {
    esp_task_wdt_reset();
    Network::gasEmergency.tick();
    Services::health.recordHeartbeat(Core::TaskId::GasEmergency);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(100));
  }
}
#endif

//=============================================================================
// BOOT BANNER
//=============================================================================
void printBootBanner() {
  Serial.println("\n========================================");
  Serial.printf("  PLTS Monitor — Production-Grade v%s\n", Core::FIRMWARE_VERSION);
  // Build profile name (macro — not in namespace)
  const char* profile = "development";
#ifdef PRODUCTION_BUILD
  profile = "production";
#elif defined(STAGING_BUILD)
  profile = "staging";
#endif
  Serial.printf("  Protocol v1 | Build: %s\n", profile);
  Serial.printf("  %s\n", Core::FIRMWARE_BUILD_DATE);
  Serial.println("========================================\n");
}

//=============================================================================
// HELPERS — dew point (Magnus formula), condensation risk
//=============================================================================
// In namespace Core (matching Globals.h declarations)
namespace Core {

float computeDewPoint(float tempC, float humidity) {
  if (std::isnan(tempC) || std::isnan(humidity) || humidity < 0 || humidity > 100) {
    return NAN;
  }
  const float a = 17.625f;
  const float b = 243.04f;
  float alpha = logf(humidity / 100.0f) + (a * tempC) / (b + tempC);
  return (b * alpha) / (a - alpha);
}

bool checkCondensationRisk(float tempC, float humidity) {
  if (std::isnan(tempC) || std::isnan(humidity)) return false;
  float dew = Core::computeDewPoint(tempC, humidity);
  if (std::isnan(dew)) return false;
  return (tempC - dew) < 3.0f;
}

} // namespace Core

//=============================================================================
// OTA TASK — pumps OtaManager.tickDownload() when an MQTT OTA download is
// in progress. (Phase 13-F: MqttOtaHandler now exists; this task activates
// only when Services::ota.getState() == Downloading.)
// Brief §72: streaming SHA-256 + anti-downgrade + Ed25519 (production).
//=============================================================================
void otaTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(500);  // 2 Hz pump rate

  while (true) {
    esp_task_wdt_reset();

    // Only do work when OTA is actively downloading or checking.
    // (In Idle/Verifying/Applying/Done/Failed states, this task sleeps.)
    // [WAVE-6 / FW6-4] Checking = ota.check manifest fetch (pumped here so
    // the MQTT callback stays non-blocking).
    if (Services::ota.getState() == Services::OtaState::Downloading) {
      Services::ota.tickDownload();
    } else if (Services::ota.getState() == Services::OtaState::Checking) {
      Services::ota.tickManifestCheck();
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

// =============================================================================
// [v1.8.0] relayTask — 8-channel relay controller tick (5 Hz)
// =============================================================================
#if PLTS_ENABLE_RELAYS
void relayTask(void* pv) {
  esp_task_wdt_add(NULL);
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(Core::RELAY_TICK_MS);  // 200ms = 5 Hz

  while (true) {
    esp_task_wdt_reset();
    Services::relaysController.tick();
    vTaskDelayUntil(&lastWake, period);
  }
}
#endif
