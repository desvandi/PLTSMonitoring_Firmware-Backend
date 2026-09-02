// =============================================================================
// Services/OtaManager.h — Boot health + auto-rollback + streaming SHA-256
//                          + Ed25519 verify + anti-downgrade + URL allowlist
// -----------------------------------------------------------------------------
// Brief §38:
//   - Boot health check: increment boot_attempts in NVS. If >= 3 fails →
//     esp_ota_mark_app_invalid_rollback_and_reboot()
//   - Streaming SHA-256 (ESP32 RAM constraint — do NOT buffer full binary)
//   - Anti-downgrade: strict SemVer comparison (must be strictly greater)
//   - X-Firmware-Version + X-Expected-SHA256 mandatory in MQTT OTA command
//   - REST OTA: multipart upload + streaming SHA-256 (Ed25519 optional)
//   - MQTT OTA: HTTPS download + streaming SHA-256 + Ed25519 verify
//   - URL allowlist (suffix match) for OTA host
//   - Fail-closed: empty OTA_ED25519_PUBLIC_KEY_HEX → OTA refused (MQTT path)
// [WAVE-6 / FW6-4] ota.check is REAL: beginManifestCheck(url) + tickManifestCheck()
//   fetch a signed manifest JSON over the SAME TLS/allowlist policy and hand
//   it to the SAME beginDownload() chain as ota.start (single trust boundary).
//   It used to be an honest no-op reject — the polling implementation the
//   operator was promised never existed. It does now.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_OTA_MANAGER_H
#define PLTS_SERVICES_OTA_MANAGER_H

#include <Arduino.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

namespace Services {

enum class OtaState : uint8_t {
  Idle        = 0,
  Downloading  = 1,
  Verifying   = 2,
  Applying     = 3,
  Failed       = 4,
  Done         = 5,
  Checking     = 6,   // [WAVE-6 / FW6-4] fetching a manifest for ota.check
};

class OtaManager {
public:
  void begin();
  void markBootHealthy();
  // Returns true if firmware should auto-rollback now.
  bool shouldRollback();
  void triggerRollback();

  // REST OTA: open upload session
  bool beginUpload(size_t totalSize, const char* expectedVersion);
  // Feed one chunk — updates streaming SHA-256.
  bool feedChunk(const uint8_t* data, size_t len);
  // Finalize: verify SHA-256 + Ed25519 (if provided) + version → apply
  bool finalizeUpload(const char* expectedSha256Hex,
                      const char* signatureHex,
                      const char* newVersion);

  // MQTT OTA: HTTPS download + streaming SHA-256 + Ed25519
  bool beginDownload(const char* url, const char* expectedVersion,
                      size_t expectedSize, const char* expectedSha256Hex,
                      const char* signatureHex);
  // Called by OtaTask to drive the download
  void tickDownload();

  // [WAVE-6 / FW6-4] ota.check — manifest polling, for real this time.
  // beginManifestCheck: validate + arm (called from the MQTT handler, fast,
  //   non-blocking). tickManifestCheck: fetch the manifest JSON over TLS
  //   (same allowlist/CA policy), then either report "no update" or hand the
  //   parsed fields to beginDownload() — the exact same trust chain as
  //   ota.start (pumped by OtaTask when state == Checking).
  bool beginManifestCheck(const char* manifestUrl);
  void tickManifestCheck();

  OtaState getState() const { return _state; }
  size_t   getBytesProcessed() const { return _bytesProcessed; }
  size_t   getTotalBytes() const { return _totalBytes; }
  String   getLastError() const { return _lastError; }
  uint32_t getBootAttempts() const { return _bootAttempts; }

private:
  OtaState _state = OtaState::Idle;
  size_t   _bytesProcessed = 0;
  size_t   _totalBytes = 0;
  String   _expectedVersion;
  String   _expectedSha256;
  String   _signature;
  String   _lastError;
  String   _downloadUrl;          // [FW-09] retained between beginDownload and tickDownload
  String   _manifestUrl;          // [WAVE-6 / FW6-4] retained between beginManifestCheck and tickManifestCheck
  uint32_t _bootAttempts = 0;

  // Streaming SHA-256 via mbedtls (incremental)
  void*    _shaCtx = nullptr;
  uint8_t  _hashResult[32];

  bool _validateVersion(const String& newVer);
  bool _validateSha256();
  bool _validateEd25519();
  bool _validateUrlAllowlist(const String& url);
  bool _validateCa();

  // Helpers
  static bool _semverParse(const String& s, uint16_t& maj, uint16_t& min, uint16_t& pat);
};

extern OtaManager ota;

} // namespace Services

#endif // PLTS_SERVICES_OTA_MANAGER_H
