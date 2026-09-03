// =============================================================================
// Services/OtaManager.cpp
// =============================================================================
#include "OtaManager.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/Crypto.h"
#include "../Network/MqttTelemetryPublisher.h"
#include "LogService.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>

// [FW-26-adjacent fix] The macro must be defined before _validateCa() uses it;
// previously _validateCa() referenced the NON-EXISTENT Core::OTA_HTTPS_ROOT_CA
// (compile error — proof that prior "build verified" claims were unreliable).
#ifndef OTA_HTTPS_ROOT_CA
#define OTA_HTTPS_ROOT_CA ""
#endif
#ifndef OTA_ED25519_PUBLIC_KEY_HEX
#define OTA_ED25519_PUBLIC_KEY_HEX ""
#endif

namespace Services {

OtaManager ota;

static bool semverParse(const String& s, uint16_t& maj, uint16_t& min, uint16_t& pat) {
  int a = 0, b = 0, c = 0;
  if (sscanf(s.c_str(), "%u.%u.%u", &a, &b, &c) != 3) return false;
  maj = (uint16_t)a; min = (uint16_t)b; pat = (uint16_t)c;
  return true;
}

bool OtaManager::_semverParse(const String& s, uint16_t& maj, uint16_t& min, uint16_t& pat) {
  return semverParse(s, maj, min, pat);
}

bool OtaManager::_validateVersion(const String& newVer) {
  uint16_t nMaj, nMin, nPat, cMaj, cMin, cPat;
  if (!semverParse(newVer, nMaj, nMin, nPat)) {
    _lastError = "invalid new version (not semver)";
    return false;
  }
  if (!semverParse(Core::FIRMWARE_VERSION, cMaj, cMin, cPat)) {
    _lastError = "invalid current version";
    return false;
  }
  // Strict anti-downgrade: new must be strictly greater
  if (nMaj < cMaj || (nMaj == cMaj && nMin < cMin) ||
      (nMaj == cMaj && nMin == cMin && nPat <= cPat)) {
    _lastError = "anti-downgrade: new version must be > current";
    return false;
  }
  return true;
}

bool OtaManager::_validateUrlAllowlist(const String& url) {
  if (Core::OTA_ALLOWED_HOSTS[0] == nullptr) {
#ifdef PRODUCTION_BUILD
    _lastError = "PRODUCTION: OTA host allowlist empty";
    return false;
#else
    return true;  // dev/staging: allow any HTTPS URL
#endif
  }
  if (!url.startsWith("https://")) {
    _lastError = "URL must be HTTPS";
    return false;
  }
  // Extract host
  String host = url.substring(8);
  int slash = host.indexOf('/');
  if (slash > 0) host = host.substring(0, slash);
  int colon = host.indexOf(':');
  if (colon > 0) host = host.substring(0, colon);
  // [FW-09 REMEDIATION 2026-08] The allowlist was joined with '\n' but split
  // on ',' — the whole array collapsed into ONE unmatched string, so every
  // host (including github.com itself) was REJECTED. Iterate the array
  // directly; exact or parent-domain suffix match.
  for (int i = 0; Core::OTA_ALLOWED_HOSTS[i] != nullptr; i++) {
    String one = Core::OTA_ALLOWED_HOSTS[i];
    one.trim();
    if (one.length() == 0) continue;
    if (host == one || host.endsWith("." + one)) return true;
  }
  _lastError = "host not in allowlist";
  return false;
}

bool OtaManager::_validateCa() {
#ifdef PRODUCTION_BUILD
  if (strlen(OTA_HTTPS_ROOT_CA) == 0) {
    _lastError = "PRODUCTION: OTA HTTPS root CA empty";
    return false;
  }
#endif
  return true;
}

bool OtaManager::_validateSha256() {
  if (_expectedSha256.length() != 64) {
    _lastError = "SHA-256 hex must be 64 chars";
    return false;
  }
  char got[65];
  Utils::bytesToHex(_hashResult, 32, got);
  if (!Utils::constantTimeMemEquals((const volatile uint8_t*)got,
                                      (const volatile uint8_t*)_expectedSha256.c_str(), 64)) {
    _lastError = "SHA-256 mismatch";
    return false;
  }
  return true;
}

bool OtaManager::_validateEd25519() {
  // Ed25519 fail-closed: if signature provided but key empty → refuse
  if (_signature.length() == 0) {
#ifdef PRODUCTION_BUILD
    _lastError = "PRODUCTION: signature required";
    return false;
#else
    return true;  // dev: allow unsigned
#endif
  }
  // [audit-2 R-4] Removed duplicate #ifndef OTA_ED25519_PUBLIC_KEY_HEX block
  // (the macro is already defined at top-of-file if not passed via -D). The
  // duplicate was dead preprocessor code (condition always false).
  if (OTA_ED25519_PUBLIC_KEY_HEX[0] == '\0') {
    _lastError = "Ed25519 public key not configured";
    return false;
  }
  return Utils::ed25519VerifyHash(OTA_ED25519_PUBLIC_KEY_HEX,
                                    _signature.c_str(),
                                    _hashResult, 32);
}

void OtaManager::begin() {
  Preferences p;
  if (p.begin("plts_ota", false)) {
    _bootAttempts = p.getUInt("boot_att", 0);
    p.end();
  }
  _bootStartedAt = millis();
  _markedValid   = false;

  // [W13-1] A boot attempt counts ONLY when the running image is a fresh,
  // unconfirmed OTA image (PENDING_VERIFY). The old code incremented the
  // counter at UPLOAD time (once per upload, never per boot) — the threshold
  // was unreachable and triggerRollback() was dead code. The download path
  // never touched the counter at all.
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  _pendingVerify = (esp_ota_get_state_partition(running, &state) == ESP_OK &&
                    state == ESP_OTA_IMG_PENDING_VERIFY);
  if (_pendingVerify) {
    _bootAttempts++;
    if (p.begin("plts_ota", false)) {
      p.putUInt("boot_att", _bootAttempts);
      p.end();
    }
    Serial.printf("[OTA] pending-verify boot try #%u\n", (unsigned)_bootAttempts);
  }

  if (shouldRollback()) {
    Serial.println(F("[OTA] Boot attempts exceeded — rolling back"));
    triggerRollback();
  }

  // [W14-2b] Bootloader-revert detection. Verified against the real IDF
  // v4.4.7 bootloader sources: the stock arduino bootloader gives a fresh
  // image exactly ONE unconfirmed boot; any reset before the 60 s confirm
  // (power blip, panic, WDT) marks it ABORTED and boots THIS older image
  // instead — silently (attempt #2 never happens, so the >= 3 path above
  // cannot fire for this case). Running an image OLDER than the stored
  // lf_ver marker can only mean the update was reverted.
  {
    Preferences p;
    String lf;
    if (p.begin("plts_ota", true)) {
      lf = p.getString("lf_ver", "");
      p.end();
    }
    if (lf.length() > 0 && lf != String(Core::FIRMWARE_VERSION)) {
      uint16_t lfM, lfm, lfp, cM, cm, cp;
      bool newer = semverParse(lf, lfM, lfm, lfp) &&
                   semverParse(String(Core::FIRMWARE_VERSION), cM, cm, cp) &&
                   (lfM > cM || (lfM == cM && lfm > cm) ||
                    (lfM == cM && lfm == cm && lfp > cp));
      if (newer) {
        _bootRollbackVersion = lf;
        Log.append(Core::LogType::OtaFailed,
                   "OTA ROLLBACK (bootloader revert): v" + lf +
                   " unconfirmed within its single boot — running v" +
                   String(Core::FIRMWARE_VERSION), 0);
        // [P1-6 FIX audit-2 S-3/S-4] Emit ROLLBACK lifecycle event with the
        // CORRECT version (lf = the version that failed; was reporting
        // Core::FIRMWARE_VERSION = the running old image).
        // Set _expectedVersion temporarily so _emitLifecycle reports lf, not
        // the running version (which would mislead GAS into thinking v1.7.1
        // rolled back when it was actually v1.7.2 that failed).
        String savedExpected = _expectedVersion;
        _expectedVersion = lf;
        _emitLifecycle("ROLLBACK",
          ("bootloader revert: v" + lf + " unconfirmed within single boot").c_str());
        _expectedVersion = savedExpected;
      }
      // Consume the marker either way (single-shot semantics; a later
      // manual re-flash must not misfire a second report).
      if (p.begin("plts_ota", false)) {
        p.remove("lf_ver");
        p.end();
      }
    }
  }
}

bool OtaManager::shouldRollback() {
  return _bootAttempts >= Core::OTA_MAX_BOOT_ATTEMPTS;
}

void OtaManager::triggerRollback() {
  Log.append(Core::LogType::OtaFailed,
             "Boot health check failed — marking app invalid for rollback", 0);
  // [P1-6] Emit ROLLBACK — the lifecycle's terminal failure state. The
  // device is about to reboot into the previous image; this is the last
  // event we can publish before reboot.
  _emitLifecycle("ROLLBACK", "Boot health check failed — reverting to previous image");
  // [W14-2b] this path reports the rollback itself; consume the marker so
  // the reverted image's next boot does not double-report.
  Preferences p;
  if (p.begin("plts_ota", false)) {
    p.remove("lf_ver");
    p.end();
  }
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

void OtaManager::markBootHealthyIfPending() {
  // [W13-1] One-shot per boot. Called from the 1 s main loop — cheap until
  // the window elapses (two integer compares), then settles.
  if (_markedValid) return;
  if (millis() - _bootStartedAt < Core::OTA_HEALTHY_AFTER_MS) return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    // THE missing call: without it the image stays PENDING_VERIFY and the
    // bootloader reverts it on the next reset — a "successful" OTA that
    // silently goes back to the old version after any reboot/power blip.
    esp_ota_mark_app_valid_cancel_rollback();
    Log.append(Core::LogType::OtaSuccess,
               "OTA image ACTIVATED — rollback cancelled after healthy window", 0);
    // [P1-6] Emit ACTIVATED — the lifecycle's terminal success state.
    // _jobId may be empty if the device rebooted before reading NVS; in that
    // case we still emit so GAS knows the running image is healthy.
    _emitLifecycle("ACTIVATED", "Boot healthy window elapsed — image confirmed");
  }
  markBootHealthy();          // reset the boot-attempt ledger
  _markedValid = true;
}

void OtaManager::markBootHealthy() {
  Preferences p;
  if (p.begin("plts_ota", false)) {
    p.putUInt("boot_att", 0);
    p.remove("lf_ver");          // [W14-2b] image confirmed — revert detection off
    p.end();
  }
  _bootAttempts = 0;
  Log.append(Core::LogType::Boot, "Boot healthy — boot-attempt ledger reset", 0);
}

// [W14-2a/b] Image-write bookkeeping, called at Update.end(true) success:
// a FRESH boot-try ledger per image (the old one accumulated across image
// generations — two power-blipped updates left boot_att=2, so a perfectly
// healthy THIRD image would boot at 2 → 3 and instantly self-rollback in
// begin()) + the revert marker (a next boot of an image OLDER than this
// version means the bootloader reverted the update).
void OtaManager::_recordFlashedImage(const String& version) {
  Preferences p;
  if (p.begin("plts_ota", false)) {
    p.putUInt("boot_att", 0);
    p.putString("lf_ver", version);
    p.end();
  }
  _bootAttempts = 0;
}

bool OtaManager::beginUpload(size_t totalSize, const char* expectedVersion) {
  if (_state != OtaState::Idle) {
    _lastError = "OTA already in progress";
    return false;
  }
  if (totalSize > Core::OTA_MAX_SIZE) {
    _lastError = "Binary too large";
    return false;
  }
  if (!_validateVersion(expectedVersion)) return false;
  if (!Update.begin(totalSize)) {
    _lastError = "Update.begin failed";
    return false;
  }
  _bytesProcessed = 0;
  _totalBytes = totalSize;
  _expectedVersion = expectedVersion;
  // [P1-6] Start a new OTA job id — correlates lifecycle events on GAS.
  // Format: "<unixTime>-<bytes>" (compact, sortable, unique per session).
  _jobId = String(Services::timeManager.getUnixTime()) + "-" + String(totalSize);
  _shaCtx = malloc(sizeof(mbedtls_md_context_t));
  if (_shaCtx) {
    mbedtls_md_init((mbedtls_md_context_t*)_shaCtx);
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_setup((mbedtls_md_context_t*)_shaCtx, info, 0) != 0 ||
        mbedtls_md_starts((mbedtls_md_context_t*)_shaCtx) != 0) {
      free(_shaCtx); _shaCtx = nullptr;
      _lastError = "SHA-256 init failed";
      return false;
    }
  }
  // [W13-1] The boot-attempt counter is NO LONGER incremented here. It is
  // incremented in begin() when the running image is actually PENDING_VERIFY
  // (once per boot of a fresh image) — see the [W13-1] note there. Incrementing
  // at upload time wrote the counter exactly once per upload and could never
  // reach OTA_MAX_BOOT_ATTEMPTS, making the auto-rollback path unreachable.
  _state = OtaState::Downloading;
  Log.append(Core::LogType::OtaStarted,
             "OTA upload started: v=" + _expectedVersion + " size=" + totalSize, 0);
  _emitLifecycle("ACCEPTED", "REST OTA upload accepted");
  _emitLifecycle("DOWNLOADING", "REST OTA upload in progress");
  return true;
}

