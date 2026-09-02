// =============================================================================
// Comm/ModbusTcpClient.cpp — see header.
// =============================================================================

#include "ModbusTcpClient.h"
#include "ModbusRtuClient.h"
#include "ModbusMap.h"

#if PLTS_ENABLE_MODBUS_TCP

#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Core/Globals.h"   // cfgBmsModbusTcpHost/Port/SlaveId runtime config
#include "../Services/WifiManager.h"
#include <cstring>

namespace Comm {

bool ModbusTcpClient::begin() {
  // Active only when a host is configured — otherwise the manager skips this
  // slot (honest: no host, no probing, no false "detected").
  return Core::cfgBmsModbusTcpHost[0] != '\0';
}

void ModbusTcpClient::end() {
  if (_client.connected()) _client.stop();
  _phase = Phase::Idle;
}

bool ModbusTcpClient::requestReading() {
  if (Core::cfgBmsModbusTcpHost[0] == '\0') return false;
  if (WiFi.status() != WL_CONNECTED) { _data.errorCount++; return false; }
  if (_client.connected()) _client.stop();      // fresh connection per poll
                                                               // (keep-alive is
                                                               // a later optimization)
  // [AUDIT v1.6.0] setTimeout() on ESP32 sets the CONNECT timeout in
  // MILLISECONDS and is honored by NetworkClient::connect(). 1 ms made every
  // connect fail instantly; 1500 ms bounds the blocking window well inside
  // the 10 s task WDT while still failing fast on dead hosts.
  _client.setTimeout(CONNECT_TIMEOUT_MS);
  if (!_client.connect(Core::cfgBmsModbusTcpHost, Core::cfgBmsModbusTcpPort)) {
    _phase = Phase::Idle;
    _data.errorCount++;
    return false;
  }
  _phase = Phase::Sent;                          // connect succeeded
  _phaseStartMs = millis();

  // MBAP: txId(2) proto(2)=0 len(2)=7 unit(1) + PDU: FC03 start(2) count(2)
  uint8_t pdu[5];
  pdu[0] = 0x03;
  pdu[1] = (uint8_t)(ModbusMap::REG_BASE >> 8);
  pdu[2] = (uint8_t)(ModbusMap::REG_BASE & 0xFF);
  pdu[3] = (uint8_t)(ModbusMap::REG_COUNT >> 8);
  pdu[4] = (uint8_t)(ModbusMap::REG_COUNT & 0xFF);
  uint8_t frame[12];
  _mbapSeq++;
  frame[0] = (uint8_t)(_mbapSeq >> 8); frame[1] = (uint8_t)_mbapSeq;
  frame[2] = 0; frame[3] = 0;
  frame[4] = 0; frame[5] = 6;                    // remaining length (unit+pdu)
  frame[6] = (uint8_t)Core::cfgBmsModbusSlaveId;
  memcpy(frame + 7, pdu, 5);
  _client.write(frame, sizeof(frame));

  _rxLen = 0;
  return true;
}

bool ModbusTcpClient::pollReading(uint32_t nowMs) {
  if (_phase != Phase::Sent) return false;

  // Expected: MBAP(7) + fc(1) + byteCount(1) + data(2*REG_COUNT)
  const size_t expect = 9 + 2 * ModbusMap::REG_COUNT;

  while (_client.available() && _rxLen < sizeof(_rxBuf)) {
    _rxBuf[_rxLen++] = (uint8_t)_client.read();
  }

  if (_rxLen >= expect) {
    _phase = Phase::Idle;
    _client.stop();
    // Validate MBAP + PDU
    if (_rxBuf[2] != 0 || _rxBuf[3] != 0) { _data.errorCount++; return false; }
    if (_rxBuf[6] != (uint8_t)Core::cfgBmsModbusSlaveId) { _data.errorCount++; return false; }
    if (_rxBuf[7] != 0x03 || (_rxBuf[7] & 0x80)) { _data.errorCount++; return false; }
    if (_rxBuf[8] != 2 * ModbusMap::REG_COUNT) { _data.errorCount++; return false; }

    uint16_t regs[ModbusMap::REG_COUNT];
    for (uint16_t k = 0; k < ModbusMap::REG_COUNT; k++) {
      regs[k] = (uint16_t)(((uint16_t)_rxBuf[9 + 2 * k] << 8) | _rxBuf[10 + 2 * k]);
    }
    ModbusRtuClient::decodeRegisters(regs, ModbusMap::REG_COUNT, _data);
    _data.lastUpdateMs = nowMs;
    _data.frameCount++;
    return true;
  }

  if (nowMs - _phaseStartMs > RESPONSE_TIMEOUT_MS) {
    _phase = Phase::Idle;
    _client.stop();
    _data.errorCount++;
  }
  return false;
}

} // namespace Comm

#endif // PLTS_ENABLE_MODBUS_TCP
