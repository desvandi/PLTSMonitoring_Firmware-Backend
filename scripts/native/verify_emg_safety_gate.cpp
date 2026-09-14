// verify_emg_safety_gate.cpp — native ASAN harness for ROUND-6
// EmergencySupervisor remediation (p.457 safety predicate + p.465 mutex
// starvation watchdog).
//
// Predicates are copied VERBATIM from firmware/Core/Types.h; the tick()
// starvation/reconcile skeleton is copied from EmergencySupervisor.cpp.
// Build: g++ -std=c++17 -g -fsanitize=address,undefined -o verify_emg_safety_gate verify_emg_safety_gate.cpp
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// ---- Core::Types.h (verbatim subset) --------------------------------------
namespace Core {
enum class MeasurementQuality : uint8_t {
  Valid, Stale, Invalid, OutOfRange, SensorError, NotAvailable,
  Estimated, Derived, Calibrating, Suspect
};
enum class MeasurementSource : uint8_t { Measured, Derived, Estimated };
struct Measurement {
  float value;
  MeasurementQuality quality;
  MeasurementSource source;
  uint32_t timestamp;
  uint32_t sequence;
  bool isValid() const {                                     // DASHBOARD predicate
    return (quality == MeasurementQuality::Valid ||
            quality == MeasurementQuality::Derived ||
            quality == MeasurementQuality::Estimated) && !std::isnan(value);
  }
  bool isSafetyUsable() const {                              // SAFETY predicate [p.457]
    return quality == MeasurementQuality::Valid
        && source  == MeasurementSource::Measured
        && std::isfinite(value);
  }
};
enum class AlarmSeverity : uint8_t { Info = 0, Warning = 1, Critical = 2 };
} // namespace Core

// ---- Spy registry -----------------------------------------------------------
struct AlarmSpy {
  std::vector<std::string> active;
  bool has(const char* c) const {
    for (auto& s : active) if (s == c) return true;
    return false;
  }
  void raise(const char* c) { if (!has(c)) active.push_back(c); }
  void clear(const char* c) {
    for (size_t i = 0; i < active.size(); i++)
      if (active[i] == c) { active.erase(active.begin() + i); return; }
  }
};
static AlarmSpy alarms;

// ---- EmergencySupervisor mirror (relevant subset) --------------------------
struct EmgSensors {
  float vbat, idc, iac, igen;
  bool ina219Present;
};
enum class EmgState : uint8_t { Run, Emergency };
static const char* EMG_REASON_INTERNAL = "INTERNAL";
static const char* EMG_REASON_SENSOR_LOSS = "SENSOR_LOSS";

struct EmgMirror {
  static constexpr uint8_t EMG_LOCK_STARVE_LIMIT = 20;
  static constexpr uint8_t EMG_DEBOUNCE_N = 3;

  EmgState _state = EmgState::Emergency;
  std::string _reason = "BOOT";
  uint32_t _trips = 0;
  uint8_t  _debounceSensorLoss = 0;
  uint8_t  _lockStarveCycles = 0;
  bool     _lockStarveEscalated = false;
  uint32_t _lockStarveEvents = 0;
  // spies:
  bool     relayEnergized = false;
  bool     mutexAvailable = true;

  // [p.457] _readSensors mirror: STRICT predicate per safety input
  EmgSensors _readSensors(const Core::Measurement& v, const Core::Measurement& i,
                          const Core::Measurement& ac) {
    EmgSensors s{};
    s.vbat = NAN; s.idc = NAN; s.iac = NAN; s.igen = NAN;
    s.ina219Present = true;
    if (v.isSafetyUsable()) s.vbat = v.value;
    if (i.isSafetyUsable()) s.idc = i.value;
    if (ac.isSafetyUsable()) s.iac = ac.value;
    return s;
  }

  void _trip(const char* reason) {
    if (_state == EmgState::Emergency) return;
    _state = EmgState::Emergency;
    _reason = reason;
    _trips++;
    relayEnergized = false;
    alarms.raise("EMERGENCY_TRIP");
  }

  // tick() skeleton — starvation branch + reconcile + sensor-loss trigger
  void tick(const Core::Measurement& v, const Core::Measurement& i,
            const Core::Measurement& ac) {
    if (!mutexAvailable) {
      _lockStarveCycles++;
      if (_lockStarveCycles >= EMG_LOCK_STARVE_LIMIT && !_lockStarveEscalated) {
        _lockStarveEscalated = true;
        _lockStarveEvents++;
        if (relayEnergized) relayEnergized = false;   // forced ISOLATION
        alarms.raise("EMERGENCY_TRIP");
      }
      return;
    }
    _lockStarveCycles = 0;
    if (_lockStarveEscalated) {
      _lockStarveEscalated = false;
      if (_state == EmgState::Run) {
        _trip(EMG_REASON_INTERNAL);
      }
    }
    if (_state == EmgState::Run) {
      EmgSensors s = _readSensors(v, i, ac);
      // [5] SENSOR_LOSS (fail-closed)
      bool sensorLoss = !std::isfinite(s.vbat) || !std::isfinite(s.idc) || !std::isfinite(s.iac);
      if (sensorLoss) {
        if (++_debounceSensorLoss >= EMG_DEBOUNCE_N) { _trip(EMG_REASON_SENSOR_LOSS); return; }
      } else {
        _debounceSensorLoss = 0;
      }
    }
  }
};

// ============================================================================
static int fails = 0;
static void expect(bool cond, const char* name) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  if (!cond) fails++;
}
static Core::Measurement mk(float v, Core::MeasurementQuality q, Core::MeasurementSource s) {
  return Core::Measurement{ v, q, s, 0, 0 };
}