bool OtaManager::feedChunk(const uint8_t* data, size_t len) {
  if (_state != OtaState::Downloading) return false;
  if (Update.write((uint8_t*)data, len) != len) {
    _lastError = "Update.write failed";
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return false;
  }
  if (_shaCtx) {
    mbedtls_md_update((mbedtls_md_context_t*)_shaCtx, data, len);
  }
  _bytesProcessed += len;
  return true;
}

bool OtaManager::finalizeUpload(const char* expectedSha256Hex,
                                const char* signatureHex,
                                const char* newVersion) {
  if (_state != OtaState::Downloading) return false;
  _expectedSha256 = expectedSha256Hex ? expectedSha256Hex : "";
  _signature     = signatureHex ? signatureHex : "";

  if (_shaCtx) {
    if (mbedtls_md_finish((mbedtls_md_context_t*)_shaCtx, _hashResult) != 0) {
      _lastError = "SHA-256 finish failed";
      _state = OtaState::Failed;
      _emitLifecycle("FAILED", _lastError.c_str());
      return false;
    }
  }
  if (!_validateSha256()) {
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", ("SHA-256 mismatch: " + _lastError).c_str());
    return false;
  }
  if (!_validateEd25519()) {
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", ("Ed25519: " + _lastError).c_str());
    return false;
  }
  _emitLifecycle("VERIFIED", "SHA-256 + Ed25519 OK");
  if (!Update.end(true)) {
    _lastError = "Update.end failed: " + String(Update.getError());
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return false;
  }
  _recordFlashedImage(_expectedVersion);   // [W14-2a/b] fresh ledger + marker
  _state = OtaState::Done;
  mbedtls_md_free((mbedtls_md_context_t*)_shaCtx);
  free(_shaCtx); _shaCtx = nullptr;
  Log.append(Core::LogType::OtaSuccess,
             "OTA upload verified + applied: v=" + _expectedVersion, 0);
  _emitLifecycle("FLASHED", "Image written — pending boot verify");
  // ACTIVATED is emitted by markBootHealthyIfPending() after the 60s healthy
  // window; ROLLBACK is emitted by triggerRollback() if health checks fail.
  return true;
}

