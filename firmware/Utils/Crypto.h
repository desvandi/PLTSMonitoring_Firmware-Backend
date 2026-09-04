// =============================================================================
// Utils/Crypto.h — SHA-256, HMAC-SHA256, PBKDF2, JWT (HS256), Ed25519 verify,
//                  base64url, constant-time compare, CSPRNG
// =============================================================================
#pragma once
#ifndef PLTS_UTILS_CRYPTO_H
#define PLTS_UTILS_CRYPTO_H

#include <Arduino.h>
#include <cstdint>
#include <stddef.h>

uint32_t getCurrUnixTime();
uint32_t getMonotonicSec();
namespace Utils {

// Constant-time memory compare (for tokens/hashes)
bool constantTimeMemEquals(const volatile uint8_t* a, const volatile uint8_t* b, size_t len);

// CSPRNG using esp_random()
void generateRandomBytes(uint8_t* buf, size_t len);

// Hex encoding/decoding
void bytesToHex(const uint8_t* in, size_t len, char* out);
bool hexToBytes(const char* hex, uint8_t* out, size_t outLen);

// SHA-256
bool sha256(const uint8_t* data, size_t len, uint8_t* outHash);  // 32 bytes
String sha256Hex(const String& data);

// HMAC-SHA256
bool hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t* outHash);  // 32 bytes

// PBKDF2-HMAC-SHA256 (RFC 8018) → 32-byte derived key
bool pbkdf2HmacSha256(const char* pass, size_t passLen,
                      const uint8_t* salt, size_t saltLen,
                      uint16_t iterations, uint8_t* outHash);

// base64url (no padding)
String base64urlEncode(const uint8_t* data, size_t len);
String base64urlEncode(const String& s);

// JWT (HS256)
String jwtSign(const String& username, const String& secret, uint32_t ttlSeconds);
bool jwtVerify(const String& token, const String& secret, String& outUsername);

// Random hex token (e.g. CSRF / factory reset)
String generateToken(size_t hexChars);

// [WAVE-6 / FW6-2] Wall-clock persistence across reboots (see Crypto.cpp):
// persistEpochEstimate() stores the current (NTP-synced) epoch as an
// estimated BOOT epoch in NVS; getCurrUnixTime() uses it as the fallback
// base when the system clock is unsynced — instead of the old fixed
// 1700000000 base that let pre-reboot JWTs live far past their exp.
void persistEpochEstimate();

// Ed25519 signature verification over a 32-byte SHA-256 hash.
// Fail-closed: returns false if MBEDTLS_ED25519_SUPPORTED is not defined
// in the build. (Default ESP32 framework lacks Ed25519 curve support.)
bool ed25519VerifyHash(const char* publicKeyHex,
                       const char* signatureHex,
                       const uint8_t* hashBytes, size_t hashLen);

uint32_t getCurrUnixTime();
uint32_t getMonotonicSec();
} // namespace Utils

#endif // PLTS_UTILS_CRYPTO_H
