// =============================================================================
// Storage/ConfigStore.cpp — Atomic A/B + CRC32 persistence
// =============================================================================
#include "ConfigStore.h"
#include "FileSystem.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include "../Utils/Crypto.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>
#include <Preferences.h>

namespace Storage {

ConfigStore config;

// [WAVE-6 / FW6-3] Masked secret for the every-boot dev/staging log line:
// first 3 chars + '…' + total length — enough to correlate with the
// commissioning-time full reveal, useless to a shoulder-surfer.
static const char* maskSecret_(const char* s) {
  static char buf[48];
  size_t len = strlen(s);
  if (len == 0) { snprintf(buf, sizeof(buf), "(empty)"); return buf; }
  char prefix[4] = {0};
  prefix[0] = s[0];
  if (len > 1) prefix[1] = s[1];
  if (len > 2) prefix[2] = s[2];
  snprintf(buf, sizeof(buf), "%s...(%u chars)", prefix, (unsigned)len);
  return buf;
}

// ============================================================================
// USER CONFIG (auth credentials)
// ============================================================================
void ConfigStore::initDefaultUserConfig() {
  char defaultPass[17];
  static const char charset[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // no I,O,0,1
  for (uint8_t i = 0; i < 16; i++) {
    defaultPass[i] = charset[esp_random() % (sizeof(charset) - 1)];
  }
  defaultPass[16] = '\0';
  strcpy(Core::wwwUser, "admin");
  Utils::generateRandomBytes(Core::salt, Core::SALT_LEN);
  Core::iterations = Core::PBKDF2_ITERATIONS;
  uint8_t hash[32];
  if (!Utils::pbkdf2HmacSha256(defaultPass, strlen(defaultPass),
                               Core::salt, Core::SALT_LEN,
                               Core::iterations, hash)) {
    memset(Core::passHashHex, 0, sizeof(Core::passHashHex));
    memset(defaultPass, 0, sizeof(defaultPass));
    return;
  }
  Utils::bytesToHex(hash, 32, Core::passHashHex);
  Services::Log.append(Core::LogType::ConfigurationChanged,
                        "Default user config created (random password)", 0);
  // [AUDIT 2026-08-28 F-G18] One-time reveal at GENERATION (commissioning
  // moment) — SEMUA profil build, termasuk PRODUCTION_BUILD. Sebelumnya
  // produksi meredaksi password dan menyarankan "baca via /api/config" —
  // MELINGKAR: /api/config butuh login, login butuh password yang sedang
  // dicari, dan endpoint itu memang tidak pernah mengembalikan password.
  // Perangkat produksi baru TIDAK PERNAH bisa diakses operator. Model ini
  // identik dengan password AP (F-G15): ungkap sekali saat boot pertama /
  // setelah factory reset, redaksi pada boot berikutnya, ganti password
  // segera setelah login pertama (PWA Settings → Security).
  Serial.println(F("========================================================="));
  Serial.println(F("[ConfigStore] Password admin BARU (CATAT & SIMPAN!):"));
  Serial.printf(  "[ConfigStore]   user: admin  pass: %s\n", defaultPass);
  Serial.println(F("[ConfigStore] Hanya ditampilkan SEKALI saat di-generate."));
  Serial.println(F("[ConfigStore] Segera ganti setelah login pertama!"));
  Serial.println(F("========================================================="));
  memset(defaultPass, 0, sizeof(defaultPass));
}

void ConfigStore::loadUserConfig() {
  if (!fs.exists(Core::PATH_CONFIG_JSON)) {
    if (!fs.exists(Core::PATH_CONFIG_BAK)) {
      initDefaultUserConfig();
      saveUserConfig();
      return;
    }
    fs.rename(Core::PATH_CONFIG_BAK, Core::PATH_CONFIG_JSON);
  }
  String content = fs.readAll(Core::PATH_CONFIG_JSON);
  if (content.length() == 0) { initDefaultUserConfig(); saveUserConfig(); return; }

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, content)) {
    initDefaultUserConfig(); saveUserConfig(); return;
  }
  if (!Utils::verifyCRC(doc)) {
    Serial.println(F("[ConfigStore] config.json CRC mismatch — resetting to defaults"));
    initDefaultUserConfig(); saveUserConfig(); return;
  }
  if (doc.containsKey("user")) {
    const char* u = doc["user"];
    if (u) { strncpy(Core::wwwUser, u, Core::MAX_USER_LEN);
             Core::wwwUser[Core::MAX_USER_LEN] = '\0'; }
  }
  if (doc.containsKey("passhash")) {
    const char* h = doc["passhash"];
    if (h && strlen(h) == Core::PASS_HASH_HEX_LEN) {
      strncpy(Core::passHashHex, h, Core::PASS_HASH_HEX_LEN);
      Core::passHashHex[Core::PASS_HASH_HEX_LEN - 1] = '\0';
    }
  }
  // [FW-06 REMEDIATION 2026-08] Key mismatch fixed: containsKey("salt") but
  // the reader/writer used the literal key "Core::salt" — after reboot the
  // salt NEVER loaded and PBKDF2 was computed against an all-zero salt,
  // permanently breaking login. Read/write now use the SAME key "salt".
  // A one-time backward-compatible read of the legacy "Core::salt" key is
  // kept so already-provisioned devices migrate transparently on next save.
  if (doc.containsKey("salt")) {
    const char* s = doc["salt"];
    if (s && strlen(s) == Core::SALT_LEN * 2) {
      Utils::hexToBytes(s, Core::salt, Core::SALT_LEN);
    }
  } else if (doc.containsKey("Core::salt")) {
    // legacy key migration path (pre-remediation provisioning)
    const char* s = doc["Core::salt"];
    if (s && strlen(s) == Core::SALT_LEN * 2) {
      Utils::hexToBytes(s, Core::salt, Core::SALT_LEN);
    }
  }
  if (doc.containsKey("iterations")) {
    Core::iterations = doc["iterations"] | Core::PBKDF2_ITERATIONS;
    if (Core::iterations < 1000) Core::iterations = Core::PBKDF2_ITERATIONS;
  }
}

