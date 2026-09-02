// =============================================================================
// Utils/Crc.cpp — CRC-32 + CRC-16/CCITT + ArduinoJson helpers
// =============================================================================
#include "Crc.h"
#include <cstring>

namespace Utils {

uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t init) {
  uint16_t crc = init;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

static uint32_t s_crc32Table[256];
static bool     s_crc32Init = false;

static void initCrc32Table() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (uint8_t k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    s_crc32Table[i] = c;
  }
  s_crc32Init = true;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t init) {
  if (!s_crc32Init) initCrc32Table();
  uint32_t crc = init;
  for (size_t i = 0; i < len; i++) {
    crc = s_crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32String(const String& s) {
  return crc32((const uint8_t*)s.c_str(), s.length());
}

// ArduinoJson CRC helpers
// We serialize the doc (minus the _crc key) and compute CRC-32 of the JSON
// string. This is a simple integrity check against bit-rot, not a security
// mechanism. For tamper resistance, use HMAC-SHA256 (Crypto.h).
void appendCRC(JsonDocument& doc) {
  // Remove any existing _crc, serialize, compute CRC, re-add
  doc.remove("_crc");
  String s;
  serializeJson(doc, s);
  uint32_t c = crc32String(s);
  doc["_crc"] = (uint32_t)c;
}

bool verifyCRC(const JsonDocument& doc) {
  if (!doc.containsKey("_crc")) return false;
  uint32_t stored = doc["_crc"] | 0;
  // Make a shallow copy via serialization to drop _crc
  String s;
  serializeJson(doc, s);
  // Remove the trailing ,"\"_crc\":NULong}" before computing CRC
  int idx = s.lastIndexOf("\"_crc\":");
  if (idx < 0) return false;
  // Walk back to skip the comma before "_crc"
  int endIdx = s.length();
  // Find the closing brace after _crc value
  int closing = s.indexOf('}', idx);
  if (closing < 0) return false;
  // Substring is s[0 .. idx-2] (drop the trailing comma) + "}"
  String head;
  if (idx >= 2 && s[idx - 1] == ',') head = s.substring(0, idx - 1);
  else                                head = s.substring(0, idx);
  head += "}";
  uint32_t calc = crc32String(head);
  return stored == calc;
}

} // namespace Utils