int main() {
  printf("== A: isSafetyUsable() predicate matrix [p.457] ==\n");
  expect(mk(51.2f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured).isSafetyUsable(),
         "Valid + Measured + finite → TRUE");
  expect(!mk(51.2f, Core::MeasurementQuality::Derived, Core::MeasurementSource::Measured).isSafetyUsable(),
         "quality Derived → FALSE (the audit's core case)");
  expect(!mk(51.2f, Core::MeasurementQuality::Estimated, Core::MeasurementSource::Measured).isSafetyUsable(),
         "quality Estimated → FALSE");
  expect(!mk(0.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Derived).isSafetyUsable(),
         "source Derived → FALSE");
  expect(!mk(0.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Estimated).isSafetyUsable(),
         "source Estimated → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::Stale, Core::MeasurementSource::Measured).isSafetyUsable(),
         "Stale → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::Suspect, Core::MeasurementSource::Measured).isSafetyUsable(),
         "Suspect → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::Calibrating, Core::MeasurementSource::Measured).isSafetyUsable(),
         "Calibrating → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::Invalid, Core::MeasurementSource::Measured).isSafetyUsable(),
         "Invalid → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::OutOfRange, Core::MeasurementSource::Measured).isSafetyUsable(),
         "OutOfRange → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::SensorError, Core::MeasurementSource::Measured).isSafetyUsable(),
         "SensorError → FALSE");
  expect(!mk(51.2f, Core::MeasurementQuality::NotAvailable, Core::MeasurementSource::Measured).isSafetyUsable(),
         "NotAvailable → FALSE");
  expect(!mk(NAN, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured).isSafetyUsable(),
         "NaN → FALSE");
  expect(!mk(INFINITY, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured).isSafetyUsable(),
         "+inf → FALSE (isfinite, not merely !isnan)");

  printf("== B: isValid() dashboard contract UNCHANGED ==\n");
  expect(mk(51.2f, Core::MeasurementQuality::Derived, Core::MeasurementSource::Derived).isValid(),
         "Derived still isValid()=true for dashboards");
  expect(mk(0.0f, Core::MeasurementQuality::Estimated, Core::MeasurementSource::Estimated).isValid(),
         "Estimated still isValid()=true for dashboards");
  expect(!mk(NAN, Core::MeasurementQuality::SensorError, Core::MeasurementSource::Measured).isValid(),
         "NaN still invalid for dashboards");

  printf("== C: derived current cannot feed the I_DC protection [p.457] ==\n");
  {
    EmgMirror emg;
    emg._state = EmgState::Run;
    emg.relayEnergized = true;
    // vbat + iac are real; the current sensor dropped out and the pipeline
    // left a DERIVED 0 A stand-in — isValid() accepts it, safety must not.
    Core::Measurement v = mk(51.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured);
    Core::Measurement i = mk(0.0f, Core::MeasurementQuality::Derived, Core::MeasurementSource::Derived);
    Core::Measurement ac = mk(3.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured);
    expect(i.isValid(), "pipeline says the derived stand-in is isValid()");
    for (int t = 0; t < 5; t++) emg.tick(v, i, ac);
    expect(emg._state == EmgState::Emergency, "SENSOR_LOSS fail-closed trip fired");
    expect(emg._reason == EMG_REASON_SENSOR_LOSS, "trip reason = SENSOR_LOSS");
    expect(!emg.relayEnergized, "relay ISOLATED");
  }

  printf("== D: mutex starvation watchdog [p.465] ==\n");
  {
    EmgMirror emg;
    emg._state = EmgState::Run;
    emg.relayEnergized = true;
    emg.mutexAvailable = false;
    Core::Measurement good = mk(51.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured);
    for (int t = 0; t < EmgMirror::EMG_LOCK_STARVE_LIMIT - 1; t++) emg.tick(good, good, good);
    expect(emg.relayEnergized, "below limit: relay untouched (bounded skip tolerated)");
    emg.tick(good, good, good);   // the LIMIT-th consecutive starved tick
    expect(!emg.relayEnergized, "watchdog FORCED the relay ISOLATED without the mutex");
    expect(alarms.has("EMERGENCY_TRIP"), "EMERGENCY_TRIP alarm raised from the watchdog");
    expect(emg._state == EmgState::Run, "mutex-owned state NOT touched from outside the lock");
    // Mutex returns — reconcile into an honest latched trip
    emg.mutexAvailable = true;
    emg.tick(good, good, good);
    expect(emg._state == EmgState::Emergency, "reconciled to EMERGENCY");
    expect(emg._reason == EMG_REASON_INTERNAL, "reason = INTERNAL (honest, wire-additive)");
    expect(emg._trips == 1, "trip bookkeeping done exactly once");
    expect(emg._lockStarveEvents == 1, "one escalation counted");
  }

  printf("== E: transient contention (limit-1) does NOT escalate ==\n");
  {
    EmgMirror emg;
    emg._state = EmgState::Run;
    emg.relayEnergized = true;
    Core::Measurement good = mk(51.0f, Core::MeasurementQuality::Valid, Core::MeasurementSource::Measured);
    for (int round = 0; round < 50; round++) {
      for (int t = 0; t < EmgMirror::EMG_LOCK_STARVE_LIMIT - 1; t++) {
        emg.mutexAvailable = false;
        emg.tick(good, good, good);
      }
      emg.mutexAvailable = true;
      emg.tick(good, good, good);
    }
    expect(emg.relayEnergized, "relay stayed energized — sub-limit contention resets cleanly");
    expect(emg._lockStarveEvents == 0, "no watchdog events");
    expect(emg._state == EmgState::Run, "no trip");
  }

  printf("\n== HASIL: %s (%d kegagalan) ==\n", fails == 0 ? "SEMUA LULUS" : "ADA KEGAGALAN", fails);
  return fails;
}