void ConfigStore::saveUserConfig() {
  StaticJsonDocument<2048> doc;
  doc["user"] = Core::wwwUser;
  doc["passhash"] = Core::passHashHex;
  char saltHex[Core::SALT_LEN * 2 + 1] = {0};
  Utils::bytesToHex(Core::salt, Core::SALT_LEN, saltHex);
  doc["salt"] = saltHex;              // [FW-06] same key as the loader
  doc["iterations"] = Core::iterations;  // [FW-06] same key as the loader
  Utils::appendCRC(doc);
  String out; serializeJson(doc, out);
  if (fs.atomicWrite(Core::PATH_CONFIG_JSON, out)) {
    Services::Log.append(Core::LogType::ConfigurationChanged, "User config saved", 0);
  } else {
    Services::Log.append(Core::LogType::StorageError, "Failed to save user config", 0);
  }
}

// ============================================================================
// DEVICE CONFIG (name, timezone, secrets)
// ============================================================================
void ConfigStore::loadDeviceConfig() {
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, true);  // RO first
  strncpy(Core::deviceName, p.getString("name", "PLTS-Monitor").c_str(), 39);
  Core::deviceName[39] = '\0';
  strncpy(Core::cfgTimezone, p.getString("tz", Core::DEFAULT_TIMEZONE).c_str(), 39);
  Core::cfgTimezone[39] = '\0';

  // Per-device CSPRNG secrets (brief §17)
  String jwtS = p.getString("jwt", "");
  String mqtt = p.getString("mqtt_pass", "");
  String pin  = p.getString("pin", "");
  String gas  = p.getString("gas", "");
  String ap   = p.getString("ap_pass", "");
  p.end();

  // JWT secret (32 random bytes → 64 hex chars)
  if (jwtS.length() == 64) {
    strncpy(Core::jwtSecret, jwtS.c_str(), 64); Core::jwtSecret[64] = '\0';
  } else {
    uint8_t b[32]; Utils::generateRandomBytes(b, 32);
    char hex[65]; Utils::bytesToHex(b, 32, hex); hex[64] = '\0';
    strncpy(Core::jwtSecret, hex, 64); Core::jwtSecret[64] = '\0';
    memset(b, 0, sizeof(b));
    Preferences w; w.begin(Core::NVS_NAMESPACE, false);
    w.putString("jwt", Core::jwtSecret); w.end();
    Serial.println(F("[ConfigStore] Generated new JWT secret"));
  }

  // MQTT password (16 chars)
  if (mqtt.length() >= 16) {
    strncpy((char*)Core::mqttPassword, mqtt.c_str(), 16); Core::mqttPassword[16] = '\0';
  } else {
    static const char cs[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < 16; i++) Core::mqttPassword[i] = cs[esp_random() % (sizeof(cs)-1)];
    Core::mqttPassword[16] = '\0';
    Preferences w; w.begin(Core::NVS_NAMESPACE, false);
    w.putString("mqtt_pass", Core::mqttPassword); w.end();
    // [WAVE-6 / FW6-3] One-time reveal at GENERATION (commissioning moment)
    // — the broker ACL credential is needed to attach this device to the
    // MQTT broker; after this moment it only ever appears masked.
    Serial.println(F("========================================================="));
    Serial.println(F("[ConfigStore] Password MQTT BARU (CATAT & SIMPAN!):"));
    Serial.printf(  "[ConfigStore]   %s\n", Core::mqttPassword);
    Serial.println(F("[ConfigStore] Hanya ditampilkan SEKALI saat di-generate."));
    Serial.println(F("========================================================="));
  }

  // Device PIN (6 digits)
  if (pin.length() == 6) {
    strncpy(Core::devicePin, pin.c_str(), 6); Core::devicePin[6] = '\0';
  } else {
    for (int i = 0; i < 6; i++) Core::devicePin[i] = '0' + (esp_random() % 10);
    Core::devicePin[6] = '\0';
    Preferences w; w.begin(Core::NVS_NAMESPACE, false);
    w.putString("pin", Core::devicePin); w.end();
    // [WAVE-6 / FW6-3] One-time reveal at GENERATION — previously the PIN
    // existed ONLY in the dev/staging every-boot print (and nowhere in
    // production); now every profile reveals it exactly once, at the moment
    // it is created.
    Serial.println(F("========================================================="));
    Serial.println(F("[ConfigStore] Device PIN BARU (CATAT & SIMPAN!):"));
    Serial.printf(  "[ConfigStore]   %s\n", Core::devicePin);
    Serial.println(F("[ConfigStore] Hanya ditampilkan SEKALI saat di-generate."));
    Serial.println(F("========================================================="));
  }

  // GAS HMAC secret (32 bytes → 64 hex)
  if (gas.length() == 64) {
    strncpy(Core::gasSecret, gas.c_str(), 64); 
  } else {
    uint8_t b[32]; Utils::generateRandomBytes(b, 32);
    char hex[65]; Utils::bytesToHex(b, 32, hex); hex[64] = '\0';
    strncpy(Core::gasSecret, hex, 64); 
    memset(b, 0, sizeof(b));
    Preferences w; w.begin(Core::NVS_NAMESPACE, false);
    w.putString("gas", Core::gasSecret); w.end();
    // [WAVE-6 / FW6-3] One-time reveal at GENERATION — the operator must
    // enroll this secret in the GAS Devices sheet at commissioning time.
    Serial.println(F("========================================================="));
    Serial.println(F("[ConfigStore] Secret GAS HMAC BARU (CATAT & SIMPAN!):"));
    Serial.printf(  "[ConfigStore]   %s\n", Core::gasSecret);
    Serial.println(F("[ConfigStore] Hanya ditampilkan SEKALI saat di-generate."));
    Serial.println(F("========================================================="));
  }

  // AP fallback password (32 chars)
  if (ap.length() >= 16) {
    strncpy(Core::apPassword, ap.c_str(), 32); Core::apPassword[32] = '\0';
  } else {
    static const char cs[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789abcdefghijkmnpqrstuvwxyz";
    for (int i = 0; i < 32; i++) Core::apPassword[i] = cs[esp_random() % (sizeof(cs)-1)];
    Core::apPassword[32] = '\0';
    Preferences w; w.begin(Core::NVS_NAMESPACE, false);
    w.putString("ap_pass", Core::apPassword); w.end();
    // [AUDIT 2026-08-28 F-G15] One-time reveal at GENERATION (commissioning
    // moment). Semua profil build — termasuk PRODUCTION_BUILD — WAJIB
    // menampilkan password ini sekali: tanpa itu perangkat baru tidak bisa
    // di-provision via portal AP (fail-closed ke tidak-terpakai). Boot AP
    // berikutnya meredaksinya di WifiManager (PRODUCTION_BUILD). Bila
    // hilang: factory reset → regenerasi + tampil sekali lagi.
    Serial.println(F("========================================================="));
    Serial.println(F("[ConfigStore] Password AP BARU (CATAT & SIMPAN!):"));
    Serial.printf(  "[ConfigStore]   %s\n", Core::apPassword);
    Serial.println(F("[ConfigStore] Hanya ditampilkan SEKALI saat di-generate."));
    Serial.println(F("========================================================="));
  }

#ifdef PRODUCTION_BUILD
  // Hide per-device secrets in production
  Serial.println(F("[ConfigStore] Per-device secrets loaded (redacted in PRODUCTION_BUILD)"));
#else
  // [WAVE-6 / FW6-3] dev/staging: MASKED on every boot. The old code printed
  // the full Device PIN + MQTT password + GAS secret at EVERY boot — a
  // shoulder-surfing / shared-terminal-log leak that repeated daily. Full
  // values still print ONCE at GENERATION (commissioning moment, see the
  // generation branches above); if a secret is lost, factory reset
  // regenerates it with a fresh one-time reveal. The mask shows enough
  // (prefix + length) to correlate with the commissioning log entry.
  Serial.printf("[ConfigStore] Device PIN: %s (masked; full value shown once at generation)\n",
                maskSecret_(Core::devicePin));
  Serial.printf("[ConfigStore] MQTT pass: %s (masked; full value shown once at generation)\n",
                maskSecret_(Core::mqttPassword));
  Serial.printf("[ConfigStore] GAS secret: %s (masked; full value shown once at generation)\n",
                maskSecret_(Core::gasSecret));
#endif
}

