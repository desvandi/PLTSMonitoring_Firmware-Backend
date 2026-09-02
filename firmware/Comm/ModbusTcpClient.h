// =============================================================================
// Comm/ModbusTcpClient.h — Modbus TCP client over WiFi (MBAP + FC03)
// -----------------------------------------------------------------------------
// The ESP32 acts as a Modbus TCP CLIENT polling the BMS/inverter gateway
// (e.g., a battery with Ethernet/WiFi bridge, or an inverter's Modbus TCP
// server). We deliberately NEVER open a listening Modbus port — Modbus TCP
// has no authentication, so the device must not be remotely writable.
//
// Non-blocking connect/send/receive state machine; only active when
// cfgBmsModbusTcpHost is non-empty. Shares the register map + decoders with
// the RTU client (Comm/ModbusMap.h, decodeRegisters) so wire-verified RTU
// behavior carries over.
// =============================================================================

#pragma once
#ifndef PLTS_COMM_MODBUS_TCP_CLIENT_H
#define PLTS_COMM_MODBUS_TCP_CLIENT_H

#include "BatteryProtocol.h"

#if PLTS_ENABLE_MODBUS_TCP

#include <WiFiClient.h>

namespace Comm {

class ModbusTcpClient : public BatteryProtocolClient {
public:
  const char* name() const override { return "MODBUS_TCP"; }
  ProtocolId  id() const override   { return ProtocolId::ModbusTcp; }

  bool begin() override;
  void end() override;
  bool requestReading() override;
  bool pollReading(uint32_t nowMs) override;
  const BmsData& lastData() const override { return _data; }

  static constexpr uint32_t CONNECT_TIMEOUT_MS = 1500;
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 800;

private:
  enum class Phase : uint8_t { Idle, Connecting, Sent, Done };

  BmsData    _data;
  WiFiClient _client;
  Phase      _phase = Phase::Idle;
  uint32_t   _phaseStartMs = 0;
  uint16_t   _mbapSeq = 0;
  uint8_t    _rxBuf[64];
  size_t     _rxLen = 0;
};

} // namespace Comm

#endif // PLTS_ENABLE_MODBUS_TCP
#endif // PLTS_COMM_MODBUS_TCP_CLIENT_H
