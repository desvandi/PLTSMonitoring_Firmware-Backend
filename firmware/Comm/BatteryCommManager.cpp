// =============================================================================
// Comm/BatteryCommManager.cpp — see header for the state machine contract.
// =============================================================================

#include "BatteryCommManager.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include "../Services/LogService.h"

#if PLTS_ENABLE_BMS_COMM

#include "PylontechCanClient.h"
#include "ModbusRtuClient.h"
#include "ModbusTcpClient.h"
#include <cstring>
#include <cmath>

namespace Comm {

BatteryCommManager batteryComm;

// Probe priority order (documented in README): CAN first (fully specified
// public protocol), then RS485 Modbus RTU, then Modbus TCP.
// NOTE: probe order is the *detection* priority, NOT a value judgement —
// CAN first because its frame set is fully standardized by Pylontech's
// public protocol document (lowest false-positive risk on a shared bench).
static const ProtocolId PROBE_ORDER[] = {
  ProtocolId::PylontechCan,
  ProtocolId::ModbusRtu,
  ProtocolId::ModbusTcp,
  ProtocolId::PylontechRs485,   // reserved slot — never instantiated today
};
static constexpr uint8_t PROBE_ORDER_LEN = sizeof(PROBE_ORDER) / sizeof(PROBE_ORDER[0]);

void BatteryCommManager::begin() {
  if (!_mutex) _mutex = xSemaphoreCreateMutex();
  _rebuildClients();
  const char* mode = Core::cfgBmsProtocol;
  if (strcmp(mode, "none") == 0) {
    _setState(State::Disabled);
  } else if (strcmp(mode, "rs485_console") == 0) {
    // v1.7.0 — bench capture mode: the shared RS485 port is owned exclusively
    // by Comm::rs485Console (passive, never transmits). No polling client is
    // built, no probing runs — capture and polling never collide on the bus.
    _setState(State::Disabled);
    _log("RS485_CONSOLE_MODE — passive capture, no BMS polling", ProtocolId::None);
  } else {
    _setState(State::Probing);
    _log("BMS_COMM_START", ProtocolId::None);
  }
}

void BatteryCommManager::end() {
  for (uint8_t i = 0; i < _clientCount; i++) {
    auto* c = reinterpret_cast<BatteryProtocolClient*>(_clients[i]);
    if (c) { c->end(); delete c; _clients[i] = nullptr; }
  }
  _clientCount = 0;
  _probing = nullptr;
  _state = State::Disabled;
}

void BatteryCommManager::reconfigure() {
  end();
  begin();
}

BatteryProtocolClient* BatteryCommManager::_clientFor(ProtocolId id) {
  for (uint8_t i = 0; i < _clientCount; i++) {
    auto* c = reinterpret_cast<BatteryProtocolClient*>(_clients[i]);
    if (c && c->id() == id) return c;
  }
  return nullptr;
}

void BatteryCommManager::_rebuildClients() {
  for (uint8_t i = 0; i < _clientCount; i++) {
    auto* c = reinterpret_cast<BatteryProtocolClient*>(_clients[i]);
    if (c) { c->end(); delete c; }
  }
  _clientCount = 0;
  memset(_clients, 0, sizeof(_clients));

  const char* mode = Core::cfgBmsProtocol;
  bool autoMode = (strcmp(mode, "auto") == 0);

#if PLTS_ENABLE_PYLONTECH_CAN
  if (autoMode || strcmp(mode, "pylontech_can") == 0) {
    auto* c = new PylontechCanClient();
    if (c->begin()) _clients[_clientCount++] = c;
    else delete c;
  }
#endif
#if PLTS_ENABLE_MODBUS_RTU
  if (autoMode || strcmp(mode, "modbus_rtu") == 0) {
    auto* c = new ModbusRtuClient();
    if (c->begin()) _clients[_clientCount++] = c;
    else delete c;
  }
#endif
#if PLTS_ENABLE_MODBUS_TCP
  if (autoMode || strcmp(mode, "modbus_tcp") == 0) {
    auto* c = new ModbusTcpClient();
    if (c->begin()) _clients[_clientCount++] = c;
    else delete c;
  }
#endif
  // PYLONTECH_RS485: reserved slot — implementation pending bench capture of
  // the vendor console frames (README §RS485-console). NOT silently guessed.
}

void BatteryCommManager::_setState(State s) {
  _state = s;
}

void BatteryCommManager::_log(const char* event, ProtocolId p) {
  char msg[80];
  snprintf(msg, sizeof(msg), "%s proto=%s state=%s", event,
           protocolIdToStr(p), stateStr());
  Services::Log.append(Core::LogType::Info, String(msg));
}

const char* BatteryCommManager::stateStr() const {
  switch (_state) {
    case State::Disabled:  return "DISABLED";
    case State::Probing:   return "PROBING";
    case State::Locked:    return "LOCKED";
    case State::Lost:      return "LOST";
    case State::IdleNoBms: return "IDLE_NO_BMS";
  }
  return "UNKNOWN";
}

BatteryCommManager::State BatteryCommManager::getState() const {
  // Single-writer (bmsTask) enum read is benign on 32-bit ESP32; mutex only
  // for the data snapshot getters where torn structs would matter.
  return _state;
}
ProtocolId BatteryCommManager::activeProtocol() const { return _active; }
const char* BatteryCommManager::activeProtocolStr() const {
  return protocolIdToStr(_active);
}
bool BatteryCommManager::isLocked() const { return _state == State::Locked; }
uint32_t BatteryCommManager::getLockMs() const { return _lockMs; }
uint32_t BatteryCommManager::getProbeCycleCount() const { return _probeCycle; }

BmsData BatteryCommManager::getData() const {
  BmsData copy;
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    copy = _data;
    xSemaphoreGive(_mutex);
  }
  return copy;
}