void ConfigStore::saveDeviceConfig() {
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, false);
  p.putString("name", Core::deviceName);
  p.putString("tz", Core::cfgTimezone);
  p.end();
  Services::Log.append(Core::LogType::ConfigurationChanged, "Device config saved", 0);
}

// ============================================================================
// BATTERY CONFIG (capacity, thresholds, intervals)
// ============================================================================
void ConfigStore::loadBatteryConfig() {
  Preferences p;
  p.begin("plts_batt", true);
  Core::cfgBatteryCapacityAh          = p.getFloat("capAh", Core::BATTERY_CAPACITY_AH);
  Core::cfgBatteryNominalVoltage            = p.getFloat("nomV",  Core::BATTERY_NOMINAL_V);
  Core::cfgFullVoltage                 = p.getFloat("fullV", Core::BATTERY_FULL_V);
  Core::cfgLowVoltage                  = p.getFloat("lowV",  Core::BATTERY_LOW_V);
  Core::cfgIdleCurrentThreshold        = p.getFloat("idleA", Core::IDLE_CURRENT_THRESHOLD_A);
  Core::cfgFullChargeCurrentThreshold  = p.getFloat("endA",  Core::FULL_CHARGE_CURRENT_THRESHOLD_A);
  Core::cfgFullChargePersistenceSec    = p.getULong("persistS", Core::FULL_CHARGE_PERSISTENCE_SEC);
  Core::cfgTelemetryIntervalSec        = p.getUShort("telS", Core::cfgTelemetryIntervalSec);
  // v1.6.0 — BMS/inverter comm config
  Core::cfgBmsPollIntervalMs           = p.getULong("bmsPoll", Core::BMS_POLL_INTERVAL_MS);
  if (Core::cfgBmsPollIntervalMs < 1000 || Core::cfgBmsPollIntervalMs > 600000) {
    Core::cfgBmsPollIntervalMs = Core::BMS_POLL_INTERVAL_MS;   // sanitize
  }
  String proto = p.getString("bmsProto", Core::BMS_PROTOCOL_DEFAULT);
  strncpy(Core::cfgBmsProtocol, proto.c_str(), sizeof(Core::cfgBmsProtocol) - 1);
  Core::cfgBmsProtocol[sizeof(Core::cfgBmsProtocol) - 1] = '\0';
  Core::cfgBmsModbusSlaveId            = p.getUChar("bmsSlave", Core::BMS_MODBUS_SLAVE_ID);
  if (Core::cfgBmsModbusSlaveId < 1 || Core::cfgBmsModbusSlaveId > 247) {
    Core::cfgBmsModbusSlaveId = Core::BMS_MODBUS_SLAVE_ID;     // valid Modbus range
  }
  String host = p.getString("bmsHost", "");
  strncpy(Core::cfgBmsModbusTcpHost, host.c_str(), sizeof(Core::cfgBmsModbusTcpHost) - 1);
  Core::cfgBmsModbusTcpHost[sizeof(Core::cfgBmsModbusTcpHost) - 1] = '\0';
  Core::cfgBmsModbusTcpPort            = p.getUShort("bmsPort", Core::BMS_MODBUS_TCP_PORT);
  Core::calibration.version               = 1;
  p.end();
}

