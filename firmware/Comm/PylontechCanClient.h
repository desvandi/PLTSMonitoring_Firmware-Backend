// =============================================================================
// Comm/PylontechCanClient.h — Pylontech battery, CAN 2.0A @ 500 kbps (TWAI)
// -----------------------------------------------------------------------------
// Implements the Pylontech "Battery Communication Protocol" CAN layer used by
// US2000/US3000/Force/H48050-style packs and Pylontech-compatible clones
// (many LiFePO4 server-rack batteries speak this exact map — supporting it
// gives wide multi-vendor coverage with one parser).
//
// CAN map (standard 11-bit IDs, BIG-ENDIAN multi-byte fields):
//   0x351  6 B  charge voltage limit  ×0.1 V (u16)
//              charge current limit   ×0.1 A (u16)
//              discharge current lim. ×0.1 A (u16)
//   0x355  6 B  SOC (u8 %), SOH (u8 %)
//   0x356  8 B  pack voltage ×0.01 V (i16)
//              pack current ×0.1 A  (i16)  — POSITIVE = DISCHARGE (Pylontech
//              convention) → parser NEGATES to canonical +charge
//              pack temperature ×0.1 °C (i16)
//   0x359  8 B  4 × cell voltage ×1 mV (u16) — one frame per cell group;
//              parallel modules repeat the ID; min/max tracked across all
//   0x35A  6 B  module count / online module count (per protocol doc)
//   0x35E  8 B  bit0-15 pack alarm flags, bit16-31 pack fault flags
//              (bit layout documented in README §BMS-fault-flags)
//
// Completion rule (frame-set semantics, NOT per-frame): the BMS cycles
// 0x351→0x35E continuously every ~1 s. This client declares a reading
// COMPLETE when a new 0x356 (V/I/T) AND 0x355 (SOC) have both been received
// within the poll window — voltage/current without SOC (or vice versa) is
// held as partial data and reported NOT fresh.
//
// Compile-time switch: PLTS_ENABLE_PYLONTECH_CAN (platformio.ini). When 0 the
// entire implementation compiles out and auto-detect skips the slot.
// =============================================================================

#pragma once
#ifndef PLTS_COMM_PYLONTECH_CAN_CLIENT_H
#define PLTS_COMM_PYLONTECH_CAN_CLIENT_H

#include "BatteryProtocol.h"

#if PLTS_ENABLE_PYLONTECH_CAN

#include <driver/twai.h>

namespace Comm {

class PylontechCanClient : public BatteryProtocolClient {
public:
  const char* name() const override { return "PYLONTECH_CAN"; }
  ProtocolId  id() const override   { return ProtocolId::PylontechCan; }

  bool begin() override;
  void end() override;
  bool requestReading() override;     // TWAI is push-based: arms window
  bool pollReading(uint32_t nowMs) override;
  const BmsData& lastData() const override { return _data; }

  // Exposed for the Python mirror test (scripts/test_bms_comm.py parses the
  // same semantics). Static pure decoders keep parsing testable and free of
  // hardware state.
  static float decodeCcl(const uint8_t d[8]);   // 0x351 charge current limit (A)
  static float decodeDcl(const uint8_t d[8]);   // 0x351 discharge current limit (A)
  static float decodeCvl(const uint8_t d[8]);   // 0x351 charge voltage limit (V)
  static void  decodeSocSoh(const uint8_t d[8], float* soc, float* soh); // 0x355
  static void  decodeVit(const uint8_t d[8], float* v, float* i, float* t); // 0x356
  static void  decodeCells(const uint8_t d[8], float out[4]);             // 0x359

private:
  void _handleFrame(uint32_t id, const uint8_t* d, uint8_t len, uint32_t nowMs);

  BmsData    _data;
  bool       _armed = false;
  uint32_t   _windowStartMs = 0;
  bool       _got355 = false;   // SOC/SOH seen in current cycle
  bool       _got356 = false;   // V/I/T seen in current cycle
  bool       _driverInstalled = false;
  uint32_t   _lastFrameMs = 0;
};

} // namespace Comm

#endif // PLTS_ENABLE_PYLONTECH_CAN
#endif // PLTS_COMM_PYLONTECH_CAN_CLIENT_H
