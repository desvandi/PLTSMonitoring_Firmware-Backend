// =============================================================================
// Utils/Crypto.cpp — SHA-256, HMAC, PBKDF2, JWT, Ed25519 verify, base64url
// =============================================================================
#include "Crypto.h"
#include <ctime>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace Utils {

bool constantTimeMemEquals(const volatile uint8_t* a, const volatile uint8_t* b, size_t len) {
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

void generateRandomBytes(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(esp_random() & 0xFF);
}

void bytesToHex(const uint8_t* in, size_t len, char* out) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2]     = hex[(in[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[in[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

bool hexToBytes(const char* hex, uint8_t* out, size_t outLen) {
  if (!hex) return false;
  size_t hexLen = strlen(hex);
  if (hexLen != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    char hi = hex[i * 2], lo = hex[i * 2 + 1];
    uint8_t b = 0;
    if (hi >= '0' && hi <= '9')      b = (hi - '0') << 4;
    else if (hi >= 'a' && hi <= 'f') b = (hi - 'a' + 10) << 4;
    else if (hi >= 'A' && hi <= 'F') b = (hi - 'A' + 10) << 4;
    else return false;
    if (lo >= '0' && lo <= '9')      b |= (lo - '0');
    else if (lo >= 'a' && lo <= 'f') b |= (lo - 'a' + 10);
    else if (lo >= 'A' && lo <= 'F') b |= (lo - 'A' + 10);
    else return false;
    out[i] = b;
  }
  return true;
}

bool sha256(const uint8_t* data, size_t len, uint8_t* outHash) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_setup(&ctx, info, 0) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_starts(&ctx) != 0)         { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_update(&ctx, data, len) != 0) { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_finish(&ctx, outHash) != 0)   { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);
  return true;
}

String sha256Hex(const String& data) {
  uint8_t hash[32];
  if (!sha256((const uint8_t*)data.c_str(), data.length(), hash)) return String();
  char buf[65];
  bytesToHex(hash, 32, buf);
  return String(buf);
}

bool hmacSha256(const uint8_t* key, size_t keyLen,
                const uint8_t* msg, size_t msgLen,
                uint8_t* outHash) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0)            { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_starts(&ctx, key, keyLen) != 0)  { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_update(&ctx, msg, msgLen) != 0)   { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_finish(&ctx, outHash) != 0)      { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);
  return true;
}

bool pbkdf2HmacSha256(const char* pass, size_t passLen,
                      const uint8_t* salt, size_t saltLen,
                      uint16_t iterations, uint8_t* outHash) {
  if (!pass || !salt || !outHash || iterations == 0) return false;
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;

  uint8_t saltBlock[64 + 4];
  if (saltLen > sizeof(saltBlock) - 4) return false;
  memcpy(saltBlock, salt, saltLen);
  saltBlock[saltLen] = 0; saltBlock[saltLen + 1] = 0;
  saltBlock[saltLen + 2] = 0; saltBlock[saltLen + 3] = 1;

  uint8_t u[32], t[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0)              { mbedtls_md_free(&ctx); return false; }
  if (mbedtls_md_hmac_starts(&ctx, (const unsigned char*)pass, passLen) != 0) {
    mbedtls_md_free(&ctx); return false;
  }
  if (mbedtls_md_hmac_update(&ctx, saltBlock, saltLen + 4) != 0) {
    mbedtls_md_free(&ctx); return false;
  }
  if (mbedtls_md_hmac_finish(&ctx, u) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_free(&ctx);
  memcpy(t, u, 32);

  for (uint16_t i = 1; i < iterations; i++) {
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0)               { mbedtls_md_free(&ctx); return false; }
    if (mbedtls_md_hmac_starts(&ctx, (const unsigned char*)pass, passLen) != 0) {
      mbedtls_md_free(&ctx); return false;
    }
    if (mbedtls_md_hmac_update(&ctx, u, 32) != 0)          { mbedtls_md_free(&ctx); return false; }
    if (mbedtls_md_hmac_finish(&ctx, u) != 0)              { mbedtls_md_free(&ctx); return false; }
    mbedtls_md_free(&ctx);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
  }
  memcpy(outHash, t, 32);
  memset(u, 0, sizeof(u)); memset(t, 0, sizeof(t));
  return true;
}

String base64urlEncode(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  uint8_t* buf = (uint8_t*)malloc(outLen);
  if (!buf) return String();
  if (mbedtls_base64_encode(buf, outLen, &outLen, data, len) != 0) { free(buf); return String(); }
  String s; s.reserve(outLen);
  for (size_t i = 0; i < outLen; i++) {
    char c = (char)buf[i];
    if      (c == '+') s += '-';
    else if (c == '/') s += '_';
    else if (c == '=') continue;
    else               s += c;
  }
  free(buf);
  return s;
}

String base64urlEncode(const String& s) {
  return base64urlEncode((const uint8_t*)s.c_str(), s.length());
}

String jwtSign(const String& username, const String& secret, uint32_t ttlSeconds) {
  extern uint32_t getCurrUnixTime();
  extern uint32_t getMonotonicSec();

  String headerJson  = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  String headerB64  = base64urlEncode(headerJson);

  uint32_t iat = getCurrUnixTime();
  if (iat == 0) iat = getMonotonicSec();
  uint32_t exp = iat + ttlSeconds;
  String payloadJson = "{\"sub\":\"" + username + "\",\"iat\":" + String(iat) +
                       ",\"exp\":" + String(exp) + "}";
  String payloadB64 = base64urlEncode(payloadJson);

  String signingInput = headerB64 + "." + payloadB64;
  uint8_t sig[32];
  if (!hmacSha256((const uint8_t*)secret.c_str(), secret.length(),
                  (const uint8_t*)signingInput.c_str(), signingInput.length(), sig)) {
    return String();
  }
  return signingInput + "." + base64urlEncode(sig, 32);
}

bool jwtVerify(const String& token, const String& secret, String& outUsername) {
  int firstDot = token.indexOf('.');
  int lastDot  = token.lastIndexOf('.');
  if (firstDot <= 0 || lastDot <= firstDot) return false;
  String headerB64  = token.substring(0, firstDot);
  String payloadB64 = token.substring(firstDot + 1, lastDot);
  String sigB64     = token.substring(lastDot + 1);
  String signingInput = headerB64 + "." + payloadB64;

  uint8_t expectedSig[32];
  if (!hmacSha256((const uint8_t*)secret.c_str(), secret.length(),
                  (const uint8_t*)signingInput.c_str(), signingInput.length(), expectedSig)) {
    return false;
  }
  String expectedSigB64 = base64urlEncode(expectedSig, 32);
  if (sigB64.length() != expectedSigB64.length()) return false;
  if (!constantTimeMemEquals((const volatile uint8_t*)sigB64.c_str(),
                              (const volatile uint8_t*)expectedSigB64.c_str(),
                              sigB64.length())) return false;

  // Decode payload to extract sub + exp
  String b64 = payloadB64;
  b64.replace('-', '+'); b64.replace('_', '/');
  while (b64.length() % 4) b64 += '=';

  size_t outLen = 0;
  mbedtls_base64_decode(nullptr, 0, &outLen,
                         (const unsigned char*)b64.c_str(), b64.length());
  if (outLen == 0 || outLen > 512) return false;
  uint8_t* buf = (uint8_t*)malloc(outLen + 1);
  if (!buf) return false;
  if (mbedtls_base64_decode(buf, outLen, &outLen,
                            (const unsigned char*)b64.c_str(), b64.length()) != 0) {
    free(buf); return false;
  }
  buf[outLen] = '\0';
  String payload((const char*)buf);
  free(buf);

  int subIdx = payload.indexOf("\"sub\":\"");
  if (subIdx < 0) return false;
  subIdx += 7;
  int subEnd = payload.indexOf("\"", subIdx);
  if (subEnd < 0) return false;
  outUsername = payload.substring(subIdx, subEnd);

  int expIdx = payload.indexOf("\"exp\":");
  if (expIdx >= 0) {
    expIdx += 6;
    int expEnd = payload.indexOf(',', expIdx);
    if (expEnd < 0) expEnd = payload.indexOf('}', expIdx);
    String expStr = payload.substring(expIdx, expEnd);
    uint32_t exp = (uint32_t)expStr.toInt();
    extern uint32_t getCurrUnixTime();
    extern uint32_t getMonotonicSec();
    uint32_t now = getCurrUnixTime();
    if (now == 0) now = getMonotonicSec();
    if (now > exp) return false;
    // [WAVE-6 / FW6-2] iat-regression check: a token claiming issuance in
    // the future (beyond a small skew) means our clock went BACKWARDS
    // (reboot without NTP on a board with stale/no RTC). Fail-closed: the
    // token predates the regression and its remaining lifetime is unknown.
    int iatIdx = payload.indexOf("\"iat\":");
    if (iatIdx >= 0) {
      iatIdx += 6;
      int iatEnd = payload.indexOf(',', iatIdx);
      if (iatEnd < 0) iatEnd = payload.indexOf('}', iatIdx);
      uint32_t iat = (uint32_t)payload.substring(iatIdx, iatEnd).toInt();
      if (iat > 0 && now + 300u < iat) return false;   // 5 min skew allowance
    }
  }
  return true;
}

String generateToken(size_t hexChars) {
  size_t byteLen = (hexChars + 1) / 2;
  uint8_t* buf = (uint8_t*)malloc(byteLen);
  if (!buf) return String();
  generateRandomBytes(buf, byteLen);
  char* hex = (char*)malloc(hexChars + 1);
  if (!hex) { free(buf); return String(); }
  bytesToHex(buf, byteLen, hex);
  hex[hexChars] = '\0';
  String s(hex);
  free(buf); free(hex);
  return s;
}

bool ed25519VerifyHash(const char* publicKeyHex,
                       const char* signatureHex,
                       const uint8_t* hashBytes, size_t hashLen) {
  if (!publicKeyHex || !signatureHex || !hashBytes) return false;
  if (strlen(publicKeyHex) != 64) {
    Serial.println(F("[Ed25519] Invalid public key length (need 64 hex chars)"));
    return false;
  }
  if (strlen(signatureHex) != 128) {
    Serial.println(F("[Ed25519] Invalid signature length (need 128 hex chars)"));
    return false;
  }
  uint8_t publicKey[32], signature[64];
  if (!hexToBytes(publicKeyHex, publicKey, 32)) return false;
  if (!hexToBytes(signatureHex, signature, 64)) return false;
  if (hashLen != 32) return false;

#if defined(MBEDTLS_ED25519_SUPPORTED)
  // PSA Crypto path — enabled when ESP32 framework is rebuilt with
  // CONFIG_MBEDTLS_ECP_DP_ED25519_ENABLED=y. Sign the SHA-256 hash as
  // the "message" under PureEdDSA. Match the sign_firmware.py convention:
  //   signature = ed25519_sign(SHA256(firmware.bin), privateKey)
  #include <psa/crypto.h>
  if (psa_crypto_init() != PSA_SUCCESS) {
    memset(publicKey, 0, sizeof(publicKey));
    memset(signature, 0, sizeof(signature));
    return false;
  }
  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_TWISTED_EDWARDS));
  psa_set_key_bits(&attrs, 255);
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_VERIFY_MESSAGE);
  psa_set_key_algorithm(&attrs, PSA_ALG_PURE_EDDSA);
  psa_key_id_t key_id = 0;
  if (psa_import_key(&attrs, publicKey, 32, &key_id) != PSA_SUCCESS) {
    memset(publicKey, 0, sizeof(publicKey));
    memset(signature, 0, sizeof(signature));
    return false;
  }
  psa_status_t status = psa_verify_message(key_id, PSA_ALG_PURE_EDDSA,
                                            hashBytes, hashLen,
                                            signature, 64);
  psa_destroy_key(key_id);
  memset(publicKey, 0, sizeof(publicKey));
  memset(signature, 0, sizeof(signature));
  return status == PSA_SUCCESS;