void ConfigStore::saveBatteryConfig() {
  Preferences p;
  p.begin("plts_batt", false);
  p.putFloat("capAh", Core::cfgBatteryCapacityAh);
  p.putFloat("nomV",  Core::cfgBatteryNominalVoltage);
  p.putFloat("fullV", Core::cfgFullVoltage);
  p.putFloat("lowV",  Core::cfgLowVoltage);
  p.putFloat("idleA", Core::cfgIdleCurrentThreshold);
  p.putFloat("endA",  Core::cfgFullChargeCurrentThreshold);
  p.putULong("persistS", Core::cfgFullChargePersistenceSec);
  p.putUShort("telS", Core::cfgTelemetryIntervalSec);
  // v1.6.0 — BMS/inverter comm config
  p.putULong("bmsPoll", Core::cfgBmsPollIntervalMs);
  p.putString("bmsProto", Core::cfgBmsProtocol);
  p.putUChar("bmsSlave", Core::cfgBmsModbusSlaveId);
  p.putString("bmsHost", Core::cfgBmsModbusTcpHost);
  p.putUShort("bmsPort", Core::cfgBmsModbusTcpPort);
  p.end();
  Services::Log.append(Core::LogType::ConfigurationChanged, "Battery config saved", 0);
}

// ============================================================================
// CALIBRATION (atomic + CRC)
// ============================================================================
void ConfigStore::initDefaultCalibration() {
  Core::calibration = {};
  Core::calibration.version = (uint8_t)1;
  Core::calibration.voltageLow      = { 45.0f, 45.0f, 0 };
  Core::calibration.voltageNominal   = { 50.0f, 50.0f, 0 };
  Core::calibration.voltageFull      = { 54.0f, 54.0f, 0 };
  Core::calibration.acs712Offset     = 1650.0f;  // mid-supply (1.65V × 4095/3.3)
  Core::calibration.acs712Sensitivity = Core::ACS712_SENSITIVITY_MV_PER_A;
  Core::calibration.sht31TempOffset  = 0.0f;
  Core::calibration.sht31HumOffset    = 0.0f;
  Core::calibration.timestamp         = 0;
  strncpy(Core::calibration.source, "DEFAULT", sizeof(Core::calibration.source)-1);
}

