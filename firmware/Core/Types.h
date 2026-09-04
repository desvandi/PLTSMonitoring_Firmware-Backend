#ifndef PLTS_TYPES_H
#define PLTS_TYPES_H

//=============================================================================
// PLTS Monitor — Canonical Type Definitions
// Brief §1, §31-37 — Measurement quality is the core principle.
// "Never fabricate certainty." Every measurement has value/unit/quality/source/timestamp/sequence.
//
// Phase 13-B changes:
//   - RC-7: All enum values use PascalCase (Valid, Stale, Invalid, etc.)
//           to match .cpp usage. String serialization remains UPPERCASE
//           for telemetry/PWA/GAS wire compatibility.
//   - RC-5: CalibrationPoint is now a STRUCT (reference + raw + timestamp),
//           not an enum. The enum is removed. A separate CalibrationPointId
//           enum identifies which point (Low/Nominal/Full).
//   - RC-11: Measurement struct is the single canonical measurement type.
//            Duplicate types in Globals.h (BatteryTelemetry, etc.) are
//            consolidated here or removed.
//=============================================================================

#include <cstdint>
#include <cstddef>
#include <cmath>

namespace Core {

// ---------------------------------------------------------------------------
// Measurement Quality (brief §1) — immutable contract
// PascalCase per D-4. Wire format (JSON/telemetry) remains UPPERCASE string.
// ---------------------------------------------------------------------------
enum class MeasurementQuality : uint8_t {
  Valid,          // measurement is trustworthy and current
  Stale,          // measurement is old (past freshness window)
  Invalid,        // measurement failed validation
  OutOfRange,     // measurement is outside plausible physical range
  SensorError,    // sensor reported an error (I2C failure, CRC error, etc.)
  NotAvailable,   // sensor not initialized / not present (DISTINCT from SensorError)
  Estimated,      // value is an estimate (e.g., SOC, AC power from assumptions)
  Derived,        // value is derived from other measurements (e.g., power = V × I)
  Calibrating,    // sensor is in calibration mode
  Suspect         // measurement is suspect (anomaly detected but not confirmed invalid)
};

inline const char* qualityToStr(MeasurementQuality q) {
  switch (q) {
    case MeasurementQuality::Valid:        return "VALID";
    case MeasurementQuality::Stale:        return "STALE";
    case MeasurementQuality::Invalid:      return "INVALID";
    case MeasurementQuality::OutOfRange:   return "OUT_OF_RANGE";
    case MeasurementQuality::SensorError:   return "SENSOR_ERROR";
    case MeasurementQuality::NotAvailable: return "NOT_AVAILABLE";
    case MeasurementQuality::Estimated:    return "ESTIMATED";
    case MeasurementQuality::Derived:      return "DERIVED";
    case MeasurementQuality::Calibrating:  return "CALIBRATING";
    case MeasurementQuality::Suspect:      return "SUSPECT";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Measurement Source (brief §37) — distinguishes measured vs derived vs estimated
// ---------------------------------------------------------------------------
enum class MeasurementSource : uint8_t {
  Measured,   // directly measured by a sensor (battery V, battery I, AC I, T, H)
  Derived,    // computed from measured values (power = V × I, dew point)
  Estimated   // estimated with assumptions (SOC, runtime, AC power from assumed V/PF)
};

inline const char* sourceToStr(MeasurementSource s) {
  switch (s) {
    case MeasurementSource::Measured:  return "MEASURED";
    case MeasurementSource::Derived:   return "DERIVED";
    case MeasurementSource::Estimated: return "ESTIMATED";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// SOC Provenance (v1.6.0 multi-protocol BMS integration)
// Answers "WHERE does the SOC number come from?" — the operator-facing honesty
// field. Quality says *how trustworthy*; provenance says *whose measurement*.
// Cascade priority: BMS_DIRECT > SHUNT_COULOMB > OCV_ESTIMATED > UNKNOWN.
// Transitions are LOGGED, never silent (directive: no silent fallback).
// ---------------------------------------------------------------------------
enum class SocProvenance : uint8_t {
  Unknown,       // SOC is NaN / not yet resolved
  BmsDirect,     // read directly from the battery BMS (CAN/Modbus) — freshest truth
  ShuntCoulomb,  // INA219 shunt coulomb counting (BMS absent or lost)
  OcvEstimated   // boot-time open-circuit-voltage estimate (rest-gated, coarse)
};

inline const char* socProvenanceToStr(SocProvenance p) {
  switch (p) {
    case SocProvenance::Unknown:       return "UNKNOWN";
    case SocProvenance::BmsDirect:     return "BMS_DIRECT";
    case SocProvenance::ShuntCoulomb:  return "SHUNT_COULOMB";
    case SocProvenance::OcvEstimated:  return "OCV_ESTIMATED";
  }
  return "UNKNOWN";
}

inline SocProvenance socProvenanceFromStr(const char* s) {
  if (!s || !*s) return SocProvenance::Unknown;
  if (strcasecmp(s, "BMS_DIRECT") == 0)    return SocProvenance::BmsDirect;
  if (strcasecmp(s, "SHUNT_COULOMB") == 0) return SocProvenance::ShuntCoulomb;
  if (strcasecmp(s, "OCV_ESTIMATED") == 0) return SocProvenance::OcvEstimated;
  return SocProvenance::Unknown;
}

// ---------------------------------------------------------------------------
// Generic Measurement (brief §36) — the atomic telemetry unit
// value is NaN when quality is Invalid/SensorError/NotAvailable — NEVER 0.
// ---------------------------------------------------------------------------
struct Measurement {
  float value;                    // NaN if invalid/unavailable — NEVER 0
  MeasurementQuality quality;
  MeasurementSource source;
  uint32_t timestamp;            // Unix epoch seconds (wall-clock, for telemetry)
  uint32_t sequence;              // monotonic per-device

  bool isValid() const {
    return (quality == MeasurementQuality::Valid ||
            quality == MeasurementQuality::Derived ||
            quality == MeasurementQuality::Estimated) && !std::isnan(value);
  }
  bool isNull() const {
    return std::isnan(value) ||
           quality == MeasurementQuality::Invalid ||
           quality == MeasurementQuality::SensorError ||
           quality == MeasurementQuality::NotAvailable;
  }
};

// Helper: construct an invalid measurement (NaN + quality)
inline Measurement makeInvalidMeasurement(MeasurementQuality q, uint32_t ts, uint32_t seq) {
  Measurement m;
  m.value = NAN;
  m.quality = q;
  m.source = MeasurementSource::Measured;
  m.timestamp = ts;
  m.sequence = seq;
  return m;
}

// Helper: construct a valid measurement
inline Measurement makeMeasurement(float value, MeasurementSource source,
                                    MeasurementQuality quality, uint32_t ts, uint32_t seq) {
  Measurement m;
  m.value = value;
  m.quality = quality;
  m.source = source;
  m.timestamp = ts;
  m.sequence = seq;
  return m;
}

// ---------------------------------------------------------------------------
// Battery Direction (brief §5) — sign convention immutable
// Positive current = CHARGING (entering battery)
// Negative current = DISCHARGING (leaving battery)
// Near-zero (within deadband) = IDLE
// ---------------------------------------------------------------------------
enum class Direction : uint8_t {
  Charging,
  Discharging,
  Idle
};

inline const char* directionToStr(Direction d) {
  switch (d) {
    case Direction::Charging:    return "CHARGING";
    case Direction::Discharging: return "DISCHARGING";
    case Direction::Idle:        return "IDLE";
  }
  return "UNKNOWN";
}

inline Direction classifyDirection(float currentA, float idleThreshold) {
  if (std::isnan(currentA)) return Direction::Idle;
  if (currentA > idleThreshold)  return Direction::Charging;
  if (currentA < -idleThreshold) return Direction::Discharging;
  return Direction::Idle;
}

// ---------------------------------------------------------------------------
// SOC State Machine (brief §20)
// ---------------------------------------------------------------------------
enum class SocState : uint8_t {
  Normal,           // default state
  Charging,         // current > 0 and not full candidate
  FullCandidate,   // V >= 54 and I < threshold, awaiting persistence period
  FullConfirmed,   // persistence period elapsed, SOC = 100%
  Synchronized      // SOC has been synchronized to a known reference
};

inline const char* socStateToStr(SocState s) {
  switch (s) {
    case SocState::Normal:        return "NORMAL";
    case SocState::Charging:      return "CHARGING";
    case SocState::FullCandidate: return "FULL_CANDIDATE";
    case SocState::FullConfirmed: return "FULL_CONFIRMED";
    case SocState::Synchronized:  return "SYNCHRONIZED";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// System Health (brief §33)
// ---------------------------------------------------------------------------
enum class SystemState : uint8_t {
  Healthy,
  Warning,
  Degraded,
  Failed,
  Recovering
};

inline const char* systemStateToStr(SystemState s) {
  switch (s) {
    case SystemState::Healthy:    return "HEALTHY";
    case SystemState::Warning:    return "WARNING";
    case SystemState::Degraded:   return "DEGRADED";
    case SystemState::Failed:     return "FAILED";
    case SystemState::Recovering: return "RECOVERING";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Sensor Health (brief §31-32)
// ---------------------------------------------------------------------------
enum class SensorHealth : uint8_t {
  Online,
  Offline,
  Error,
  Recovering
};

inline const char* sensorHealthToStr(SensorHealth s) {
  switch (s) {
    case SensorHealth::Online:     return "ONLINE";
    case SensorHealth::Offline:    return "OFFLINE";
    case SensorHealth::Error:      return "ERROR";
    case SensorHealth::Recovering: return "RECOVERING";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Alarm (brief §34)
// ---------------------------------------------------------------------------
enum class AlarmSeverity : uint8_t {
  Info,
  Warning,
  Critical
};

inline const char* severityToStr(AlarmSeverity s) {
  switch (s) {
    case AlarmSeverity::Info:     return "INFO";
    case AlarmSeverity::Warning:   return "WARNING";
    case AlarmSeverity::Critical: return "CRITICAL";
  }
  return "UNKNOWN";
}

enum class AlarmLifecycle : uint8_t {
  Active,
  Acknowledged,
  Cleared
};

inline const char* lifecycleToStr(AlarmLifecycle l) {
  switch (l) {
    case AlarmLifecycle::Active:       return "ACTIVE";
    case AlarmLifecycle::Acknowledged: return "ACKNOWLEDGED";
    case AlarmLifecycle::Cleared:     return "CLEARED";
  }
  return "UNKNOWN";
}

// Alarm codes (brief §35) — string constants for telemetry wire format
namespace AlarmCode {
  // Battery
  constexpr const char* BATTERY_VOLTAGE_LOW            = "BATTERY_VOLTAGE_LOW";
  constexpr const char* BATTERY_VOLTAGE_HIGH           = "BATTERY_VOLTAGE_HIGH";
  constexpr const char* BATTERY_VOLTAGE_INVALID       = "BATTERY_VOLTAGE_INVALID";
  constexpr const char* BATTERY_OVERCURRENT_CHARGE    = "BATTERY_OVERCURRENT_CHARGE";
  constexpr const char* BATTERY_OVERCURRENT_DISCHARGE = "BATTERY_OVERCURRENT_DISCHARGE";
  constexpr const char* BATTERY_CURRENT_SENSOR_ERROR  = "BATTERY_CURRENT_SENSOR_ERROR";
  constexpr const char* BATTERY_CURRENT_SENSOR_SUSPECT= "BATTERY_CURRENT_SENSOR_SUSPECT";
  constexpr const char* BATTERY_SOC_LOW                = "BATTERY_SOC_LOW";
  // BMS / external comm (v1.6.0 multi-protocol integration)
  constexpr const char* BMS_PROTOCOL_LOST            = "BMS_PROTOCOL_LOST";
  constexpr const char* BMS_CURRENT_MISMATCH         = "BMS_CURRENT_MISMATCH";
  constexpr const char* BMS_CELL_IMBALANCE           = "BMS_CELL_IMBALANCE";
  constexpr const char* BMS_FAULT                    = "BMS_FAULT";
  // Environment
  constexpr const char* TEMPERATURE_HIGH        = "TEMPERATURE_HIGH";
  constexpr const char* TEMPERATURE_CRITICAL    = "TEMPERATURE_CRITICAL";
  constexpr const char* HUMIDITY_HIGH           = "HUMIDITY_HIGH";
  constexpr const char* CONDENSATION_RISK       = "CONDENSATION_RISK";
  constexpr const char* SHT31_FAILURE            = "SHT31_FAILURE";
  // AC
  constexpr const char* AC_OVERCURRENT              = "AC_OVERCURRENT";
  constexpr const char* AC_CURRENT_SENSOR_ERROR     = "AC_CURRENT_SENSOR_ERROR";
  constexpr const char* AC_CURRENT_SENSOR_STALE     = "AC_CURRENT_SENSOR_STALE";
  // System
  constexpr const char* DEVICE_OFFLINE         = "DEVICE_OFFLINE";
  constexpr const char* TELEMETRY_STALE        = "TELEMETRY_STALE";
  constexpr const char* TIME_UNSYNCED          = "TIME_UNSYNCED";
  constexpr const char* STORAGE_ERROR           = "STORAGE_ERROR";
  constexpr const char* NETWORK_DEGRADED        = "NETWORK_DEGRADED";
  constexpr const char* OTA_FAILURE             = "OTA_FAILURE";
  constexpr const char* CONFIGURATION_ERROR     = "CONFIGURATION_ERROR";
  constexpr const char* CALIBRATION_ERROR       = "CALIBRATION_ERROR";
  constexpr const char* WATCHDOG_RESET          = "WATCHDOG_RESET";
  constexpr const char* BROWNOUT_RESET          = "BROWNOUT_RESET";
  constexpr const char* BOOT_LOOP               = "BOOT_LOOP";
  // Emergency layer (v1.7.0 E-WAVE port) — raised on ANY transition into
  // EMERGENCY (sensor trip / E-stop / operator DISARM / crash-loop hold),
  // cleared on operator ARM. The relay itself is latched in hardware; this
  // alarm exists so alarm-center/PWA visibility matches relay reality.
  constexpr const char* EMERGENCY_TRIP          = "EMERGENCY_TRIP";
  // Relay layer (v1.8.0 8-channel relay integration)
  constexpr const char* RELAY_FAULT              = "RELAY_FAULT";
  constexpr const char* RELAY_MAX_ON_TIME        = "RELAY_MAX_ON_TIME";
  constexpr const char* RELAY_INTERLOCK_VIOLATION= "RELAY_INTERLOCK_VIOLATION";
  constexpr const char* RELAY_STATE_DRIFT        = "RELAY_STATE_DRIFT";
  constexpr const char* RELAY_COMMAND_REJECTED   = "RELAY_COMMAND_REJECTED";
  constexpr const char* RELAY_CONFIGURATION_ERROR= "RELAY_CONFIGURATION_ERROR";
}

// ---------------------------------------------------------------------------
// Time Quality (brief §46)
// ---------------------------------------------------------------------------
enum class TimeQuality : uint8_t {
  Valid,
  Unsynced
};

inline const char* timeQualityToStr(TimeQuality t) {
  switch (t) {
    case TimeQuality::Valid:    return "VALID";
    case TimeQuality::Unsynced: return "UNSYNCED";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Health Supervisor Task IDs (brief §78)
// ---------------------------------------------------------------------------
enum class TaskId : uint8_t {
  Ina219Battery,
  Acs712,
  AdcVoltage,
  Sht31,
  Mqtt,
  Telemetry,
  Ota,
  HealthMonitor,
  Persistence,
  BmsComm,      // v1.6.0: battery/inverter protocol manager task
  Emergency,    // v1.7.0: E-WAVE supervisor tick (local-first, 10 Hz)
  GasEmergency, // v1.7.0: GAS command poll / ACK / event flush task
  Relay,        // v1.8.0: 8-channel relay controller tick (5 Hz)
  COUNT
};

constexpr uint8_t TASK_COUNT = static_cast<uint8_t>(TaskId::COUNT);

inline const char* taskIdToStr(TaskId t) {
  switch (t) {
    case TaskId::Ina219Battery:  return "ina219Battery";
    case TaskId::Acs712:        return "acs712";
    case TaskId::AdcVoltage:    return "adcVoltage";
    case TaskId::Sht31:         return "sht31";
    case TaskId::Mqtt:          return "mqtt";
    case TaskId::Telemetry:    return "telemetry";
    case TaskId::Ota:          return "ota";
    case TaskId::HealthMonitor: return "healthMonitor";
    case TaskId::Persistence:  return "persistence";
    case TaskId::BmsComm:      return "bmsComm";
    case TaskId::Emergency:    return "emergency";
    case TaskId::GasEmergency: return "gasEmergency";
    case TaskId::Relay:        return "relay";
    case TaskId::COUNT:        break;
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// Reset Reasons — CANONICAL esp_reset_reason_t mapping
// [FW-19 REMEDIATION 2026-08] The old table decoded the esp_reset_reason_t
// IDF enum using ROM hardware reset-code labels (three mutually inconsistent
// tables existed across Types.h and HealthSupervisor). Result: brownout
// (enum 8) was reported as "TG1WDT_SYS_RESET", generic WDT (enum 6) as
// "BROWNOUT_RESET", and the HealthSupervisor brownout counter (checked at
// 11) never fired. All tables now use the single canonical mapping below.
// ---------------------------------------------------------------------------
inline const char* resetReasonStr(uint8_t reason) {
  switch (reason) {
    case 0:  return "UNKNOWN";            // ESP_RST_UNKNOWN
    case 1:  return "POWERON_RESET";      // ESP_RST_POWERON
    case 2:  return "SW_RESET";           // ESP_RST_SW (esp_restart / OTA)
    case 3:  return "SW_PANIC_RESET";     // ESP_RST_PANIC (exception/abort)
    case 4:  return "INT_WDT_RESET";      // ESP_RST_INT_WDT (interrupt WDT)
    case 5:  return "TASK_WDT_RESET";     // ESP_RST_TASK_WDT
    case 6:  return "WDT_RESET";          // ESP_RST_WDT (other watchdog)
    case 7:  return "DEEPSLEEP_RESET";    // ESP_RST_DEEPSLEEP
    case 8:  return "BROWNOUT_RESET";     // ESP_RST_BROWNOUT
    case 9:  return "SDIO_RESET";         // ESP_RST_SDIO
    case 10: return "USB_RESET";          // ESP_RST_USB
    case 11: return "JTAG_RESET";         // ESP_RST_JTAG
    case 12: return "EFUSE_RESET";        // ESP_RST_EFUSE
    case 13: return "PWR_GLITCH_RESET";   // ESP_RST_PWR_GLITCH
    default: return "UNKNOWN_RESET";
  }
}

// [FW-19] Canonical classification helpers (single source of truth).
inline bool isWatchdogReset(uint8_t reason) {
  return reason == 4 || reason == 5 || reason == 6;   // INT_WDT / TASK_WDT / WDT
}
inline bool isBrownoutReset(uint8_t reason) {
  return reason == 8;                                  // ESP_RST_BROWNOUT
}

// ---------------------------------------------------------------------------
// Voltage Calibration (brief §11-14)
// ---------------------------------------------------------------------------
// RC-5: CalibrationPoint is now a STRUCT (reference + raw + timestamp),
// not an enum. The canonical representation of a 3-point calibration point.
struct CalibrationPoint {
  float    reference;    // known-good voltage from trusted multimeter (V)
  float    raw;          // ESP32 reading at that reference (V, pre-correction)
  uint32_t timestamp;    // when this point was captured (Unix epoch seconds)
};

// Identifies which calibration point (Low / Nominal / Full)
enum class CalibrationPointId : uint8_t {
  Low,       // 45.0 V
  Nominal,   // user-provided actual reference (~50 V)
  Full       // 54.0 V
};

inline const char* calibrationPointIdToStr(CalibrationPointId p) {
  switch (p) {
    case CalibrationPointId::Low:      return "LOW";
    case CalibrationPointId::Nominal:   return "NOMINAL";
    case CalibrationPointId::Full:      return "FULL";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// AC Signal Quality (brief §26)
// ---------------------------------------------------------------------------
enum class AcSignalQuality : uint8_t {
  Good,
  Degraded,
  Poor,
  Invalid
};

inline const char* acSignalQualityToStr(AcSignalQuality q) {
  switch (q) {
    case AcSignalQuality::Good:     return "GOOD";
    case AcSignalQuality::Degraded: return "DEGRADED";
    case AcSignalQuality::Poor:    return "POOR";
    case AcSignalQuality::Invalid: return "INVALID";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Auth Attempt (used by AuthManager rate limiter)
// ---------------------------------------------------------------------------
struct AuthAttempt {
  // [P0-004] Packed IPv4 (network order, 0 = empty slot). char[40] string
  // storage was never populated by the old implementation — every client
  // shared the literal "unknown" bucket. IPv4 only: the ESP32 WebServer
  // remoteIP() is AF_INET; IPv6 is unsupported by this stack.
  uint32_t ip;
  uint8_t  count;
  uint32_t firstFailTime;   // monotonic ms — failure window anchor
  uint32_t lastFailTime;    // monotonic ms — LRU eviction anchor
  uint32_t blockUntil;      // monotonic ms — 0 = not blocked
};

// ---------------------------------------------------------------------------
// Log Types (used by LogService)
// ---------------------------------------------------------------------------
enum class LogType : uint8_t {
  Boot,
  WifiConnected,
  WifiDisconnected,
  TimeSynced,
  SensorFailure,
  SensorRecovered,
  AlarmActive,
  AlarmAcknowledged,
  AlarmCleared,
  SocBaselineCorrected,
  CalibrationChanged,
  ConfigurationChanged,
  OtaStarted,
  OtaSuccess,
  OtaFailed,
  StorageError,
  Login,
  AuthFail,
  Logout,
  Reboot,
  FactoryReset,
  Info,
  Custom
};

inline const char* logTypeToStr(LogType t) {
  switch (t) {
    case LogType::Boot:                 return "DEVICE_BOOT";
    case LogType::WifiConnected:        return "WIFI_CONNECTED";
    case LogType::WifiDisconnected:     return "WIFI_DISCONNECTED";
    case LogType::TimeSynced:           return "TIME_SYNCED";
    case LogType::SensorFailure:        return "SENSOR_FAILURE";
    case LogType::SensorRecovered:     return "SENSOR_RECOVERED";
    case LogType::AlarmActive:          return "ALARM_ACTIVE";
    case LogType::AlarmAcknowledged:   return "ALARM_ACKNOWLEDGED";
    case LogType::AlarmCleared:         return "ALARM_CLEARED";
    case LogType::SocBaselineCorrected: return "SOC_BASELINE_CORRECTED";
    case LogType::CalibrationChanged:   return "CALIBRATION_CHANGED";
    case LogType::ConfigurationChanged: return "CONFIGURATION_CHANGED";
    case LogType::OtaStarted:           return "OTA_STARTED";
    case LogType::OtaSuccess:            return "OTA_SUCCESS";
    case LogType::OtaFailed:             return "OTA_FAILED";
    case LogType::StorageError:         return "STORAGE_ERROR";
    case LogType::Login:                return "LOGIN";
    case LogType::AuthFail:             return "AUTH_FAIL";
    case LogType::Logout:               return "LOGOUT";
    case LogType::Reboot:               return "REBOOT";
    case LogType::FactoryReset:         return "FACTORY_RESET";
    case LogType::Info:                 return "INFO";
    case LogType::Custom:               return "CUSTOM";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// [v1.8.0] 8-Channel Relay Types
// ---------------------------------------------------------------------------

// Relay state confidence — honest reporting of software vs verified state
enum class RelayStateConfidence : uint8_t {
  SoftwareOnly,  // GPIO commanded, no physical feedback (default)
  Verified,      // Aux contact confirms (future HW)
  Unknown,       // Never commanded / boot indeterminate
  Fault          // State drift / driver error
};

inline const char* relayStateConfidenceToStr(RelayStateConfidence c) {
  switch (c) {
    case RelayStateConfidence::SoftwareOnly: return "SOFTWARE_ONLY";
    case RelayStateConfidence::Verified:     return "VERIFIED";
    case RelayStateConfidence::Unknown:      return "UNKNOWN";
    case RelayStateConfidence::Fault:        return "FAULT";
  }
  return "UNKNOWN";
}

// Relay command source — provenance for audit trail
enum class RelaySource : uint8_t {
  Off,         // default state, no command
  Manual,      // operator REST/MQTT command
  Schedule,    // RTC-based schedule (future)
  Automation,  // remote automation rule (future)
  Safety,      // safety supervisor (maxOnTime FORCE OFF, etc.)
  System,      // system (E-WAVE cascade, boot policy)
};

inline const char* relaySourceToStr(RelaySource s) {
  switch (s) {
    case RelaySource::Off:        return "OFF";
    case RelaySource::Manual:     return "MANUAL";
    case RelaySource::Schedule:   return "SCHEDULE";
    case RelaySource::Automation: return "AUTOMATION";
    case RelaySource::Safety:     return "SAFETY";
    case RelaySource::System:     return "SYSTEM";
  }
  return "UNKNOWN";
}

// Relay safety lockout state machine (5-state, NVS-persisted)
// NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
// ACK = operator has seen alarm (NOT permission to re-enable)
// CLEAR requires fault condition resolved
enum class RelayLockoutState : uint8_t {
  Normal,       // operating normally
  Tripped,      // safety condition detected (maxOnTime, fault)
  Acknowledged, // operator has acknowledged the alarm
  Cleared,      // fault condition resolved, awaiting ARM
  Armed         // re-armed, will return to NORMAL on next tick
};

inline const char* relayLockoutStateToStr(RelayLockoutState s) {
  switch (s) {
    case RelayLockoutState::Normal:       return "NORMAL";
    case RelayLockoutState::Tripped:      return "TRIPPED";
    case RelayLockoutState::Acknowledged: return "ACKNOWLEDGED";
    case RelayLockoutState::Cleared:      return "CLEARED";
    case RelayLockoutState::Armed:        return "ARMED";
  }
  return "UNKNOWN";
}

// Relay command semantics
enum class RelayCommandSemantics : uint8_t {
  IdempotentState   = 0,  // SET_STATE: ON/OFF — replay-safe
  NonIdempotentAction = 1, // PULSE — NOT replay-safe (has duration)
};

} // namespace Core

#endif // PLTS_TYPES_H