#else
  // Fail-closed: Ed25519 not compiled into this framework build.
  // MQTT OTA is rejected. REST OTA (streaming SHA-256) remains usable.
  Serial.println(F("[Ed25519] NOT AVAILABLE — define MBEDTLS_ED25519_SUPPORTED "
                    "after rebuilding ESP32 framework with Ed25519 support"));
  memset(publicKey, 0, sizeof(publicKey));
  memset(signature, 0, sizeof(signature));
  return false;
#endif
}

} // namespace Utils

// [FW-35 REMEDIATION 2026-08] Time source for JWT iat/exp:
//   - wall-clock (::time) when the system clock is synced (real epoch)
//   - NVS-persisted boot-epoch estimate otherwise (WAVE-6 / FW6-2)
// The old stub returned 0 unconditionally — tokens issued before NTP sync
// had iat=0/exp=0 and invalidated the instant the clock synced.
// [WAVE-6 / FW6-2] The 1700000000+millis() fallback had an honest hole: a
// token issued BEFORE a reboot (real-epoch exp, e.g. 1.78e9) was compared
// against a 1.7e9-base clock after the reboot — now < exp forever → the
// token stayed valid for ~89 years instead of its 15-minute TTL. The fix
// persists the boot-epoch (now - uptime) whenever the clock IS synced, and
// resumes from it after reboots. Residual error = persist cadence + offline
// downtime (minutes, bounded) — no longer decades. jwtVerify additionally
// rejects tokens whose iat is in the future beyond a small skew (clock
// regression = fail-closed).
static const char* EPOCH_NVS_NAMESPACE = "plts_time";
static const char* EPOCH_NVS_KEY       = "boot_epoch";
static uint32_t s_epochBootBase = 0;        // estimated epoch AT BOOT
static bool     s_epochLoaded   = false;    // lazy NVS load guard
static uint32_t s_epochLastPersistMs = 0;   // cadence guard
static const uint32_t EPOCH_PERSIST_EVERY_MS = 900000UL;  // 15 min

