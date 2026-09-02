// =============================================================================
// Utils/Crc.h — CRC-32 (ArduinoJson integrity) + CRC-16/CCITT (spool records)
// =============================================================================
#pragma once
#ifndef PLTS_UTILS_CRC_H
#define PLTS_UTILS_CRC_H

#include <Arduino.h>
#include <cstdint>
#include <ArduinoJson.h>

namespace Utils {

// CRC-16/CCITT-FALSE (init 0xFFFF, poly 0x1021) — used by TelemetrySpool records
uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t init = 0xFFFF);

// CRC-32 (IEEE 802.3, init 0xFFFFFFFF, final XOR 0xFFFFFFFF, poly 0xEDB88320)
uint32_t crc32(const uint8_t* data, size_t len, uint32_t init = 0xFFFFFFFF);

// Compute + append CRC-32 to a JSON document under "_crc" key
void appendCRC(JsonDocument& doc);

// Verify "_crc" key matches recomputed CRC of the rest of the document
bool verifyCRC(const JsonDocument& doc);

// Convenience wrapper for crc32 over Arduino String
uint32_t crc32String(const String& s);

} // namespace Utils

#endif // PLTS_UTILS_CRC_H