void ConfigStore::loadCalibration() {
  initDefaultCalibration();
  if (!fs.exists(Core::PATH_CALIBRATION_JSON)) {
    if (fs.exists(Core::PATH_CALIBRATION_BAK)) {
      fs.rename(Core::PATH_CALIBRATION_BAK, Core::PATH_CALIBRATION_JSON);
    } else {
      saveCalibration(true);
      return;
    }
  }
  String content = fs.readAll(Core::PATH_CALIBRATION_JSON);
  if (content.length() == 0) { saveCalibration(true); return; }
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, content)) { saveCalibration(true); return; }
  if (!Utils::verifyCRC(doc)) { saveCalibration(true); return; }

  Core::calibration.version = doc["version"] | (uint8_t)1;
  JsonObject vl = doc["voltageLow"];
  if (vl) {
    Core::calibration.voltageLow.reference = vl["reference"] | 45.0f;
    Core::calibration.voltageLow.raw        = vl["raw"] | 45.0f;
    Core::calibration.voltageLow.timestamp = vl["timestamp"] | 0;
  }
  JsonObject vn = doc["voltageNominal"];
  if (vn) {
    Core::calibration.voltageNominal.reference = vn["reference"] | 50.0f;
    Core::calibration.voltageNominal.raw        = vn["raw"] | 50.0f;
    Core::calibration.voltageNominal.timestamp = vn["timestamp"] | 0;
  }
  JsonObject vf = doc["voltageFull"];
  if (vf) {
    Core::calibration.voltageFull.reference = vf["reference"] | 54.0f;
    Core::calibration.voltageFull.raw        = vf["raw"] | 54.0f;
    Core::calibration.voltageFull.timestamp = vf["timestamp"] | 0;
  }
  Core::calibration.acs712Offset      = doc["acs712Offset"]      | 1650.0f;
  Core::calibration.acs712Sensitivity  = doc["acs712Sensitivity"] | Core::ACS712_SENSITIVITY_MV_PER_A;
  Core::calibration.sht31TempOffset   = doc["sht31TempOffset"]   | 0.0f;
  Core::calibration.sht31HumOffset     = doc["sht31HumOffset"]     | 0.0f;
  Core::calibration.timestamp          = doc["timestamp"] | 0;
  const char* src = doc["source"];
  if (src) {
    strncpy(Core::calibration.source, src, sizeof(Core::calibration.source)-1);
    Core::calibration.source[sizeof(Core::calibration.source)-1] = '\0';
  }
}

