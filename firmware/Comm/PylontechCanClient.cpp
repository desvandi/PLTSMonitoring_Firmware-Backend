// =============================================================================
// Comm/PylontechCanClient.cpp — see header for the protocol map.
// -----------------------------------------------------------------------------
// Robustness notes (production hardening):
//   - Bus-off recovery: TWAI on a dead/unterminated bus eventually enters
//     bus-off; we detect it via twai_get_status_info and STOP + re-INSTALL
//     the driver (bounded: one recovery attempt per poll cycle).
//   - RX queue drained every tick so the 32-slot hardware queue cannot
//     overflow between manager polls.
//   - Timeout: a poll window that expires without BOTH 0x355+0x356 counts
//     as one error toward the manager's failure threshold.
// =============================================================================

#include "PylontechCanClient.h"

#if PLTS_ENABLE_PYLONTECH_CAN

#include "../Core/Config.h"
#include "../Core/Common.h"

namespace Comm {

// Poll window: Pylontech broadcasts the full frame set every ~1 s.
static constexpr uint32_t PYLON_CAN_WINDOW_MS = 1500;

bool PylontechCanClient::begin() {
  if (_driverInstalled) return true;

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      static_cast<gpio_num_t>(Core::PIN_CAN_TX),
      static_cast<gpio_num_t>(Core::PIN_CAN_RX),
      TWAI_MODE_NORMAL);
  g.rx_queue_len = 32;          // full frame set + margin
  g.tx_queue_len = 0;           // monitoring-only: we never transmit (brief §102)
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();  // modules differ per-ID

  if (twai_driver_install(&g, &t, &f) != ESP_OK) {
    return false;                // pins held / driver conflict — skip this slot
  }
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    return false;
  }
  _driverInstalled = true;
  _data.reset();
  return true;
}

void PylontechCanClient::end() {
  if (!_driverInstalled) return;
  twai_stop();
  twai_driver_uninstall();
  _driverInstalled = false;
  _armed = false;
}

bool PylontechCanClient::requestReading() {
  if (!_driverInstalled) return false;
  _got355 = false;
  _got356 = false;
  _armed = true;
  _windowStartMs = millis();
  return true;
}

bool PylontechCanClient::pollReading(uint32_t nowMs) {
  if (!_armed || !_driverInstalled) return false;
  bool completed = false;

  // --- Drain RX queue (non-blocking) ---
  twai_status_info_t st;
  if (twai_get_status_info(&st) == ESP_OK) {
    if (st.state == TWAI_STATE_BUS_OFF) {
      // Dead/unterminated bus: bounded recovery, count as error.
      twai_stop();
      twai_driver_uninstall();
      _driverInstalled = false;
      _data.errorCount++;
      _armed = false;
      return false;
    }
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {   // 0-tick timeout = drain
      if (!msg.extd && msg.data_length_code <= 8) {
        _handleFrame(msg.identifier, msg.data, msg.data_length_code, nowMs);
      }
    }
  }

  // --- Completion: both mandatory frames arrived ---
  if (_got355 && _got356) {
    _data.lastUpdateMs = nowMs;
    _data.frameCount++;
    completed = true;
    _armed = false;
  } else if (nowMs - _windowStartMs > PYLON_CAN_WINDOW_MS) {
    _data.errorCount++;         // incomplete frame set within window
    _armed = false;             // manager counts this as a failed attempt
  }
  return completed;
}

