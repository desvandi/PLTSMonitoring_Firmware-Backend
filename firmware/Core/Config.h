#ifndef PLTS_CONFIG_H
#define PLTS_CONFIG_H

#include <cstdint>
#include <cstddef>

//=============================================================================
// PLTS Monitor — Configuration Header
// Production-Grade 48V LiFePO4 PLTS Monitoring System
// Brief §2-9, §71-73, §98
//
// Phase 13-B (RC-6): All constants wrapped in `namespace Core` so that
// `Core::INA219_SHUNT_OHM` etc. resolve correctly across the codebase.
// Phase 13-B (RC-12): Compile-time constants (immutable hardware specs) are
// distinguished from runtime cfg* globals (mutable, persisted, loaded by
// ConfigStore at boot). See Globals.h for cfg* extern declarations.
//=============================================================================

// ---------------------------------------------------------------------------
// v1.6.0 — BMS/INVERTER COMMUNICATION FEATURE FLAGS
// Each protocol module compiles out entirely when disabled (RAM/flash budget:
// the spool was already trimmed to 4 slots — every KB counts). Override per
// environment in platformio.ini. Defaults below apply when the build does not
// pass -D flags (e.g. Arduino IDE).
// ---------------------------------------------------------------------------
#ifndef PLTS_ENABLE_BMS_COMM
#define PLTS_ENABLE_BMS_COMM 1        // master switch for the whole layer
#endif
#ifndef PLTS_ENABLE_PYLONTECH_CAN
#define PLTS_ENABLE_PYLONTECH_CAN 1   // TWAI @500k + SN65HVD230
#endif
#ifndef PLTS_ENABLE_MODBUS_RTU
#define PLTS_ENABLE_MODBUS_RTU 1      // UART2 + MAX3485 RS485
#endif
#ifndef PLTS_ENABLE_MODBUS_TCP
#define PLTS_ENABLE_MODBUS_TCP 1      // WiFi client polling
#endif
#ifndef PLTS_ENABLE_PYLONTECH_RS485
#define PLTS_ENABLE_PYLONTECH_RS485 0 // RESERVED — console protocol slot (not
#endif                                 // implemented; see README §RS485-console)

// v1.7.0 — RS485 vendor-frame CAPTURE console (bench tool for the reserved
// Pylontech RS485 slot). Strictly passive (DE pinned LOW, never transmits);
// runtime-activated ONLY via cfgBmsProtocol = "rs485_console".
#ifndef PLTS_ENABLE_RS485_CONSOLE
#define PLTS_ENABLE_RS485_CONSOLE 1
#endif

// ---------------------------------------------------------------------------
// v1.7.0 — E-WAVE EMERGENCY CONTROL LAYER (ported from firmware-generic)
// Relay + E-stop + GAS command polling. When enabled, this firmware is no
// longer purely monitoring-only: it drives ONE safety relay whose FAIL-SAFE
// direction is ISOLATED (active-LOW module; GPIO Hi-Z at reset/crash =
// de-energized = isolated). When disabled (0), the module compiles out and
// the system behaves exactly like v1.6.3 (monitoring-only).
// ---------------------------------------------------------------------------
#ifndef PLTS_ENABLE_EMERGENCY
#define PLTS_ENABLE_EMERGENCY 1
#endif
#ifndef PLTS_ENABLE_PZEM_AC
#define PLTS_ENABLE_PZEM_AC 0          // RESERVED — PZEM-004T AC power meter
#endif                                 // (bench-validation pending; see README §PZEM)

// ---------------------------------------------------------------------------
// Build Profile Guard (brief §73) — must select exactly one
// ---------------------------------------------------------------------------
#ifndef DEVELOPMENT_BUILD
#ifndef STAGING_BUILD
#ifndef PRODUCTION_BUILD
#error "No build profile selected. Define one of: -DDEVELOPMENT_BUILD | -DSTAGING_BUILD | -DPRODUCTION_BUILD"
#endif
#endif
#endif

#if defined(DEVELOPMENT_BUILD) + defined(STAGING_BUILD) + defined(PRODUCTION_BUILD) != 1
#error "Exactly ONE build profile must be defined."
#endif

#define BUILD_PROFILE_NAME \
  (defined(PRODUCTION_BUILD) ? "production" : (defined(STAGING_BUILD) ? "staging" : "development"))