bool OtaManager::beginDownload(const char* url, const char* expectedVersion,
                              size_t expectedSize, const char* expectedSha256Hex,
                              const char* signatureHex) {
  if (_state != OtaState::Idle) {
    _lastError = "OTA already in progress";
    return false;
  }
  if (!_validateVersion(expectedVersion)) return false;
  if (!_validateUrlAllowlist(url)) return false;
  if (!_validateCa()) return false;
  _expectedSha256 = expectedSha256Hex;
  _signature = signatureHex ? signatureHex : "";
  _bytesProcessed = 0;
  _totalBytes = expectedSize;
  _expectedVersion = expectedVersion;
  _downloadUrl = url;                     // [FW-09] retained for tickDownload()
  // [P1-6 FIX audit-2 S-2] Set jobId for MQTT path too. REST path sets it in
  // beginUpload. Without this, ACTIVATED/ROLLBACK events after MQTT OTA used
  // the sentinel "boot-verify" and GAS could not correlate to the MQTT job.
  _jobId = String(Services::timeManager.getUnixTime()) + "-mqtt-" + String(expectedSize);
  _state = OtaState::Downloading;
  Log.append(Core::LogType::OtaStarted,
             "OTA download started: " + String(url) + " v=" + _expectedVersion, 0);
  _emitLifecycle("ACCEPTED", "MQTT OTA accepted");
  _emitLifecycle("DOWNLOADING", "MQTT OTA download in progress");
  return true;
}

