// =============================================================================
// Services/SecurityPosture.h — Device hardware-security posture + eFuse
//                               secure-version floor + running-image self-hash
// -----------------------------------------------------------------------------
// [AUDIT 2026-09 ROUND 4 / p.437+ — Secure Boot / Flash Encryption /
// hardware anti-rollback] The auditor's finding: the firmware had ZERO
// source-level visibility into — or use of — the ESP32 hardware security
// state. Ed25519 OTA signing answers "is this image signed by the release
// key?" but says nothing about the DEVICE: whether flash encryption is
// provisioned, whether secure boot is enabled, or whether the chip has a
// burned one-way anti-rollback floor. This service makes that state a
// first-class, inspectable, enforceable part of the firmware.
//
// WHAT THIS SERVICE DOES (app-level, "Layer 2"):
//   1. READS the hardware posture from eFuse (flash encryption parity,
//      secure-boot V1/V2 bits, BLK3 SECURE_VERSION popcount, coding scheme).
//   2. MAINTAINS the eFuse secure-version floor using the SAME semantics as
//      the ESP-IDF bootloader (esp_efuse_read_secure_version = popcount,
//      esp_efuse_check_secure_version = candidate >= floor). The NVS ledger
//      ("plts_sec") interprets each burned bit as one security epoch and
//      records which firmware (major, minor) that epoch represents. The
//      ledger is cross-checked against the hardware floor at every boot —
//      a ledger BELOW the floor means NVS was rolled back / wiped while the
//      chip advanced: fail-closed for OTA + CRITICAL log.
//   3. SELF-MEASURES the running image (SHA-256 over the running partition
//      via esp_partition_mmap — cache reads are transparently decrypted when
//      flash encryption is on, so the digest is of the PLAINTEXT image and
//      can be compared byte-for-byte against release.json firmwareSha256).
//      This is the device-side half of "proof that the flashed binary is the
//      audited artifact" (operator-side half: scripts/verify_flashed_image.py).
//
// WHAT IT DOES NOT DO (honest scope — secure-provisioning "Layers"):
//   - It does NOT make the BOOTLOADER reject images; the prebuilt
//     arduino-esp32 bootloader has no secure-boot / anti-rollback support
//     compiled in. Bootloader-level enforcement ("Layer 1") requires the
//     ESP-IDF build migration (Gate B of the secure-provisioning plan).
//   - It does NOT enable flash encryption or secure boot; those are one-way
//     per-device provisioning steps performed by the operator (espefuse).
//
// NVS: namespace "plts_sec" is intentionally NOT in FactoryReset's 13-namespace
// sweep — security epoch state must survive a factory reset (it is device
// security state, not user configuration). See FactoryReset.cpp.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_SECURITY_POSTURE_H
#define PLTS_SERVICES_SECURITY_POSTURE_H

#include <Arduino.h>
#include <cstdint>

namespace Services {

// Ledger status — result of reconciling the NVS ledger against the hardware
// eFuse floor at boot.
enum class LedgerStatus : uint8_t {
  Ok          = 0,   // ledger.epoch == efuse floor
  SelfHealed  = 1,   // lost ledger write mid-activation; recovered provably
  Tamper      = 2,   // ledger below floor with no valid explanation
  NoLedger    = 3,   // floor > 0 but the ledger never existed (NVS wiped)
  BurnFail    = 4,   // last activation could not burn (posture degraded)
};

struct RunningImageInfo {
  bool     measured;       // self-measurement completed
  char     sha256[65];     // hex digest of the running app image (plaintext)
  uint32_t lengthBytes;    // parsed image length (header walk)
  char     partition[12];  // "factory" / "ota_0" / "ota_1"
  bool     pendingVerify;  // running image awaits the healthy-window confirm
  char     state[16];      // "VALID" / "PENDING_VERIFY" / "UNKNOWN"
};

struct SecurityPostureSnapshot {
  // Build identity (attestation anchors — see scripts/verify_flashed_image.py)
  const char* buildProfile;
  const char* firmwareVersion;
  const char* buildDate;

