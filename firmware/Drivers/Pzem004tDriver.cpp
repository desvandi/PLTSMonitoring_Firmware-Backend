// =============================================================================
// Drivers/Pzem004tDriver.cpp — PZEM-004T v3 AC power meter (OPTIONAL)
// -----------------------------------------------------------------------------
// See Pzem004tDriver.h for the protocol + honesty contract. Non-blocking
// request/response state machine (Acs712Driver style: no task of its own).
// =============================================================================
#include "Pzem004tDriver.h"

#if PLTS_ENABLE_PZEM_AC

#include <HardwareSerial.h>
#include <cmath>
#include "../Core/Common.h"
#include "../Core/Globals.h"
#include "../Services/LogService.h"

namespace Drivers {

Pzem004tDriver pzemAc;

// Plausibility gates — "physically possible on an Indonesian 230 V feed",
// deliberately WIDER than "normal" so real anomalies are REPORTED, never
// hidden (possible is not the same as safe — the emergency layer handles
// unsafe; this gate only rejects impossible).
static bool pzemVoltPlausible(float v)  { return Core::isValidFloat(v) && v >= 80.0f && v <= 270.0f; }
static bool pzemAmpPlausible(float a)   { return Core::isValidFloat(a) && a >= 0.0f && a <= 100.0f; }
static bool pzemWattPlausible(float p)  { return Core::isValidFloat(p) && p >= 0.0f && p <= 30000.0f; }
static bool pzemHzPlausible(float f)    { return Core::isValidFloat(f) && f >= 45.0f && f <= 65.0f; }
static bool pzemPfPlausible(float pf)   { return Core::isValidFloat(pf) && pf >= 0.0f && pf <= 1.0f; }

// Null the measurement fields (energyWh is a counter, not a measurement — kept).
static void pzemResetFields(PzemReading& r) {
  r.voltageV = NAN; r.currentA = NAN; r.powerW = NAN;
  r.frequencyHz = NAN; r.powerFactor = NAN;
}

bool Pzem004tDriver::begin() {
  if (_begun) return true;
  _begun = true;
  Serial1.begin(Core::PZEM_BAUD, SERIAL_8N1, Core::PIN_PZEM_RX, Core::PIN_PZEM_TX);
  Serial1.setTimeout(0);
  _resetReading();
  _available = false;   // presence unproven until the first valid frame
  Serial.printf("[PZEM] init: uart1 @%u 8N1 rx=%u tx=%u addr=0x%02X (awaiting first frame)\n",
                (unsigned)Core::PZEM_BAUD, Core::PIN_PZEM_RX, Core::PIN_PZEM_TX, _addr);
  Services::Log.append(Core::LogType::Info, "PZEM driver init — AC meter slot armed (bench-validation pending)");
  return true;
}

void Pzem004tDriver::tick() {
  uint32_t now = millis();

  if (!_awaiting) {
    if (now - _lastReqMs >= Core::PZEM_POLL_MS) {
      _sendRequest();
    }
    return;
  }
  _pollResponse(now);
}

void Pzem004tDriver::_sendRequest() {
  uint8_t frame[8];
  frame[0] = _addr;
  frame[1] = 0x03;                       // FC 03 read holding-style registers
  frame[2] = 0x00; frame[3] = 0x00;      // start register 0x0000
  frame[4] = 0x00; frame[5] = 0x0A;      // 10 registers (20 bytes)
  uint16_t crc = crc16(frame, 6);
  frame[6] = (uint8_t)(crc & 0xFF);      // PZEM: LOW byte first (Modbus order)
  frame[7] = (uint8_t)(crc >> 8);
  Serial1.write(frame, sizeof(frame));
  Serial1.flush();
  _awaiting = true;
  _awaitStartMs = millis();
  _bufLen = 0;
  _lastReqMs = _awaitStartMs;
}

bool Pzem004tDriver::_pollResponse(uint32_t nowMs) {
  // Collect bytes; complete at FRAME_LEN, abort at timeout.
  while (Serial1.available() > 0 && _bufLen < FRAME_LEN) {
    _buf[_bufLen++] = (uint8_t)Serial1.read();
  }
  if (_bufLen >= FRAME_LEN) {
    _awaiting = false;
    // Frame validation: address echo + function code + CRC.
    if (_buf[0] != _addr || _buf[1] != 0x03) {
      _reading.errorCount++;
      _resetReading();
      _reading.status = PzemStatus::CrcError;   // frame mismatch — treat as error
      return false;
    }
    uint16_t crcCalc = crc16(_buf, FRAME_LEN - 2);
    uint16_t crcRecv = (uint16_t)(_buf[FRAME_LEN - 2] | (_buf[FRAME_LEN - 1] << 8));
    if (crcCalc != crcRecv) {
      _reading.errorCount++;
      _resetReading();
      _reading.status = PzemStatus::CrcError;
      return false;
    }
    PzemReading r = decodeRegisters(&_buf[2], nowMs);
    r.errorCount = _reading.errorCount;          // keep the lifetime counter
    _reading = r;
    if (r.status == PzemStatus::Ok) {
      if (!_available) {
        _available = true;
        Services::Log.append(Core::LogType::Info, "PZEM AC meter DETECTED — AC power now MEASURED (was estimated)");
      }
    }
    return true;
  }
  if (nowMs - _awaitStartMs >= Core::PZEM_TIMEOUT_MS) {
    _awaiting = false;
    _reading.errorCount++;
    _resetReading();
    _reading.status = PzemStatus::Timeout;
    _available = false;    // meter stopped answering — presence retracted
  }
  return false;
}

PzemReading Pzem004tDriver::decodeRegisters(const uint8_t* d, uint32_t nowMs) {
  // PZEM-004T v3 register layout (public Peacefair protocol document):
  //   u16 voltage x0.1 V | u32 current x0.001 A | u32 power x0.1 W |
  //   u32 energy x1 Wh   | u16 frequency x0.1 Hz | u16 pf x0.01    | u16 alarm
  PzemReading r = {};
  r.timestampMs = nowMs;
  r.status = PzemStatus::Ok;

  uint16_t volt = (uint16_t)((uint16_t)d[0] << 8 | d[1]);
  uint32_t amp  = ((uint32_t)d[2] << 24) | ((uint32_t)d[3] << 16) |
                  ((uint32_t)d[4] << 8)  | (uint32_t)d[5];
  uint32_t watt = ((uint32_t)d[6] << 24) | ((uint32_t)d[7] << 16) |
                  ((uint32_t)d[8] << 8)  | (uint32_t)d[9];
  uint32_t ener = ((uint32_t)d[10] << 24) | ((uint32_t)d[11] << 16) |
                  ((uint32_t)d[12] << 8) | (uint32_t)d[13];
  uint16_t hz   = (uint16_t)((uint16_t)d[14] << 8 | d[15]);
  uint16_t pf   = (uint16_t)((uint16_t)d[16] << 8 | d[17]);
  uint16_t alrm = (uint16_t)((uint16_t)d[18] << 8 | d[19]);

  r.voltageV    = (float)volt * 0.1f;
  r.currentA    = (float)amp * 0.001f;
  r.powerW      = (float)watt * 0.1f;
  r.energyWh    = (float)ener;
  r.frequencyHz = (float)hz * 0.1f;
  r.powerFactor = (float)pf * 0.01f;
  r.alarmFlag   = alrm;

  // Plausibility — an implausible field nulls THE WHOLE reading (single
  // instrument, single verdict: partial trust in a power meter is a lie).
  if (!pzemVoltPlausible(r.voltageV) || !pzemAmpPlausible(r.currentA) ||
      !pzemWattPlausible(r.powerW)   || !pzemHzPlausible(r.frequencyHz) ||
      !pzemPfPlausible(r.powerFactor)) {
    pzemResetFields(r);
    r.status = PzemStatus::OutOfRange;
  }
  return r;
}

uint16_t Pzem004tDriver::crc16(const uint8_t* buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else              { crc >>= 1; }
    }
  }
  return crc;
}

void Pzem004tDriver::_resetReading() {
  pzemResetFields(_reading);
  _reading.status = PzemStatus::NotInitialized;
}

} // namespace Drivers

#endif // PLTS_ENABLE_PZEM_AC
