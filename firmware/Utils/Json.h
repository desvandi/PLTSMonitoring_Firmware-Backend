// =============================================================================
// Utils/Json.h — NaN-safe JSON helpers (setOptionalFloat emits null for NaN/Inf)
// =============================================================================
#pragma once
#ifndef PLTS_UTILS_JSON_H
#define PLTS_UTILS_JSON_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cmath>
#include "../Core/Common.h"
#include "../Core/Types.h"

namespace Utils {

using Core::isValidFloat;

// Emit value as JSON number if valid, otherwise null. NaN/Inf → null.
// Per brief §36 / §83: NEVER silent zero substitution.
inline void setOptionalFloat(JsonVariant v, float value) {
  if (!isValidFloat(value)) v.set(nullptr);
  else                       v.set(value);
}

inline void setMeasurement(JsonObject obj, const char* key,
                           const Core::Measurement& m) {
  JsonObject sub = obj.createNestedObject(key);
  if (isValidFloat(m.value)) sub["value"].set(m.value);
  else                        sub["value"].set(nullptr);
  sub["quality"]   = Core::qualityToStr(m.quality);
  sub["source"]    = Core::sourceToStr(m.source);
  sub["timestamp"]  = m.timestamp;
  sub["sequence"]  = m.sequence;
}

// Parse "HH:MM" → minutes since midnight. Returns false on invalid format.
inline bool parseMinutes(const char* s, uint16_t& outMinutes) {
  if (!s || strlen(s) != 5) return false;
  if (s[2] != ':') return false;
  int h = (s[0] - '0') * 10 + (s[1] - '0');
  int m = (s[3] - '0') * 10 + (s[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  outMinutes = (uint16_t)(h * 60 + m);
  return true;
}

} // namespace Utils

#endif // PLTS_UTILS_JSON_H