void OtaManager::tickDownload() {
  if (_state != OtaState::Downloading) return;
  if (_downloadUrl.length() == 0) {
    _state = OtaState::Failed; _lastError = "no download URL";
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }

  // [FW-09 REMEDIATION 2026-08] Previously an EMPTY STUB — beginDownload()
  // validated the manifest and then nothing ever fetched the image; MQTT OTA
  // could never complete. Now: HTTPS streaming download into the inactive
  // partition with SHA-256 computed on the fly, followed by the SAME crypto
  // chain as the REST upload path (P0-005 single trust boundary):
  //   download → SHA-256 → Ed25519 → Update.end → pending boot.
  WiFiClientSecure tls;
#ifdef OTA_HTTPS_ROOT_CA
  if (strlen(OTA_HTTPS_ROOT_CA) > 0) {
    tls.setCACert(OTA_HTTPS_ROOT_CA);
  } else {
#ifdef PRODUCTION_BUILD
    _lastError = "PRODUCTION: OTA HTTPS root CA empty";
    _state = OtaState::Failed;
    return;
#else
    tls.setInsecure();   // EXPLICIT DEV BYPASS — never in production builds
#endif
  }
#else
#ifdef PRODUCTION_BUILD
    _lastError = "PRODUCTION: OTA_HTTPS_ROOT_CA not defined";
    _state = OtaState::Failed;
    return;
#else
    tls.setInsecure();   // EXPLICIT DEV BYPASS
#endif
#endif
  tls.setTimeout(Core::OTA_TIMEOUT_MS / 1000);

  HTTPClient http;
  if (!http.begin(tls, _downloadUrl)) {
    _lastError = "HTTP begin failed";
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }
  http.setTimeout(Core::OTA_TIMEOUT_MS);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    _lastError = "HTTP GET failed: " + String(code);
    http.end();
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }
  int total = http.getSize();
  if (total > 0 && (uint32_t)total > Core::OTA_MAX_SIZE) {
    _lastError = "image too large";
    http.end();
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }

  if (!Update.begin((total > 0) ? (size_t)total : Core::OTA_MAX_SIZE)) {
    _lastError = "Update.begin failed";
    http.end();
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }
  mbedtls_md_context_t sha;
  mbedtls_md_init(&sha);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_setup(&sha, info, 0) != 0 || mbedtls_md_starts(&sha) != 0) {
    _lastError = "SHA-256 init failed";
    mbedtls_md_free(&sha);
    http.end();
    Update.end();
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  _bytesProcessed = 0;
  bool failed = false;
  uint32_t lastActive = millis();
  while (http.connected() && (total < 0 || (int)_bytesProcessed < total)) {
    // [v1.6.3 / audit-noise] Feed the task WDT INSIDE the download loop:
    // a slow flash write of a 1.5 MB image on a busy inverter-noise rail
    // can exceed the 20 s WDT window without ever being "stalled" — the
    // old code only fed the WDT between states, so a long download =
    // watchdog reboot mid-flash = rollback + a wedged OTA state. Plus a
    // 1 ms yield so the idle task / WiFi task can still run.
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
    size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - lastActive > Core::OTA_TIMEOUT_MS) { failed = true; _lastError = "download stalled"; break; }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    lastActive = millis();
    size_t n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (n == 0) continue;
    if (Update.write(buf, n) != n) {
      failed = true; _lastError = "Update.write failed"; break;
    }
    mbedtls_md_update(&sha, buf, n);
    _bytesProcessed += n;
    esp_task_wdt_reset();   // after each flash-write batch too
  }
  http.end();

  if (failed || (total > 0 && (int)_bytesProcessed != total)) {
    // Partial download — [P0-005 acceptance] RECOVER: abort, inactive
    // partition stays unbootable, device continues on current firmware.
    _lastError = failed ? _lastError : "partial download";
    mbedtls_md_free(&sha);
    Update.end();
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA download failed: " + _lastError, 0);
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }

  mbedtls_md_finish(&sha, _hashResult);
  mbedtls_md_free(&sha);

  // SAME verification chain as finalizeUpload (single trust boundary).
  if (!_validateSha256()) {
    _state = OtaState::Failed; Update.end();
    _emitLifecycle("FAILED", ("SHA-256 mismatch: " + _lastError).c_str());
    return;
  }
  if (!_validateEd25519()) {
    _state = OtaState::Failed; Update.end();
    _emitLifecycle("FAILED", ("Ed25519: " + _lastError).c_str());
    return;
  }
  _emitLifecycle("VERIFIED", "MQTT OTA SHA-256 + Ed25519 OK");
  if (!Update.end(true)) {
    _lastError = "Update.end failed: " + String(Update.getError());
    _state = OtaState::Failed;
    _emitLifecycle("FAILED", _lastError.c_str());
    return;
  }
  _recordFlashedImage(_expectedVersion);   // [W14-2a/b] fresh ledger + marker
  _state = OtaState::Done;
  Log.append(Core::LogType::OtaSuccess,
             "OTA download verified + applied: v=" + _expectedVersion, 0);
  _emitLifecycle("FLASHED", "MQTT OTA image written — pending boot verify");
  // ACTIVATED is emitted by markBootHealthyIfPending() after the 60s healthy
  // window; ROLLBACK is emitted by triggerRollback() if health checks fail.
}

