// =============================================================================
// Comm/BatteryProtocol.h — Canonical BMS/inverter protocol interface
// -----------------------------------------------------------------------------
// MULTI-PROTOCOL FEATURE (2026-08, user directive):
//   The ESP32 can talk to a battery BMS / inverter over several transports:
//     - Pylontech CAN       (TWAI controller + SN65HVD230 transceiver)
//     - Modbus RTU          (UART2 + MAX3485 RS485 transceiver)
//     - Modbus TCP          (WiFi client — ESP32 polls the peer)
//     - Pylontech RS485     (console protocol — RESERVED slot, see below)
//   I2C remains RESERVED for the internal sensor bus (INA219 DC shunt, SHT31)
//   and SPI is reserved for future W5500/MCP2515 expansion. ACS712 stays on
//   ADC1 GPIO35 (analog, NOT I2C — correcting the original idea's assumption).
//
// HONESTY CONTRACT (directive §1 "never fabricate certainty"):
//   - Every numeric field in BmsData is NaN when the protocol does not
//     provide it or the value failed plausibility validation. NEVER 0.
//   - `fresh` is true only when the last reading is within the poll window.
//   - Provenance: when BMS is LOCKED and fresh, SOC provenance = BMS_DIRECT;
//     otherwise the system falls back to SHUNT_COULOMB / OCV_ESTIMATED and
//     the transition is LOGGED (never silent).
//   - Auto-detection uses strict frame validation + 2-consecutive-success
//     hysteresis to lock and 3-consecutive-failure to declare LOST. A noisy
//     bus cannot flip the protocol selector on a single lucky CRC pass.
// =============================================================================

#pragma once
#ifndef PLTS_COMM_BATTERY_PROTOCOL_H
#define PLTS_COMM_BATTERY_PROTOCOL_H

#include <Arduino.h>
#include <cmath>
#include "../Core/Types.h"
#include "../Core/Common.h"
#include "../Core/Config.h"   // PLTS_ENABLE_* feature flags + pin map

namespace Comm {

// ---------------------------------------------------------------------------
// Protocol identity
// ---------------------------------------------------------------------------
enum class ProtocolId : uint8_t {
  None = 0,
  PylontechCan,     // Pylontech CAN 2.0A 500 kbps (public protocol doc)
  ModbusRtu,        // Modbus RTU master over RS485 (UART2 + MAX3485)
  ModbusTcp,        // Modbus TCP client over WiFi
  PylontechRs485,   // Pylontech RS485 console — RESERVED (see .cpp note)
  COUNT
};

inline const char* protocolIdToStr(ProtocolId p) {
  switch (p) {
    case ProtocolId::PylontechCan:   return "PYLONTECH_CAN";
    case ProtocolId::ModbusRtu:      return "MODBUS_RTU";
    case ProtocolId::ModbusTcp:      return "MODBUS_TCP";
    case ProtocolId::PylontechRs485: return "PYLONTECH_RS485";
    case ProtocolId::None:           return "NONE";
    case ProtocolId::COUNT:          break;
  }
  return "UNKNOWN";
}

// Parse a config string into a ProtocolId. "auto"/"none" handled by the
// manager (returns None here for unknown strings — config validation
// happens in ConfigHandlers BEFORE the value is stored).
inline ProtocolId protocolIdFromStr(const char* s) {
  if (!s || !*s) return ProtocolId::None;
  if (strcasecmp(s, "pylontech_can") == 0) return ProtocolId::PylontechCan;
  if (strcasecmp(s, "modbus_rtu") == 0)    return ProtocolId::ModbusRtu;
  if (strcasecmp(s, "modbus_tcp") == 0)    return ProtocolId::ModbusTcp;
  if (strcasecmp(s, "pylontech_rs485") == 0) return ProtocolId::PylontechRs485;
  return ProtocolId::None;
}

// ---------------------------------------------------------------------------
// Canonical BMS dataset (transport-agnostic)
//   NaN everywhere the protocol does not supply the value. The serializer
//   emits null for NaN — the PWA shows "N/A", never a fabricated 0.
//   Sign convention: current is POSITIVE = CHARGING (matches Core::Types
//   brief §5). Each client parser is responsible for sign normalization
//   (e.g. Pylontech CAN reports discharge-positive → parser negates).
// ---------------------------------------------------------------------------
struct BmsData {
  float    soc;                  // %  (0..100)
  float    soh;                  // %  (0..100)
  float    voltage;              // V   pack voltage
  float    current;              // A   +charging / -discharging
  float    power;                // W   NaN → derived by consumer as V×I
  float    temperature;          // °C  pack (or max cell) temperature
  float    cellVoltageMin;       // V
  float    cellVoltageMax;       // V
  uint16_t cellCount;            // cells seen (0 = not reported)
  float    chargeCurrentLimit;   // A   CCL — BMS-requested charge ceiling
  float    dischargeCurrentLimit;// A   DCL — BMS-requested discharge ceiling
  uint32_t cycleCount;           // equivalent full cycles
  uint16_t faultFlags;           // protocol-specific bitfield (see README)
  uint16_t moduleCount;          // modules in parallel (0 = not reported)
  uint32_t lastUpdateMs;         // millis() of the last VALID frame set
  uint32_t frameCount;           // valid frames decoded (diagnostics)
  uint32_t errorCount;           // CRC/timeout/parse errors (diagnostics)

