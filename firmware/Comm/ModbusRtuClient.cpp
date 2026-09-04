// =============================================================================
// Comm/ModbusRtuClient.cpp — see header. Standard Modbus RTU master.
// =============================================================================

#include "ModbusRtuClient.h"
#include "ModbusMap.h"

#if PLTS_ENABLE_MODBUS_RTU

#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Core/Globals.h"   // cfgBmsModbusSlaveId runtime config
#include <cstring>

namespace Comm {

bool ModbusRtuClient::begin() {
  if (_started) return true;
  // UART2: 115200 8N1 (most rack BMSes default; Pylontech console also 115200
  // so ONE RS485 transceiver serves both protocol slots).
  Serial2.begin(115200, SERIAL_8N1, Core::PIN_RS485_RX, Core::PIN_RS485_TX);
  pinMode(Core::PIN_RS485_DE, OUTPUT);
  digitalWrite(Core::PIN_RS485_DE, LOW);      // receive mode by default
  _started = true;
  _data.reset();
  return true;
}

void ModbusRtuClient::end() {
  if (!_started) return;
  Serial2.end();
  _started = false;
  _awaiting = false;
}

void ModbusRtuClient::_flushRx() {
  while (Serial2.available()) Serial2.read();
}

void ModbusRtuClient::_setDirection(bool transmit) {
  digitalWrite(Core::PIN_RS485_DE, transmit ? HIGH : LOW);
  if (transmit) delayMicroseconds(200);       // DE setup before first bit
}

bool ModbusRtuClient::requestReading() {
  if (!_started) return false;
  _flushRx();

  uint8_t slave = (uint8_t)Core::cfgBmsModbusSlaveId;
  buildReadHolding(slave, ModbusMap::REG_BASE, ModbusMap::REG_COUNT, _txBuf, &_txLen);

  _setDirection(true);
  Serial2.write(_txBuf, _txLen);
  Serial2.flush();                            // blocks only for 8 bytes @115200
  _setDirection(false);

  _rxLen = 0;
  _sentMs = millis();
  _awaiting = true;
  return true;
}

bool ModbusRtuClient::pollReading(uint32_t nowMs) {
  if (!_awaiting) return false;

  // Expected response: addr(1)+fc(1)+byteCount(1)+data(2*REG_COUNT)+crc(2)
  const size_t expect = 5 + 2 * ModbusMap::REG_COUNT;

  while (Serial2.available() && _rxLen < sizeof(_rxBuf)) {
    _rxBuf[_rxLen++] = (uint8_t)Serial2.read();
  }

  if (_rxLen >= expect) {
    _awaiting = false;
    // Validate address + function + no exception
    if (_rxBuf[0] != Core::cfgBmsModbusSlaveId || _rxBuf[1] != 0x03) {
      _data.errorCount++;
      return false;
    }
    if ((_rxBuf[1] & 0x80) != 0) {            // Modbus exception code
      _data.errorCount++;
      return false;
    }
    if (_rxBuf[2] != 2 * ModbusMap::REG_COUNT) {
      _data.errorCount++;
      return false;
    }
    // CRC over everything except the trailing CRC itself
    uint16_t crc = crc16(_rxBuf, expect - 2);
    uint16_t rxCrc = (uint16_t)(_rxBuf[expect - 2] | ((uint16_t)_rxBuf[expect - 1] << 8));
    if (crc != rxCrc) {
      _data.errorCount++;
      return false;
    }

    // Decode big-endian register values.
    uint16_t regs[ModbusMap::REG_COUNT];
    for (uint16_t k = 0; k < ModbusMap::REG_COUNT; k++) {
      regs[k] = (uint16_t)(((uint16_t)_rxBuf[3 + 2 * k] << 8) | _rxBuf[4 + 2 * k]);
    }
    decodeRegisters(regs, ModbusMap::REG_COUNT, _data);
    _data.lastUpdateMs = nowMs;
    _data.frameCount++;
    return true;
  }

  if (nowMs - _sentMs > RESPONSE_TIMEOUT_MS) {
    _awaiting = false;
    _data.errorCount++;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Static pure helpers — mirrored by scripts/test_bms_comm.py
// ---------------------------------------------------------------------------

uint16_t ModbusRtuClient::crc16(const uint8_t* buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else               { crc >>= 1; }
    }
  }
  return crc;
}

void ModbusRtuClient::buildReadHolding(uint8_t slave, uint16_t startReg,
                                       uint16_t count, uint8_t* out, size_t* outLen) {
  out[0] = slave;
  out[1] = 0x03;                              // function code 03
  out[2] = (uint8_t)(startReg >> 8);
  out[3] = (uint8_t)(startReg & 0xFF);
  out[4] = (uint8_t)(count >> 8);
  out[5] = (uint8_t)(count & 0xFF);
  uint16_t crc = crc16(out, 6);
  out[6] = (uint8_t)(crc & 0xFF);             // Modbus: LOW byte first
  out[7] = (uint8_t)(crc >> 8);
  *outLen = 8;
}

void ModbusRtuClient::decodeRegisters(const uint16_t* regs, size_t count, BmsData& out) {
  auto rdU16 = [&](uint16_t off) -> uint16_t {
    return (off < count) ? regs[off] : 0;
  };
  auto rdI16 = [&](uint16_t off) -> int16_t {
    return (int16_t)((off < count) ? regs[off] : 0);
  };

  float v   = (float)rdU16(ModbusMap::OFF_PACK_VOLTAGE) * ModbusMap::SCALE_VOLTAGE;
  float i   = (float)rdI16(ModbusMap::OFF_PACK_CURRENT) * ModbusMap::SCALE_CURRENT
              * Core::MODBUS_RACK_CURRENT_SIGN;
  float soc = (float)rdU16(ModbusMap::OFF_PACK_SOC);
  float soh = (float)rdU16(ModbusMap::OFF_PACK_SOH);
  float cMin = (float)rdU16(ModbusMap::OFF_CELL_MIN_MV) * ModbusMap::SCALE_CELL_MV;
  float cMax = (float)rdU16(ModbusMap::OFF_CELL_MAX_MV) * ModbusMap::SCALE_CELL_MV;
  float t   = (float)rdI16(ModbusMap::OFF_TEMP_X01C) * ModbusMap::SCALE_TEMPERATURE;
  float ccl = (float)rdU16(ModbusMap::OFF_CHG_LIMIT_A) * ModbusMap::SCALE_LIMIT;
  float dcl = (float)rdU16(ModbusMap::OFF_DIS_LIMIT_A) * ModbusMap::SCALE_LIMIT;

  // Plausibility-gated assignment — implausible fields stay NaN (honest).
  if (bmsVoltPlausible(v))    out.voltage = v;
  if (bmsCurrentPlausible(i)) out.current = i;
  if (bmsSocPlausible(soc))   out.soc = soc;
  if (bmsSohPlausible(soh))   out.soh = soh;
  if (bmsCellVPlausible(cMin)) out.cellVoltageMin = cMin;
  if (bmsCellVPlausible(cMax)) out.cellVoltageMax = cMax;
  if (bmsTempPlausible(t)) out.temperature = t;
  if (bmsCurrentPlausible(ccl)) out.chargeCurrentLimit = ccl;
  if (bmsCurrentPlausible(dcl)) out.dischargeCurrentLimit = dcl;
  out.cycleCount = rdU16(ModbusMap::OFF_CYCLE_COUNT);
  out.faultFlags = rdU16(ModbusMap::OFF_FAULT_FLAGS);
  uint16_t cells = rdU16(ModbusMap::OFF_CELL_COUNT);
  out.cellCount = (cells > 0 && cells < 256) ? cells : 0;
}

} // namespace Comm

#endif // PLTS_ENABLE_MODBUS_RTU