bool OtaManager::beginManifestCheck(const char* manifestUrl) {
  // [WAVE-6 / FW6-4] Arm a manifest check: validate the URL against the SAME
  // allowlist + CA + HTTPS policy as the binary download, then let OtaTask
  // fetch it (non-blocking from the MQTT handler's perspective).
  if (_state != OtaState::Idle) {
    _lastError = "OTA already in progress";
    return false;
  }
  if (!manifestUrl || manifestUrl[0] == '\0') {
    _lastError = "ota.check requires a manifest url";
    return false;
  }
  if (!_validateUrlAllowlist(manifestUrl)) return false;
  if (!_validateCa()) return false;
  _manifestUrl = manifestUrl;
  _state = OtaState::Checking;
  _lastError = "";
  Log.append(Core::LogType::OtaStarted,
             "OTA check started: " + String(manifestUrl), 0);
  return true;
}

void OtaManager::tickManifestCheck() {
  if (_state != OtaState::Checking) return;
  if (_manifestUrl.length() == 0) {
    _state = OtaState::Failed;
    _lastError = "no manifest URL";
    return;
  }

  // TLS policy IDENTICAL to tickDownload — one trust boundary, no special
  // cases for the (smaller) manifest fetch.
  WiFiClientSecure tls;
#ifdef OTA_HTTPS_ROOT_CA
  if (strlen(OTA_HTTPS_ROOT_CA) > 0) {
    tls.setCACert(OTA_HTTPS_ROOT_CA);
  } else {
#ifdef PRODUCTION_BUILD
    _lastError = "PRODUCTION: OTA HTTPS root CA empty";
    _state = OtaState::Failed;
    return;
#else
    tls.setInsecure();   // EXPLICIT DEV BYPASS — never in production builds
#endif
  }
#else
#ifdef PRODUCTION_BUILD
  _lastError = "PRODUCTION: OTA_HTTPS_ROOT_CA not defined";
  _state = OtaState::Failed;
  return;
#else
  tls.setInsecure();   // EXPLICIT DEV BYPASS
#endif
#endif

  // A manifest is a tiny JSON document — 10 s is generous and keeps the
  // watchdog fed on the other side of the call.
  HTTPClient http;
  tls.setTimeout(10);
  if (!http.begin(tls, _manifestUrl)) {
    _lastError = "manifest HTTP begin failed";
    _state = OtaState::Failed;
    return;
  }
  http.setTimeout(10000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    _lastError = "manifest HTTP GET failed: " + String(code);
    http.end();
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check failed: " + _lastError, 0);
    return;
  }
  String body = http.getString();
  http.end();
  esp_task_wdt_reset();

  if (body.length() == 0 || body.length() > 8192) {
    _lastError = "manifest size out of bounds (0 < size <= 8 KiB)";
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check failed: " + _lastError, 0);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err != DeserializationError::Ok) {
    _lastError = "manifest JSON parse error: " + String(err.c_str());
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check failed: " + _lastError, 0);
    return;
  }

  // Accept both "fwVersion" (canonical) and "version" (legacy) — the same
  // tolerance ota.start applies.
  const char* ver = doc["fwVersion"] | "";
  if (ver[0] == '\0') ver = doc["version"] | "";
  const char* url = doc["url"] | "";
  const char* sha = doc["sha256"] | "";
  const char* sig = doc["signature"] | "";
  size_t size = (size_t)(doc["size"] | (uint32_t)0);

  if (ver[0] == '\0' || url[0] == '\0' || sha[0] == '\0') {
    _lastError = "manifest missing fields (fwVersion/version, url, sha256)";
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check failed: " + _lastError, 0);
    return;
  }

  // [W13-2] Mixed-fleet self-check: a manifest explicitly targeted at the
  // other firmware tree must never flash here ('' = fleet-wide, passes).
  String manifestTarget(doc["target"] | "");
  manifestTarget.trim();
  manifestTarget.toLowerCase();
  if (manifestTarget.length() > 0 && manifestTarget != "modular") {
    _lastError = "manifest target '" + manifestTarget +
                 "' does not match this device (modular)";
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check refused: " + _lastError, 0);
    return;
  }

  // Version policy BEFORE download: strictly newer only (anti-downgrade —
  // the same rule beginDownload enforces, applied early for an honest
  // "no update" answer instead of a failed download).
  uint16_t nMaj, nMin, nPat, cMaj, cMin, cPat;
  if (!semverParse(String(ver), nMaj, nMin, nPat) ||
      !semverParse(Core::FIRMWARE_VERSION, cMaj, cMin, cPat)) {
    _lastError = "manifest version not semver";
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check failed: " + _lastError, 0);
    return;
  }
  bool newer = (nMaj > cMaj) || (nMaj == cMaj && nMin > cMin) ||
               (nMaj == cMaj && nMin == cMin && nPat > cPat);
  if (!newer) {
    _state = OtaState::Idle;
    _lastError = "no update: manifest v" + String(ver) +
                 " <= running v" + String(Core::FIRMWARE_VERSION);
    Log.append(Core::LogType::Custom,
               "OTA check: " + _lastError, 0);
    return;
  }

  // Newer + signed → hand off to the EXACT ota.start trust chain.
  _state = OtaState::Idle;   // beginDownload requires Idle
  if (!beginDownload(url, ver, size, sha, sig)) {
    _state = OtaState::Failed;
    Log.append(Core::LogType::OtaFailed,
               "OTA check: download rejected — " + _lastError, 0);
    return;
  }
  Log.append(Core::LogType::OtaStarted,
             "OTA check: newer version v" + String(ver) +
             " — download started", 0);
}

