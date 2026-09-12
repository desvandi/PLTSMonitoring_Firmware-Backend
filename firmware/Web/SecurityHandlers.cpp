// =============================================================================
// Web/SecurityHandlers.cpp — GET /api/security (audit round 4 / p.436+p.437)
// -----------------------------------------------------------------------------
// Device hardware-security posture + running-image self-attestation.
//
// WHO CONSUMES THIS:
//   - The acceptance engineer, during per-device provisioning: every field
//     here is the EVIDENCE that flash encryption / secure boot / the eFuse
//     anti-rollback floor are actually provisioned on THIS physical unit
//     (docs/SECURE_PROVISIONING.md).
//   - scripts/verify_flashed_image.py --device-url, which fetches this
//     endpoint and compares runningImage.sha256 against the signed release
//     manifest (release.json) — "proof that the flashed binary is the
//     audited artifact" (auditor conclusion item 4).
//
// HONESTY CONTRACT (same as the rest of the diagnostics family):
//   - unmeasured/unavailable values are reported as such (never faked);
//   - verdicts are computed, not asserted: "PASS" only when every required
//     provisioning fact is present for a production build.
// =============================================================================
#include "SecurityHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Services/SecurityPosture.h"
#include <ArduinoJson.h>

namespace Web {
namespace SecurityHandlers {

static const char* ledgerStatusStr(Services::LedgerStatus s) {
  switch (s) {
    case Services::LedgerStatus::Ok:         return "OK";
    case Services::LedgerStatus::SelfHealed:  return "SELF_HEALED";
    case Services::LedgerStatus::Tamper:     return "TAMPER";
    case Services::LedgerStatus::NoLedger:   return "NO_LEDGER";
    case Services::LedgerStatus::BurnFail:   return "BURN_FAIL";
  }
  return "UNKNOWN";
}

void handleGet() {
  if (!requireAuth()) { sendError(401, "Unauthorized"); return; }
  const Services::SecurityPostureSnapshot& s = Services::securityPosture.snapshot();

  // Lazily self-measure the running image (cached after the first call).
  const Services::RunningImageInfo& img = Services::securityPosture.runningImage();

  StaticJsonDocument<3072> doc;

  // ---- Build identity (attestation anchors) -----------------------------
  doc["buildProfile"]    = s.buildProfile;
  doc["firmwareVersion"] = s.firmwareVersion;
  doc["buildDate"]       = s.buildDate;

  // ---- Hardware posture (eFuse facts) ------------------------------------
  JsonObject fe = doc.createNestedObject("flashEncryption");
  fe["enabled"] = s.flashEncryption;
  fe["source"]  = "efuse FLASH_CRYPT_CNT parity (odd = enabled)";

  JsonObject sb = doc.createNestedObject("secureBoot");
  sb["v1"] = s.secureBootV1;   // ABS_DONE_0
  sb["v2"] = s.secureBootV2;   // ABS_DONE_1

  JsonObject ef = doc.createNestedObject("efuseAntiRollback");
  ef["secureVersionFloor"] = s.efuseFloor;
  ef["fieldBits"]          = s.efuseFieldBits;
  ef["burnSupported"]       = s.burnSupported;
  ef["codingSchemeNone"]    = s.efuseCodingNone;
  ef["burnEnabled"]         = s.efuseBurnEnabled;   // production builds only

  // ---- App-level ledger ---------------------------------------------------
  JsonObject lg = doc.createNestedObject("ledger");
  lg["epoch"]       = s.ledgerEpoch;
  lg["epochVersion"] = String(s.ledgerMaj) + "." + String(s.ledgerMin);
  lg["status"]      = ledgerStatusStr(s.ledgerStatus);

  // ---- Policy -------------------------------------------------------------
  JsonObject pol = doc.createNestedObject("otaPolicy");
  pol["requireFlashEncryption"] = s.otaRequiresFlashEncryption;
  pol["efuseBurnEnabled"]       = s.efuseBurnEnabled;

  // ---- Running image self-measurement -------------------------------------
  JsonObject ri = doc.createNestedObject("runningImage");
  ri["measured"]   = img.measured;
  ri["sha256"]     = img.measured ? img.sha256 : "";
  ri["lengthBytes"] = img.measured ? (uint32_t)img.lengthBytes : 0;
  ri["partition"]  = img.partition;
  ri["state"]       = img.state;
  ri["method"]      = "mmap sha256 over running partition (cache-decrypted)";

  // ---- Provisioning verdict (computed) -------------------------------------
  // PASS      — every fact a production device must have.
  // DEGRADED  — encrypted but secure boot off (or burn unavailable).
  // FAIL      — unencrypted flash on a production build, or ledger tampered.
  // N/A       — development/staging build (facts still reported raw).
  JsonArray reasons = doc.createNestedArray("reasons");
  const char* verdict = "PASS";
  bool isProduction = (strcmp(s.buildProfile, "PRODUCTION") == 0);
  if (!isProduction) {
    verdict = "N/A (non-production build)";
  } else {
    if (!s.flashEncryption) {
      verdict = "FAIL";
      reasons.add("flash encryption NOT enabled — OTA refused on this device");
    }
    if (!s.secureBootV1 && !s.secureBootV2) {
      if (s.flashEncryption) verdict = "DEGRADED";
      reasons.add("secure boot NOT enabled (V1/V2 eFuses clear) — bootloader "
                  "accepts unsigned images");
    }
    if (s.ledgerStatus == Services::LedgerStatus::Tamper ||
        s.ledgerStatus == Services::LedgerStatus::NoLedger) {
      verdict = "FAIL";
      reasons.add("anti-rollback ledger inconsistent with eFuse floor — "
                  "possible rollback/tamper; OTA refused");
    } else if (s.ledgerStatus == Services::LedgerStatus::BurnFail) {
      if (strcmp(verdict, "PASS") == 0) verdict = "DEGRADED";
      reasons.add("last epoch burn FAILED — floor not advanced");
    }
    if (!s.burnSupported && s.ledgerEpoch >= s.efuseFieldBits) {
      if (strcmp(verdict, "PASS") == 0) verdict = "DEGRADED";
      reasons.add("eFuse secure-version field exhausted — no further epochs");
    }
  }
  doc["provisioning"] = verdict;
  doc["verifyHint"] = "compare runningImage.sha256 with release.json "
                      "firmwareSha256 (scripts/verify_flashed_image.py "
                      "--device-url http://<device>/api/security --release-json "
                      "release.json)";

  String out; serializeJson(doc, out);
  sendSuccess("OK", out);
}

void registerRoutes() {
  http.on("/api/security", HTTP_GET, handleGet);
}

} // namespace SecurityHandlers
} // namespace Web