namespace Core {

// ---------------------------------------------------------------------------
// Versioning (brief §74) — single source of truth
// ---------------------------------------------------------------------------
static constexpr const char* FIRMWARE_VERSION        = "1.7.1";  // W13: OTA boot-health fix (PENDING_VERIFY confirm via esp_ota_mark_app_valid_cancel_rollback + 60 s window; boot-attempt counting per boot, not per upload) + mixed-fleet manifest target self-check. Parity line with firmware-generic 1.7.1. Separate product line — see scripts/test_version_identity.py G7.
static constexpr const char* FIRMWARE_BUILD_DATE     = __DATE__ " " __TIME__;
static constexpr const char* PROTOCOL_VERSION         = "1";     // protocol v1 (PLTS)
static constexpr const char* CONFIG_SCHEMA_VERSION    = "1";
static constexpr const char* CALIBRATION_SCHEMA_VERSION = "1";
static constexpr const char* SPOOL_SCHEMA_VERSION     = "1";
static constexpr const char* JOURNAL_SCHEMA_VERSION   = "1";

// ---------------------------------------------------------------------------
// Canonical Hardware (brief §2)
// ---------------------------------------------------------------------------
static constexpr uint8_t  PIN_I2C_SDA       = 21;
static constexpr uint8_t  PIN_I2C_SCL       = 22;
static constexpr uint8_t  PIN_BATTERY_ADC    = 34;     // ADC1 CH6, input-only, WiFi-safe (brief §2.1)
static constexpr uint8_t  PIN_ACS712_ADC     = 35;     // ADC1 CH7, input-only
static constexpr uint32_t I2C_FREQUENCY      = 100000; // 100 kHz

// ---------------------------------------------------------------------------
// BMS / INVERTER COMMUNICATION PORTS (v1.6.0 multi-protocol feature)
// I2C (GPIO21/22) stays RESERVED for the internal sensor bus (INA219 + SHT31)
// — it is NOT a battery/inverter port. ACS712 remains analog on ADC1 GPIO35.
// ---------------------------------------------------------------------------
// RS485 half-duplex port (UART2 + MAX3485/MAX485 transceiver, 115200 8N1).
// Serves Modbus RTU today; the Pylontech RS485-console slot shares this port.
static constexpr uint8_t  PIN_RS485_TX       = 16;     // UART2 TX → MAX3485 DI
static constexpr uint8_t  PIN_RS485_RX       = 17;     // UART2 RX ← MAX3485 RO
static constexpr uint8_t  PIN_RS485_DE       = 4;      // DE+RE tied together (direction)
// v1.7.0 — RS485 capture-console bounds (Comm/Rs485Console.h).
static constexpr uint16_t RS485_FRAME_MAX_BYTES = 64;   // vendor console frames are short
static constexpr uint16_t RS485_FRAME_GAP_MS    = 6;    // bus-idle frame boundary
static constexpr uint8_t  RS485_FRAME_RING      = 16;   // last N frames served via REST
// CAN 2.0 port (TWAI controller + SN65HVD230 transceiver, 500 kbps).
static constexpr uint8_t  PIN_CAN_TX         = 25;     // TWAI TX → SN65HVD230 TXD
static constexpr uint8_t  PIN_CAN_RX         = 26;     // TWAI RX ← SN65HVD230 RXD

// ---------------------------------------------------------------------------
// v1.7.0 — E-WAVE emergency layer pins (parity with firmware-generic).
// Relay: 5V optocoupler module, ACTIVE-LOW input (GPIO LOW = energized =
// RUN; Hi-Z at boot/crash = de-energized = ISOLATED — the ESP32 must be
// ALIVE to keep the system running). E-stop sense: normally-closed physical
// line (INPUT_PULLUP; HIGH = OPEN = tripped); the E-stop itself breaks the
// relay module's negative supply IN HARDWARE — the sense line only latches.
// LED: local indicator (RUN solid / EMERGENCY 2 Hz blink).
// All three sit on ADC1-safe / non-strapping GPIOs, clear of the pin map
// above (21/22/34/35/16/17/4/25/26 all in use).
// ---------------------------------------------------------------------------
static constexpr uint8_t  PIN_EMERGENCY_RELAY = 27;    // relay module IN (active-LOW)
static constexpr int8_t   PIN_EMERGENCY_ESTOP = 14;    // E-stop sense (INPUT_PULLUP; -1 = disabled)
static constexpr uint8_t  PIN_EMERGENCY_LED   = 2;     // local state LED

// ---------------------------------------------------------------------------
// v1.7.0 — PZEM-004T v3 AC power meter (OPTIONAL, PLTS_ENABLE_PZEM_AC,
// default 0 — bench validation pending, see README §PZEM). Serial1 (UART1)
// 9600 8N1, isolated TTL UART. Wiring: ESP32 TX(19) -> PZEM RX,
// ESP32 RX(18) <- PZEM TX. Pins 18/19 chosen because they are free,
// non-strapping, and NOT ADC1 — pin 32 stays reserved for a possible future
// second ACS712 (genset channel of the E-WAVE layer).
// ---------------------------------------------------------------------------
static constexpr uint8_t  PIN_PZEM_RX         = 18;     // UART1 RX <- PZEM TX
static constexpr uint8_t  PIN_PZEM_TX         = 19;     // UART1 TX -> PZEM RX
static constexpr uint32_t PZEM_BAUD           = 9600;   // PZEM-004T v3 fixed rate
static constexpr uint32_t PZEM_POLL_MS        = 1000;   // 1 Hz measurement
static constexpr uint32_t PZEM_TIMEOUT_MS     = 400;    // response window
static constexpr uint8_t  PZEM_DEFAULT_ADDR   = 0x01;   // factory slave address

// E-WAVE default trigger config (runtime-overridable via NVS cfgEmg* /
// GAS EMERGENCY_CONFIG; validation ranges match Code.gs EMERGENCY_CONFIG_
// FIELDS exactly — mixed-version fleets must agree on the same table).
static constexpr float    EMG_VBAT_LOW_V        = 42.0f;   // trip below (V)
static constexpr float    EMG_VBAT_LOW_HYST_V   = 1.0f;    // hysteresis (V)
static constexpr float    EMG_VBAT_HIGH_V       = 55.0f;   // trip above (V)
static constexpr float    EMG_VBAT_HIGH_HYST_V  = 1.0f;    // hysteresis (V)
static constexpr float    EMG_IDC_OVER_A        = 110.0f;  // |I_dc| trip (A)
static constexpr float    EMG_IAC_LOAD_OVER_A   = 28.0f;   // I_ac load trip (A)
static constexpr float    EMG_IAC_GEN_OVER_A    = 28.0f;   // RESERVED genset channel (A)
static constexpr uint8_t  EMG_DEBOUNCE_N        = 3;       // consecutive violating ticks (10 Hz)
static constexpr uint32_t EMG_RECOVERY_SEC      = 60;      // clear-time before ARM
static constexpr uint8_t  EMG_SENSOR_FAIL_POLICY = 1;      // 1 = fail-closed (P1-SC3)

// BMS polling default (NVS-overridable via cfgBmsPollIntervalMs).
static constexpr uint32_t BMS_POLL_INTERVAL_MS    = 5000;
static constexpr uint8_t  BMS_MODBUS_SLAVE_ID     = 1;    // typical rack BMS default
static constexpr uint16_t BMS_MODBUS_TCP_PORT     = 502;  // IANA standard

// Sign conventions of the EXTERNAL protocols (verified on bench G-05):
// Pylontech CAN current is discharge-positive → negate to canonical +charge.
static constexpr float PYLONTECH_CAN_CURRENT_SIGN = -1.0f;
// Most Modbus rack BMSes also report discharge-positive → negate likewise.
// The shunt cross-check (BMS_CURRENT_MISMATCH alarm) catches a wrong constant
// within one poll cycle — see Comm/BatteryCommManager.h.
static constexpr float MODBUS_RACK_CURRENT_SIGN  = -1.0f;

// Cell-imbalance alarm threshold (15S LiFePO4 — healthy pack delta < ~80 mV).
static constexpr float BMS_CELL_IMBALANCE_V      = 0.250f;

// Runtime BMS protocol selection strings (NVS cfgBmsProtocol values).
static constexpr const char* BMS_PROTOCOL_DEFAULT = "auto";

// INA219 (brief §4-6)
static constexpr uint8_t  INA219_ADDRESS        = 0x40;
static constexpr float    INA219_SHUNT_OHM      = 0.00075f;   // 75mV @ 100A → 0.75 mΩ
static constexpr float    INA219_MAX_CURRENT_A  = 100.0f;
static constexpr uint16_t INA219_CONFIG         = 0x3FFB;     // 32V FSR, ±320mV PGA, 16-sample avg
// Sign correction: raw INA219 shunt voltage is POSITIVE when current leaves
// battery (discharge). Canonical software semantics require positive = charging.
// Therefore signCorrection = -1.0f inverts the raw reading.
// STATUS: ASSUMED — NOT HARDWARE VERIFIED. Requires INA-001..INA-004 (Phase 13-K).
static constexpr float    INA219_SIGN_CORRECTION = -1.0f;
// Idle deadband (brief §5) — ±0.5 A configurable at runtime via cfgIdleCurrentThreshold
static constexpr float    IDLE_CURRENT_THRESHOLD_A = 0.5f;

// [v1.9.0 / DYNAMIC-GAIN] INA219 PGA dynamic gain switching
// -----------------------------------------------------------------------------
// The PLTS bus spans 1 A (standby) to 150 A (peak load). A single PGA range
// cannot cover both: ±80 mV saturates at ~106 A (150 A × 0.75 mΩ = 112.5 mV),
// ±160 mV has poor resolution at 1–2 A. Solution: switch PGA on-the-fly.
//
// Two PGA modes:
//   PGA_80MV  — ±80 mV range (gain /8,  10 µV/bit). High resolution for standby.
//               Max measurable: 80 mV / 0.75 mΩ = 106.7 A
//   PGA_160MV — ±160 mV range (gain /4, 10 µV/bit). Full range for peak load.
//               Max measurable: 160 mV / 0.75 mΩ = 213.3 A (shunt saturates first)
//
// Hysteresis thresholds (prevent chattering at the boundary):
//   Switch UP   (80mV → 160mV) when |I| >= 100 A
//   Switch DOWN (160mV → 80mV) when |I| < 90 A
// The 10 A gap prevents rapid oscillation when load hovers near the threshold.
static constexpr float    INA219_PGA_SWITCH_UP_A    = 100.0f;  // switch to 160mV
static constexpr float    INA219_PGA_SWITCH_DOWN_A  = 90.0f;   // switch back to 80mV
// Max current the shunt can measure in each PGA mode (for saturation detection)
static constexpr float    INA219_PGA_80MV_MAX_A     = 106.0f;  // 80mV / 0.75mΩ
static constexpr float    INA219_PGA_160MV_MAX_A    = 150.0f;  // shunt physical limit (75mV@100A → 112.5mV@150A)
// INA219 config register values for each PGA mode
// Bits 13-12 (PGA): 00 = ±320mV, 01 = ±160mV, 10 = ±80mV, 11 = ±40mV
// Bits 11-7  (BADC): 1000 = 12-bit, 128 samples (oversampling for noise reduction)
// Bits 6-2   (SADC): 1000 = 12-bit, 128 samples
// Bits 1-0   (mode): 11 = shunt+bus continuous
// 0x3FFB = 32V FSR, ±320mV, 16-sample avg (legacy default — kept for backwards compat)
// 0x3BFF = 32V FSR, ±80mV,  12-bit/128-sample (standby mode — high resolution)
// 0x39FF = 32V FSR, ±160mV, 12-bit/128-sample (peak mode — full range)
static constexpr uint16_t INA219_CONFIG_PGA_80MV   = 0x3BFF;
static constexpr uint16_t INA219_CONFIG_PGA_160MV  = 0x39FF;
static constexpr uint16_t INA219_CONFIG_LEGACY     = 0x3FFB;   // ±320mV, 16-sample (old default)

// Battery voltage divider (brief §7 + v1.9.0 update)
// [v1.9.0 / DYNAMIC-GAIN] New divider for high-side battery measurement:
//   R1 = 190 kΩ (high side, battery+ to ADC pin)
//   R2 = 10 kΩ  (low side, ADC pin to GND)
//   Ratio = (R1+R2)/R2 = 200/10 = 20.0
//   At Vbat=57.5V → Vpin = 2.875V (safe for ESP32 ADC with 11dB attenuation)
//   0.1 µF ceramic cap parallel to R2 for high-freq noise filtering
// Previous: R1=100kΩ, R2=5.6kΩ, ratio ≈18.857 (legacy, kept as fallback)
static constexpr float DIVIDER_R1       = 190000.0f;  // 190 kΩ (was 100 kΩ)
static constexpr float DIVIDER_R2       = 10000.0f;   // 10 kΩ  (was 5.6 kΩ)
static constexpr float ADC_SERIES_R     = 0.0f;       // no series resistor (was 2.2 kΩ)
static constexpr float ADC_VREF          = 3.3f;
static constexpr uint16_t ADC_RESOLUTION = 4095;     // 12-bit
static constexpr uint8_t  ADC_ATTENUATION_DB = 11;    // 11 dB → ~3.3V full-scale
// Computed divider ratio (R1 + R2) / R2 — used by AdcVoltageDriver
static constexpr float DIVIDER_RATIO    = (DIVIDER_R1 + DIVIDER_R2) / DIVIDER_R2;  // = 20.0
// Fine-tune calibration multiplier (operator adjusts after multimeter comparison)
// Example: if true=50.0V but reads 49.8V → set to (50.0/49.8) = 1.004016
static constexpr float ADC_FINE_TUNE    = 1.000000f;
// Number of ADC samples to average per reading (noise reduction for standby V)
static constexpr uint8_t  ADC_NUM_SAMPLES = 64;
// Plausibility bounds for battery voltage (brief §9 — software must detect implausible)
static constexpr float VBAT_MIN_PLAUSIBLE = 30.0f;   // below this = OUT_OF_RANGE
static constexpr float VBAT_MAX_PLAUSIBLE = 60.0f;   // above this = OUT_OF_RANGE
// ADC filtering (EMA alpha — 0..1, higher = faster response, more noise)
static constexpr float ADC_FILTER_ALPHA   = 0.2f;

// Current processing
// [v1.9.0 / DYNAMIC-GAIN] Raised from 120A to 160A to accommodate peak load
// up to 150A (PGA 160mV mode supports up to 213A theoretically, but the shunt
// physical rating is 100A @ 75mV continuous, 150A peak short-duration).
static constexpr float CURRENT_SPIKE_REJECT_A = 160.0f;  // reject |I| > this (was 120A)
static constexpr float CURRENT_SMOOTH_ALPHA    = 0.3f;     // EMA smoothing

// ACS712 (brief §26-27)
static constexpr float    ACS712_SENSITIVITY       = 0.185f;     // V/A for ACS712-20A (configurable)
static constexpr float    ACS712_SENSITIVITY_MV_PER_A = 185.0f;  // mV/A (same as above in mV)
static constexpr uint16_t ACS712_SAMPLE_RATE_HZ    = 1000;        // 1 kHz for 50 Hz AC
static constexpr uint16_t ACS712_WINDOW_MS          = 40;          // 2 cycles of 50 Hz = 40 ms
static constexpr uint16_t ACS712_WINDOW_CYCLES     = 2;
static constexpr uint16_t ACS712_SAMPLES_PER_WINDOW = (ACS712_SAMPLE_RATE_HZ * ACS712_WINDOW_MS) / 1000; // 40
static constexpr float    AC_OVERCURRENT_THRESHOLD  = 30.0f;      // A (configurable)
// AC power estimation assumptions (brief §28 — AC power is ESTIMATED, not measured)
// These are configurable at runtime but defaults assume typical Indonesian grid
static constexpr float    ASSUMED_AC_VOLTAGE      = 220.0f;   // V (no AC voltage sensor)
static constexpr float    ASSUMED_POWER_FACTOR    = 0.9f;     // typical inductive load

// SHT31 (brief §29)
static constexpr uint8_t SHT31_ADDRESS = 0x44;

// ---------------------------------------------------------------------------
// Battery Canonical Profile (brief §3)
// ---------------------------------------------------------------------------
static constexpr float    BATTERY_NOMINAL_V    = 48.0f;
static constexpr float    BATTERY_FULL_V       = 54.0f;
static constexpr float    BATTERY_LOW_V        = 45.0f;
static constexpr uint8_t  BATTERY_SERIES_CELLS = 15;    // 15S LiFePO4
static constexpr float    BATTERY_CELL_NOMINAL_V = 3.2f;
static constexpr float    BATTERY_CELL_FULL_V  = 3.6f;
static constexpr float    BATTERY_CELL_LOW_V   = 3.0f;
static constexpr float    BATTERY_CAPACITY_AH  = 200.0f;   // compile-time default; runtime cfgBatteryCapacityAh overrides

// Hysteresis (brief §24)
static constexpr float BATTERY_LOW_CLEAR_V  = 46.0f;   // clear LOW alarm above this
static constexpr float BATTERY_HIGH_V       = 55.0f;   // HIGH alarm
static constexpr float BATTERY_HIGH_CLEAR_V  = 54.5f;

// Full-charge detection (brief §19) — compile-time defaults; runtime cfg* overrides
static constexpr float    FULL_CHARGE_CURRENT_THRESHOLD_A = 2.0f;   // configurable
static constexpr uint32_t FULL_CHARGE_PERSISTENCE_SEC      = 600;    // 10 min, configurable

// Overcurrent (brief §25 — monitoring only, no control)
static constexpr float OVERCURRENT_CHARGE_A    = 80.0f;
static constexpr float OVERCURRENT_DISCHARGE_A = 100.0f;

// SOC thresholds (brief §24 — for alarm, NOT for control)
static constexpr float    BATTERY_LOW_SOC_PCT     = 20.0f;   // low SOC warning
static constexpr float    BATTERY_CRIT_SOC_PCT   = 10.0f;   // critical SOC alarm
// Temperature thresholds (brief §30)
static constexpr float    TEMP_HIGH_THRESHOLD_C    = 40.0f;
static constexpr float    TEMP_CRIT_THRESHOLD_C   = 50.0f;
static constexpr float    HUMIDITY_HIGH_PCT       = 85.0f;

// ---------------------------------------------------------------------------
// Telemetry (brief §39-43)
// ---------------------------------------------------------------------------
static constexpr uint32_t TELEMETRY_INTERVAL_MS     = 5000;     // 5s publish
static constexpr uint32_t SENSOR_SAMPLE_INTERVAL_MS  = 200;     // 5 Hz INA219/ADC
static constexpr uint32_t SHT31_SAMPLE_INTERVAL_MS   = 1000;    // 1 Hz
static constexpr uint32_t ENERGY_CALC_INTERVAL_MS     = 1000;   // 1 Hz integration
static constexpr uint32_t PERSIST_INTERVAL_MS        = 300000;  // 5 min NVS save (wear)
// [FW-17] Sequence high-water mark margin: the persisted telemetry sequence
// is stored as (current + margin) so that after ANY reboot the resumed
// sequence is strictly GREATER than every pre-reboot value. Margin covers the
// maximum counter increments possible between two checkpoints:
// PERSIST_INTERVAL_MS / SENSOR_SAMPLE_INTERVAL_MS = 1500 → margin 2048.
// Result: gaps may be reported (honest — the interval is unknown), but the
// sequence NEVER regresses across a reboot (backend dedupe stays sound).
static constexpr uint32_t SEQ_REBOOT_MARGIN          = 2048;

// Spool (brief §43)
static constexpr uint8_t  SPOOL_RAM_SIZE          = 16;
static constexpr uint8_t  SPOOL_NVS_CRITICAL_SIZE = 8;
static constexpr uint8_t  MAX_REPLAY_PER_SEC     = 2;

// Transaction journal (brief §42)
// [FW-27 REMEDIATION 2026-08] 64 slots × ~1.2 KB ≈ 76 KB cannot fit the old
// 20 KB NVS partition — putBytes would start failing once full and command
// idempotency would silently degrade. Reduced to 16 slots (~19 KB) AND the
// NVS partition is enlarged to 64 KB (partitions_ota_1mb5.csv), keeping the
// journal + SOC + spool-critical + config namespaces within budget.
static constexpr uint8_t JOURNAL_SIZE = 16;

// Alarm registry (brief §34)
static constexpr uint8_t MAX_ALARMS = 24;

// ---------------------------------------------------------------------------
// Network (brief §47-49)
// ---------------------------------------------------------------------------
static constexpr uint16_t  HTTP_PORT               = 80;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS  = 15000;
static constexpr uint32_t MQTT_CONNECT_TIMEOUT_MS  = 10000;
static constexpr uint32_t MQTT_RECONNECT_MIN_MS    = 5000;
static constexpr uint32_t MQTT_RECONNECT_MAX_MS    = 60000;
static constexpr uint16_t MQTT_BUFFER_SIZE         = 16384;
static constexpr uint32_t NTP_SYNC_INTERVAL_MS     = 3600000; // 1 hour
static constexpr char     DEFAULT_TIMEZONE[]       = "Asia/Jakarta";

// GAS (brief §49, §95)
static constexpr uint32_t GAS_POST_INTERVAL_MS = 3600000;  // 1 hour
static constexpr uint32_t GAS_TIMEOUT_MS      = 30000;
static constexpr uint16_t GAS_MAX_BODY_SIZE   = 16384;
static constexpr uint8_t  GAS_MAX_POSTS_PER_HOUR = 10;

// OTA (brief §72)
// [FW-27] app partitions resized 0x180000 → 0x170000 to fund the larger NVS.
static constexpr uint32_t OTA_MAX_SIZE          = 0x170000;  // 1.4375 MB
static constexpr uint8_t  OTA_MAX_BOOT_ATTEMPTS = 3;
static constexpr uint16_t OTA_TIMEOUT_MS        = 60000;
// [W13-1] A fresh OTA image must run stably this long before it is confirmed
// (esp_ota_mark_app_valid_cancel_rollback). Matches firmware-generic's
// OTA_HEALTHY_AFTER_MS so both trees share one activation criterion.
static constexpr uint32_t OTA_HEALTHY_AFTER_MS  = 60000;
static constexpr const char* OTA_ALLOWED_HOSTS[] = {
  "github.com",
  "raw.githubusercontent.com",
  "objects.githubusercontent.com",
  nullptr
};

// Auth (brief §71)
static constexpr uint32_t JWT_ACCESS_TTL_SEC       = 900;     // 15 min
static constexpr uint32_t JWT_REFRESH_TTL_SEC      = 604800;  // 7 days
static constexpr uint16_t PBKDF2_ITERATIONS         = 10000;
static constexpr uint8_t  MAX_REFRESH_TOKENS        = 4;      // NVS LRU
static constexpr uint8_t  MAX_TRACKED_IPS           = 8;
static constexpr uint8_t  CSRF_TOKEN_LEN           = 32;
static constexpr uint16_t MAX_ACTIVITY_LOG_ENTRIES  = 200;
static constexpr uint16_t RATE_LIMIT_SHORT_BLOCK_SEC = 60;
static constexpr uint16_t RATE_LIMIT_LONG_BLOCK_SEC  = 300;
static constexpr uint8_t  RATE_LIMIT_SHORT_THRESHOLD = 5;
static constexpr uint8_t  RATE_LIMIT_LONG_THRESHOLD  = 10;
// [P0-004] Failure window: failures older than this no longer count toward
// a block. Prevents a permanent block from slow-drip attacks and gives
// legitimate clients a clean slate after the window passes.
static constexpr uint32_t RATE_LIMIT_WINDOW_MS       = 600000;  // 10 min
static constexpr uint32_t FACTORY_RESET_TOKEN_TTL_SEC = 60;
static constexpr uint32_t AUTH_BLOCK_SHORT_MS        = RATE_LIMIT_SHORT_BLOCK_SEC * 1000;
static constexpr uint32_t AUTH_BLOCK_LONG_MS         = RATE_LIMIT_LONG_BLOCK_SEC * 1000;
static constexpr uint32_t FACTORY_RESET_TOKEN_TTL_MS = FACTORY_RESET_TOKEN_TTL_SEC * 1000;

// HTTP body limits
static constexpr uint16_t HTTP_MAX_BODY_SIZE        = 16384;
static constexpr uint16_t MAX_LOG_ENTRIES           = 200;
static constexpr uint32_t AUDIT_LOG_ROTATE_BYTES    = 8192;

// Auth credentials
// NVS namespaces + keys
static constexpr const char* NVS_NAMESPACE = "plts";
static constexpr const char* NVS_KEY_WIFI_SSID = "wifi_ssid";
static constexpr const char* NVS_KEY_WIFI_PASS = "wifi_pass";
static constexpr int8_t WIFI_TX_POWER_DBM = 20;
static constexpr uint32_t WIFI_STA_TIMEOUT_MS = 15000;
static constexpr uint8_t WIFI_STA_MAX_RETRIES = 3;
static constexpr uint8_t WIFI_CHANNEL = 11;
static constexpr const char* AP_SSID_PREFIX = "PLTS-Setup";
static constexpr uint8_t  MAX_USER_LEN   = 32;
static constexpr uint8_t  SALT_LEN       = 16;
static constexpr uint8_t  PASS_HASH_HEX_LEN = 65;  // 32 bytes × 2 + null

// Topic structure (brief §48)
// plts/<deviceId>/status   QoS 0  — telemetry publish (5s)
// plts/<deviceId>/log      QoS 0  — log events
// plts/<deviceId>/online   QoS 1 retain — presence (LWT)
// plts/<deviceId>/config   QoS 1  — config commands (PWA → ESP32)
// plts/<deviceId>/ack      QoS 1  — command ACKs (ESP32 → PWA)
// plts/<deviceId>/ota      QoS 1  — OTA commands
static constexpr const char* MQTT_TOPIC_PREFIX = "plts";

// LittleFS paths
static constexpr const char* PATH_CONFIG_JSON = "/config.json";
static constexpr const char* PATH_CONFIG_BAK  = "/config.bak";
static constexpr const char* PATH_CALIB_JSON  = "/calibration.json";
static constexpr const char* PATH_CALIB_BAK   = "/calibration.bak";
static constexpr const char* PATH_AUDIT_LOG   = "/audit.log";
static constexpr const char* PATH_ACTIVITY_LOG = "/activity.log";
static constexpr const char* PATH_CALIBRATION_JSON = "/calibration.json";
static constexpr const char* PATH_CALIBRATION_BAK = "/calibration.bak";
static constexpr const char* PATH_CALIBRATION_TMP = "/calibration.tmp";
static constexpr uint8_t CONFIG_SCHEMA_VERSION_NUM = 1;
extern bool calibrationDirty;

} // namespace Core

