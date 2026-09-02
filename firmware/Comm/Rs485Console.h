// =============================================================================
// Comm/Rs485Console.h — Pylontech RS485 vendor-frame CAPTURE console (bench)
// -----------------------------------------------------------------------------
// [README §13 limitation #2 closure — "menunggu capture frame vendor
// (jangan menebak)"]
//
// WHAT THIS IS: a strictly PASSIVE recorder for the shared RS485 port
// (UART2 + MAX3485). Pylontech battery modules in "console" mode push
// periodic data frames on the bus; this module segments them by inter-byte
// gap, time-stamps them, hex-dumps them to the log ring and serves the last
// N frames via GET /api/rs485/frames so an operator can capture real vendor
// traffic and hand the hex to the PylontechRs485 parser implementation.
//
// WHAT THIS DELIBERATELY IS NOT:
//   * It NEVER transmits (DE pinned LOW = permanent receive). The vendor bus
//     is never disturbed, and no frame is ever "guessed" — the reason the
//     PylontechRs485 client slot is still RESERVED.
//   * It interprets NOTHING. Bytes in, hex out. Frame boundaries come from
//     idle-gap segmentation only — a documented heuristic, visible in the
//     output (frames are labeled raw).
//
// ACTIVATION (bench-only, default off): set cfgBmsProtocol = "rs485_console"
// (NVS plts_batt/bmsProto). BatteryCommManager then builds NO polling client
// and stays Disabled — the UART is owned by this console exclusively, so
// passive capture and Modbus RTU polling never collide on the same wires.
// =============================================================================
#pragma once
#ifndef PLTS_COMM_RS485_CONSOLE_H
#define PLTS_COMM_RS485_CONSOLE_H

#if PLTS_ENABLE_RS485_CONSOLE

#include <Arduino.h>
#include <cstdint>
#include "../Core/Config.h"

namespace Comm {

// One captured raw frame (bounded).
struct Rs485Frame {
  uint32_t tsMs;          // millis() at first byte
  uint16_t len;           // byte count (cap = RS485_FRAME_MAX_BYTES)
  uint8_t  bytes[Core::RS485_FRAME_MAX_BYTES];
};

class Rs485Console {
public:
  Rs485Console() = default;

  // Idempotent. Self-gated: only touches the UART when
  // Core::cfgBmsProtocol == "rs485_console". Pins DE LOW (permanent receive).
  void begin();

  // 10 Hz pump from bmsCommTask. Non-blocking; reads whatever Serial2 has
  // buffered and closes a frame after RS485_FRAME_GAP_MS of bus idle.
  void tick(uint32_t nowMs);

  bool isActive() const { return _active; }
  uint16_t capturedFrames() const { return _totalFrames; }

  // REST payload for GET /api/rs485/frames (ring of last N frames).
  String framesJson() const;

  // Accepted protocol id string (kept adjacent to the gate that reads it).
  static constexpr const char* PROTOCOL_ID = "rs485_console";

private:
  void _closeFrame(uint32_t nowMs);
  void _logFrame() const;

  bool     _active = false;
  bool     _begun = false;
  uint8_t  _buf[Core::RS485_FRAME_MAX_BYTES];
  uint16_t _bufLen = 0;
  uint32_t _lastByteMs = 0;
  Rs485Frame _ring[Core::RS485_FRAME_RING];
  uint8_t  _ringHead = 0;      // next write slot
  uint16_t _totalFrames = 0;
};

extern Rs485Console rs485Console;

} // namespace Comm

#endif // PLTS_ENABLE_RS485_CONSOLE
#endif // PLTS_COMM_RS485_CONSOLE_H
