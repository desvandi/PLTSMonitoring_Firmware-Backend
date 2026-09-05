/* PLTS Monitor Generic Firmware v1.9.3
 * Flash once. Runtime WiFi/GAS credentials live in LittleFS /config.json.
 * Monitoring only: this firmware never controls an inverter, charger, or relay.
 *
 * v1.8.0 additions (W14 — bench wave: OTA rollback observability):
 *   - [W14-2a] applyOta() resets the NVS boot-try ledger and stores the
 *     flashed version ("lfver") at Update.end success. The old ledger
 *     accumulated across image generations: two power-blipped updates left
 *     tries=2 in NVS, so a perfectly healthy THIRD image would boot at
 *     tries=2 → 3 and INSTANTLY self-rollback at setup. Reset-at-write
 *     makes the counter per-image, as documented.
 *   - [W14-2b] Bootloader-initiated reverts are now OBSERVABLE. Verified
 *     against the real arduino-esp32 2.0.17 sdkconfig + IDF v4.4.7
 *     bootloader sources: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y gives a
 *     fresh image exactly ONE unconfirmed boot — ANY reset before the 60 s
 *     confirm (power blip included) makes the bootloader mark the image
 *     ABORTED and revert to the previous one, SILENTLY (the ROLLBACK
 *     branch below could never fire — attempt #2 of the new image never
 *     happens). The reverted image now detects running < lastFlashed at
 *     boot and reports an honest OTA_STATUS ROLLBACK to GAS once STA is
 *     up; the NVS marker survives reboots and is consumed only after the
 *     report is actually delivered (HTTP 200).
 *   - [W14-2c] reportOtaStatus() returns the HTTP code so the deferred
 *     ROLLBACK report can retry until delivered.
 *
 * v1.8.0 additions (W13 — mixed-fleet OTA hardening):
 *   - [W13-2] OTA manifest target self-check: Code.gs now serves a `target`
 *     field ('' = fleet-wide, 'generic' | 'modular') with every manifest. A
 *     manifest explicitly targeted at the other firmware tree is REFUSED
 *     here (honest OTA_STATUS event) BEFORE any download — closing the
 *     mixed-fleet cross-flash where a generic device would pull a modular
 *     image (or vice versa) through the shared HMAC trust domain and retry
 *     every hour. DEPLOY ORDER: redeploy Code.gs BEFORE publishing a
 *     targeted manifest (GAS must emit the target field first).
 *   - [W13-4] platformio.ini partition comment corrected (default.csv OTA
 *     slots are 1.25 MB each, not 1.5 MB).
 *
 * v1.7.0 additions (P1-REMEDIATION / audit wave — sensor fail-closed):
 *   - [P1-SC1] NEW CONFIG field `sensorFailPolicy` (0/1, default 1).
 *     Policy 1 (default, fail-closed): the INA219 + ACS712 current sensors
 *     are treated as MANDATORY SAFETY INPUTS, because the emergency layer
 *     uses them as trip triggers (I_DC_OVER / I_AC_LOAD_OVER / I_AC_GEN_OVER).
 *     ARM is REJECTED while any safety sensor is absent/invalid, and a
 *     sensor that dies mid-run (I2C loss) TRIPS the system to ISOLATED
 *     after the standard debounce window. Rationale: for a safety
 *     interlock, "unmonitored" IS unsafe — the trip cannot fire on a
 *     sensor that is not reporting.
 *     Policy 0 (legacy, explicit operator opt-out for bench/commissioning):
 *     old behavior — unmonitored ≠ unsafe. Documented as unsafe for
 *     production. Mixed-version fleets: field absent → firmware default 1.
 *   - [P1-SC2] ARM gate hardening: invalid iDc/iAc/iGen now block ARM when
 *     policy=1 (previously only vBat was fail-closed).
 *   - [P1-SC3] SENSOR_LOSS runtime trip (debounced, policy-gated): a running
 *     system that loses a safety sensor isolates instead of silently
 *     continuing without overcurrent protection.
 *   - SCHEMA: EMERGENCY_CONFIG_FIELDS is now 13 fields (sensorFailPolicy
 *     appended) — PWA + GAS updated in lockstep; GAS/PWA omit the field →
 *     firmware default (1) applies.
 *   - DEPLOY ORDER: redeploy Code.gs BEFORE publishing the v1.7.0 manifest
 *     (GAS must accept the 13th field first).
 *
 * v1.6.0 additions (E-WAVE — emergency relay layer):
 *   - Emergency relay (active-LOW opto), E-stop sense, 5 sensor trip
 *     triggers, ARM/DISARM operator commands with local re-validation,
 *     NVS trip counter + crash-chain detector, 2nd ACS712 channel.
 *
 * v1.5.4 additions (WAVE-6 / firmware audit completion):
 *   - [FW6-1] Every OTA failure path now reports OTA_STATUS DOWNLOAD_FAILED
 *     (and REFUSED for version-policy refusals) to GAS — OtaEvents had the
 *     schema but nothing ever sent it; failures were serial-only whispers.
 *   - [FW6-5] Telemetry POST failures get an honest capped backoff. GAS
 *     always answers HTTP 200 with an embedded status — the [TX] log used
 *     to print "HTTP=200" even when the body said ERROR. Now the envelope
 *     status is parsed: failures back off (x2/cycle, cap 10 min) instead of
 *     hammering a dead/revoked endpoint at full cadence forever.
 *   - [FW6-6] AP password NVS persist is verified by read-back; a failed
 *     persist now warns LOUDLY (silent failure = new password every boot =
 *     operator locked out with a password they faithfully wrote down).
 *   - [FW6-7] /save validates ssid/token/device_key/interval server-side —
 *     the form's `required` attribute was the only gate (browsers can be
 *     told to ignore it).
 *   - [FW6-8] OTA boot-health marking no longer requires STA: a fresh image
 *     that runs stably for 60 s in setup/AP mode is marked valid (rollback
 *     criterion is BOOT health, not connectivity — an image must not be
 *     rolled back just because the router is down).
 *   - [FW6-9] Per-device OTA manifest key: HMAC key is now derived as
 *     HMAC-SHA256(auth_token, device_key) instead of the raw fleet token.
 *     GAS serves per-device hmacs to fw >= 1.5.4; a leaked AUTH_TOKEN alone
 *     can no longer author OTA manifests for the fleet. Legacy devices
 *     (< 1.5.4, no fw_version in request) keep the fleet-keyed hmac.
 *     DEPLOY ORDER: redeploy Code.gs BEFORE publishing the v1.5.4 manifest.
 *
 * v1.5.3 additions (WAVE-5 / firmware audit):
 *   - [FW-A1] CALIBRATION_ACK transport pinned to GTS Root R4 — the 6th TLS
 *     site that Wave 4 missed (it carried AUTH_TOKEN + DEVICE_KEY plaintext).
 *   - [FW-A2/A7] Calibration range clamped IN FIRMWARE (v [0.1,100],
 *     i [0.1,50] — mirrors GAS-2-K server gate; defense-in-depth so a stale
 *     GAS or a mixed-version fleet can never park a zero/negative factor).
 *   - [FW-A3] Setup AP now WPA2-protected with a CSPRNG 12-char password
 *     generated once at first boot (NVS), printed ONCE at generation —
 *     same commissioning model as the production firmware (F-G15).
 *   - [FW-A4] gasUrl and OTA manifest URLs must be https:// (explicit).
 *   - [FW-A6] Anti-downgrade: OTA manifest version must be strictly newer
 *     (numeric semver compare — "1.10.0" > "1.9.0", string compare lies).
 *   - [FW-A5] WiFi loss mid-run: bounded reconnect (2 min) before restart —
 *     kills the ~2 s restart loop that wrote NVS on every cycle.
 *
 * v1.5.1 additions (WAVE-3 / GAS-2-I — contract sync with Code.gs v2):
 *   - CALIBRATION_ACK now carries `device_key`: Code.gs binds every ACK to
 *     the device the command was queued for. A bare command_id is rejected
 *     400 (fail-closed) — device A can no longer swallow device B's command.
 *
 * v1.5.0 additions (WAVE-1 / GAS-2-A — contract sync with Code.gs v2):
 *   - TELEMETRY now carries a MONOTONIC `sequence` (identity: device_key +
 *     sequence). High-water mark persisted in NVS with a reboot margin —
 *     numbers are never reused across reboots; numbers skipped by the
 *     margin surface as honest ledger gaps on GAS, never fabricated rows.
 *   - v1.4.0 and earlier NEVER sent `sequence` → Code.gs v2 rejected every
 *     TELEMETRY with 400 "requires a numeric sequence" (re-audit GAS-2-A).
 *
 * v1.1.0 additions:
 *   - Captive Portal now auto-fills form fields when reached with
 *     `http://192.168.4.1/#plts=<base64-json>` (QR onboarding).
 *   - Signed OTA: firmware polls GAS `OTA_MANIFEST` and verifies
 *     HMAC-SHA256(AUTH_TOKEN, "version|url|sha256") before flashing.
 *     Downloaded image is SHA-256 verified again before commit.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
#include "RootCas.h"   // [WAVE-4 / GAS-2-D] pinned TLS anchors (GTS R4 + ISRG X1)
#include <Wire.h>
#include <nvs.h>
#include <nvs_flash.h>

// ============================================================================
// Constants — Hardware Assignment Audit v1.3.0
// ----------------------------------------------------------------------------
// DC domain (baterai 48V):
//   • Tegangan  : pembagi resistif → ADC GPIO 34 (VOLTAGE_PIN)
//   • Arus DC   : INA219 di I²C 0x40 dengan Rshunt eksternal 100A/75mV
//                 (0.75 mΩ). Register shunt voltage dibaca langsung; LSB 10 µV.
// AC domain (output inverter 220V):
//   • Arus AC   : ACS712-30A (versi Modified 3.3V) di ADC GPIO 35
//                 (AC_CURRENT_PIN), sampling RMS 2 siklus @ 50 Hz.
// ============================================================================
static const char*    FIRMWARE_VERSION   = "1.9.3";  // v1.9.3: version-parity with modular tree (reproducible build release). Measurement behavior identical to v1.9.2 — this tree embeds no __DATE__/__TIME__, so its binaries were already wall-clock-free; the v1.9.3 bump keeps the fleet-wide version identity aligned.
static const char*    CONFIG_PATH        = "/config.json";
static const uint8_t  RESET_PIN          = 0;         // BOOT button
static const uint8_t  LED_PIN            = 2;         // Built-in LED
static const uint8_t  SAMPLE_COUNT       = 10;        // Moving-Average window
static const uint8_t  VOLTAGE_PIN        = 34;        // ADC1_CH6 — V-Bat DC
static const uint8_t  AC_CURRENT_PIN     = 35;        // ADC1_CH7 — ACS712 AC (inverter→load)
// v1.6.0 [E-WAVE] — 2nd ACS712 (genset→inverter feed) + emergency relay + E-stop sense.
// All on ADC1 / safe GPIOs (never ADC2 — ADC2 is unusable while WiFi is on).
static const uint8_t  AC_GEN_PIN         = 32;        // ADC1_CH4 — ACS712 AC #2 (genset)
static const uint8_t  RELAY_EMERGENCY_PIN = 27;       // Emergency relay module IN (active-LOW)
static const int8_t   ESTOP_SENSE_PIN    = 14;        // E-stop line sense (INPUT_PULLUP, -1 = disabled)
static const bool     RELAY_ACTIVE_LOW   = true;      // module opto: IN=LOW → relay ENERGIZED
static const uint8_t  I2C_SDA_PIN        = 21;
static const uint8_t  I2C_SCL_PIN        = 22;
static const uint8_t  INA219_ADDR        = 0x40;
static const uint8_t  INA219_REG_SHUNT   = 0x01;      // shunt voltage register
static const uint8_t  INA219_REG_CONFIG  = 0x00;
static const float    INA219_SHUNT_OHM   = 0.00075f;  // 100A / 75mV shunt
static const float    INA219_LSB_UV      = 10.0f;     // 10 µV per bit
static const float    ACS712_SENSITIVITY = 0.066f;    // V per A (30A module)
static const float    ACS712_ZERO_V      = 1.65f;     // Modified 3.3V midpoint
static const uint16_t AC_RMS_SAMPLES     = 400;       // ≈2 cycles @ 50Hz
static const uint16_t AC_RMS_INTERVAL_US = 100;
static const uint32_t WIFI_TIMEOUT_MS    = 45000UL;
static const uint32_t AP_FALLBACK_MS     = 300000UL;
static const uint32_t FACTORY_RESET_MS   = 10000UL;
static const uint32_t HTTP_TIMEOUT_MS    = 7000UL;
static const uint32_t OTA_INTERVAL_MS    = 3600000UL;
static const uint32_t OTA_HTTP_TIMEOUT   = 60000UL;
static const uint32_t CALIB_INTERVAL_MS  = 300000UL;  // 5 min
// v1.6.0 [E-WAVE] — emergency layer timing. The command poll rides alongside
// the telemetry cadence (piggyback) plus this dedicated poll — bounded extra
// GAS load (~2x telemetry request rate, well inside consumer quotas).
static const uint32_t EMERGENCY_POLL_MS  = 15000UL;   // dedicated EMERGENCY_PENDING poll
static const uint32_t EMERGENCY_EVENT_MIN_INTERVAL_MS = 5000UL;  // event POST rate limit
static const uint32_t EMERGENCY_EVENT_MAX_TRIES = 20; // drop after 20 failed attempts
static const uint32_t EMERGENCY_LED_BLINK_MS = 250;   // isolated → 2 Hz blink
static const uint8_t  WDT_TIMEOUT_S      = 20;
static const uint8_t  OTA_MAX_BOOT_ATTEMPTS = 3;
static const uint32_t OTA_HEALTHY_AFTER_MS  = 60000UL;
static const char*    NVS_NAMESPACE      = "plts";
static const char*    NVS_KEY_BOOT_TRIES = "boot_tries";
// [W14-2b] Last OTA-flashed version (string) — lets a reverted (older)
// image detect the bootloader rollback and report it to GAS.
static const char*    NVS_KEY_LAST_FLASHED = "lfver";
static const char*    NVS_KEY_AP_PASS    = "ap_pass";   // [FW-A3] WPA2 setup AP
// v1.6.0 [E-WAVE] — emergency NVS keys: trip counter + crash-chain detector
// ("did the previous boot reach 5 min of stable runtime?").
static const char*    NVS_KEY_EMG_TRIPS  = "emg_trips";
static const char*    NVS_KEY_EMG_RUN_OK = "emg_run_ok";
static const char*    NVS_KEY_EMG_CHAIN  = "emg_chain";
static const uint8_t  EMG_CRASH_CHAIN_LIMIT = 3;       // ≥3 unhealthy reboots → CRASHLOOP hold

// [FW-A2/A7] Calibration plausibility bounds — mirrors the GAS-2-K server
// gate (Code.gs CALIBRATION_PUBLISH). Firmware-side clamp = defense-in-depth:
// vCalib is a dimensionless divider multiplier, iCalib* are fine trims.
static const float    CALIB_V_MIN  = 0.1f;
static const float    CALIB_V_MAX  = 100.0f;
static const float    CALIB_I_MIN  = 0.1f;
static const float    CALIB_I_MAX  = 50.0f;

// [FW-A5] WiFi-loss recovery: bounded reconnect before resorting to restart.
static const uint32_t STA_RECONNECT_WINDOW_MS = 120000UL;  // 2 min honest retry
static const uint32_t STA_RECONNECT_EVERY_MS  = 10000UL;   // retry cadence

// [FW6-5] Telemetry failure backoff: doubles per consecutive failure,
// capped. Sampling keeps running — only the POST cadence backs off.
static const uint8_t  TX_BACKOFF_MAX_SHIFT = 5;      // x2^5 = x32 max multiplier
static const uint32_t TX_BACKOFF_CAP_MS    = 600000UL;  // 10 min

// [FW6-8] Runtime reference for OTA boot-health marking (any mode).
uint32_t runtimeStartedAt = 0;

// [W14-2b] Deferred ROLLBACK report state — armed by checkBootloaderRevert()
// at boot when the bootloader reverted an unconfirmed image; delivered by
// the STA loop as soon as WiFi is up (marker cleared only on HTTP 200).
bool   otaRollbackReportPending = false;
String otaRollbackReportVersion = "";

// Forward declaration — Arduino's prototype inserter places generated
// prototypes above this point, BEFORE struct OtaManifest below; without
// this line those prototypes (e.g. `OtaManifest fetchOtaManifest()`) fail.
struct OtaManifest;

// Returns a clamped value or NAN when the input is not a finite number.
float clampCalibration(float v, float lo, float hi) {
  if (!isfinite(v)) return NAN;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// [FW-A6] Numeric semver compare: a > b -> +1, a < b -> -1, equal -> 0.
// String compare would rank "1.10.0" < "1.9.0" — the classic downgrade lie.
int semverCompare(const String& a, const String& b) {
  uint32_t amaj = 0, amin = 0, apat = 0;
  uint32_t bmaj = 0, bmin = 0, bpat = 0;
  if (sscanf(a.c_str(), "%u.%u.%u", &amaj, &amin, &apat) != 3) return -2;
  if (sscanf(b.c_str(), "%u.%u.%u", &bmaj, &bmin, &bpat) != 3) return -2;
  if (amaj != bmaj) return amaj > bmaj ? 1 : -1;
  if (amin != bmin) return amin > bmin ? 1 : -1;
  if (apat != bpat) return apat > bpat ? 1 : -1;
  return 0;
}

// [WAVE-1 / GAS-2-A] Monotonic telemetry sequence — NVS high-water mark.
//   SEQ_REBOOT_MARGIN  : jump applied on every boot so numbers are never
//                        reused across reboots/crashes.
//   SEQ_PERSIST_EVERY  : NVS write cadence during runtime (flash-wear bound).
// Worst case (crash just before a periodic persist): the next boot resumes
// at persistedBase + MARGIN, still strictly above every number ever used —
// skipped numbers become honest ledger gaps on GAS, never collisions.
static const char*    NVS_KEY_SEQ_BASE   = "seq_base";
static const uint32_t SEQ_REBOOT_MARGIN  = 256;
static const uint32_t SEQ_PERSIST_EVERY  = 64;

// ============================================================================
// Forward-declared types (Arduino auto-inserts function prototypes at file top
// which need to see these definitions before use).
// ============================================================================
struct OtaManifest {
  bool   valid = false;
  String version;
  String url;
  String sha256;
  String hmac;
  size_t size = 0;
  // [W13-2] Manifest target ('' = fleet-wide, 'generic', 'modular').
  // Echoed by Code.gs so the device can self-check even when the operator
  // has not (yet) filled firmware_type in the DEVICES sheet — defense in
  // depth against cross-flashing in a mixed fleet.
  String target;
};

WebServer server(80);
DNSServer dnsServer;

// v1.6.0 [E-WAVE] — Emergency trigger configuration. ALL fields are
// operator-adjustable at runtime (PWA → GAS EMERGENCY_COMMAND/CONFIG →
// device applies + persists). Validation ranges mirror Code.gs
// EMERGENCY_CONFIG_FIELDS exactly (mixed-version fleets must never park
// garbage in LittleFS).
struct EmergencyConfig {
  float    vbatLowV      = 42.0f;   // trip below (V)      [30..60]
  float    vbatLowHystV  = 1.0f;    // clear above trip+hyst [0.1..5]
  float    vbatHighV     = 55.0f;   // trip above (V)      [48..60]
  float    vbatHighHystV = 1.0f;    // clear below trip-hyst [0.1..5]
  float    iDcOverA      = 110.0f;  // |I| trip (A)        [10..120]
  float    iAcLoadOverA  = 28.0f;   // inverter→load trip  [5..40]
  float    iAcGenOverA   = 28.0f;   // genset→inverter trip [5..40]
  uint8_t  debounceN     = 3;       // consecutive samples to confirm [1..10]
  uint32_t recoverySec   = 60;      // min time post-clear before ARM [0..3600]
  uint8_t  relayPin      = 27;      // GPIO of the relay IN line [12..39]
  int8_t   estopPin      = 14;      // E-stop sense GPIO, -1 = disabled [-1..39]
  uint8_t  estopEnabled  = 1;       // 1 = monitor the E-stop line [0..1]
  // v1.7.0 [P1-SC1] — safety-sensor failure policy (13th schema field).
  //   1 (default) = fail-closed: current sensors are MANDATORY safety inputs
  //                 (they feed I_DC/I_AC_LOAD/I_AC_GEN trip triggers).
  //                 ARM rejected + runtime SENSOR_LOSS trip while any safety
  //                 sensor is absent/invalid.
  //   0 = legacy opt-out (bench/commissioning only, documented unsafe).
  uint8_t  sensorFailPolicy = 1;    // [0..1]
};

// v1.6.0 [E-WAVE] — Emergency relay runtime state.
//   RUN       : relay ENERGIZED (GPIO LOW on active-LOW module) — system on.
//   EMERGENCY : relay DE-ENERGIZED — system isolated. LATCHED: only an
//               operator ARM (validated locally) clears it.
enum class EmgState { RUN, EMERGENCY };

struct RuntimeConfig {
  String   ssid;
  String   password;
  String   gasUrl;
  String   token;
  String   deviceKey;
  uint16_t interval    = 15;
  float    vCalib      = 11.0f;   // voltage divider multiplier (dimensionless)
  float    iCalibDc    = 1.0f;    // INA219 fine trim (dimensionless)
  float    iCalibAc    = 1.0f;    // ACS712 fine trim (dimensionless)
  float    acZeroVolt  = 1.65f;   // ACS712 midpoint (auto-tared at boot)
  float    acGenZeroVolt = 1.65f; // ACS712 #2 midpoint (auto-tared at boot)
  EmergencyConfig emg;            // v1.6.0 — emergency trigger config
} config;

enum class Mode { AP_MODE, STA_MODE };
Mode currentMode = Mode::AP_MODE;

// Moving-Average windows — one per measurement channel
float    voltageSamples[SAMPLE_COUNT] = {};
float    dcCurrentSamples[SAMPLE_COUNT] = {};
float    acCurrentSamples[SAMPLE_COUNT] = {};
float    acGenCurrentSamples[SAMPLE_COUNT] = {};   // v1.6.0 — ACS712 #2
uint8_t  sampleIndex     = 0;
bool     ina219Present   = false;
uint32_t buttonDownAt  = 0;
uint32_t apStartedAt   = 0;
uint32_t lastTelemetry = 0;
uint32_t lastOtaCheck  = 0;
uint32_t lastCalibCheck = 0;
uint32_t staConnectedAt = 0;
bool     otaHealthyMarked = false;

// v1.6.0 [E-WAVE] — emergency runtime state (state itself is RAM-only: every
// boot re-enters EMERGENCY = fail-safe; the trip COUNTER is NVS-persisted).
EmgState emgState         = EmgState::EMERGENCY;
String   emgReason        = "BOOT";       // BOOT | VBAT_LOW | VBAT_HIGH | I_DC_OVER | I_AC_LOAD_OVER | I_AC_GEN_OVER | SENSOR_LOSS | ESTOP | OPERATOR | CRASHLOOP
uint32_t emgTripAtMs      = 0;            // when the current trip started
uint32_t emgTrips         = 0;            // lifetime counter (NVS)
bool     emgEstopOpen     = false;        // last raw E-stop sense reading
// v1.7.0 [P1-SC3] — slot 5 = safety-sensor-loss (policy-gated).
uint8_t  emgDebounce[6]   = {0,0,0,0,0,0};// consecutive-violation counters (vbatLo, vbatHi, iDc, iAc, iAcGen, sensorLoss)
uint32_t emgClearAtMs     = 0;            // when ALL triggers became clear
uint32_t lastEmgPoll      = 0;            // dedicated EMERGENCY_PENDING cadence
uint32_t lastEmgEventAt   = 0;            // event POST rate limiting
String   emgPendingEvent;                 // unsent event (type|reason|state)
uint8_t  emgEventTries    = 0;
uint8_t  emgCrashChain    = 0;            // consecutive unhealthy boots (NVS)
uint32_t emgLastLedToggle = 0;
bool     emgLedState      = false;

// [WAVE-1 / GAS-2-A] Monotonic telemetry sequence counter.
uint32_t seqCounter       = 0;
uint32_t seqLastPersisted = 0;

// [FW6-5] Consecutive telemetry POST failures + next-attempt deadline.
uint8_t  txConsecutiveFails = 0;
uint32_t txNextAttemptAt    = 0;

// ============================================================================
// NVS helpers — track consecutive failed boot attempts for OTA rollback
// ============================================================================
uint8_t nvsGetBootTries() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
  uint8_t value = 0;
  nvs_get_u8(handle, NVS_KEY_BOOT_TRIES, &value);
  nvs_close(handle);
  return value;
}

void nvsSetBootTries(uint8_t value) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_set_u8(handle, NVS_KEY_BOOT_TRIES, value);
  nvs_commit(handle);
  nvs_close(handle);
}

// [W14-2b] Last OTA-flashed version marker ("" = none pending).
String nvsGetLastFlashed() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return "";
  char buf[24] = {0};
  size_t len = sizeof(buf);
  if (nvs_get_str(handle, NVS_KEY_LAST_FLASHED, buf, &len) == ESP_OK) {
    nvs_close(handle);
    return String(buf);
  }
  nvs_close(handle);
  return "";
}

void nvsSetLastFlashed(const String& value) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  if (value.length() == 0) nvs_erase_key(handle, NVS_KEY_LAST_FLASHED);
  else                     nvs_set_str(handle, NVS_KEY_LAST_FLASHED, value.c_str());
  nvs_commit(handle);
  nvs_close(handle);
}

// [WAVE-1 / GAS-2-A] Sequence high-water mark in NVS (u32).
uint32_t nvsGetSeqBase() {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return 0;
  uint32_t value = 0;
  nvs_get_u32(handle, NVS_KEY_SEQ_BASE, &value);
  nvs_close(handle);
  return value;
}

void nvsSetSeqBase(uint32_t value) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_set_u32(handle, NVS_KEY_SEQ_BASE, value);
  nvs_commit(handle);
  nvs_close(handle);
}

// Boot-time initialization: jump MARGIN above the persisted base and
// reserve MARGIN ahead for the NEXT boot. If the very first write of this
// boot survives, monotonicity is guaranteed even if we crash mid-flight.
void initSequence() {
  uint32_t base = nvsGetSeqBase();
  seqCounter = (base == 0) ? 1 : base + SEQ_REBOOT_MARGIN;
  nvsSetSeqBase(seqCounter + SEQ_REBOOT_MARGIN);
  seqLastPersisted = seqCounter;
  Serial.printf("[SEQ] sequence counter starts at %lu (base=%lu)\n",
                (unsigned long)seqCounter, (unsigned long)base);
}

// Runtime persistence policy — one NVS write per SEQ_PERSIST_EVERY sends.
void persistSequenceIfDue() {
  if (seqCounter - seqLastPersisted < SEQ_PERSIST_EVERY) return;
  nvsSetSeqBase(seqCounter + SEQ_REBOOT_MARGIN);
  seqLastPersisted = seqCounter;
}

// Forward declaration of GAS status reporter (used by OTA rollback path).
// [W14-2c] Returns the HTTP code (0 = not attempted) so callers can retry.
int reportOtaStatus(const char* event, const String& version, const String& message);
// [W14-2b] Bootloader-revert detection — defined next to handleOtaRollback().
void checkBootloaderRevert();

// ============================================================================
// Config persistence
// ============================================================================
// [FW-A3] WPA2 setup-AP password — CSPRNG, generated ONCE at first boot,
// persisted in NVS, revealed exactly once at generation (commissioning
// moment — same model as the production firmware, audit F-G15). Lost it?
// Hold BOOT 10 s (factory reset) to regenerate.
String getOrCreateApPassword() {
  char pass[13];
  size_t len = sizeof(pass);
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_str(handle, NVS_KEY_AP_PASS, pass, &len) == ESP_OK &&
        len == 12) {
      nvs_close(handle);
      return String(pass);
    }
    nvs_close(handle);
  }
  static const char cs[] = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 12; i++) pass[i] = cs[esp_random() % (sizeof(cs) - 1)];
  pass[12] = '\0';
  bool persisted = false;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_str(handle, NVS_KEY_AP_PASS, pass);
    persisted = (nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
  }
  // [FW6-6] Verify by read-back: a password that silently failed to persist
  // regenerates at every boot — the operator wrote down a password that
  // stops working after the first reboot. That is a lock-out wearing an
  // audit-proof costume; warn loudly instead.
  if (persisted) {
    char verify[13]; size_t vlen = sizeof(verify);
    nvs_handle_t rh;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &rh) == ESP_OK) {
      persisted = (nvs_get_str(rh, NVS_KEY_AP_PASS, verify, &vlen) == ESP_OK &&
                   vlen == 12 && memcmp(verify, pass, 12) == 0);
      nvs_close(rh);
    }
  }
  Serial.println("=========================================================");
  if (persisted) {
    Serial.println("[AP] Password WiFi setup BARU (CATAT & SIMPAN!):");
    Serial.printf("  %s\n", pass);
    Serial.println("[AP] Hanya ditampilkan SEKALI saat di-generate.");
  } else {
    Serial.println("[AP] !!! PERINGATAN: password GAGAL tersimpan ke NVS !!!");
    Serial.printf("  Password sementara: %s\n", pass);
    Serial.println("[AP] Password akan BERUBAH tiap boot sampai NVS sehat.");
    Serial.println("[AP] Cek NVS partisi / lakukan factory reset (BOOT 10 s).");
  }
  Serial.println("[AP] Hilang? Tahan tombol BOOT 10 detik untuk factory reset.");
  Serial.println("=========================================================");
  return String(pass);
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return false;
  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) return false;
  config.ssid      = doc["wifi_ssid"].as<String>();
  config.password  = doc["wifi_pass"].as<String>();
  config.gasUrl    = doc["gas_url"].as<String>();
  config.token     = doc["auth_token"].as<String>();
  config.deviceKey = doc["device_key"].as<String>();
  config.interval  = doc["telemetry_interval_sec"] | 15;
  config.vCalib    = doc["v_calib"] | 11.0f;
  // Legacy configs use a single "i_calib". Accept it, and prefer the
  // split "i_calib_dc" / "i_calib_ac" when present.
  float legacyI    = doc["i_calib"] | 1.0f;
  config.iCalibDc  = doc["i_calib_dc"] | legacyI;
  config.iCalibAc  = doc["i_calib_ac"] | legacyI;
  // v1.6.0 [E-WAVE] — emergency config block; every field individually
  // optional (older config.json keeps firmware defaults) and range-checked
  // against the same table GAS enforces (mixed-version fleet defense).
  {
    JsonObject emg = doc["emg"];
    if (!emg.isNull()) {
      float v;
      v = emg["vbatLowV"]      | NAN; if (isfinite(v) && v >= 30 && v <= 60)  config.emg.vbatLowV = v;
      v = emg["vbatLowHystV"]  | NAN; if (isfinite(v) && v >= 0.1f && v <= 5)  config.emg.vbatLowHystV = v;
      v = emg["vbatHighV"]     | NAN; if (isfinite(v) && v >= 48 && v <= 60)  config.emg.vbatHighV = v;
      v = emg["vbatHighHystV"] | NAN; if (isfinite(v) && v >= 0.1f && v <= 5)  config.emg.vbatHighHystV = v;
      v = emg["iDcOverA"]      | NAN; if (isfinite(v) && v >= 10 && v <= 120)  config.emg.iDcOverA = v;
      v = emg["iAcLoadOverA"]  | NAN; if (isfinite(v) && v >= 5 && v <= 40)   config.emg.iAcLoadOverA = v;
      v = emg["iAcGenOverA"]   | NAN; if (isfinite(v) && v >= 5 && v <= 40)   config.emg.iAcGenOverA = v;
      int n = emg["debounceN"]   | -1; if (n >= 1 && n <= 10)   config.emg.debounceN = (uint8_t)n;
      int r = emg["recoverySec"] | -1; if (r >= 0 && r <= 3600) config.emg.recoverySec = (uint32_t)r;
      int rp = emg["relayPin"]   | -1; if (rp >= 12 && rp <= 39) config.emg.relayPin = (uint8_t)rp;
      int ep = emg["estopPin"]   | -99; if (ep >= -1 && ep <= 39) config.emg.estopPin = (int8_t)ep;
      int ee = emg["estopEnabled"] | -1; if (ee == 0 || ee == 1) config.emg.estopEnabled = (uint8_t)ee;
      // v1.7.0 [P1-SC1] — absent field (old config.json / mixed fleet) keeps
      // the fail-closed default 1.
      int sp = emg["sensorFailPolicy"] | -1; if (sp == 0 || sp == 1) config.emg.sensorFailPolicy = (uint8_t)sp;
    }
  }
  // [FW-A2] Clamp persisted calibration on load too — a hand-edited or
  // corrupted config.json must not silently zero the telemetry math.
  float vc  = clampCalibration(config.vCalib, CALIB_V_MIN, CALIB_V_MAX);
  float idc = clampCalibration(config.iCalibDc, CALIB_I_MIN, CALIB_I_MAX);
  float iac = clampCalibration(config.iCalibAc, CALIB_I_MIN, CALIB_I_MAX);
  if (isnan(vc) || isnan(idc) || isnan(iac)) return false;   // non-finite = invalid config
  config.vCalib = vc; config.iCalibDc = idc; config.iCalibAc = iac;
  // [FW-A4] GAS endpoint MUST be https — TLS pinning (GAS_ROOT_CA) assumes a
  // TLS endpoint; an http:// URL fails opaquely at runtime otherwise.
  return config.ssid.length() > 0 && config.gasUrl.startsWith("https://");
}

bool saveConfig() {
  JsonDocument doc;
  doc["wifi_ssid"]              = config.ssid;
  doc["wifi_pass"]              = config.password;
  doc["gas_url"]                = config.gasUrl;
  doc["auth_token"]             = config.token;
  doc["device_key"]             = config.deviceKey;
  doc["telemetry_interval_sec"] = config.interval;
  doc["v_calib"]                = config.vCalib;
  doc["i_calib_dc"]             = config.iCalibDc;
  doc["i_calib_ac"]             = config.iCalibAc;
  // v1.6.0 [E-WAVE] — persisted so a reboot keeps operator-tuned triggers.
  JsonObject emg = doc["emg"].to<JsonObject>();
  emg["vbatLowV"]      = config.emg.vbatLowV;
  emg["vbatLowHystV"]  = config.emg.vbatLowHystV;
  emg["vbatHighV"]     = config.emg.vbatHighV;
  emg["vbatHighHystV"] = config.emg.vbatHighHystV;
  emg["iDcOverA"]      = config.emg.iDcOverA;
  emg["iAcLoadOverA"]  = config.emg.iAcLoadOverA;
  emg["iAcGenOverA"]   = config.emg.iAcGenOverA;
  emg["debounceN"]     = config.emg.debounceN;
  emg["recoverySec"]   = config.emg.recoverySec;
  emg["relayPin"]      = config.emg.relayPin;
  emg["estopPin"]      = config.emg.estopPin;
  emg["estopEnabled"]  = config.emg.estopEnabled;
  emg["sensorFailPolicy"] = config.emg.sensorFailPolicy;   // v1.7.0 [P1-SC1]
  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) return false;
  bool ok = serializeJson(doc, file) > 0;
  file.close();
  return ok;
}

// ============================================================================
// Sensors — dual channel
// ----------------------------------------------------------------------------
//   DC current : INA219 @ 0x40 (shunt-voltage register), 100A/75mV mod shunt
//   AC current : ACS712-30A (Modified 3.3V) via ADC → true-RMS across 2 cycles
// Both feed a 10-sample Moving Average per NFR §5.1.
// ============================================================================

bool ina219Init() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(INA219_REG_CONFIG);
  // 32V bus range, PGA=/8 (±320 mV shunt), 12-bit shunt+bus ADC, cont. mode.
  Wire.write(0x39);
  Wire.write(0x9F);
  uint8_t err = Wire.endTransmission();
  ina219Present = (err == 0);
  if (!ina219Present) {
    Serial.printf("[INA219] not found (I2C err=%u) — DC current disabled.\n", err);
  } else {
    Serial.println("[INA219] configured @ 0x40 (100A/75mV shunt).");
  }
  return ina219Present;
}

// Return DC current in Amperes. Positive = discharge, negative = charge
// (direction depends on how the shunt is oriented — user-selectable via
// iCalibDc = ±1.0).
float readDcCurrentAmps() {
  if (!ina219Present) return NAN;
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(INA219_REG_SHUNT);
  if (Wire.endTransmission(false) != 0) return NAN;
  if (Wire.requestFrom((int)INA219_ADDR, 2) != 2) return NAN;
  int16_t raw = ((int16_t)Wire.read() << 8) | Wire.read();
  float shuntV_uV = raw * INA219_LSB_UV;              // µV
  float amps = (shuntV_uV * 1e-6f) / INA219_SHUNT_OHM;
  return amps * config.iCalibDc;
}

// Return true-RMS AC current in Amperes.
float readAcRmsAmps() {
  double sumSq = 0.0;
  for (uint16_t i = 0; i < AC_RMS_SAMPLES; i++) {
    int adc = analogRead(AC_CURRENT_PIN);
    float v = (adc * (3.3f / 4095.0f)) - config.acZeroVolt;
    sumSq += (double)v * v;
    delayMicroseconds(AC_RMS_INTERVAL_US);
  }
  float rmsV = sqrtf((float)(sumSq / AC_RMS_SAMPLES));
  return (rmsV / ACS712_SENSITIVITY) * config.iCalibAc;
}

// v1.6.0 [E-WAVE] — 2nd ACS712 (genset→inverter feed): identical true-RMS
// pipeline with its own zero-offset, so the two channels calibrate independently.
float readAcGenRmsAmps() {
  double sumSq = 0.0;
  for (uint16_t i = 0; i < AC_RMS_SAMPLES; i++) {
    int adc = analogRead(AC_GEN_PIN);
    float v = (adc * (3.3f / 4095.0f)) - config.acGenZeroVolt;
    sumSq += (double)v * v;
    delayMicroseconds(AC_RMS_INTERVAL_US);
  }
  float rmsV = sqrtf((float)(sumSq / AC_RMS_SAMPLES));
  return (rmsV / ACS712_SENSITIVITY) * config.iCalibAc;
}

// Auto-tare the ACS712 midpoint at boot (no AC current flowing yet, ideally).
void acs712Autotare() {
  const uint16_t samples = 200;
  uint32_t sum = 0;
  for (uint16_t i = 0; i < samples; i++) {
    sum += analogRead(AC_CURRENT_PIN);
    delayMicroseconds(200);
  }
  float avgAdc = (float)sum / samples;
  float avgV = avgAdc * (3.3f / 4095.0f);
  // v1.6.0 [E-WAVE] — tare BOTH channels (each ACS712 has its own offset).
  uint32_t sumGen = 0;
  for (uint16_t i = 0; i < samples; i++) {
    sumGen += analogRead(AC_GEN_PIN);
    delayMicroseconds(200);
  }
  float avgVGen = ((float)sumGen / samples) * (3.3f / 4095.0f);
  // Only accept a plausible midpoint (0.8V .. 2.5V). Otherwise keep default.
  if (avgV > 0.8f && avgV < 2.5f) {
    config.acZeroVolt = avgV;
  }
  Serial.printf("[ACS712] zero=%.3f V\n", config.acZeroVolt);
}

// ============================================================================
// Moving Average Filter (NFR §5.1)
// ============================================================================
float average(const float* values, uint8_t length) {
  if (length == 0) return 0.0f;
  float total = 0.0f;
  uint8_t counted = 0;
  for (uint8_t i = 0; i < length; i++) {
    if (isnan(values[i])) continue;
    total += values[i];
    counted++;
  }
  if (counted == 0) return NAN;
  return total / (float)counted;
}

void sampleSensors() {
  voltageSamples[sampleIndex] =
      analogRead(VOLTAGE_PIN) * (3.3f / 4095.0f) * config.vCalib;
  dcCurrentSamples[sampleIndex] = readDcCurrentAmps();
  acCurrentSamples[sampleIndex] = readAcRmsAmps();
  acGenCurrentSamples[sampleIndex] = readAcGenRmsAmps();   // v1.6.0 — ACS712 #2
  sampleIndex = (sampleIndex + 1) % SAMPLE_COUNT;
}

// ============================================================================
// Crypto helpers (mbedtls) — HMAC-SHA256 + SHA-256 hex
// ============================================================================
static const char HEX_CHARS[] = "0123456789abcdef";

String toHex(const uint8_t* bytes, size_t len) {
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += HEX_CHARS[(bytes[i] >> 4) & 0x0F];
    out += HEX_CHARS[bytes[i] & 0x0F];
  }
  return out;
}

String hmacSha256Hex(const String& secret, const String& message) {
  uint8_t out[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info,
                  (const uint8_t*)secret.c_str(), secret.length(),
                  (const uint8_t*)message.c_str(), message.length(),
                  out);
  return toHex(out, sizeof(out));
}

// ============================================================================
// v1.6.0 [E-WAVE] — EMERGENCY RELAY LAYER
// ----------------------------------------------------------------------------
// Semantics (MUST match Code.gs WAVE-7):
//   Relay module 5V with optocoupler, ACTIVE-LOW input.
//     GPIO LOW  → opto conducts → relay ENERGIZED → kontaktor path CLOSED
//                 → system RUN.
//     GPIO HIGH / Hi-Z (boot, crash, WDT reset) → relay DE-ENERGIZED →
//                 system ISOLATED. The ESP32 must be ALIVE to keep the
//                 system running — that is the fail-safe contract.
//   Physical E-stop (normally-closed) breaks the relay module's negative
//     supply: pressed → module powerless → relay OFF, independent of the
//     ESP32. ESTOP_SENSE_PIN reads the line so firmware can LATCH the state
//     (release alone never re-energizes — GPIO stays HIGH until operator
//     re-ARMS from the PWA).
//   Sneak-path note: while the E-stop is OPEN, the opto LED can only find a
//     return through the ESP32 GPIO when it drives LOW. The sense-poll below
//     reacts within one loop cycle by driving the GPIO HIGH, and a dead/Hi-Z
//     ESP32 sinks nothing anyway — the hardware path stays authoritative.
//   Boot: relay ISOLATED first (before WiFi, before config) — then, and only
//     then, everything else. Trips are LATCHED; only a locally-validated
//     operator ARM clears them (trigger clear + hysteresis + recoverySec).
// ============================================================================

// NVS u8/u32 helpers (namespace "plts", same as the seq/OTA keys).
uint32_t nvsGetU32(const char* key, uint32_t dflt) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return dflt;
  uint32_t v = dflt;
  nvs_get_u32(h, key, &v);
  nvs_close(h);
  return v;
}
void nvsSetU32(const char* key, uint32_t value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, key, value);
  nvs_commit(h);
  nvs_close(h);
}
uint8_t nvsGetU8(const char* key, uint8_t dflt) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return dflt;
  uint8_t v = dflt;
  nvs_get_u8(h, key, &v);
  nvs_close(h);
  return v;
}
void nvsSetU8(const char* key, uint8_t value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, key, value);
  nvs_commit(h);
  nvs_close(h);
}

// --- Relay GPIO primitives --------------------------------------------------

// energized=true → relay ON (system RUN). Active-LOW module: GPIO LOW.
void emgRelayWrite(bool energized) {
  bool level = energized ? LOW : HIGH;   // RELAY_ACTIVE_LOW
  digitalWrite(config.emg.relayPin, level);
}

// --- Event reporting (rate-limited, retrying) --------------------------------

void emgQueueEvent(const String& type, const String& reason) {
  // Keep only the LATEST unsent event (a burst of trips resolves to one
  // representative notification; the telemetry stream carries full state).
  emgPendingEvent = type + "|" + reason;
  emgEventTries = 0;
}

// One HTTPS POST to GAS; returns the response body ("" on failure).
String gasPostJson(const String& bodyStr) {
  if (WiFi.status() != WL_CONNECTED || config.gasUrl.length() == 0) return "";
  WiFiClientSecure client;
  client.setCACert(GAS_ROOT_CA);           // same pinned anchor as every path
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, config.gasUrl)) { client.stop(); return ""; }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(bodyStr);
  String body = (code == 200) ? http.getString() : "";
  http.end();
  client.stop();
  return body;
}

void emgPostEventNow(const String& type, const String& reason) {
  JsonDocument body;
  body["action"]     = "EMERGENCY_EVENT";
  body["token"]      = config.token;
  body["device_key"] = config.deviceKey;
  body["type"]       = type;
  body["reason"]     = reason;
  body["state"]      = (emgState == EmgState::RUN) ? "RUN" : "EMERGENCY";
  String encoded;
  serializeJson(body, encoded);
  gasPostJson(encoded);
}

// Pump the pending event with rate limit + bounded retries (called from loop).
void emgFlushEvent() {
  if (emgPendingEvent.length() == 0) return;
  if (emgEventTries >= EMERGENCY_EVENT_MAX_TRIES) {
    Serial.printf("[EMG] event dropped after %u tries: %s\n",
                  emgEventTries, emgPendingEvent.c_str());
    emgPendingEvent = "";
    return;
  }
  uint32_t now = millis();
  if (lastEmgEventAt != 0 && now - lastEmgEventAt < EMERGENCY_EVENT_MIN_INTERVAL_MS) return;
  lastEmgEventAt = now;
  int sep = emgPendingEvent.indexOf('|');
  String type = emgPendingEvent.substring(0, sep);
  String reason = emgPendingEvent.substring(sep + 1);
  emgPostEventNow(type, reason);
  emgEventTries++;
  // Success is not directly observable (GAS answers 200 even for logical
  // errors) — treat every attempt as consumed; the retry budget covers
  // transport-level failures (empty response).
  emgPendingEvent = "";
}

// --- Core state transitions ---------------------------------------------------

void emgTrip(const String& reason) {
  if (emgState == EmgState::EMERGENCY) return;    // latched — no re-trip
  emgState = EmgState::EMERGENCY;
  emgReason = reason;
  emgTripAtMs = millis();
  emgTrips++;
  nvsSetU32(NVS_KEY_EMG_TRIPS, emgTrips);
  emgRelayWrite(false);                            // ISOLATED — immediate, local
  Serial.printf("[EMG] *** TRIP (%s) — sistem TERISOLASI ***\n", reason.c_str());
  emgQueueEvent("TRIP", reason);
}

void emgArm(const String& source) {
  emgState = EmgState::RUN;
  emgReason = "";
  emgTripAtMs = 0;
  emgRelayWrite(true);                             // RUN
  Serial.printf("[EMG] ARM (%s) — relay energized, sistem RUN\n", source.c_str());
}

// Evaluate ALL trigger conditions from the moving averages. Debounce: each
// channel must violate for debounceN consecutive evaluations (noise-immune);
// hysteresis: a channel clears only past threshold±hyst.
void emgEvaluateTriggers() {
  float vBat = average(voltageSamples, SAMPLE_COUNT);
  float iDc  = average(dcCurrentSamples, SAMPLE_COUNT);
  float iAc  = average(acCurrentSamples, SAMPLE_COUNT);
  float iGen = average(acGenCurrentSamples, SAMPLE_COUNT);

  // --- low voltage ---
  if (isfinite(vBat) && vBat < config.emg.vbatLowV) {
    if (++emgDebounce[0] >= config.emg.debounceN) { emgTrip("VBAT_LOW"); return; }
  } else if (isfinite(vBat) && vBat > config.emg.vbatLowV + config.emg.vbatLowHystV) {
    emgDebounce[0] = 0;
  }
  // --- high voltage ---
  if (isfinite(vBat) && vBat > config.emg.vbatHighV) {
    if (++emgDebounce[1] >= config.emg.debounceN) { emgTrip("VBAT_HIGH"); return; }
  } else if (isfinite(vBat) && vBat < config.emg.vbatHighV - config.emg.vbatHighHystV) {
    emgDebounce[1] = 0;
  }
  // --- DC overcurrent (only when the INA219 is actually present) ---
  if (ina219Present && isfinite(iDc) && fabsf(iDc) > config.emg.iDcOverA) {
    if (++emgDebounce[2] >= config.emg.debounceN) { emgTrip("I_DC_OVER"); return; }
  } else if (!ina219Present || (isfinite(iDc) && fabsf(iDc) < config.emg.iDcOverA)) {
    emgDebounce[2] = 0;
  }
  // --- AC load overcurrent ---
  if (isfinite(iAc) && iAc > config.emg.iAcLoadOverA) {
    if (++emgDebounce[3] >= config.emg.debounceN) { emgTrip("I_AC_LOAD_OVER"); return; }
  } else if (isfinite(iAc) && iAc < config.emg.iAcLoadOverA) {
    emgDebounce[3] = 0;
  }
  // --- genset feed overcurrent ---
  if (isfinite(iGen) && iGen > config.emg.iAcGenOverA) {
    if (++emgDebounce[4] >= config.emg.debounceN) { emgTrip("I_AC_GEN_OVER"); return; }
  } else if (isfinite(iGen) && iGen < config.emg.iAcGenOverA) {
    emgDebounce[4] = 0;
  }
  // --- v1.7.0 [P1-SC3] safety-sensor loss (fail-closed, policy-gated) ---
  // The current sensors are emergency trip inputs: a sensor that stops
  // reporting silently disarms its protection. Under sensorFailPolicy=1 a
  // lost/unplausible safety input is itself a trip condition (debounced
  // like every other trigger). NOTE: this check runs in every state; while
  // EMERGENCY the trip is already latched so the counter just saturates —
  // emgTrip() is idempotent.
  if (config.emg.sensorFailPolicy) {
    bool sensorLoss = !isfinite(vBat) || !isfinite(iDc) || !isfinite(iAc) || !isfinite(iGen);
    if (sensorLoss) {
      if (++emgDebounce[5] >= config.emg.debounceN) {
        emgTrip("SENSOR_LOSS");
        return;
      }
    } else {
      emgDebounce[5] = 0;
    }
  } else {
    emgDebounce[5] = 0;
  }
}

// ARM gate: ALL triggers clear (with hysteresis) AND recovery window elapsed.
// Returns "" when ARM is allowed, else the blocking reason.
// v1.7.0 [P1-SC2]: under sensorFailPolicy=1 (default) the safety sensors are
// MANDATORY — an absent/invalid sensor blocks ARM, because its overcurrent
// trigger could never fire. Operator opt-out only via policy 0.
String emgArmBlockReason() {
  float vBat = average(voltageSamples, SAMPLE_COUNT);
  float iDc  = average(dcCurrentSamples, SAMPLE_COUNT);
  float iAc  = average(acCurrentSamples, SAMPLE_COUNT);
  float iGen = average(acGenCurrentSamples, SAMPLE_COUNT);
  if (!isfinite(vBat)) return "sensor tegangan tidak valid";
  if (config.emg.sensorFailPolicy) {
    // Fail-closed: unmonitored IS unsafe for a safety interlock.
    if (!ina219Present)
      return "sensor INA219 tidak terdeteksi — proteksi arus DC nonaktif (sensorFailPolicy=1)";
    if (!isfinite(iDc)) return "sensor arus DC tidak valid";
    if (!isfinite(iAc)) return "sensor arus beban AC tidak valid";
    if (!isfinite(iGen)) return "sensor arus genset tidak valid";
  }
  if (vBat <= config.emg.vbatLowV + config.emg.vbatLowHystV)
    return String("VBAT masih rendah (") + vBat + "V)";
  if (vBat >= config.emg.vbatHighV - config.emg.vbatHighHystV)
    return String("VBAT masih tinggi (") + vBat + "V)";
  if (ina219Present && isfinite(iDc) && fabsf(iDc) > config.emg.iDcOverA)
    return String("arus DC masih di atas ambang (") + fabsf(iDc) + "A)";
  if (isfinite(iAc) && iAc > config.emg.iAcLoadOverA)
    return String("arus beban masih di atas ambang (") + iAc + "A)";
  if (isfinite(iGen) && iGen > config.emg.iAcGenOverA)
    return String("arus jenset masih di atas ambang (") + iGen + "A)";
  if (emgClearAtMs == 0) emgClearAtMs = millis();
  if (millis() - emgClearAtMs < (uint32_t)config.emg.recoverySec * 1000UL)
    return String("masih dalam masa pemulihan (") +
           (config.emg.recoverySec - (millis() - emgClearAtMs) / 1000UL) + " detik lagi)";
  return "";
}

// Track when ALL triggers became clear (feeds the recovery window).
void emgTrackClear() {
  String blocked = emgArmBlockReason();
  bool clearNow = (blocked.length() == 0) || blocked.startsWith("masih dalam masa pemulihan");
  if (!clearNow) {
    emgClearAtMs = 0;
  } else if (emgClearAtMs == 0) {
    emgClearAtMs = millis();
  }
}

// --- E-stop sense -------------------------------------------------------------
// Line at the module's negative terminal: CLOSED (normal) pulls the sense pin
// LOW; OPEN (E-stop pressed / wiring fault) lets the pull-up go HIGH.
void emgPollEstop() {
  if (!config.emg.estopEnabled || config.emg.estopPin < 0) return;
  bool open = (digitalRead(config.emg.estopPin) == HIGH);
  if (open != emgEstopOpen) {
    emgEstopOpen = open;
    if (open) {
      // Hardware already broke the module negative — relay is OFF. Latch it
      // in firmware so releasing the button never re-energizes on its own.
      if (emgState == EmgState::RUN) {
        emgState = EmgState::EMERGENCY;
        emgReason = "ESTOP";
        emgTripAtMs = millis();
        emgTrips++;
        nvsSetU32(NVS_KEY_EMG_TRIPS, emgTrips);
        emgRelayWrite(false);
        Serial.println("[EMG] *** E-STOP terdeteksi — sistem TERISOLASI (latched) ***");
        emgQueueEvent("ESTOP", "physical e-stop opened the relay negative line");
      }
    } else {
      emgQueueEvent("ESTOP_RELEASED", "e-stop line closed — operator ARM required");
    }
  }
}

// --- Command application (from TELEMETRY piggyback or EMERGENCY_PENDING) ------

void emgAck(const String& commandId, const char* result, const String& message) {
  JsonDocument ack;
  ack["action"]     = "EMERGENCY_ACK";
  ack["token"]      = config.token;
  ack["device_key"] = config.deviceKey;
  ack["command_id"] = commandId;
  ack["result"]     = result;
  ack["message"]    = message;
  ack["state"]      = (emgState == EmgState::RUN) ? "RUN" : "EMERGENCY";
  String encoded;
  serializeJson(ack, encoded);
  gasPostJson(encoded);
}

// Apply an emergency command envelope. Fail-closed: every value re-validated
// locally (a mixed-version GAS or a hand-edited sheet must never arm garbage).
void emgApplyCommand(const String& commandId, const String& command,
                     JsonVariantConst cfg) {
  String cmd = command;
  cmd.toUpperCase();

  if (cmd == "ARM") {
    if (emgState == EmgState::RUN) {
      emgAck(commandId, "APPLIED", "already RUN");   // idempotent
      return;
    }
    if (emgCrashChain >= EMG_CRASH_CHAIN_LIMIT) {
      emgAck(commandId, "REJECTED", "crash-loop hold active — power-cycle stable first");
      return;
    }
    String block = emgArmBlockReason();
    if (block.length() > 0) {
      Serial.printf("[EMG] ARM DITOLAK: %s\n", block.c_str());
      emgAck(commandId, "REJECTED", block);
      return;
    }
    emgArm("operator");
    emgAck(commandId, "APPLIED", "relay energized");
    return;
  }

  if (cmd == "DISARM") {
    if (emgState == EmgState::RUN) {
      emgState = EmgState::EMERGENCY;
      emgReason = "OPERATOR";
      emgTripAtMs = millis();
      emgTrips++;
      nvsSetU32(NVS_KEY_EMG_TRIPS, emgTrips);
    }
    emgRelayWrite(false);                            // always — safe direction
    Serial.println("[EMG] DISARM (operator) — sistem TERISOLASI");
    emgAck(commandId, "APPLIED", "relay isolated");
    return;
  }

  if (cmd == "CONFIG") {
    if (cfg.isNull() || !cfg.is<JsonObject>()) {
      emgAck(commandId, "REJECTED", "missing config object");
      return;
    }
    float v;
    v = cfg["vbatLowV"]      | NAN; if (isfinite(v) && v >= 30 && v <= 60)  config.emg.vbatLowV = v;
    v = cfg["vbatLowHystV"]  | NAN; if (isfinite(v) && v >= 0.1f && v <= 5)  config.emg.vbatLowHystV = v;
    v = cfg["vbatHighV"]     | NAN; if (isfinite(v) && v >= 48 && v <= 60)  config.emg.vbatHighV = v;
    v = cfg["vbatHighHystV"] | NAN; if (isfinite(v) && v >= 0.1f && v <= 5)  config.emg.vbatHighHystV = v;
    v = cfg["iDcOverA"]      | NAN; if (isfinite(v) && v >= 10 && v <= 120)  config.emg.iDcOverA = v;
    v = cfg["iAcLoadOverA"]  | NAN; if (isfinite(v) && v >= 5 && v <= 40)   config.emg.iAcLoadOverA = v;
    v = cfg["iAcGenOverA"]   | NAN; if (isfinite(v) && v >= 5 && v <= 40)   config.emg.iAcGenOverA = v;
    int n = cfg["debounceN"]   | -1; if (n >= 1 && n <= 10)   config.emg.debounceN = (uint8_t)n;
    int r = cfg["recoverySec"] | -1; if (r >= 0 && r <= 3600) config.emg.recoverySec = (uint32_t)r;
    int rp = cfg["relayPin"]   | -1; if (rp >= 12 && rp <= 39) config.emg.relayPin = (uint8_t)rp;
    int ep = cfg["estopPin"]   | -99; if (ep >= -1 && ep <= 39) config.emg.estopPin = (int8_t)ep;
    int ee = cfg["estopEnabled"] | -1; if (ee == 0 || ee == 1) config.emg.estopEnabled = (uint8_t)ee;
    // v1.7.0 [P1-SC1] — 13th schema field; absent → keep current (default 1).
    int sp = cfg["sensorFailPolicy"] | -1; if (sp == 0 || sp == 1) config.emg.sensorFailPolicy = (uint8_t)sp;
    // Pin change → re-init GPIO immediately (fail-safe level first).
    pinMode(config.emg.relayPin, OUTPUT);
    emgRelayWrite(emgState == EmgState::RUN);
    if (config.emg.estopEnabled && config.emg.estopPin >= 0) {
      pinMode(config.emg.estopPin, INPUT_PULLUP);
    }
    saveConfig();
    Serial.println("[EMG] CONFIG applied + persisted");
    emgAck(commandId, "APPLIED", "emergency config updated");
    emgQueueEvent("CONFIG_APPLIED", "operator updated trigger thresholds");
    return;
  }

  emgAck(commandId, "REJECTED", "unknown command: " + cmd);
}

// Parse a GAS response body for data.pendingEmergency and consume it.
void emgConsumePendingFromResponse(const String& body) {
  if (body.length() == 0) return;
  JsonDocument rdoc;
  if (deserializeJson(rdoc, body)) return;
  if (String(rdoc["status"].as<const char*>() ?: "") != "SUCCESS") return;
  JsonVariantConst pend = rdoc["data"]["pendingEmergency"];
  if (pend.isNull() || !pend.is<JsonObject>()) return;
  String commandId = pend["command_id"].as<String>();
  String command   = pend["command"].as<String>();
  if (commandId.length() == 0 || command.length() == 0) return;
  Serial.printf("[EMG] command from telemetry piggyback: %s (%s)\n",
                command.c_str(), commandId.c_str());
  emgApplyCommand(commandId, command, pend["config"]);
}

// Dedicated EMERGENCY_PENDING poll (15 s cadence, bounded GAS load).
void checkEmergencyPending() {
  if (WiFi.status() != WL_CONNECTED || config.gasUrl.length() == 0) return;
  JsonDocument body;
  body["action"]     = "EMERGENCY_PENDING";
  body["token"]      = config.token;
  body["device_key"] = config.deviceKey;
  String encoded;
  serializeJson(body, encoded);
  String resp = gasPostJson(encoded);
  if (resp.length() == 0) return;
  JsonDocument rdoc;
  if (deserializeJson(rdoc, resp)) return;
  if (String(rdoc["status"].as<const char*>() ?: "") != "SUCCESS") return;
  JsonVariantConst data = rdoc["data"];
  if (data.isNull() || !data.is<JsonObject>()) return;
  String commandId = data["command_id"].as<String>();
  String command   = data["command"].as<String>();
  if (commandId.length() == 0 || command.length() == 0) return;
  Serial.printf("[EMG] pending command: %s (%s)\n", command.c_str(), commandId.c_str());
  emgApplyCommand(commandId, command, data["config"]);
}

// Re-apply operator-configured pins after loadConfig() (a relayPin change
// must take effect without a reflash). Fail-safe: OUTPUT + current state level.
void emgApplyPinsFromConfig() {
  pinMode(config.emg.relayPin, OUTPUT);
  emgRelayWrite(emgState == EmgState::RUN);
  if (config.emg.estopEnabled && config.emg.estopPin >= 0) {
    pinMode(config.emg.estopPin, INPUT_PULLUP);
  }
}

// Boot-time init — MUST run before WiFi/config/anything that can hang.
// 1) relay GPIO driven HIGH (isolated) immediately,
// 2) E-stop sense input,
// 3) crash-chain accounting (previous boot healthy?),
// 4) BOOT event queued.
void emgInit() {
  pinMode(config.emg.relayPin, OUTPUT);
  emgRelayWrite(false);                       // FAIL-SAFE FIRST — isolated
  if (config.emg.estopEnabled && config.emg.estopPin >= 0) {
    pinMode(config.emg.estopPin, INPUT_PULLUP);
  }
  emgTrips = nvsGetU32(NVS_KEY_EMG_TRIPS, 0);
  // Crash-chain: "emg_run_ok" is set after 5 stable minutes and cleared at
  // every boot — a chain of unhealthy reboots (brownout loop, OTA boot loop)
  // keeps the system isolated until a manual power-cycle.
  uint8_t runOk = nvsGetU8(NVS_KEY_EMG_RUN_OK, 1);
  nvsSetU8(NVS_KEY_EMG_RUN_OK, 0);
  emgCrashChain = runOk ? 0 : (uint8_t)(nvsGetU8(NVS_KEY_EMG_CHAIN, 0) + 1);
  nvsSetU8(NVS_KEY_EMG_CHAIN, emgCrashChain);
  if (emgCrashChain >= EMG_CRASH_CHAIN_LIMIT) {
    emgReason = "CRASHLOOP";
    emgQueueEvent("CRASHLOOP", String("reboot chain ") + emgCrashChain +
                  " tanpa runtime sehat — tahan terisolasi");
    Serial.printf("[EMG] CRASH-LOOP hold (chain=%u) — sistem tetap TERISOLASI\n",
                  emgCrashChain);
  } else {
    emgReason = "BOOT";
    emgQueueEvent("BOOT", "boot sehat — menunggu operator ARM");
  }
  Serial.printf("[EMG] init: relay ISOLATED (fail-safe), trips=%u, chain=%u\n",
                (unsigned)emgTrips, (unsigned)emgCrashChain);
}

// Mark runtime healthy after 5 stable minutes (one NVS write per boot).
void emgMarkRuntimeHealthy() {
  static bool marked = false;
  if (marked) return;
  if (millis() < 300000UL) return;
  nvsSetU8(NVS_KEY_EMG_RUN_OK, 1);
  marked = true;
}

// Per-loop emergency tick: E-stop poll → trigger evaluation → recovery
// tracking → event flush → LED. Non-blocking; runs every ~50 ms.
void emgTick() {
  emgPollEstop();
  if (emgState == EmgState::RUN) {
    emgEvaluateTriggers();
    emgTrackClear();
  } else {
    emgTrackClear();      // keep the recovery clock honest while isolated
  }
  emgFlushEvent();
  emgMarkRuntimeHealthy();
  // LED: RUN = solid, EMERGENCY = 2 Hz blink (local, no-display indicator).
  if (emgState == EmgState::RUN) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    if (millis() - emgLastLedToggle >= EMERGENCY_LED_BLINK_MS) {
      emgLastLedToggle = millis();
      emgLedState = !emgLedState;
      digitalWrite(LED_PIN, emgLedState);
    }
  }
}

// ============================================================================
// Captive Portal
// ============================================================================
String buildSetupPage(const String& message = "") {
  String html = F(
    "<!doctype html><html lang='id'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PLTS Monitor Setup</title>"
    "<style>body{font-family:system-ui,sans-serif;background:#0b1220;color:#e8ecf1;"
    "margin:0;padding:24px}h1{color:#fbbf24;font-size:22px}"
    "form{max-width:420px;margin:0 auto;background:#111a2b;padding:20px;"
    "border-radius:14px;border:1px solid #1f2a44}"
    "label{display:block;margin:10px 0 4px;font-size:13px;color:#94a3b8}"
    "input{width:100%;padding:10px;border-radius:8px;border:1px solid #334155;"
    "background:#0b1220;color:#e8ecf1;box-sizing:border-box}"
    "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:10px;"
    "background:#fbbf24;color:#0b1220;font-weight:600;cursor:pointer}"
    ".msg{background:#1e293b;padding:10px;border-radius:8px;margin-bottom:12px;"
    "border-left:3px solid #fbbf24}.tag{display:inline-block;background:#1e293b;"
    "color:#7dd3fc;padding:3px 8px;border-radius:8px;font-size:11px;margin-bottom:8px}"
    "</style></head><body>"
    "<form method='post' action='/save' id='fm'>"
    "<h1>PLTS Monitor - Setup Awal</h1>");
  html += "<span class='tag'>fw v" + String(FIRMWARE_VERSION) + "</span>";
  html += F(
    "<div class='msg'>Sambungkan ke jaringan <b>PLTS-Monitor-Setup-XXXX</b> "
    "dengan password AP yang tertera di Serial Monitor perangkat "
    "(dicetak sekali saat pertama menyala; lupa? tahan BOOT 10 detik).</div>");
  if (message.length()) {
    html += "<div class='msg'>" + message + "</div>";
  }
  html += F(
    "<label>WiFi SSID</label>"
    "<input name='ssid' id='ssid' placeholder='Nama WiFi' required>"
    "<label>WiFi Password</label>"
    "<input name='password' id='password' type='password' placeholder='Password WiFi'>"
    "<label>GAS Web App URL</label>"
    "<input name='gas_url' id='gas_url' type='url' placeholder='https://script.google.com/...' required>"
    "<label>Auth Token</label>"
    "<input name='token' id='token' placeholder='plts_sec_...' required>"
    "<label>Device Key</label>"
    "<input name='device_key' id='device_key' value='PLTS_MONITOR_01' required>"
    "<label>Interval Telemetri (detik)</label>"
    "<input name='interval' id='interval' type='number' min='5' max='300' value='15'>"
    "<label>Kalibrasi Tegangan (V-Bat)</label>"
    "<input name='v_calib' id='v_calib' type='number' step='0.01' value='11.00'>"
    "<label>Kalibrasi Arus DC (INA219)</label>"
    "<input name='i_calib_dc' id='i_calib_dc' type='number' step='0.01' value='1.00'>"
    "<label>Kalibrasi Arus AC (ACS712)</label>"
    "<input name='i_calib_ac' id='i_calib_ac' type='number' step='0.01' value='1.00'>"
    "<button type='submit'>Simpan &amp; Restart</button></form>"
    // QR prefill: decode base64 JSON from `#plts=<b64>` and populate the form.
    "<script>(function(){try{var h=location.hash||'';var m=h.match(/plts=([^&]+)/);"
    "if(!m)return;var j=JSON.parse(decodeURIComponent(escape(atob(m[1]))));"
    "var map={ssid:'ssid',password:'password',gas_url:'gas_url',auth_token:'token',"
    "device_key:'device_key',telemetry_interval_sec:'interval',v_calib:'v_calib',"
    "i_calib_dc:'i_calib_dc',i_calib_ac:'i_calib_ac'};"
    "Object.keys(map).forEach(function(k){var e=document.getElementById(map[k]);"
    "if(e && j[k]!==undefined && j[k]!==null && String(j[k]).length){e.value=j[k];}});"
    "var ok=document.createElement('div');ok.className='msg';"
    "ok.textContent='Form terisi otomatis dari QR onboarding.';"
    "document.getElementById('fm').prepend(ok);}catch(e){}})();</script>"
    "</body></html>");
  return html;
}

void handleRoot()      { server.send(200, "text/html", buildSetupPage()); }
void handleNotFound()  { server.sendHeader("Location", "/", true);
                         server.send(302, "text/plain", "redirect"); }

void handleSave() {
  config.ssid      = server.arg("ssid");
  config.password  = server.arg("password");
  config.gasUrl    = server.arg("gas_url");
  config.token     = server.arg("token");
  config.deviceKey = server.arg("device_key");
  uint16_t interval = server.arg("interval").toInt();
  // [FW6-7] Server-side validation — the HTML `required` attribute is a UI
  // nicety, not a gate (any HTTP client skips it). Reject honestly with a
  // message the operator can act on, BEFORE touching LittleFS.
  if (config.ssid.length() == 0 || config.token.length() == 0 ||
      config.deviceKey.length() == 0) {
    server.send(400, "text/plain",
                "SSID, Auth Token, dan Device Key wajib diisi");
    return;
  }
  if (interval < 5 || interval > 300) interval = 15;   // clamp to form bounds
  config.interval  = interval;
  float vc = server.arg("v_calib").toFloat();
  float idc = server.arg("i_calib_dc").toFloat();
  float iac = server.arg("i_calib_ac").toFloat();
  if (idc == 0.0f) idc = server.arg("i_calib").toFloat(); // legacy fallback
  if (iac == 0.0f) iac = server.arg("i_calib").toFloat(); // legacy fallback
  // [FW-A7] Same plausibility clamp as the GAS path (GAS-2-K) — the local
  // portal must not be the weak sibling of the remote command channel.
  if (vc == 0.0f)  vc  = 11.0f;
  if (idc == 0.0f) idc = 1.0f;
  if (iac == 0.0f) iac = 1.0f;
  config.vCalib   = clampCalibration(vc,  CALIB_V_MIN, CALIB_V_MAX);
  config.iCalibDc = clampCalibration(idc, CALIB_I_MIN, CALIB_I_MAX);
  config.iCalibAc = clampCalibration(iac, CALIB_I_MIN, CALIB_I_MAX);
  // [FW-A4] Refuse non-https GAS URLs at SAVE time (clear error now beats an
  // opaque TLS failure at first telemetry POST).
  if (!config.gasUrl.startsWith("https://")) {
    server.send(400, "text/plain",
                "GAS URL harus https:// (endpoint Google Apps Script)");
    return;
  }
  if (saveConfig()) {
    server.send(200, "text/html",
                F("<h2 style='font-family:sans-serif'>Konfigurasi tersimpan. "
                  "ESP32 akan reboot...</h2>"));
    delay(1200);
    ESP.restart();
  } else {
    server.send(500, "text/plain", "Gagal menyimpan konfigurasi");
  }
}

void startPortal() {
  currentMode  = Mode::AP_MODE;
  apStartedAt  = millis();
  WiFi.mode(WIFI_AP);
  String macTail = String((uint32_t)ESP.getEfuseMac(), HEX);
  macTail.toUpperCase();
  String ssid = "PLTS-Monitor-Setup-" + macTail.substring(macTail.length() - 4);
  // [FW-A3] WPA2 with a per-device CSPRNG password — an OPEN setup AP let
  // anyone in radio range rewrite the whole config (incl. their own GAS URL
  // + token → full fleet telemetry redirect) during the setup window.
  String apPass = getOrCreateApPassword();
  WiFi.softAP(ssid.c_str(), apPass.c_str());
  IPAddress apIp(192, 168, 4, 1);
  dnsServer.start(53, "*", apIp);
  server.on("/",     handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[AP] SSID=%s (WPA2) IP=%s\n", ssid.c_str(), apIp.toString().c_str());
}

// ============================================================================
// Telemetry
// ============================================================================
void sendTelemetry() {
  if (WiFi.status() != WL_CONNECTED || config.gasUrl.length() == 0) return;
  // [FW6-5] Failure backoff gate: sampling continues, POST cadence backs off.
  if ((int32_t)(millis() - txNextAttemptAt) < 0) return;

  float vBat  = average(voltageSamples,   SAMPLE_COUNT);
  float iDc   = average(dcCurrentSamples, SAMPLE_COUNT);
  float iAc   = average(acCurrentSamples,  SAMPLE_COUNT);
  float iAcGen = average(acGenCurrentSamples, SAMPLE_COUNT);   // v1.6.0 — ACS712 #2
  float pDc   = (isnan(vBat) || isnan(iDc)) ? NAN : vBat * iDc;

  JsonDocument body;
  body["action"]     = "TELEMETRY";
  body["token"]      = config.token;
  body["device_key"] = config.deviceKey;
  JsonObject data    = body["data"].to<JsonObject>();
  // [WAVE-1 / GAS-2-A] Identity: (device_key, sequence) — REQUIRED by
  // Code.gs v2 (400 without it). Monotonic via NVS high-water mark.
  data["sequence"]   = seqCounter;
  data["v_bat"]      = vBat;
  data["i_bat_dc"]   = iDc;
  data["p_bat_dc"]   = pDc;
  data["i_ac_load"]  = iAc;
  // v1.6.0 [E-WAVE] — 2nd ACS712 (genset→inverter) + emergency relay state.
  data["i_ac_gen"]   = iAcGen;
  data["emg_state"]  = (emgState == EmgState::RUN) ? "RUN" : "EMERGENCY";
  data["emg_reason"] = emgReason;
  data["emg_estop"]  = emgEstopOpen;
  data["emg_trips"]  = (uint32_t)emgTrips;
  data["ina219_ok"]  = ina219Present;
  data["free_heap"]  = ESP.getFreeHeap();
  data["rssi"]       = WiFi.RSSI();
  data["fw_version"] = FIRMWARE_VERSION;

  String encoded;
  serializeJson(body, encoded);

  // Consume this sequence number (one per ATTEMPT — a failed POST becomes an
  // honest ledger gap on GAS, which is exactly what the gap ledger is for).
  seqCounter++;
  persistSequenceIfDue();

  WiFiClientSecure client;
  // [WAVE-4 / GAS-2-D] was setInsecure() — the channel that carries
  // AUTH_TOKEN was MITM-exposed. GAS contract §2.4 pins the endpoint to
  // script.google.com, anchored by GTS Root R4 (fail-closed for any
  // non-Google gasUrl — visible immediately at setup, never silent).
  client.setCACert(GAS_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, config.gasUrl)) {
    txConsecutiveFails++;              // [FW6-5] transport failure = failure
    txNextAttemptAt = millis() + _txBackoffMs();
    Serial.printf("[TX] seq=%lu HTTP_BEGIN_FAILED backoff=%lus\n",
                  (unsigned long)(seqCounter - 1),
                  (unsigned long)(_txBackoffMs() / 1000));
    client.stop();
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(encoded);
  // [FW6-5] GAS answers HTTP 200 even for logical errors — the envelope
  // `status` is the truth. "HTTP=200" in the old log could mean the token
  // was revoked; that lie is retired here.
  bool ok = false;
  String gasMessage = "";
  String gasBody = "";
  if (code == 200) {
    String body = http.getString();
    gasBody = body;
    JsonDocument rdoc;
    if (!deserializeJson(rdoc, body)) {
      ok = String(rdoc["status"].as<const char*>() ?: "") == "SUCCESS";
      gasMessage = String(rdoc["message"].as<const char*>() ?: "");
    }
  }
  if (ok) {
    if (txConsecutiveFails > 0) {
      Serial.printf("[TX] recovered after %u failure(s)\n", txConsecutiveFails);
    }
    txConsecutiveFails = 0;
    txNextAttemptAt    = 0;
    Serial.printf("[TX] seq=%lu OK vBat=%.2fV iDc=%.2fA iAc=%.2fA iGen=%.2fA emg=%s heap=%u\n",
                  (unsigned long)(seqCounter - 1), vBat, iDc, iAc, iAcGen,
                  (emgState == EmgState::RUN) ? "RUN" : "EMERGENCY",
                  ESP.getFreeHeap());
    // v1.6.0 [E-WAVE] — consume any pending emergency command riding the
    // ingest response (zero extra polls on the happy path).
    emgConsumePendingFromResponse(gasBody);
  } else {
    txConsecutiveFails++;
    txNextAttemptAt = millis() + _txBackoffMs();
    Serial.printf("[TX] seq=%lu FAILED HTTP=%d msg=%s fails=%u backoff=%lus\n",
                  (unsigned long)(seqCounter - 1), code,
                  gasMessage.c_str(), txConsecutiveFails,
                  (unsigned long)(_txBackoffMs() / 1000));
  }
  http.end();
  client.stop();
}

// [FW6-5] Capped exponential backoff: interval * 2^fails, hard cap 10 min.
uint32_t _txBackoffMs() {
  uint32_t shift = txConsecutiveFails;
  if (shift > TX_BACKOFF_MAX_SHIFT) shift = TX_BACKOFF_MAX_SHIFT;
  uint64_t base = (uint64_t)config.interval * 1000UL;
  uint64_t backoff = base << shift;
  if (backoff > TX_BACKOFF_CAP_MS) backoff = TX_BACKOFF_CAP_MS;
  return (uint32_t)backoff;
}

// ============================================================================
// Signed OTA
// ============================================================================

OtaManifest fetchOtaManifest() {
  OtaManifest m;
  if (config.gasUrl.length() == 0) return m;

  JsonDocument body;
  body["action"]      = "OTA_MANIFEST";
  body["token"]       = config.token;
  // [FW6-9] Identity + version let Code.gs serve a per-device manifest hmac
  // (key = HMAC(auth_token, device_key)) to fw >= 1.5.4. Old Code.gs ignores
  // these fields (extra JSON keys are harmless); new Code.gs keeps serving
  // the fleet-keyed hmac to old firmware that sends no fw_version.
  body["device_key"]  = config.deviceKey;
  body["fw_version"]  = FIRMWARE_VERSION;
  String encoded;
  serializeJson(body, encoded);

  WiFiClientSecure client;
  // [WAVE-4 / GAS-2-D] was setInsecure() — the channel that carries
  // AUTH_TOKEN was MITM-exposed. GAS contract §2.4 pins the endpoint to
  // script.google.com, anchored by GTS Root R4 (fail-closed for any
  // non-Google gasUrl — visible immediately at setup, never silent).
  client.setCACert(GAS_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, config.gasUrl)) return m;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(encoded);
  if (code != 200) { http.end(); return m; }
  String payload = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return m;
  if (String(doc["status"].as<const char*>() ?: "") != "SUCCESS") return m;
  JsonVariantConst data = doc["data"];
  if (data.isNull()) return m;

  m.version = data["version"].as<String>();
  m.url     = data["url"].as<String>();
  m.sha256  = data["sha256"].as<String>();
  m.hmac    = data["hmac"].as<String>();
  m.size    = data["size"] | 0;
  m.target  = data["target"].as<String>();
  m.target.trim();
  m.target.toLowerCase();
  // [W13-2] Mixed-fleet self-check: a manifest explicitly targeted at the
  // OTHER firmware tree must never flash here. Code.gs already filters by
  // the device's declared DEVICES!firmware_type — this is the second layer
  // (an undeclared/legacy deployment still gets an honest REFUSED instead
  // of a cross-flash). Fleet-wide manifests ('') pass as before.
  if (m.target.length() > 0 && m.target != "generic") {
    Serial.printf("[OTA] manifest targets '%s' tree — refusing\n", m.target.c_str());
    reportOtaStatus("REFUSED", m.version,
                    String("manifest target '") + m.target +
                    "' does not match this device (generic)");
    m.valid = false;
    return m;
  }
  m.valid   = m.version.length() && m.url.startsWith("https://") &&
              m.sha256.length() == 64 && m.hmac.length() == 64;
  return m;
}

bool applyOta(const OtaManifest& m) {
  // Verify HMAC BEFORE downloading anything.
  // [FW6-9] Per-device key derivation (fw >= 1.5.4): the manifest hmac is
  // keyed by HMAC-SHA256(auth_token, device_key) instead of the raw fleet
  // token. Code.gs derives the same key server-side when serving the
  // manifest. A leaked AUTH_TOKEN alone can no longer author OTA for the
  // fleet — the attacker also needs a registered device_key. Empty
  // device_key (unconfigured device): fleet key, legacy behavior.
  String otaKey = config.deviceKey.length() > 0
      ? hmacSha256Hex(config.token, config.deviceKey)
      : config.token;
  String expected = hmacSha256Hex(otaKey, m.version + "|" + m.url + "|" + m.sha256);
  if (!expected.equalsIgnoreCase(m.hmac)) {
    Serial.println("[OTA] HMAC mismatch — aborting.");
    // [FW6-1] Tamper/forgery is reportable, not a whisper.
    reportOtaStatus("DOWNLOAD_FAILED", m.version, "manifest HMAC mismatch");
    return false;
  }

  WiFiClientSecure client;
  // [WAVE-4 / GAS-2-D] OTA binary transport pinned to the R4+ISRG bundle
  // (Google + Let's Encrypt/Vercel). Content authenticity is independently
  // enforced (HMAC over version|url|sha256 + SHA-256 of written bytes) —
  // TLS here is transport protection, failing CLOSED for hosts on CAs
  // outside the bundle (rebuild with that root; documented, never silent).
  client.setCACert(OTA_DOWNLOAD_ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(OTA_HTTP_TIMEOUT);
  if (!http.begin(client, m.url)) {
    reportOtaStatus("DOWNLOAD_FAILED", m.version, "http.begin failed");
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] download HTTP=%d\n", code);
    http.end();
    reportOtaStatus("DOWNLOAD_FAILED", m.version,
                    String("download HTTP=") + code);
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("[OTA] invalid content-length");
    http.end();
    reportOtaStatus("DOWNLOAD_FAILED", m.version, "invalid content-length");
    return false;
  }
  if (!Update.begin((size_t)contentLength)) {
    Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
    http.end();
    reportOtaStatus("DOWNLOAD_FAILED", m.version,
                    String("Update.begin: ") + Update.errorString());
    return false;
  }

  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[1024];
  while (http.connected() && (written < (size_t)contentLength)) {
    esp_task_wdt_reset();
    size_t available = stream->available();
    if (available > 0) {
      int read = stream->readBytes(buffer, min<size_t>(sizeof(buffer), available));
      if (read <= 0) break;
      if ((int)Update.write(buffer, read) != read) {
        Serial.println("[OTA] Update.write short");
        mbedtls_md_free(&ctx);
        Update.abort();
        http.end();
        reportOtaStatus("DOWNLOAD_FAILED", m.version, "Update.write short");
        return false;
      }
      mbedtls_md_update(&ctx, buffer, read);
      written += read;
    } else {
      delay(1);
    }
  }
  http.end();
  client.stop();

  uint8_t hashOut[32];
  mbedtls_md_finish(&ctx, hashOut);
  mbedtls_md_free(&ctx);

  String actual = toHex(hashOut, sizeof(hashOut));
  if (!actual.equalsIgnoreCase(m.sha256)) {
    Serial.printf("[OTA] SHA-256 mismatch expected=%s got=%s\n",
                  m.sha256.c_str(), actual.c_str());
    Update.abort();
    reportOtaStatus("DOWNLOAD_FAILED", m.version, "SHA-256 mismatch");
    return false;
  }
  if (!Update.end(true)) {
    Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
    reportOtaStatus("DOWNLOAD_FAILED", m.version,
                    String("Update.end: ") + Update.errorString());
    return false;
  }
  // [W14-2a] Fresh ledger per image: the old counter accumulated across
  // image generations — two power-blipped updates left tries=2 in NVS, so
  // a perfectly healthy THIRD image would boot at tries=2 → 3 and
  // instantly self-rollback at setup. Reset at write time makes the
  // counter per-image, as documented.
  nvsSetBootTries(0);
  // [W14-2b] Revert marker: a next boot of an image OLDER than this
  // version means the bootloader reverted the update — report it.
  nvsSetLastFlashed(m.version);
  Serial.printf("[OTA] Success v%s (%u bytes). Rebooting...\n",
                m.version.c_str(), (unsigned)written);
  delay(1000);
  ESP.restart();
  return true;
}

void checkOta() {
  if (WiFi.status() != WL_CONNECTED) return;
  OtaManifest m = fetchOtaManifest();
  if (!m.valid) return;
  if (m.version == String(FIRMWARE_VERSION)) return;
  // [FW-A6] Anti-downgrade — numeric semver, not string compare. A manifest
  // older than the running image is refused (the HMAC chain still gates
  // authorship; this gate closes accidental/forced rollbacks).
  // [FW6-1] Refusals are REPORTED (event REFUSED) so an operator pushing a
  // stale manifest learns why nothing happened instead of waiting forever.
  int cmp = semverCompare(m.version, String(FIRMWARE_VERSION));
  if (cmp == -2) {
    Serial.printf("[OTA] manifest version '%s' is not semver — refusing\n",
                  m.version.c_str());
    reportOtaStatus("REFUSED", m.version, "manifest version not semver");
    return;
  }
  if (cmp <= 0) {
    Serial.printf("[OTA] manifest %s <= running %s — downgrade refused\n",
                  m.version.c_str(), FIRMWARE_VERSION);
    reportOtaStatus("REFUSED", m.version,
                    String("downgrade refused (running ") + FIRMWARE_VERSION + ")");
    return;
  }
  Serial.printf("[OTA] new version %s (running %s)\n",
                m.version.c_str(), FIRMWARE_VERSION);
  applyOta(m);
}

// ============================================================================
// Auto Calibration — poll GAS for pending, apply to LittleFS, ACK.
// ============================================================================
void checkCalibration() {
  if (WiFi.status() != WL_CONNECTED || config.gasUrl.length() == 0) return;

  JsonDocument body;
  body["action"]     = "CALIBRATION_PENDING";
  body["token"]      = config.token;
  body["device_key"] = config.deviceKey;
  String encoded;
  serializeJson(body, encoded);

  WiFiClientSecure client;
  // [WAVE-4 / GAS-2-D] was setInsecure() — the channel that carries
  // AUTH_TOKEN was MITM-exposed. GAS contract §2.4 pins the endpoint to
  // script.google.com, anchored by GTS Root R4 (fail-closed for any
  // non-Google gasUrl — visible immediately at setup, never silent).
  client.setCACert(GAS_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, config.gasUrl)) return;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(encoded);
  if (code != 200) { http.end(); client.stop(); return; }
  String payload = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  JsonVariantConst data = doc["data"];
  if (data.isNull()) return;

  String commandId = data["command_id"].as<String>();
  if (commandId.length() == 0) return;

  float v  = data["v_calib"]    | NAN;
  float dc = data["i_calib_dc"] | NAN;
  float ac = data["i_calib_ac"] | NAN;
  if (isnan(v) || isnan(dc) || isnan(ac)) return;
  // [FW-A2] Firmware-side clamp mirroring the GAS-2-K server gate
  // (v [0.1,100], i [0.1,50]). GAS is the primary validator, but a
  // mixed-version fleet or a hand-edited sheet must never park a
  // zero/negative factor into the telemetry math — clamp, log, apply.
  {
    float cv  = clampCalibration(v,  CALIB_V_MIN, CALIB_V_MAX);
    float cdc = clampCalibration(dc, CALIB_I_MIN, CALIB_I_MAX);
    float cac = clampCalibration(ac, CALIB_I_MIN, CALIB_I_MAX);
    if (cv != v || cdc != dc || cac != ac) {
      Serial.printf("[CALIB] values clamped to contract range "
                    "(v %.4f->%.4f, dc %.4f->%.4f, ac %.4f->%.4f)\n",
                    v, cv, dc, cdc, ac, cac);
    }
    v = cv; dc = cdc; ac = cac;
  }

  Serial.printf("[CALIB] applying cmd=%s v=%.4f dc=%.4f ac=%.4f\n",
                commandId.c_str(), v, dc, ac);
  config.vCalib   = v;
  config.iCalibDc = dc;
  config.iCalibAc = ac;
  saveConfig();

  // ACK back to GAS so the row is marked applied.
  // [WAVE-3 / GAS-2-I] device_key rides WITH the ACK — Code.gs rejects an
  // ACK that does not name the device the command was queued for.
  JsonDocument ack;
  ack["action"]     = "CALIBRATION_ACK";
  ack["token"]      = config.token;
  ack["device_key"] = config.deviceKey;
  ack["command_id"] = commandId;
  String ackEncoded;
  serializeJson(ack, ackEncoded);

  WiFiClientSecure ackClient;
  // [WAVE-5 / FW-A1] Was the one setInsecure() Wave 4 missed — the ACK
  // carries AUTH_TOKEN + DEVICE_KEY on the same GAS endpoint as every
  // other pinned site. Same anchor, same fail-closed policy.
  ackClient.setCACert(GAS_ROOT_CA);
  HTTPClient ackHttp;
  ackHttp.setTimeout(HTTP_TIMEOUT_MS);
  if (ackHttp.begin(ackClient, config.gasUrl)) {
    ackHttp.addHeader("Content-Type", "application/json");
    int ackCode = ackHttp.POST(ackEncoded);
    Serial.printf("[CALIB] ACK HTTP=%d\n", ackCode);
    ackHttp.end();
  }
  ackClient.stop();
}

// ============================================================================
// OTA status reporter — posts activated / rollback / boot_failed to GAS
// ============================================================================
// [W14-2c] Returns the HTTP code (0 = not attempted — no WiFi/config).
int reportOtaStatus(const char* event, const String& version, const String& message) {
  if (config.gasUrl.length() == 0 || config.token.length() == 0) return 0;
  if (WiFi.status() != WL_CONNECTED) return 0;

  JsonDocument body;
  body["action"]     = "OTA_STATUS";
  body["token"]      = config.token;
  body["device_key"] = config.deviceKey;
  body["event"]      = event;
  body["version"]    = version;
  body["message"]    = message;
  String encoded;
  serializeJson(body, encoded);

  WiFiClientSecure client;
  // [WAVE-4 / GAS-2-D] was setInsecure() — the channel that carries
  // AUTH_TOKEN was MITM-exposed. GAS contract §2.4 pins the endpoint to
  // script.google.com, anchored by GTS Root R4 (fail-closed for any
  // non-Google gasUrl — visible immediately at setup, never silent).
  client.setCACert(GAS_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  int code = 0;
  if (http.begin(client, config.gasUrl)) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(encoded);
    Serial.printf("[OTA-STATUS] %s v%s -> HTTP=%d\n",
                  event, version.c_str(), code);
    http.end();
  }
  client.stop();
  return code;
}

// ============================================================================
// Rollback bookkeeping — called from setup() to detect boot loops on new img.
// ============================================================================
void handleOtaRollback() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    // Not a fresh OTA image; nothing to do.
    return;
  }

  uint8_t tries = nvsGetBootTries();
  tries++;
  Serial.printf("[OTA] pending-verify boot try #%u\n", tries);
  if (tries >= OTA_MAX_BOOT_ATTEMPTS) {
    Serial.println("[OTA] max boot attempts reached — rolling back!");
    nvsSetBootTries(0);
    // [W14-2b] this path reports ROLLBACK directly; consume the marker so
    // the reverted image does not double-report on its next boot.
    nvsSetLastFlashed("");
    // Try to report BEFORE rollback (may fail if WiFi not up yet — that's fine).
    reportOtaStatus("ROLLBACK", FIRMWARE_VERSION, "boot loop detected");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    // Unreachable, board reboots.
  } else {
    nvsSetBootTries(tries);
  }
}

// ============================================================================
// [W14-2b] Bootloader-revert detection. Verified against the REAL IDF v4.4.7
// bootloader sources + the arduino-esp32 2.0.17 sdkconfig
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y): a fresh image gets exactly ONE
// unconfirmed boot. ANY reset before the 60 s confirm (power blip, panic,
// WDT — all equal) makes the bootloader mark it ABORTED and boot this
// (older) image instead — silently, and with attempt #2 of the new image
// never happening, so the >= 3 branch above cannot fire for this case.
// Running an image OLDER than the stored lastFlashed marker can therefore
// only mean the update was reverted: arm a deferred ROLLBACK report. The
// NVS marker is kept until the report is actually delivered (HTTP 200) —
// it survives reboots and AP-mode boots.
// ============================================================================
void checkBootloaderRevert() {
  String lf = nvsGetLastFlashed();
  if (lf.length() == 0) return;
  if (lf == String(FIRMWARE_VERSION)) return;   // the new image itself is running
  int cmp = semverCompare(lf, String(FIRMWARE_VERSION));
  if (cmp == 1) {
    Serial.printf("[OTA] update v%s was reverted by the bootloader — running v%s\n",
                  lf.c_str(), FIRMWARE_VERSION);
    otaRollbackReportPending = true;
    otaRollbackReportVersion = lf;
    // Marker deliberately KEPT until delivery — see the STA loop.
  } else {
    // Running NEWER than the marker (manual USB flash of a higher version) —
    // no rollback happened; consume the marker so it cannot misfire later.
    nvsSetLastFlashed("");
  }
}

void markOtaHealthyIfPending() {
  if (otaHealthyMarked) return;
  if (runtimeStartedAt == 0) return;
  if (millis() - runtimeStartedAt < OTA_HEALTHY_AFTER_MS) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] new image validated — rollback cancelled.");
    // [FW6-8] The rollback criterion is BOOT health, not connectivity: an
    // image that serves its setup portal for 60 s has proven it boots and
    // runs. Report honestly which mode validated it.
    reportOtaStatus("ACTIVATED", FIRMWARE_VERSION,
                    WiFi.status() == WL_CONNECTED
                      ? "healthy after boot"
                      : "healthy in setup/AP mode (no STA)");
    // [W14-2b] the marker is consumed ONLY when this image actually
    // confirmed (it was PENDING_VERIFY). On the reverted OLD image the
    // marker must survive the 60 s mark — it is the retry persistence for
    // the deferred ROLLBACK report (cleared on HTTP 200 in the STA loop).
    nvsSetLastFlashed("");
  }
  nvsSetBootTries(0);
  otaHealthyMarked = true;
}

// ============================================================================
// Factory reset — hold BOOT (GPIO0) 10s
// ============================================================================
void handleFactoryReset() {
  if (digitalRead(RESET_PIN) == LOW) {
    if (buttonDownAt == 0) buttonDownAt = millis();
    if (millis() - buttonDownAt >= FACTORY_RESET_MS) {
      LittleFS.remove(CONFIG_PATH);
      pinMode(LED_PIN, OUTPUT);
      for (uint8_t i = 0; i < 20; i++) {
        digitalWrite(LED_PIN, i % 2);
        delay(80);
      }
      ESP.restart();
    }
  } else {
    buttonDownAt = 0;
  }
}

// [FW-A5] STA link-state flag — set every cycle while connected; the
// disconnected branch uses it to start a FRESH bounded-reconnect window
// after each recovery (no stale timers across flapping links).
bool staWasConnected = true;

// ============================================================================
// Setup / Loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(LED_PIN,   OUTPUT);
  // v1.6.0 [E-WAVE] — FAIL-SAFE FIRST: drive the emergency relay pin HIGH
  // (ISOLATED) before LittleFS/WiFi/anything that can hang. ESP32 GPIOs are
  // Hi-Z at reset → module opto OFF → isolated anyway; this makes it EXPLICIT
  // and covers the window until user code runs.
  pinMode(RELAY_EMERGENCY_PIN, OUTPUT);
  digitalWrite(RELAY_EMERGENCY_PIN, HIGH);          // active-LOW → ISOLATED
  pinMode(ESTOP_SENSE_PIN, INPUT_PULLUP);
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  // [FW6-8] Runtime reference for boot-health marking — set BEFORE any
  // early return so AP-mode boots can validate a pending OTA image too.
  runtimeStartedAt = millis();

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] mount failed");
  }

  Serial.printf("[BOOT] PLTS Monitor firmware v%s\n", FIRMWARE_VERSION);

  ina219Init();
  acs712Autotare();

  // v1.6.0 [E-WAVE] — emergency init on the compile-time/default pins FIRST
  // (crash-chain accounting + BOOT event + explicit ISOLATED level), then
  // re-apply the operator-configured pins once the config is loaded.
  emgInit();

  bool configured = loadConfig();
  if (configured) {
    emgApplyPinsFromConfig();   // operator may have moved relayPin/estopPin
  }
  if (!configured) {
    Serial.println("[BOOT] no config -> AP mode");
    handleOtaRollback();          // still count attempt so rollback works
    checkBootloaderRevert();      // [W14-2b] revert → deferred ROLLBACK report
    startPortal();
    return;
  }

  handleOtaRollback();             // may reboot if boot-loop detected
  checkBootloaderRevert();         // [W14-2b] detect bootloader revert → report

  // [WAVE-1 / GAS-2-A] Monotonic sequence init — must run before the first
  // sendTelemetry() so every POST carries a never-reused number.
  initSequence();

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    esp_task_wdt_reset();
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[BOOT] WiFi failed -> AP fallback");
    startPortal();
  } else {
    currentMode      = Mode::STA_MODE;
    staConnectedAt   = millis();
    Serial.printf("[STA] Connected IP=%s\n", WiFi.localIP().toString().c_str());
    checkOta();
    lastOtaCheck = millis();
  }
}

void loop() {
  esp_task_wdt_reset();
  handleFactoryReset();

  if (currentMode == Mode::AP_MODE) {
    dnsServer.processNextRequest();
    server.handleClient();
    // [FW6-8] A pending-verify OTA image proves itself by RUNNING — in AP
    // mode too. Previously only an STA connection could cancel the rollback,
    // so a healthy image + a dead router = 3 reboots = pointless rollback.
    markOtaHealthyIfPending();
    if (apStartedAt > 0 && config.ssid.length() > 0 &&
        millis() - apStartedAt > AP_FALLBACK_MS) {
      ESP.restart();
    }
  } else {
    // v1.6.0 [E-WAVE] — LOCAL-FIRST: sensor sampling + the ENTIRE emergency
    // layer (E-stop, triggers, LED, event queue) run regardless of WiFi state.
    // A safety function must never depend on the network being up.
    sampleSensors();
    emgTick();
    if (WiFi.status() == WL_CONNECTED) {
      staWasConnected = true;   // [FW-A5] fresh window on the next drop
      markOtaHealthyIfPending();
      // [W14-2b] Deliver the deferred ROLLBACK report (armed at boot when
      // the bootloader reverted an unconfirmed image). Marker consumed
      // only on HTTP 200 — retries on later boots if GAS was unreachable.
      if (otaRollbackReportPending) {
        int rc = reportOtaStatus("ROLLBACK", otaRollbackReportVersion,
                                 "bootloader revert (image unconfirmed within its single boot)");
        if (rc == 200) {
          nvsSetLastFlashed("");
          otaRollbackReportPending = false;
        }
      }
      if (millis() - lastTelemetry >= (uint32_t)config.interval * 1000UL) {
        sendTelemetry();
        lastTelemetry = millis();
      }
      if (millis() - lastOtaCheck >= OTA_INTERVAL_MS) {
        checkOta();
        lastOtaCheck = millis();
      }
      if (millis() - lastCalibCheck >= CALIB_INTERVAL_MS) {
        checkCalibration();
        lastCalibCheck = millis();
      }
      // v1.6.0 [E-WAVE] — dedicated emergency command poll (15 s). Commands
      // usually arrive via the telemetry piggyback; this bounds the latency
      // when the next telemetry POST is backed off.
      if (millis() - lastEmgPoll >= EMERGENCY_POLL_MS) {
        checkEmergencyPending();
        lastEmgPoll = millis();
      }
    } else {
      // [FW-A5] Bounded reconnect instead of an instant restart: the old
      // ESP.restart()-every-2s loop hammered NVS (1+ write per boot: seq
      // high-water mark) — a day-long outage ≈ thousands of erase cycles.
      // Honest behavior: keep sampling, retry STA every 10 s for 2 min, only
      // then restart (which re-enters the normal 45 s connect + AP fallback).
      static uint32_t staLostAt   = 0;
      static uint32_t lastRetryAt = 0;
      uint32_t now = millis();
      if (staWasConnected) {
        staLostAt = now;
        lastRetryAt = now;
        staWasConnected = false;
        Serial.println("[STA] disconnected — bounded reconnect window starts");
      }
      if (now - lastRetryAt >= STA_RECONNECT_EVERY_MS) {
        lastRetryAt = now;
        Serial.println("[STA] reconnect attempt");
        WiFi.reconnect();
      }
      if (now - staLostAt >= STA_RECONNECT_WINDOW_MS) {
        Serial.println("[STA] reconnect window exhausted — restarting");
        delay(1000);
        ESP.restart();
      }
    }
  }
  delay(50);
}