// ---------------------------------------------------------------------------
// Security (brief §71, §98) — production fail-closed guards
// These #error checks MUST remain at file scope (outside namespace) because
// they are preprocessor directives evaluated before namespace resolution.
// ---------------------------------------------------------------------------
#ifdef PRODUCTION_BUILD
  // [WAVE-5 / FW-C1] Value-shape validation (public-broker refusal, wildcard
  // CORS, 64-hex Ed25519 key, PEM presence) moved OUT of the preprocessor —
  // strcmp()/strlen() are NOT valid #if expressions, so the old lines
  // GUARANTEED a compile failure for any production build that supplied real
  // credentials ("missing binary operator before token '('"). Those checks
  // are enforced — and were already duplicated — by
  // firmware/scripts/assert_production_secrets.py (fail-closed pre-build
  // gate: is_hex64, not_public_broker, not_wildcard, PEM body, placeholder
  // scan). Presence checks below remain compile-time; value checks are the
  // script's job.
  #if !defined(MQTT_BROKER_HOST) || !defined(MQTT_BROKER_PORT)
  #error "PRODUCTION_BUILD requires MQTT_BROKER_HOST and MQTT_BROKER_PORT"
  #endif
  #if MQTT_BROKER_PORT != 8883 && MQTT_BROKER_PORT != 8884
  #error "PRODUCTION_BUILD requires MQTT TLS port 8883 or 8884"
  #endif
  #if !defined(MQTT_USERNAME) || !defined(MQTT_PASSWORD)
  #error "PRODUCTION_BUILD requires MQTT_USERNAME and MQTT_PASSWORD"
  #endif
  #if !defined(MQTT_ROOT_CA)
  #error "PRODUCTION_BUILD requires MQTT_ROOT_CA (PEM)"
  #endif
  #if !defined(ALLOWED_CORS_ORIGINS)
  #error "PRODUCTION_BUILD requires ALLOWED_CORS_ORIGINS"
  #endif
  #if !defined(OTA_ED25519_PUBLIC_KEY_HEX)
  #error "PRODUCTION_BUILD requires OTA_ED25519_PUBLIC_KEY_HEX (64 hex chars)"
  #endif
  #if !defined(OTA_HTTPS_ROOT_CA)
  #error "PRODUCTION_BUILD requires OTA_HTTPS_ROOT_CA (PEM)"
  #endif
#endif

#endif // PLTS_CONFIG_H