void PylontechCanClient::_handleFrame(uint32_t id, const uint8_t* d,
                                      uint8_t len, uint32_t nowMs) {
  switch (id) {
    case 0x351:
      if (len >= 6) {
        float ccl = decodeCcl(d);
        float dcl = decodeDcl(d);
        float cvl = decodeCvl(d);
        if (bmsCurrentPlausible(ccl))  _data.chargeCurrentLimit = ccl;
        if (bmsCurrentPlausible(dcl))  _data.dischargeCurrentLimit = dcl;
        if (bmsVoltPlausible(cvl))     { /* CVL informational only */ }
      }
      break;
    case 0x355:
      if (len >= 2) {
        float soc, soh;
        decodeSocSoh(d, &soc, &soh);
        if (bmsSocPlausible(soc)) { _data.soc = soc; _got355 = true; }
        if (bmsSohPlausible(soh))   _data.soh = soh;
      }
      break;
    case 0x356:
      if (len >= 6) {
        float v, i, t;
        decodeVit(d, &v, &i, &t);
        if (bmsVoltPlausible(v))    _data.voltage = v;
        if (bmsCurrentPlausible(i)) _data.current = i;
        if (bmsTempPlausible(t)) _data.temperature = t;
        _got356 = true;
      }
      break;
    case 0x359: {
      if (len < 8) break;
      float cells[4];
      decodeCells(d, cells);
      for (int k = 0; k < 4; k++) {
        if (!bmsCellVPlausible(cells[k])) continue;
        if (Core::isValidFloat(_data.cellVoltageMin)) {
          if (cells[k] < _data.cellVoltageMin) _data.cellVoltageMin = cells[k];
          if (cells[k] > _data.cellVoltageMax) _data.cellVoltageMax = cells[k];
        } else {
          _data.cellVoltageMin = cells[k];
          _data.cellVoltageMax = cells[k];
        }
        if (_data.cellCount < 255) _data.cellCount++;   // observed cell slots
      }
      break;
    }
    case 0x35A:
      if (len >= 1 && d[0] > 0 && d[0] < 64) _data.moduleCount = d[0];
      break;
    case 0x35E:
      if (len >= 4) {
        // bit0-15 alarms, bit16-31 faults → merged faultFlags (nonzero = fault)
        uint16_t alarms = (uint16_t)((uint16_t)d[0] << 8 | d[1]);
        uint16_t faults = (uint16_t)((uint16_t)d[2] << 8 | d[3]);
        _data.faultFlags = (uint16_t)((alarms & 0x00FF) | (faults & 0x00FF));
      }
      break;
    default:
      break;   // 0x35C per-module alarms etc. — intentionally not merged yet
  }
  (void)nowMs;
}

// ---------------------------------------------------------------------------
// Static decoders — pure functions, mirrored by scripts/test_bms_comm.py
// ---------------------------------------------------------------------------

float PylontechCanClient::decodeCvl(const uint8_t d[8]) {
  uint16_t raw = (uint16_t)((uint16_t)d[0] << 8 | d[1]);
  return (float)raw * 0.1f;
}

float PylontechCanClient::decodeCcl(const uint8_t d[8]) {
  uint16_t raw = (uint16_t)((uint16_t)d[2] << 8 | d[3]);
  return (float)raw * 0.1f;
}

float PylontechCanClient::decodeDcl(const uint8_t d[8]) {
  uint16_t raw = (uint16_t)((uint16_t)d[4] << 8 | d[5]);
  return (float)raw * 0.1f;
}

void PylontechCanClient::decodeSocSoh(const uint8_t d[8], float* soc, float* soh) {
  if (soc) *soc = (float)d[0];
  if (soh) *soh = (float)d[1];
}

void PylontechCanClient::decodeVit(const uint8_t d[8], float* v, float* i, float* t) {
  int16_t rawV = (int16_t)((uint16_t)d[0] << 8 | d[1]);
  int16_t rawI = (int16_t)((uint16_t)d[2] << 8 | d[3]);
  int16_t rawT = (int16_t)((uint16_t)d[4] << 8 | d[5]);
  if (v) *v = (float)rawV * 0.01f;
  if (i) *i = (float)rawI * 0.1f * Core::PYLONTECH_CAN_CURRENT_SIGN;
  if (t) *t = (float)rawT * 0.1f;
}

void PylontechCanClient::decodeCells(const uint8_t d[8], float out[4]) {
  for (int k = 0; k < 4; k++) {
    uint16_t mv = (uint16_t)((uint16_t)d[k * 2] << 8 | d[k * 2 + 1]);
    out[k] = (float)mv * 0.001f;
  }
}

} // namespace Comm

#endif // PLTS_ENABLE_PYLONTECH_CAN
