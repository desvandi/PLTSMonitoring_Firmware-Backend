// =============================================================================
// Services/EmergencySupervisor.cpp — E-WAVE v1.6.0 emergency state machine
// -----------------------------------------------------------------------------
// Logic ported line-for-line from firmware-generic/src/plts_firmware_v1.ino
// (emgInit/emgTick/emgTrip/emgArm/emgEvaluateTriggers/emgArmBlockReason/
// emgTrackClear/emgPollEstop/emgApplyCommand), adapted to the modular
// service/task pattern. See EmergencySupervisor.h for port deltas.
// =============================================================================
#include "EmergencySupervisor.h"

#if PLTS_ENABLE_EMERGENCY

#include <cmath>
#include <Preferences.h>
#include "../Drivers/EmergencyRelayDriver.h"
#include "../Drivers/Ina219Driver.h"
#include "../Core/Globals.h"
#include "../Services/AlarmRegistry.h"
#include "../Services/LogService.h"
#include "../Storage/ConfigStore.h"
#if PLTS_ENABLE_RELAYS
#include "../Services/RelayController.h"   // [v1.8.0] E-WAVE safety cascade
#endif

namespace Services {

EmergencySupervisor emergency;

// NVS namespace + keys (counters owned here; the 13 config fields live in the
// same namespace under Storage::ConfigStore — different keys, no overlap).
static const char* NVS_NS_EMG       = "plts_emg";
static const char* NVS_KEY_TRIPS    = "emg_trips";   // u32 lifetime counter
static const char* NVS_KEY_RUN_OK   = "emg_run_ok";  // u8 previous boot reached 5 stable min
static const char* NVS_KEY_CHAIN    = "emg_chain";   // u8 consecutive unhealthy reboots

// LED cadence: RUN = solid ON, EMERGENCY = 2 Hz blink.
static const uint32_t EMG_LED_BLINK_MS = 250;

// ---------------------------------------------------------------------------
void EmergencySupervisor::begin() {
  if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();

  // Driver pins from persisted operator config (compile-time defaults when
  // the config has never been written — defaults-on-read, no migration).
  Drivers::emergencyRelay.applyPins(Core::cfgEmgRelayPin, Core::cfgEmgEstopPin,
                                     Core::cfgEmgEstopEnabled != 0);
  pinMode(Core::PIN_EMERGENCY_LED, OUTPUT);
  digitalWrite(Core::PIN_EMERGENCY_LED, LOW);

  // Crash-chain bookkeeping (fail-safe boot accounting):
  //   run_ok=1 (previous boot stayed healthy >=5 min) -> chain resets to 0.
  //   run_ok=0 (died early / crash-looped)             -> chain increments.
  Preferences p;
  p.begin(NVS_NS_EMG, false);
  _trips      = p.getUInt(NVS_KEY_TRIPS, 0);
  uint8_t runOk = p.getUChar(NVS_KEY_RUN_OK, 1);
  p.putUChar(NVS_KEY_RUN_OK, 0);          // assume unhealthy until proven
  _crashChain = runOk ? 0 : (uint8_t)(p.getUChar(NVS_KEY_CHAIN, 0) + 1);
  p.putUChar(NVS_KEY_CHAIN, _crashChain);
  p.end();
  _runOkMarked = false;

  _state  = EmgState::Emergency;          // RAM-only: EVERY boot re-enters EMERGENCY
  _tripAtMs = 0;
  _clearAtMs = 0;
  memset(_debounce, 0, sizeof(_debounce));
  _estopOpen = Drivers::emergencyRelay.isEstopOpen();

  if (_crashChain >= EMG_CRASH_CHAIN_LIMIT) {
    _reason = EMG_REASON_CRASHLOOP;
    queueEvent("CRASHLOOP", String("reboot chain ") + _crashChain +
               " tanpa runtime sehat — tahan terisolasi");
  } else {
    _reason = EMG_REASON_BOOT;
    queueEvent("BOOT", "boot sehat — menunggu operator ARM");
  }

  Services::alarms.raise(Core::AlarmCode::EMERGENCY_TRIP, Core::AlarmSeverity::Critical,
       (String("Emergency layer isolated at boot (") + _reason + ") — operator ARM required").c_str());
  publishStatus();
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::tick() {
  if (_mutex == nullptr) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;  // skip cycle — state is latched in hardware

  _pollEstop();

  EmgSensors s = _readSensors();
  if (_state == EmgState::Run) {
    _evaluateTriggers(s);
    _trackClear(s);
  } else {
    _trackClear(s);   // keep the recovery clock honest while isolated
  }
  _markRuntimeHealthy();
  _ledTick();

  xSemaphoreGive(_mutex);
  publishStatus();
}

// ---------------------------------------------------------------------------
EmgSensors EmergencySupervisor::_readSensors() {
  // Canonical pipeline snapshot (quality-gated): Valid/Derived/Estimated +
  // non-NaN -> usable value; anything else -> NaN (fail-closed below).
  EmgSensors s{};
  s.vbat = NAN; s.idc = NAN; s.iac = NAN;
  s.igen = NAN;                      // RESERVED channel (no 2nd ACS712 on this board)
  s.ina219Present = Drivers::ina219Battery.isAvailable();
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (latestStatus.battery.voltage.isValid()) s.vbat = latestStatus.battery.voltage.value;
    if (latestStatus.battery.current.isValid()) s.idc  = latestStatus.battery.current.value;
    if (latestStatus.ac.rmsCurrent.isValid())   s.iac  = latestStatus.ac.rmsCurrent.value;
    xSemaphoreGive(telemetryMutex);
  }
  return s;
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_trip(const char* reason, const char* eventType,
                                 const String& eventReason) {
  // LATCHED — no re-trip while already EMERGENCY (idempotent).
  if (_state == EmgState::Emergency) return;
  _state    = EmgState::Emergency;
  _reason   = reason;
  _tripAtMs = millis();
  _trips++;
  _nvsSetU32(NVS_KEY_TRIPS, _trips);
  Drivers::emergencyRelay.setEnergized(false);   // ISOLATED — immediate, local GPIO
#if PLTS_ENABLE_RELAYS
  // [v1.8.0] E-WAVE safety cascade — one-way gate. When E-WAVE trips,
  // all 8 relay channels are forced OFF. This CANNOT be overridden by
  // REST, MQTT, PWA, or Scheduler. The reverse (relay → E-WAVE) is FORBIDDEN.
  Services::relaysController.emergencyAllOff();
#endif
  _queueEventUnlocked(eventType, eventReason.length() > 0 ? eventReason : String(reason));
  Services::alarms.raise(Core::AlarmCode::EMERGENCY_TRIP, Core::AlarmSeverity::Critical,
       (String("EMERGENCY TRIP: ") + reason + " — relay isolated, operator ARM required").c_str());
  Services::Log.append(Core::LogType::Info, String("EMERGENCY_TRIP reason=") + reason);
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_arm(const char* source) {
  _state    = EmgState::Run;
  _reason   = "";
  _tripAtMs = 0;
  Drivers::emergencyRelay.setEnergized(true);    // RUN (energize kontaktor path)
  _queueEventUnlocked("ARMED", String("operator ARM (") + source + ") — relay energized");
  Services::alarms.clear(Core::AlarmCode::EMERGENCY_TRIP);
  Services::Log.append(Core::LogType::Info, String("EMERGENCY_ARMED source=") + source);
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_pollEstop() {
  bool open = Drivers::emergencyRelay.isEstopOpen();
  if (open == _estopOpen) return;
  _estopOpen = open;
  if (open) {
    // Hardware already cut the relay negative line — this is the LATCH copy.
    // A physical E-stop while already isolated only updates the flag.
    if (_state == EmgState::Run) {
      _trip(EMG_REASON_ESTOP, "ESTOP",
            "physical e-stop opened the relay negative line");
    } else {
      _queueEventUnlocked("ESTOP", "physical e-stop opened the relay negative line");
    }
  } else {
    // Release does NOT re-energize — stays ISOLATED until operator ARM.
    _queueEventUnlocked("ESTOP_RELEASED", "e-stop line closed — operator ARM required");
  }
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_evaluateTriggers(const EmgSensors& s) {
  // Slot map: [0]=vbatLo [1]=vbatHi [2]=iDc [3]=iAc [4]=iAcGen [5]=sensorLoss.
  // Debounce = config.debounceN consecutive violating evaluations (default 3
  // @ 10 Hz = 300 ms). Voltage channels carry hysteresis; current channels
  // reset to 0 on any clean evaluation. Dead band (between threshold and
  // threshold +/- hyst) HOLDS the counter — neither increments nor resets.

  const float n = (float)Core::cfgEmgDebounceN;

  // [0] VBAT_LOW (+ hysteresis)
  if (std::isfinite(s.vbat) && s.vbat < Core::cfgEmgVbatLowV) {
    if ((float)++_debounce[0] >= n) { _trip(EMG_REASON_VBAT_LOW); return; }
  } else if (std::isfinite(s.vbat) && s.vbat > Core::cfgEmgVbatLowV + Core::cfgEmgVbatLowHystV) {
    _debounce[0] = 0;
  }

  // [1] VBAT_HIGH (+ hysteresis)
  if (std::isfinite(s.vbat) && s.vbat > Core::cfgEmgVbatHighV) {
    if ((float)++_debounce[1] >= n) { _trip(EMG_REASON_VBAT_HIGH); return; }
  } else if (std::isfinite(s.vbat) && s.vbat < Core::cfgEmgVbatHighV - Core::cfgEmgVbatHighHystV) {
    _debounce[1] = 0;
  }

  // [2] I_DC_OVER (only meaningful when the INA219 is present)
  if (s.ina219Present && std::isfinite(s.idc) && std::fabs(s.idc) > Core::cfgEmgIDcOverA) {
    if ((float)++_debounce[2] >= n) { _trip(EMG_REASON_I_DC_OVER); return; }
  } else {
    _debounce[2] = 0;
  }

  // [3] I_AC_LOAD_OVER
  if (std::isfinite(s.iac) && s.iac > Core::cfgEmgIAcLoadOverA) {
    if ((float)++_debounce[3] >= n) { _trip(EMG_REASON_I_AC_LOAD_OVER); return; }
  } else {
    _debounce[3] = 0;
  }

  // [4] I_AC_GEN_OVER — RESERVED channel: NaN on this board until a second
  // ACS712 exists. The branch is dormant by construction (isfinite gate).
  if (std::isfinite(s.igen) && s.igen > Core::cfgEmgIAcGenOverA) {
    if ((float)++_debounce[4] >= n) { _trip(EMG_REASON_I_AC_GEN_OVER); return; }
  } else {
    _debounce[4] = 0;
  }

  // [5] SENSOR_LOSS (v1.7.0 [P1-SC3] fail-closed, policy-gated).
  // NOTE port delta: iGen is NOT part of the loss set — the channel is
  // reserved, so its permanent NaN must not hold the system hostage.
  if (Core::cfgEmgSensorFailPolicy) {
    bool sensorLoss = !std::isfinite(s.vbat) || !std::isfinite(s.idc) || !std::isfinite(s.iac);
    if (sensorLoss) {
      if ((float)++_debounce[5] >= n) { _trip(EMG_REASON_SENSOR_LOSS); return; }
    } else {
      _debounce[5] = 0;
    }
  } else {
    _debounce[5] = 0;
  }
}

// ---------------------------------------------------------------------------
String EmergencySupervisor::_armBlockReason(const EmgSensors& s) {
  // Order matters (parity with firmware-generic). Every return is a REJECT
  // reason surfaced verbatim in the ACK message — the operator sees WHY.
  if (!std::isfinite(s.vbat)) return "sensor tegangan tidak valid";
  if (Core::cfgEmgSensorFailPolicy) {
    // Fail-closed: unmonitored IS unsafe for a safety interlock.
    if (!s.ina219Present)
      return "sensor INA219 tidak terdeteksi — proteksi arus DC nonaktif (sensorFailPolicy=1)";
    if (!std::isfinite(s.idc)) return "sensor arus DC tidak valid";
    if (!std::isfinite(s.iac)) return "sensor arus beban AC tidak valid";
    // (iGen deliberately absent — reserved channel, see header port deltas.)
  }
  if (s.vbat <= Core::cfgEmgVbatLowV + Core::cfgEmgVbatLowHystV)
    return String("VBAT masih rendah (") + s.vbat + "V)";
  if (s.vbat >= Core::cfgEmgVbatHighV - Core::cfgEmgVbatHighHystV)
    return String("VBAT masih tinggi (") + s.vbat + "V)";
  if (s.ina219Present && std::isfinite(s.idc) && std::fabs(s.idc) > Core::cfgEmgIDcOverA)
    return String("arus DC masih di atas ambang (") + std::fabs(s.idc) + "A)";
  if (std::isfinite(s.iac) && s.iac > Core::cfgEmgIAcLoadOverA)
    return String("arus beban masih di atas ambang (") + s.iac + "A)";
  if (std::isfinite(s.igen) && s.igen > Core::cfgEmgIAcGenOverA)
    return String("arus jenset masih di atas ambang (") + s.igen + "A)";

  // Recovery window: triggers must stay clear for recoverySec.
  if (_clearAtMs == 0) _clearAtMs = millis();
  if (millis() - _clearAtMs < Core::cfgEmgRecoverySec * 1000UL)
    return String("masih dalam masa pemulihan (") +
           (long)(Core::cfgEmgRecoverySec - (millis() - _clearAtMs) / 1000UL) + " detik lagi)";
  return "";
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_trackClear(const EmgSensors& s) {
  String blocked = _armBlockReason(s);
  bool clearNow = (blocked.length() == 0) ||
                  blocked.startsWith("masih dalam masa pemulihan");
  if (!clearNow) {
    _clearAtMs = 0;              // a re-violation resets the recovery clock
  } else if (_clearAtMs == 0) {
    _clearAtMs = millis();
  }
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_markRuntimeHealthy() {
  if (_runOkMarked) return;                       // one NVS write per boot
  if (millis() < EMG_HEALTHY_RUNTIME_MS) return;  // 5 stable minutes
  _runOkMarked = true;
  Preferences p;
  p.begin(NVS_NS_EMG, false);
  p.putUChar(NVS_KEY_RUN_OK, 1);
  p.end();
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_ledTick() {
  if (_state == EmgState::Run) {
    digitalWrite(Core::PIN_EMERGENCY_LED, HIGH);  // solid
    _ledState = true;
    return;
  }
  uint32_t now = millis();
  if (now - _lastLedToggleMs >= EMG_LED_BLINK_MS) {   // 2 Hz blink
    _ledState = !_ledState;
    digitalWrite(Core::PIN_EMERGENCY_LED, _ledState ? HIGH : LOW);
    _lastLedToggleMs = now;
  }
}

// ---------------------------------------------------------------------------
// applyCommand — operator ARM / DISARM / CONFIG (GAS queue, device-bound).
// All commands are IDEMPOTENT by construction, which is what makes the
// GAS DELIVERED->re-delivery window safe without a device-side journal:
//   ARM re-apply    -> "already RUN" (APPLIED, no state change)
//   DISARM re-apply -> relay already isolated (APPLIED, no state change)
//   CONFIG re-apply -> same clamped fields (APPLIED, no state change)
// ---------------------------------------------------------------------------
String EmergencySupervisor::applyCommand(const String& commandId,
                                          const String& command,
                                          JsonVariantConst cfg,
                                          String& messageOut) {
  (void)commandId;   // logged by the channel; the supervisor gates on state only
  String cmd = command;
  cmd.toUpperCase();
  String result = "REJECTED";

  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    messageOut = "supervisor busy — retry";
    return result;
  }

  if (cmd == "ARM") {
    if (_state == EmgState::Run) {
      result = "APPLIED";
      messageOut = "already RUN";
    } else if (_crashChain >= EMG_CRASH_CHAIN_LIMIT) {
      messageOut = "crash-loop hold active — power-cycle stable first";
    } else {
      EmgSensors s = _readSensors();
      String block = _armBlockReason(s);
      if (block.length() > 0) {
        messageOut = block;
      } else {
        _arm("operator");
        result = "APPLIED";
        messageOut = "relay energized";
      }
    }
  } else if (cmd == "DISARM") {
    // No gates — DISARM is always the safe direction.
    if (_state == EmgState::Run) {
      _state    = EmgState::Emergency;
      _reason   = EMG_REASON_OPERATOR;
      _tripAtMs = millis();
      _trips++;
      _nvsSetU32(NVS_KEY_TRIPS, _trips);
      _queueEventUnlocked("DISARMED", "operator DISARM — relay isolated");
      Services::alarms.raise(Core::AlarmCode::EMERGENCY_TRIP, Core::AlarmSeverity::Critical,
           "Emergency layer DISARMED by operator — relay isolated");
    }
    Drivers::emergencyRelay.setEnergized(false);   // always — safe direction
    result = "APPLIED";
    messageOut = "relay isolated";
  } else if (cmd == "CONFIG") {
    if (cfg.isNull() || !cfg.is<JsonObject>()) {
      messageOut = "missing config object";
    } else {
      // Field-by-field: out-of-range/absent fields are dropped and keep the
      // current value (parity with firmware-generic + GAS EMERGENCY_CONFIG_
      // FIELDS validation ranges).
      Core::cfgEmgVbatLowV    = clampEmgFloat(cfg["vbatLowV"]    | (float)NAN, 30.0f, 60.0f, Core::cfgEmgVbatLowV);
      Core::cfgEmgVbatLowHystV= clampEmgFloat(cfg["vbatLowHystV"]| (float)NAN, 0.1f, 5.0f, Core::cfgEmgVbatLowHystV);
      Core::cfgEmgVbatHighV   = clampEmgFloat(cfg["vbatHighV"]   | (float)NAN, 48.0f, 60.0f, Core::cfgEmgVbatHighV);
      Core::cfgEmgVbatHighHystV=clampEmgFloat(cfg["vbatHighHystV"]| (float)NAN, 0.1f, 5.0f, Core::cfgEmgVbatHighHystV);
      Core::cfgEmgIDcOverA    = clampEmgFloat(cfg["iDcOverA"]    | (float)NAN, 10.0f, 120.0f, Core::cfgEmgIDcOverA);
      Core::cfgEmgIAcLoadOverA= clampEmgFloat(cfg["iAcLoadOverA"]| (float)NAN, 5.0f, 40.0f, Core::cfgEmgIAcLoadOverA);
      Core::cfgEmgIAcGenOverA = clampEmgFloat(cfg["iAcGenOverA"] | (float)NAN, 5.0f, 40.0f, Core::cfgEmgIAcGenOverA);
      Core::cfgEmgDebounceN   = (uint8_t)clampEmgInt(cfg["debounceN"]   | (long)-1, 1, 10, Core::cfgEmgDebounceN);
      Core::cfgEmgRecoverySec = (uint32_t)clampEmgInt(cfg["recoverySec"]| (long)-1, 0, 3600, (long)Core::cfgEmgRecoverySec);
      Core::cfgEmgRelayPin    = (uint8_t)clampEmgInt(cfg["relayPin"]    | (long)-1, 12, 39, Core::cfgEmgRelayPin);
      Core::cfgEmgEstopPin    = (int8_t)clampEmgInt(cfg["estopPin"]     | (long)-99, -1, 39, Core::cfgEmgEstopPin);
      Core::cfgEmgEstopEnabled = (uint8_t)clampEmgInt(cfg["estopEnabled"]| (long)-1, 0, 1, Core::cfgEmgEstopEnabled);
      Core::cfgEmgSensorFailPolicy = (uint8_t)clampEmgInt(cfg["sensorFailPolicy"] | (long)-1, 0, 1, Core::cfgEmgSensorFailPolicy);

      Drivers::emergencyRelay.applyPins(Core::cfgEmgRelayPin, Core::cfgEmgEstopPin,
                                         Core::cfgEmgEstopEnabled != 0);
      Storage::config.saveEmergencyConfig();
      _queueEventUnlocked("CONFIG_APPLIED", "operator updated trigger thresholds");
      result = "APPLIED";
      messageOut = "emergency config updated";
    }
  } else {
    messageOut = "unknown command: " + cmd;
  }

  xSemaphoreGive(_mutex);
  publishStatus();
  return result;
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::queueEvent(const char* type, const String& reason) {
  // Single-slot queue: the LATEST unsent event wins (parity with generic).
  // External entry point (takes the lock); internal callers use
  // _queueEventUnlocked because they already hold _mutex (non-recursive —
  // re-taking it from the same task would self-deadlock).
  if (_mutex == nullptr) { _queueEventUnlocked(type, reason); return; }
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  _queueEventUnlocked(type, reason);
  xSemaphoreGive(_mutex);
}

void EmergencySupervisor::_queueEventUnlocked(const char* type, const String& reason) {
  _pendingEventType   = type;
  _pendingEventReason = reason;
  _hasPendingEvent    = true;
  Services::Log.append(Core::LogType::Info,
      String("EMERGENCY_EVENT type=") + type + " reason=" + reason);
}

bool EmergencySupervisor::peekPendingEvent(String& typeOut, String& reasonOut) const {
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool has = _hasPendingEvent;
  if (has) { typeOut = _pendingEventType; reasonOut = _pendingEventReason; }
  xSemaphoreGive(_mutex);
  return has;
}

void EmergencySupervisor::consumePendingEvent() {
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  _hasPendingEvent = false;
  xSemaphoreGive(_mutex);
}

// ---------------------------------------------------------------------------
const char* EmergencySupervisor::stateStr() const {
  return (_state == EmgState::Run) ? "RUN" : "EMERGENCY";
}
const char* EmergencySupervisor::reasonStr() const { return _reason; }
bool EmergencySupervisor::isEstopOpen() const { return _estopOpen; }
bool EmergencySupervisor::relayEnergized() const {
  return Drivers::emergencyRelay.isEnergized();
}
uint32_t EmergencySupervisor::trips() const { return _trips; }
uint8_t  EmergencySupervisor::crashChain() const { return _crashChain; }

// ---------------------------------------------------------------------------
void EmergencySupervisor::publishStatus() {
  // Snapshot under _mutex, write into latestStatus under telemetryMutex.
  // telemetryMutex may not exist yet when begin() runs (created later in
  // setup()) — guard and let the first tick publish instead.
  if (telemetryMutex == nullptr) return;
  const char* st; const char* rs; bool eo; uint32_t tp, tAt; uint8_t cc;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    st = stateStr(); rs = _reason; eo = _estopOpen; tp = _trips; tAt = _tripAtMs; cc = _crashChain;
    xSemaphoreGive(_mutex);
  } else {
    return;   // contended — next tick will publish; state is latched in hardware
  }
  if (xSemaphoreTake(telemetryMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    latestStatus.emergency.state          = st;
    latestStatus.emergency.reason         = rs;
    latestStatus.emergency.estopOpen      = eo;
    latestStatus.emergency.relayEnergized = Drivers::emergencyRelay.isEnergized();
    latestStatus.emergency.trips          = tp;
    latestStatus.emergency.tripAtMs       = tAt;
    latestStatus.emergency.crashChain     = cc;
    xSemaphoreGive(telemetryMutex);
  }
}

// ---------------------------------------------------------------------------
float EmergencySupervisor::clampEmgFloat(float v, float lo, float hi, float fallback) {
  if (!std::isfinite(v) || v < lo || v > hi) return fallback;
  return v;
}
long EmergencySupervisor::clampEmgInt(long v, long lo, long hi, long fallback) {
  if (v < lo || v > hi) return fallback;
  return v;
}

// ---------------------------------------------------------------------------
void EmergencySupervisor::_nvsSetU32(const char* key, uint32_t v) {
  Preferences p;
  p.begin(NVS_NS_EMG, false);
  p.putUInt(key, v);
  p.end();
}

} // namespace Services

#endif // PLTS_ENABLE_EMERGENCY
