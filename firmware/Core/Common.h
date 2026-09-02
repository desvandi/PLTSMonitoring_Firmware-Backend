// =============================================================================
// Core/Common.h — Utility macros and shared inline helpers
// =============================================================================
#pragma once
#ifndef PLTS_CORE_COMMON_H
#define PLTS_CORE_COMMON_H

#include <Arduino.h>
#include <cmath>
#include "Types.h"

namespace Core {

// ---------------------------------------------------------------------------
// Numeric helpers
// ---------------------------------------------------------------------------
inline bool isValidFloat(float v) {
  return !std::isnan(v) && !std::isinf(v);
}

inline bool inRange(float v, float lo, float hi) {
  return isValidFloat(v) && v >= lo && v <= hi;
}

// Clamp without saturation misrepresentation
inline float clampf(float v, float lo, float hi) {
  if (!isValidFloat(v)) return NAN;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Safe divide — returns NAN on divide-by-zero (NEVER 0)
inline float safeDiv(float a, float b) {
  if (b == 0.0f || !isValidFloat(b)) return NAN;
  return a / b;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
inline uint32_t nowUnix() {
  extern uint32_t g_timeUnixEpoch;
  extern bool g_timeValid;
  if (g_timeValid) return g_timeUnixEpoch;
  return 0;
}

inline bool timeIsValid() {
  extern bool g_timeValid;
  return g_timeValid;
}

// Monotonic millis with overflow handling
inline uint32_t elapsedSince(uint32_t sinceMs, uint32_t nowMs) {
  if (nowMs >= sinceMs) return nowMs - sinceMs;
  return (0xFFFFFFFFu - sinceMs) + nowMs + 1;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
inline bool isBlank(const char* s) {
  return s == nullptr || s[0] == '\0';
}

// Constant-time string equality (for token compare)
inline bool constantTimeStrEq(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  volatile uint8_t diff = 0;
  size_t i = 0;
  while (a[i] != '\0' && b[i] != '\0') {
    diff |= (uint8_t)(a[i] ^ b[i]);
    i++;
  }
  diff |= (uint8_t)(a[i] ^ b[i]);  // both should be 0
  return diff == 0;
}

// ---------------------------------------------------------------------------
// CRC-16/CCITT (used by TelemetrySpool) — declared here for inline use
// ---------------------------------------------------------------------------
inline uint16_t crc16Ccitt(const uint8_t* data, size_t len, uint16_t init = 0xFFFF) {
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

// Auth + log types now defined in Core/Types.h (RC-11: single canonical owner)
// AuthAttempt struct and LogType enum are imported from there.

} // namespace Core

#endif // PLTS_CORE_COMMON_H