void ConfigStore::saveCalibration(bool force) {
  if (!force && !Core::calibrationDirty) return;
  StaticJsonDocument<2048> doc;
  doc["version"] = Core::calibration.version;
  JsonObject vl = doc.createNestedObject("voltageLow");
  vl["reference"] = Core::calibration.voltageLow.reference;
  vl["raw"] = Core::calibration.voltageLow.raw;
  vl["timestamp"] = Core::calibration.voltageLow.timestamp;
  JsonObject vn = doc.createNestedObject("voltageNominal");
  vn["reference"] = Core::calibration.voltageNominal.reference;
  vn["raw"] = Core::calibration.voltageNominal.raw;
  vn["timestamp"] = Core::calibration.voltageNominal.timestamp;
  JsonObject vf = doc.createNestedObject("voltageFull");
  vf["reference"] = Core::calibration.voltageFull.reference;
  vf["raw"] = Core::calibration.voltageFull.raw;
  vf["timestamp"] = Core::calibration.voltageFull.timestamp;
  doc["acs712Offset"]      = Core::calibration.acs712Offset;
  doc["acs712Sensitivity"] = Core::calibration.acs712Sensitivity;
  doc["sht31TempOffset"]   = Core::calibration.sht31TempOffset;
  doc["sht31HumOffset"]    = Core::calibration.sht31HumOffset;
  doc["timestamp"]         = Core::calibration.timestamp;
  doc["source"]            = Core::calibration.source;
  Utils::appendCRC(doc);
  String out; serializeJson(doc, out);
  if (fs.atomicWrite(Core::PATH_CALIBRATION_JSON, out)) {
    Core::calibrationDirty = false;
    Services::Log.append(Core::LogType::CalibrationChanged, "Calibration saved", 0);
  } else {
    Services::Log.append(Core::LogType::StorageError, "Failed to save calibration", 0);
  }
}

void ConfigStore::markCalibrationDirty()   { Core::calibrationDirty = true; }
void ConfigStore::clearCalibrationDirty()  { Core::calibrationDirty = false; }