bool BatteryCommManager::socAuthoritative() const {
  // Reads _state/_data without mutex: single-writer semantics make this
  // benign (worst case one poll-cycle lag). The canonical data path for
  // consumers is getData() (mutex-copied).
  return _state == State::Locked &&
         bmsSocPlausible(_data.soc) &&
         _data.isFresh(millis(), Core::cfgBmsPollIntervalMs * 2 + 2000);
}

void BatteryCommManager::tick(uint32_t nowMs) {
  if (!_mutex) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

  switch (_state) {
    case State::Disabled:
      break;

    case State::Probing: {
      if (!_probing) {
        // advance to next enabled client in probe order
        while (_probeIdx < PROBE_ORDER_LEN) {
          ProtocolId want = PROBE_ORDER[_probeIdx];
          BatteryProtocolClient* c = _clientFor(want);
          _probeIdx++;
          if (c) { _probing = c; _probeAttempts = 0; break; }
        }
        if (!_probing) {
          // nobody answered this cycle
          if (_probeCycle == 0) {
            _log("BMS_NOT_DETECTED", ProtocolId::None);
          }
          _setState(State::IdleNoBms);
          _lostSinceMs = nowMs;   // anchors the periodic re-probe timer
          _probeCycle++;
          break;
        }
      }
      if (!_probeWaiting) {
        if (_probing->requestReading()) {
          _probeWaiting = true;
          _probeStartMs = nowMs;
        } else {
          _probeAttempts++;
          if (_probeAttempts >= PROBE_ATTEMPTS_PER_CLIENT) {
            _probing = nullptr;   // next tick advances
          }
        }
        break;
      }
      bool got = _probing->pollReading(nowMs);
      if (got) {
        _lockSuccesses++;
        if (_lockSuccesses >= LOCK_SUCCESSES_REQUIRED) {
          _active = _probing->id();
          _data = _probing->lastData();
          _lockMs = nowMs;
          _setState(State::Locked);
          _pollFailures = 0;
          _pollAwaiting = false;
          _probing = nullptr;
          _lockSuccesses = 0;
          _probeIdx = 0;
          _lostSinceMs = nowMs;   // IdleNoBms/Lost timer reuse
          _log("BMS_PROTOCOL_LOCKED", _active);
          break;
        }
        _probeWaiting = false;   // success #1 — immediately try again
      } else if (nowMs - _probeStartMs > PROBE_RESPONSE_WINDOW_MS) {
        _probeWaiting = false;
        _probeAttempts++;
        _lockSuccesses = 0;      // hysteresis: must be consecutive
        if (_probeAttempts >= PROBE_ATTEMPTS_PER_CLIENT) {
          _probing = nullptr;
        }
      }
      break;
    }

    case State::Locked: {
      auto* c = _clientFor(_active);
      if (!c) { _setState(State::Lost); _lostSinceMs = nowMs; break; }
      uint32_t interval = Core::cfgBmsPollIntervalMs ? Core::cfgBmsPollIntervalMs : 5000;
      if (!_pollAwaiting && (nowMs - _lastPollMs) >= interval) {
        if (c->requestReading()) {
          _pollAwaiting = true;
          _pollSentMs = nowMs;
        } else {
          _pollFailures++;
        }
        _lastPollMs = nowMs;
      }
      if (_pollAwaiting) {
        if (c->pollReading(nowMs)) {
          _data = c->lastData();
          _pollAwaiting = false;
          _pollFailures = 0;
        } else if (nowMs - _pollSentMs > 3 * PROBE_RESPONSE_WINDOW_MS) {
          _pollAwaiting = false;
          _pollFailures++;
        }
      }
      if (_pollFailures >= LOST_FAILURES_REQUIRED) {
        _setState(State::Lost);
        _lostSinceMs = nowMs;
        _log("BMS_PROTOCOL_LOST", _active);
        // keep last _data for staleness marking; consumers see isFresh=false
      }
      break;
    }

    case State::Lost: {
      if (nowMs - _lostSinceMs >= REPROBE_COOLDOWN_MS) {
        _setState(State::Probing);
        _probeIdx = 0;
        _probing = nullptr;
        _lockSuccesses = 0;
        _probeAttempts = 0;
        _active = ProtocolId::None;
        _data.reset();
      }
      break;
    }

    case State::IdleNoBms: {
      // Periodic re-probe (quiet bench → BMS attached later without reboot).
      if (nowMs - _lostSinceMs >= REPROBE_COOLDOWN_MS) {
        _lostSinceMs = nowMs;
        _setState(State::Probing);
        _probeIdx = 0;
        _probing = nullptr;
        _lockSuccesses = 0;
        _probeAttempts = 0;
      }
      break;
    }
  }

  xSemaphoreGive(_mutex);
}

