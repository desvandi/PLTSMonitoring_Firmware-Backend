// =============================================================================
// Comm/ModbusMap.h — Default Modbus register map ("generic rack BMS")
// -----------------------------------------------------------------------------
// HONEST DISCLOSURE (production honesty contract):
//   Modbus register maps are VENDOR-SPECIFIC. This default map follows the
//   layout used by a family of 48 V LiFePO4 rack BMSes, but you MUST verify
//   it against YOUR battery's register document (bench checklist G-04).
//   If your map differs, adjust these constants and rebuild — the transport
//   layer (CRC/framing/timing) is standard and needs no changes.
//
//   All registers are HOLDING registers (function code 03), big-endian.
//   Sign convention: MODBUS_RACK_CURRENT_SIGN applies the vendor convention
//   (most rack BMSes report discharge-positive) → negated to canonical
//   +charge. VERIFY on bench (G-05) — a wrong sign silently inverts every
//   energy/label. The system cross-checks BMS current against the INA219
//   shunt and raises BMS_CURRENT_MISMATCH when they diverge, which catches
//   a wrong sign within one poll cycle.
// =============================================================================

#pragma once
#ifndef PLTS_COMM_MODBUS_MAP_H
#define PLTS_COMM_MODBUS_MAP_H

#include <cstdint>
#include "../Core/Config.h"

namespace Comm {
namespace ModbusMap {

// Window read: one FC03 transaction covering the whole map.
constexpr uint16_t REG_BASE          = 100;   // first register of the window
constexpr uint16_t REG_COUNT         = 13;    // registers 100..112

// Register offsets (add to REG_BASE).
constexpr uint16_t OFF_PACK_VOLTAGE   = 0;    // ×0.01 V  u16
constexpr uint16_t OFF_PACK_CURRENT   = 1;    // ×0.1 A   i16 (sign via constant)
constexpr uint16_t OFF_PACK_SOC       = 2;    // ×1 %     u16
constexpr uint16_t OFF_PACK_SOH       = 3;    // ×1 %     u16
constexpr uint16_t OFF_CELL_MIN_MV    = 4;    // ×1 mV    u16
constexpr uint16_t OFF_CELL_MAX_MV    = 5;    // ×1 mV    u16
constexpr uint16_t OFF_TEMP_X01C      = 6;    // ×0.1 °C  i16
constexpr uint16_t OFF_RESERVED_7     = 7;
constexpr uint16_t OFF_CHG_LIMIT_A    = 8;    // ×0.1 A   u16 (CCL)
constexpr uint16_t OFF_DIS_LIMIT_A    = 9;    // ×0.1 A   u16 (DCL)
constexpr uint16_t OFF_CYCLE_COUNT    = 10;   // ×1       u16
constexpr uint16_t OFF_FAULT_FLAGS    = 11;   // bitfield u16 (vendor-specific)
constexpr uint16_t OFF_CELL_COUNT     = 12;   // ×1       u16

// Scaling factors.
constexpr float    SCALE_VOLTAGE      = 0.01f;
constexpr float    SCALE_CURRENT      = 0.1f;
constexpr float    SCALE_TEMPERATURE  = 0.1f;
constexpr float    SCALE_LIMIT        = 0.1f;
constexpr float    SCALE_CELL_MV      = 0.001f;

} // namespace ModbusMap
} // namespace Comm

#endif // PLTS_COMM_MODBUS_MAP_H