  bool isFresh(uint32_t nowMs, uint32_t windowMs) const {
    return (lastUpdateMs != 0) && (nowMs - lastUpdateMs) < windowMs;
  }

  void reset() {
    soc = NAN; soh = NAN; voltage = NAN; current = NAN; power = NAN;
    temperature = NAN; cellVoltageMin = NAN; cellVoltageMax = NAN;
    cellCount = 0; chargeCurrentLimit = NAN; dischargeCurrentLimit = NAN;
    cycleCount = 0; faultFlags = 0; moduleCount = 0;
    lastUpdateMs = 0; frameCount = 0; errorCount = 0;
  }

  BmsData() { reset(); }
};

// ---------------------------------------------------------------------------
// BatteryProtocolClient — one transport/protocol implementation.
//   Lifecycle: begin() once → loop { requestReading() → pollReading() until
//   true or timeout } → lastData(). All methods are NON-BLOCKING beyond a few
//   ms; the manager calls them from a dedicated FreeRTOS task so probing can
//   never stall telemetry, spool or MQTT (HealthSupervisor WDT guards it).
// ---------------------------------------------------------------------------
class BatteryProtocolClient {
public:
  virtual ~BatteryProtocolClient() {}

  // Human-readable protocol name (matches protocolIdToStr of id()).
  virtual const char* name() const = 0;
  virtual ProtocolId id() const = 0;

  // Initialize hardware/interface. Returns false when the interface cannot
  // start (e.g. TWAI driver install fails, no TCP host configured) — the
  // manager then skips this client entirely. MUST be idempotent.
  virtual bool begin() = 0;

  // Release hardware (before OTA reboot).
  virtual void end() = 0;

  // Start one read cycle (send request / arm reception). Returns false when
  // the request could not even be placed on the wire.
  virtual bool requestReading() = 0;

  // Drive the read cycle. Returns true exactly once when new valid data has
  // been decoded into lastData(). Must also internally count timeouts/CRC
  // errors into lastData().errorCount.
  virtual bool pollReading(uint32_t nowMs) = 0;

  // Snapshot of the latest decoded data (thread-local to the manager task;
  // the manager copies under its own mutex).
  virtual const BmsData& lastData() const = 0;
};

// Shared plausibility validation (used by every parser — single source of
// truth so a transport cannot smuggle in an impossible value).
// 48 V / 15S LiFePO4 system bounds (Core::Config aligned).
inline bool bmsSocPlausible(float soc)   { return Core::isValidFloat(soc) && soc >= 0.0f && soc <= 100.0f; }
inline bool bmsSohPlausible(float soh)   { return Core::isValidFloat(soh) && soh >= 0.0f && soh <= 100.0f; }
inline bool bmsVoltPlausible(float v)    { return Core::isValidFloat(v) && v >= 10.0f && v <= 70.0f; }
inline bool bmsCurrentPlausible(float i) { return Core::isValidFloat(i) && i >= -1000.0f && i <= 1000.0f; }
inline bool bmsCellVPlausible(float v)   { return Core::isValidFloat(v) && v >= 1.5f && v <= 4.5f; }
// [2026-09 #4 closure] Temperature: -40..+100 C is the PHYSICALLY-POSSIBLE
// envelope (not the safe envelope — a genuinely overheating pack at 90 C is
// REAL data and must be reported, never nulled; plausibility rejects only
// impossible values like sensor garbage 3276.7 C or -55 C).
inline bool bmsTempPlausible(float t)    { return Core::isValidFloat(t) && t > -40.0f && t < 100.0f; }

} // namespace Comm

#endif // PLTS_COMM_BATTERY_PROTOCOL_H