float BatteryCommManager::crossCheckShunt(float shuntCurrentA, uint32_t nowMs) {
  (void)nowMs;
  float bmsI = _data.current;
  if (!_mutex) return NAN;
  if (!Core::isValidFloat(shuntCurrentA) || !bmsCurrentPlausible(bmsI)) {
    _mismatchStreak = 0;
    _lastMismatchA = NAN;
    _mismatchActive = false;
    return NAN;
  }
  float delta = fabsf(bmsI - shuntCurrentA);
  float threshold = fmaxf(0.5f, 0.05f * fabsf(shuntCurrentA));
  if (delta > threshold) {
    if (_mismatchStreak < MISMATCH_SUSTAIN_POLLS) _mismatchStreak++;
    if (_mismatchStreak >= MISMATCH_SUSTAIN_POLLS && !_mismatchActive) {
      _mismatchActive = true;
      // Alarm raised by energyTask (which owns AlarmRegistry pacing) — the
      // manager only computes; the task decides (single alarm owner rule).
    }
  } else {
    _mismatchStreak = 0;
    _mismatchActive = false;
  }
  _lastMismatchA = delta;
  return delta;
}

} // namespace Comm

#else  // PLTS_ENABLE_BMS_COMM == 0 — whole feature compiled out

namespace Comm {
BatteryCommManager batteryComm;

// ---- No-op stubs: whole feature compiled out (PLTS_ENABLE_BMS_COMM=0).
// Every header method keeps a valid definition so callers need no #ifdef.
void BatteryCommManager::begin() {}
void BatteryCommManager::end() {}
void BatteryCommManager::tick(uint32_t) {}
BatteryCommManager::State BatteryCommManager::getState() const { return State::Disabled; }
const char* BatteryCommManager::stateStr() const { return "DISABLED"; }
ProtocolId BatteryCommManager::activeProtocol() const { return ProtocolId::None; }
const char* BatteryCommManager::activeProtocolStr() const { return "NONE"; }
bool BatteryCommManager::isLocked() const { return false; }
BmsData BatteryCommManager::getData() const { return BmsData(); }
uint32_t BatteryCommManager::getLockMs() const { return 0; }
uint32_t BatteryCommManager::getProbeCycleCount() const { return 0; }
float BatteryCommManager::crossCheckShunt(float, uint32_t) { return NAN; }
bool BatteryCommManager::socAuthoritative() const { return false; }
void BatteryCommManager::reconfigure() {}
void BatteryCommManager::_rebuildClients() {}
BatteryProtocolClient* BatteryCommManager::_clientFor(ProtocolId) { return nullptr; }
void BatteryCommManager::_setState(State) {}
void BatteryCommManager::_log(const char*, ProtocolId) {}

} // namespace Comm

#endif // PLTS_ENABLE_BMS_COMM
