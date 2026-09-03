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
//
// [P2-2 REMEDIATION 2026-09] OTA ACK/STATE CONTRACT (explicit state machine):
//   ACK on the command channel is a JOB-level settle, not a flash result:
//     phase ACCEPTED  = job accepted, download STARTED (this ACK is what the
//                       journal replays idempotently)
//     phase REJECTED  = job refused (policy/validation failure)
//   The lifecycle continues OUT-OF-BAND: DOWNLOAD_FAILED / VERIFICATION_
//   FAILED / ROLLBACK / ACTIVATED land in the DEVICE LOG + MQTT ack channel
//   (GAS OtaEvents is served by the generic firmware's OTA_STATUS reporter;
//   the modular tree reports locally until a GAS OTA_STATUS bridge exists).
//     ACCEPTED → DOWNLOADING → VERIFIED → FLASHED → ACTIVATED   (60 s healthy)
//                                  ↘ DOWNLOAD_FAILED / VERIFICATION_FAILED
//     FLASHED  → ROLLBACK      (3 unhealthy boots → previous partition)
//   Senders MUST treat ACK==ACCEPTED as "started", never "flashed". The
//   OtaState enum below is the single source for these transitions.
//
// [W13-1 REMEDIATION 2026-09] Boot-health semantics aligned with the
// bootloader's rollback contract (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in
// the arduino-esp32 sdkconfig — verified): Update.end() leaves the new image
// PENDING_VERIFY; the firmware MUST call esp_ota_mark_app_valid_cancel_
// rollback() to confirm it, or the bootloader reverts on the next reset.
// Previously markBootHealthy() cleared an NVS counter + logged "rollback
// cancelled" WITHOUT ever calling the ESP-IDF confirm API — every modular OTA
// silently reverted on the first post-update reset. Now:
//   - begin() counts a boot attempt ONLY when the running image is actually
//     PENDING_VERIFY (was: incremented once at upload time — dead code that
//     could never reach the threshold)
//   - markBootHealthyIfPending() (called from the 1 s main loop) confirms the
//     image after a 60 s stable window, exactly like firmware-generic's
//     markOtaHealthyIfPending()
//
// [W14-2 REMEDIATION 2026-09 — bench wave] Verified against the REAL IDF
// v4.4.7 bootloader sources: the stock arduino bootloader gives a fresh image
// exactly ONE unconfirmed boot — ANY reset before the 60 s confirm (power
// blip, panic, WDT) marks it ABORTED and reverts, so the >= 3 attempt path
// is defense-in-depth, not the primary revert mechanism. Rollback
// observability added:
//   - [W14-2a] the boot-try ledger is RESET at image-write time (per-image,
//     not accumulated across generations — 2 power-blipped updates used to
//     leave boot_att=2, making a healthy 3rd image self-rollback instantly)
//   - [W14-2b] a reverted (older) image DETECTS running < lf_ver at boot and
//     logs an honest ROLLBACK entry (getBootRollbackVersion()); the GAS
//     OTA_STATUS bridge for the modular tree remains OPEN — observability is
//     local (device log) until it exists
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_OTA_MANAGER_H
#define PLTS_SERVICES_OTA_MANAGER_H

#include <Arduino.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>

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
  // [W13-1] One-shot-per-boot: confirm a PENDING_VERIFY image after the
  // 60 s stable window (calls esp_ota_mark_app_valid_cancel_rollback).
  void markBootHealthyIfPending();
  // Reset the NVS boot-attempt counter (called by markBootHealthyIfPending).
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
  // [W14-2b] Version of the OTA image the bootloader reverted at boot
  // ("" = none this boot). Local observability — consumed by the device log;
  // a future GAS OTA_STATUS bridge can emit it.
  String   getBootRollbackVersion() const { return _bootRollbackVersion; }

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
  // [W13-1] boot-health bookkeeping
  uint32_t _bootStartedAt = 0;    // millis() at begin() — the 60 s window base
  bool     _markedValid   = false;   // one-shot: image confirmed this boot
  bool     _pendingVerify = false;   // running image awaits confirmation
  String   _bootRollbackVersion;    // [W14-2b] bootloader-reverted version

  // [W14-2a/b] Image-write bookkeeping: fresh ledger + revert marker.
  void _recordFlashedImage(const String& version);

  // Streaming SHA-256 via mbedtls (incremental)
  void*    _shaCtx = nullptr;
  uint8_t  _hashResult[32];

  bool _validateVersion(const String& newVer);
  bool _validateSha256();
  bool _validateEd25519();
  bool _validateUrlAllowlist(const String& url);
  bool _validateCa();

  // [P1-6 AUDIT 2026-09] Publish an OTA lifecycle event to GAS via MQTT.
  // Wrapper around Network::mqttTelemetry.publishOtaLifecycle() that fills in
  // the current job id (derived from the active OTA session) and progress
  // counters. Safe to call when no OTA is in progress — it no-ops.
  void _emitLifecycle(const char* state, const char* detail = nullptr);

  // Helpers
  static bool _semverParse(const String& s, uint16_t& maj, uint16_t& min, uint16_t& pat);

  // [P1-6] Current OTA job id — set when an OTA session starts, cleared on
  // Done/Failed. Used to correlate lifecycle events on the GAS side.
  String   _jobId;
};

extern OtaManager ota;

} // namespace Services

#endif // PLTS_SERVICES_OTA_MANAGER_H
