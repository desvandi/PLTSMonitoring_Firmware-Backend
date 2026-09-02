// =============================================================================
// Comm/Rs485Console.cpp — Pylontech RS485 vendor-frame CAPTURE console
// -----------------------------------------------------------------------------
// Strictly passive recorder for the shared RS485 port. See Rs485Console.h
// for the honesty contract (no transmit, no interpretation, no guessing).
// =============================================================================
#include "Rs485Console.h"

#if PLTS_ENABLE_RS485_CONSOLE

#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include "../Core/Globals.h"
#include "../Services/LogService.h"

namespace Comm {

Rs485Console rs485Console;

void Rs485Console::begin() {
  if (_begun) return;
  _begun = true;
  _active = (strcmp(Core::cfgBmsProtocol, PROTOCOL_ID) == 0);
  if (!_active) return;   // normal polling modes never touch this module

  // Own the UART2 exclusively (the manager built no client for this mode).
  Serial2.begin(115200, SERIAL_8N1, Core::PIN_RS485_RX, Core::PIN_RS485_TX);
  Serial2.setRxBufferSize(512);
  // DE+RE tied: LOW = permanent RECEIVE. This module NEVER transmits — the
  // vendor bus is never disturbed and no frame is ever guessed onto it.
  pinMode(Core::PIN_RS485_DE, OUTPUT);
  digitalWrite(Core::PIN_RS485_DE, LOW);
  _bufLen = 0;
  _lastByteMs = 0;
  _totalFrames = 0;
  _ringHead = 0;
  Serial.println("[RS485-CONSOLE] passive capture ACTIVE (protocol=rs485_console, DE=RX, 115200 8N1)");
  Services::Log.append(Core::LogType::Info,
      "RS485_CONSOLE_ACTIVE — passive vendor-frame capture started (no polling, no transmit)");
}

void Rs485Console::tick(uint32_t nowMs) {
  if (!_active) return;

  // Close the in-progress frame when the bus has been idle long enough.
  if (_bufLen > 0 && (nowMs - _lastByteMs) >= Core::RS485_FRAME_GAP_MS) {
    _closeFrame(nowMs);
  }

  int avail = Serial2.available();
  while (avail-- > 0) {
    int b = Serial2.read();
    if (b < 0) break;
    if (_bufLen == 0) {
      _lastByteMs = nowMs;          // first byte starts the frame clock
    } else if ((uint32_t)(nowMs - _lastByteMs) >= Core::RS485_FRAME_GAP_MS) {
      _closeFrame(nowMs);           // gap inside the read loop — boundary
    }
    if (_bufLen < Core::RS485_FRAME_MAX_BYTES) {
      _buf[_bufLen++] = (uint8_t)b;
    } else {
      _closeFrame(nowMs);           // cap reached — flush, keep receiving
      if (_bufLen < Core::RS485_FRAME_MAX_BYTES) _buf[_bufLen++] = (uint8_t)b;
    }
    _lastByteMs = nowMs;
  }
}

void Rs485Console::_closeFrame(uint32_t nowMs) {
  if (_bufLen == 0) return;
  (void)nowMs;
  Rs485Frame& f = _ring[_ringHead];
  f.tsMs = _lastByteMs;
  f.len = _bufLen;
  memcpy(f.bytes, _buf, _bufLen);
  _ringHead = (uint8_t)((_ringHead + 1) % Core::RS485_FRAME_RING);
  _totalFrames++;
  _logFrame();
  _bufLen = 0;
}

void Rs485Console::_logFrame() const {
  // Hex dump into the log ring (bounded to the frame cap; the REST endpoint
  // serves the full ring, the log line is the bench-side convenience copy).
  const Rs485Frame& f = _ring[(uint8_t)((_ringHead + Core::RS485_FRAME_RING - 1) % Core::RS485_FRAME_RING)];
  String hex;
  hex.reserve((size_t)f.len * 3);
  static const char* HEX_CHARS = "0123456789ABCDEF";
  for (uint16_t i = 0; i < f.len; i++) {
    hex += HEX_CHARS[(f.bytes[i] >> 4) & 0x0F];
    hex += HEX_CHARS[f.bytes[i] & 0x0F];
    hex += ' ';
  }
  Services::Log.append(Core::LogType::Info,
      String("RS485_FRAME len=") + f.len + " hex=" + hex);
}

String Rs485Console::framesJson() const {
  // Ring snapshot, oldest-first. Honest ordering: tsMs is the ESP32 uptime
  // millisecond of the frame's LAST byte (monotonic per boot only).
  StaticJsonDocument<3072> doc;
  doc["mode"] = PROTOCOL_ID;
  doc["active"] = _active;
  doc["totalFrames"] = _totalFrames;
  doc["interpretation"] = "NONE — raw vendor bytes, awaiting PylontechRs485 parser";
  JsonArray arr = doc.createNestedArray("frames");
  for (uint8_t i = 0; i < Core::RS485_FRAME_RING; i++) {
    uint8_t idx = (uint8_t)((_ringHead + i) % Core::RS485_FRAME_RING);
    const Rs485Frame& f = _ring[idx];
    if (f.len == 0) continue;          // never-filled slot (boot padding)
    JsonObject o = arr.createNestedObject();
    o["tsMs"] = f.tsMs;
    o["len"] = f.len;
    String hex;
    hex.reserve((size_t)f.len * 3);
    static const char* HEX_CHARS = "0123456789ABCDEF";
    for (uint16_t k = 0; k < f.len; k++) {
      hex += HEX_CHARS[(f.bytes[k] >> 4) & 0x0F];
      hex += HEX_CHARS[f.bytes[k] & 0x0F];
      if (k + 1 < f.len) hex += ' ';
    }
    o["hex"] = hex;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

} // namespace Comm

#endif // PLTS_ENABLE_RS485_CONSOLE
