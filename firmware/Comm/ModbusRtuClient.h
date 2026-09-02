// =============================================================================
// Comm/ModbusRtuClient.h — Modbus RTU master over RS485 (UART2 + MAX3485)
// -----------------------------------------------------------------------------
// Transport layer (framing, CRC-16, T3.5 idle, DE/RE direction control,
// timeout, retries) implements the IEC 60870-5 / PI-MBUS-300 spec faithfully
// — this part is a hard standard and is fully trustworthy.
//
// REGISTER MAP — honest disclosure:
//   Unlike the transport, the *register map* of rack BMSes is NOT standardized
//   across vendors. The default map below (Comm/ModbusMap.h) models the
//   common "generic rack BMS" layout used by several 48 V LiFePO4 server-rack
//   packs. It MUST be verified against the battery's Modbus register document
//   for your specific model (bench checklist item G-04). Slave ID, sign
//   convention and scaling are field-configurable where possible.
//
//   requestReading() sends ONE function-code-03 window read covering the map;
//   pollReading() collects the response non-blocking (chunked Serial2 reads),
//   validates CRC + length + exception codes, then decodes registers.
// =============================================================================

#pragma once
#ifndef PLTS_COMM_MODBUS_RTU_CLIENT_H
#define PLTS_COMM_MODBUS_RTU_CLIENT_H

#include "BatteryProtocol.h"

#if PLTS_ENABLE_MODBUS_RTU

#include <HardwareSerial.h>

namespace Comm {

class ModbusRtuClient : public BatteryProtocolClient {
public:
  const char* name() const override { return "MODBUS_RTU"; }
  ProtocolId  id() const override   { return ProtocolId::ModbusRtu; }

  bool begin() override;
  void end() override;
  bool requestReading() override;
  bool pollReading(uint32_t nowMs) override;
  const BmsData& lastData() const override { return _data; }

  // Static pure helpers — mirrored by scripts/test_bms_comm.py.
  static uint16_t crc16(const uint8_t* buf, size_t len);        // Modbus CRC-16
  static void buildReadHolding(uint8_t slave, uint16_t startReg,
                               uint16_t count, uint8_t* out, size_t* outLen);
  // Decode one window (raw register values) into BmsData fields.
  static void decodeRegisters(const uint16_t* regs, size_t count, BmsData& out);

  // Wired with a 10 ms DE assert margin (spec: ≥1 char @9600..115200).
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 400;

private:
  void _flushRx();
  void _setDirection(bool transmit);

  BmsData    _data;
  uint8_t    _txBuf[16];
  size_t     _txLen = 0;
  uint8_t    _rxBuf[64];
  size_t     _rxLen = 0;
  uint32_t   _sentMs = 0;
  bool       _awaiting = false;
  bool       _started = false;
};

} // namespace Comm

#endif // PLTS_ENABLE_MODBUS_RTU
#endif // PLTS_COMM_MODBUS_RTU_CLIENT_H