// ============================================================================
// ENERGY COUNTERS (NVS)
// ============================================================================
void ConfigStore::loadEnergyFromNVS() {
  // Delegated to Services::EnergyCounters which owns the counters —
  // declared here so other modules can route through ConfigStore if desired.
  // Actual implementation: EnergyCounters::loadFromNVS() calls Preferences directly.
  // This is a stub for API symmetry.
}

void ConfigStore::saveEnergyToNVS() {
  // See note above — EnergyCounters::saveToNVS() is authoritative.
}

// ============================================================================
// TELEMETRY SEQUENCE (NVS) — survives reboot so monotonicity is preserved
// ============================================================================
uint32_t ConfigStore::loadTelemetrySequence() {
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, true);
  uint32_t s = p.getUInt("tseq", 0);
  p.end();
  return s;
}

void ConfigStore::saveTelemetrySequence(uint32_t seq) {
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, false);
  p.putUInt("tseq", seq);
  p.end();
}

// ============================================================================
// EXPORT / IMPORT (full backup)
// ============================================================================
String ConfigStore::exportAll() {
  StaticJsonDocument<8192> doc;
  doc["deviceName"]   = Core::deviceName;
  doc["timezone"]     = Core::cfgTimezone;
  doc["firmwareVersion"] = Core::FIRMWARE_VERSION;
  doc["protocolVersion"] = Core::PROTOCOL_VERSION;
  JsonObject batt = doc.createNestedObject("batteryConfig");
  batt["capacityAh"] = Core::cfgBatteryCapacityAh;
  batt["nominalV"]   = Core::cfgBatteryNominalVoltage;
  batt["fullV"]      = Core::cfgFullVoltage;
  batt["lowV"]       = Core::cfgLowVoltage;
  batt["idleA"]      = Core::cfgIdleCurrentThreshold;
  batt["endA"]       = Core::cfgFullChargeCurrentThreshold;
  batt["persistS"]   = Core::cfgFullChargePersistenceSec;
  batt["telS"]       = Core::cfgTelemetryIntervalSec;
  JsonObject cal = doc.createNestedObject("calibration");
  cal["version"] = Core::calibration.version;
  // ... (omit for brevity — full serialization mirrors saveCalibration)
  Utils::appendCRC(doc);
  String out; serializeJson(doc, out);
  return out;
}

bool ConfigStore::importAll(const String& json) {
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, json)) return false;
  if (!Utils::verifyCRC(doc)) return false;
  if (doc.containsKey("deviceName")) {
    const char* n = doc["deviceName"];
    if (n) { strncpy(Core::deviceName, n, 39); Core::deviceName[39] = '\0'; }
  }
  if (doc.containsKey("timezone")) {
    const char* t = doc["timezone"];
    if (t) { strncpy(Core::cfgTimezone, t, 39); Core::cfgTimezone[39] = '\0'; }
  }
  if (doc.containsKey("batteryConfig")) {
    JsonObject b = doc["batteryConfig"];
    Core::cfgBatteryCapacityAh          = b["capacityAh"] | Core::BATTERY_CAPACITY_AH;
    Core::cfgBatteryNominalVoltage            = b["nominalV"]   | Core::BATTERY_NOMINAL_V;
    Core::cfgFullVoltage                 = b["fullV"]      | Core::BATTERY_FULL_V;
    Core::cfgLowVoltage                  = b["lowV"]       | Core::BATTERY_LOW_V;
    Core::cfgIdleCurrentThreshold        = b["idleA"]      | Core::IDLE_CURRENT_THRESHOLD_A;
    Core::cfgFullChargeCurrentThreshold  = b["endA"]       | Core::FULL_CHARGE_CURRENT_THRESHOLD_A;
    Core::cfgFullChargePersistenceSec    = b["persistS"]   | Core::FULL_CHARGE_PERSISTENCE_SEC;
    Core::cfgTelemetryIntervalSec        = b["telS"]       | Core::cfgTelemetryIntervalSec;
    saveBatteryConfig();
  }
  saveDeviceConfig();
  Services::Log.append(Core::LogType::ConfigurationChanged, "Configuration imported", 0);
  return true;
}

} // namespace Storage