  // Hardware posture (eFuse facts, read-only)
  bool     flashEncryption;      // FLASH_CRYPT_CNT odd parity
  bool     secureBootV1;         // ABS_DONE_0
  bool     secureBootV2;         // ABS_DONE_1
  uint32_t efuseFloor;           // popcount(SECURE_VERSION) — one-way
  uint32_t efuseFieldBits;       // field capacity (32 on classic ESP32)
  bool     efuseCodingNone;      // BLK3 coding scheme NONE (burn-capable)
  bool     burnSupported;        // coding NONE && floor < fieldBits

  // App-level anti-rollback ledger (NVS "plts_sec", survives factory reset)
  LedgerStatus ledgerStatus;
  uint32_t ledgerEpoch;
  uint16_t ledgerMaj;            // (major, minor) the current epoch represents
  uint16_t ledgerMin;

  // Policy (compile-time — production is stricter; see Config.h)
  bool     otaRequiresFlashEncryption;
  bool     efuseBurnEnabled;      // production builds only

  // Self-measurement
  RunningImageInfo image;
};

class SecurityPosture {
public:
  void begin();   // boot: read posture, reconcile ledger, log verdict

  // ---- Hardware facts (cached at begin; eFuse state cannot change while
  //      the app runs — bits are one-way and nothing else burns them) ----
  bool     flashEncryptionEnabled() const { return _s.flashEncryption; }
  bool     secureBootV1Enabled()    const { return _s.secureBootV1; }
  bool     secureBootV2Enabled()    const { return _s.secureBootV2; }
  uint32_t efuseFloor()             const { return _s.efuseFloor; }
  bool     burnSupported()          const { return _s.burnSupported; }

  // ---- Ledger ----
  LedgerStatus ledgerStatus()       const { return _s.ledgerStatus; }
  uint32_t ledgerEpoch()            const { return _s.ledgerEpoch; }
  void     ledgerVersion(uint16_t& maj, uint16_t& min) const {
    maj = _s.ledgerMaj; min = _s.ledgerMin;
  }
  // Candidate (major, minor) must be >= the ledger's version to be accepted
  // by OTA. Combined with the existing strict-semver running-image gate this
  // rejects any downgrade below the last burned security epoch.
  bool     candidateAllowedByFloor(uint16_t maj, uint16_t min) const {
    return (maj > _s.ledgerMaj) || (maj == _s.ledgerMaj && min >= _s.ledgerMin);
  }

  // OTA gate: is this device allowed to accept OTA at all right now?
  // Fail-closed on: production + unencrypted flash; tampered/absent ledger.
  bool     otaProvisioningOk(String* whyNot = nullptr) const;

  // Called by OtaManager::markBootHealthyIfPending() — the image is now
  // CONFIRMED healthy. If the running (major, minor) advanced past the
  // ledger version, burn one more eFuse bit (production builds only) and
  // advance the ledger. Returns true when the ledger advanced.
  bool     onImageActivated();

  // ---- Self-measurement (cached; computed lazily on first call) ----
  const RunningImageInfo& runningImage();

  const SecurityPostureSnapshot& snapshot() const { return _s; }

private:
  SecurityPostureSnapshot _s = {};
  bool _imageMeasured = false;

  void _readHardwarePosture();
  void _reconcileLedger();
  bool _loadLedger(uint32_t& epoch, uint16_t& maj, uint16_t& min) const;
  void _storeLedger(uint32_t epoch, uint16_t maj, uint16_t min);
  bool _parseRunningVersion(uint16_t& maj, uint16_t& min) const;
  bool _measureRunningImage();
  static size_t _imageLengthFromHeader(const uint8_t* hdr, size_t avail,
                                       uint16_t& segCount);
};

extern SecurityPosture securityPosture;

} // namespace Services

#endif // PLTS_SERVICES_SECURITY_POSTURE_H
