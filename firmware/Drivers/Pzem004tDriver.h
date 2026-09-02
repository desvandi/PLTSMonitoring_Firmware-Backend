// =============================================================================
// Drivers/Pzem004tDriver.h — PZEM-004T v3 AC power meter (OPTIONAL upgrade)
// -----------------------------------------------------------------------------
// [README §13 limitation #9 closure path — "sensor daya AC nyata (PZEM)
// adalah upgrade yang disarankan"]
//
// Closes the AC-power-is-an-ESTIMATE gap with a REAL meter: voltage (0.1 V),
// current (0.001 A), active power (0.1 W), energy (1 Wh), frequency (0.1 Hz),
// power factor (0.01). The ACS712 220 V / PF 0.9 estimation path stays
// untouched as the fallback — the estimate is only REPLACED in reporting when
// this meter is connected and healthy (no silent fallback either way: the
// ac.meter block carries connected=false + null when absent).
//
// Protocol: PZEM-004T v3.0 (Peacefair) over isolated TTL UART, 9600 8N1 —
// Modbus-style frames with CRC16-MODBUS. Read input registers 0x0000..0x0009
// (FC 0x03): [addr, 0x03, 0x00, 0x00, 0x00, 0x0A, crcLo, crcHi] ->
// [addr, 0x03, 20 data bytes, crcLo, crcHi].
//
// STATUS: IMPLEMENTED, BENCH-VALIDATION PENDING — PLTS_ENABLE_PZEM_AC
// defaults to 0. The driver compiles out entirely until an operator has
// validated one physical unit against a reference meter (procedure in
// docs/bench/PANDUAN_VALIDASI.md). Enable only after that.
//
// Plausibility gates (same honesty as the BMS layer): the meter can lie or
// glitch — implausible values are never reported (status OutOfRange, value
// NaN): 80-270 V, 0-100 A, 0-30 kW, 45-65 Hz, PF 0.00-1.00.
// =============================================================================
#pragma once
#ifndef PLTS_DRIVERS_PZEM004T_DRIVER_H
#define PLTS_DRIVERS_PZEM004T_DRIVER_H

#if PLTS_ENABLE_PZEM_AC

#include <Arduino.h>
#include <cstdint>
#include "../Core/Config.h"

namespace Drivers {

enum class PzemStatus : uint8_t {
  Ok,               // fresh, CRC-valid, plausibility-passed reading
  NotInitialized,   // begin() not called / meter never answered
  Awaiting,         // request in flight
  Timeout,          // no (complete) response within PZEM_TIMEOUT_MS
  CrcError,         // response frame failed CRC16
  OutOfRange        // response decoded but values are implausible
};

struct PzemReading {
  float    voltageV;       // V     (NaN when invalid — NEVER 0)
  float    currentA;       // A     (NaN when invalid)
  float    powerW;         // W     (active power; NaN when invalid)
  float    energyWh;       // Wh    cumulative — RESETS ON METER POWER LOSS
  float    frequencyHz;    // Hz    (NaN when invalid)
  float    powerFactor;    // 0.00-1.00 (NaN when invalid)
  uint16_t alarmFlag;      // vendor alarm register (0x0002 = over-current)
  PzemStatus status;
  uint32_t timestampMs;    // millis() of the reading
  uint32_t errorCount;     // lifetime timeouts + CRC errors
};

class Pzem004tDriver {
public:
  Pzem004tDriver() = default;

  // Opens Serial1 @9600 8N1 on PIN_PZEM_RX/PIN_PZEM_TX. Always succeeds
  // (UART open says nothing about the meter being present — presence is
  // proven by the first valid response, isAvailable() stays false until then).
  bool begin();

  // Non-blocking state machine; call from a task (measurementTask, 5 Hz —
  // internally rate-gated to one request per PZEM_POLL_MS).
  void tick();

  bool isAvailable() const { return _available; }
  PzemReading getReading() const { return _reading; }

  // Shared decode (host-testable mirror target): 20 data bytes -> fields.
  static PzemReading decodeRegisters(const uint8_t* data20, uint32_t nowMs);
  // CRC16-MODBUS (poly 0xA001, init 0xFFFF, low byte first) — same algorithm
  // as the Modbus RTU client.
  static uint16_t crc16(const uint8_t* buf, size_t len);

  static constexpr size_t FRAME_LEN = 25;    // addr+fc+20+crc2

private:
  void _sendRequest();
  bool _pollResponse(uint32_t nowMs);
  void _resetReading();

  uint8_t  _addr = Core::PZEM_DEFAULT_ADDR;
  bool     _available = false;
  bool     _begun = false;
  bool     _awaiting = false;
  uint32_t _awaitStartMs = 0;
  uint32_t _lastReqMs = 0;
  uint8_t  _buf[FRAME_LEN];
  uint8_t  _bufLen = 0;
  PzemReading _reading = {};
};

extern Pzem004tDriver pzemAc;

} // namespace Drivers

#endif // PLTS_ENABLE_PZEM_AC
#endif // PLTS_DRIVERS_PZEM004T_DRIVER_H