// [P1-6 AUDIT 2026-09] Publish an OTA lifecycle event to GAS via MQTT.
// Wrapper that fills in the current job id (from the active session) and
// progress counters. Safe to call when no OTA is in progress (job id may
// be empty after a reboot, in which case we still emit so GAS can correlate
// via deviceKey + timestamp).
//
// [P1-6 FIX audit-2 S-2] Clear _jobId when emitting terminal states
// (ACTIVATED, ROLLBACK, FAILED) so a subsequent OTA session starts clean.
// Without this, jobId from session N leaks into session N+1's first event.
void OtaManager::_emitLifecycle(const char* state, const char* detail) {
  if (!state) return;
  // _jobId may be empty on a fresh boot (no OTA in progress this session).
  // Use a sentinel so GAS can still group the event by device.
  const char* jobId = _jobId.length() ? _jobId.c_str() : "boot-verify";
  Network::mqttTelemetry.publishOtaLifecycle(
    jobId,
    state,
    _expectedVersion.length() ? _expectedVersion.c_str() : Core::FIRMWARE_VERSION,
    detail,
    (uint32_t)_bytesProcessed,
    (uint32_t)_totalBytes
  );
  // Terminal states — clear jobId so next session starts fresh.
  if (strcmp(state, "ACTIVATED") == 0 ||
      strcmp(state, "ROLLBACK")  == 0 ||
      strcmp(state, "FAILED")    == 0) {
    _jobId = "";
  }
}

} // namespace Services
