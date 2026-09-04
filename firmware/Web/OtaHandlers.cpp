// =============================================================================
// Web/OtaHandlers.cpp — REST OTA (multipart streaming SHA-256 + Ed25519)
// -----------------------------------------------------------------------------
// REMEDIATION 2026-08 (Audit #1+#2+#3 — FW-07 / P0-005):
//   The multipart upload callback previously ran with NO authentication: the
//   ESP32 WebServer library streams the request BODY through the upload
//   callback BEFORE the final handler runs, and auth was only checked in the
//   final handler — i.e. AFTER the firmware image had already been flashed.
//   Now JWT auth + CSRF are verified at UPLOAD_FILE_START, before the first
//   byte is written to the inactive partition. Unauthenticated uploads never
//   call Update.begin()/Update.write() (fail-closed).
//
//   Trust chain (identical to the MQTT OTA path — P0-005 single trust
//   boundary): anti-downgrade version policy → size limit → streaming
//   SHA-256 → constant-time hash compare → Ed25519 signature → Update.end().
//   Both entry points converge on Services::ota (OtaManager); no weaker
//   REST-only path exists.
// =============================================================================
#ifndef OTA_ED25519_PUBLIC_KEY_HEX
#define OTA_ED25519_PUBLIC_KEY_HEX ""
#endif
#ifndef OTA_HTTPS_ROOT_CA
#define OTA_HTTPS_ROOT_CA ""
#endif
#include "OtaHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Config.h"
#include "../Services/OtaManager.h"
#include "../Services/LogService.h"
#include <ArduinoJson.h>

namespace Web {
namespace OtaHandlers {

static bool s_uploadActive = false;
static bool s_uploadRejected = false;   // [FW-07] auth/CSRF/limit failure
static String s_expectedSha;
static String s_signature;
static String s_version;
static size_t s_totalSize = 0;

void handleCheck() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  StaticJsonDocument<256> doc;
  doc["currentVersion"] = Core::FIRMWARE_VERSION;
  doc["otaEnabled"] = (OTA_ED25519_PUBLIC_KEY_HEX[0] != '\0') ||
                       (OTA_HTTPS_ROOT_CA[0] != '\0');
  doc["otaState"] = (uint8_t)Services::ota.getState();
  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void handleUploadStep() {
  // Called by WebServer for each chunk of multipart upload.
  // NOTE: headers (Authorization, X-CSRF-Token) are already parsed when the
  // body begins streaming — verify them BEFORE any flash mutation.
  HTTPUpload& upload = http.upload();

  if (upload.status == UPLOAD_FILE_START) {
    s_uploadActive = false;
    s_uploadRejected = false;

    // [FW-07] Auth gate BEFORE Update.begin() — an unauthenticated request
    // must never write a single byte to the OTA partition.
    if (!requireAuth()) {
      s_uploadRejected = true;
      Services::Log.append(Core::LogType::AuthFail,
                           "OTA upload rejected: unauthenticated", 0);
      return;   // chunks are discarded; final handler sends 401
    }
    if (!requireCsrf()) {
      s_uploadRejected = true;
      return;
    }

    s_expectedSha = http.hasHeader("X-Expected-SHA256") ? http.header("X-Expected-SHA256") : "";
    s_signature = http.hasHeader("X-Signature") ? http.header("X-Signature") : "";
    s_version = http.hasHeader("X-Firmware-Version") ? http.header("X-Firmware-Version") : "";
    s_totalSize = upload.totalSize;

    // [P0-005] Production builds refuse unsigned OTA at finalizeUpload —
    // reject early here as well so the transfer is not wasted.
#ifdef PRODUCTION_BUILD
    if (s_signature.length() == 0) {
      s_uploadRejected = true;
      Services::Log.append(Core::LogType::OtaFailed,
                            "OTA upload rejected: signature header missing (PRODUCTION)", 0);
      return;
    }
#endif

    if (!Services::ota.beginUpload(upload.totalSize, s_version.c_str())) {
      Serial.printf("[OTA] beginUpload failed: %s\n", Services::ota.getLastError().c_str());
      s_uploadRejected = true;
      return;
    }
    s_uploadActive = true;
  } else if (upload.status == UPLOAD_FILE_WRITE && s_uploadActive) {
    if (!Services::ota.feedChunk(upload.buf, upload.currentSize)) {
      Serial.printf("[OTA] feedChunk failed: %s\n", Services::ota.getLastError().c_str());
      s_uploadActive = false;
      s_uploadRejected = true;
    }
  } else if (upload.status == UPLOAD_FILE_END && s_uploadActive) {
    if (!Services::ota.finalizeUpload(s_expectedSha.c_str(), s_signature.c_str(),
                                       s_version.c_str())) {
      Serial.printf("[OTA] finalize failed: %s\n", Services::ota.getLastError().c_str());
      s_uploadActive = false;
      s_uploadRejected = true;
      return;
    }
    s_uploadActive = false;
    Serial.println(F("[OTA] REST upload complete + verified"));
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    s_uploadActive = false;
    s_uploadRejected = true;
    Serial.println(F("[OTA] upload aborted"));
  }
  // Chunks arriving while rejected are intentionally discarded (no flash
  // writes) — the connection is drained and the final handler reports the
  // rejection.
}

void handleUpload() {
  if (s_uploadRejected) {
    // [FW-07] Report the EARLY rejection reason (auth/CSRF/signature/
    // beginUpload). Falls through to the OTA state error below when the
    // rejection came from the crypto chain.
    if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
    sendError(400, "OTA rejected: " + Services::ota.getLastError());
    return;
  }
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  if (!requireCsrf()) return;
  // After upload completes, send response
  if (Services::ota.getState() == Services::OtaState::Done) {
    sendSuccess("OTA complete — reboot pending", "{}");
    // Schedule reboot
    delay(500);
    ESP.restart();
  } else {
    sendError(500, "OTA failed: " + Services::ota.getLastError());
  }
}

void registerRoutes() {
  // Multipart upload: handleUploadStep is invoked as upload callback
  http.on("/api/ota", HTTP_POST, handleUpload, handleUploadStep);
  http.on("/api/ota/check", HTTP_POST, handleCheck);
}

} // namespace OtaHandlers
} // namespace Web