void Utils::persistEpochEstimate() {
  time_t now = ::time(nullptr);
  if (now <= 1700000000LL) return;   // unsynced — nothing TRUE to persist
  // Store the estimated BOOT epoch so any later boot can reconstruct
  // "now" as base + uptime without knowing when this persist happened.
  uint32_t newBase = (uint32_t)now - (uint32_t)(millis() / 1000);
  if (newBase < 1700000000u) return;  // paranoia: never persist a bogus base
  s_epochBootBase = newBase;
  Preferences prefs;
  if (prefs.begin(EPOCH_NVS_NAMESPACE, false)) {
    prefs.putUInt(EPOCH_NVS_KEY, s_epochBootBase);
    prefs.end();
  }
  s_epochLastPersistMs = millis();
}

uint32_t Utils::getCurrUnixTime() {
  time_t now = ::time(nullptr);
  if (now > 1700000000LL) {                 // sane epoch (> Nov 2023)
    // Synced: periodically refresh the persisted boot-epoch estimate so the
    // fallback never goes stale while the device is healthy (cheap NVS write
    // every 15 min, Preferences skips identical-value writes internally).
    if (millis() - s_epochLastPersistMs >= EPOCH_PERSIST_EVERY_MS) {
      persistEpochEstimate();
    }
    return (uint32_t)now;
  }
  // Clock unsynced: resume from the last persisted boot-epoch estimate.
  if (!s_epochLoaded) {
    s_epochLoaded = true;
    Preferences prefs;
    if (prefs.begin(EPOCH_NVS_NAMESPACE, true)) {
      s_epochBootBase = prefs.getUInt(EPOCH_NVS_KEY, 0);
      prefs.end();
    }
    if (s_epochBootBase >= 1700000000u) {
      Serial.printf("[TIME] unsynced — using persisted boot-epoch %u (±persist cadence)\n",
                    (unsigned)s_epochBootBase);
    }
  }
  if (s_epochBootBase >= 1700000000u) {
    return s_epochBootBase + (uint32_t)(millis() / 1000);
  }
  // First boot ever with no NTS/NVS history: old documented fallback.
  return 1700000000u + (uint32_t)(millis() / 1000);
}

uint32_t Utils::getMonotonicSec() {
  return millis() / 1000;
}
