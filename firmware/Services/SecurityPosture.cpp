// =============================================================================
// Services/SecurityPosture.cpp — implementation
// -----------------------------------------------------------------------------
// eFuse semantics verified against ESP-IDF v4.4.7 sources (the IDF the
// prebuilt arduino-esp32 2.0.17 core was compiled against):
//   - esp_efuse_read_secure_version()      -> popcount(SECURE_VERSION) — NO
//     dependency on CONFIG_BOOTLOADER_APP_SECURE_VERSION (checked: v4.4
//     reads min(32, field size) bits and counts) — works with the prebuilt
//     core, where that config is unset.
//   - esp_efuse_update_secure_version(n)   -> burns (1<<n)-1 unary pattern;
//     requires BLK3 coding scheme NONE (classic ESP32 BLK3 is coding-NONE on
//     typical WROOM modules; we detect and report honestly when it is not).
//   - esp_efuse_check_secure_version(v)    -> v >= popcount (bootloader rule).
//   - FLASH_CRYPT_CNT is 7 bits in BLK0; flash encryption is ON iff an odd
//     number of bits is set (parity rule, same as esp_flash_encryption_enabled()).
//   - ABS_DONE_0 / ABS_DONE_1 = secure boot V1 / V2 enabled markers (BLK0).
//
// Self-measurement: esp_partition_mmap() maps flash through the MMU cache,
// which transparently decrypts when flash encryption is enabled — the
// measured digest is of the PLAINTEXT image and matches the released
// firmware.bin SHA-256 from release.json / SHA256SUMS.
// =============================================================================
#include "SecurityPosture.h"
#include "LogService.h"
#include "OtaManager.h"          // getBootRollbackVersion-style peers live here
#include "../Core/Config.h"
#include "../Core/Common.h"
#include "../Utils/Crypto.h"
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>   // pulls in esp_spi_flash.h (mmap/munmap decls)
#include <mbedtls/md.h>
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace Services {

SecurityPosture securityPosture;

// ---- Minimal ESP32 app-image header walk (no esp_image_format.h dependency;
//      fields per ESP-IDF v4.4 esp_image_format.h, verified against real
//      bootloader-built images) -------------------------------------------
//   offset 0: magic 0xE9, segment_count, spi_mode, spi_speed/size, entry(4),
//             wp_pin, spi_pin_drv[3], chip_id(2), min_chip_rev, reserved[8],
//             hash_appended   -> 24 bytes total (0x18)
//   then per segment: load_addr(4) + data_len(4), followed by data_len bytes
//   then: checksum(1), pad to 16-byte alignment, [+32 SHA-256 if hash_appended]
static constexpr uint8_t  ESP_IMAGE_MAGIC      = 0xE9;
static constexpr uint16_t MAX_IMAGE_SEGMENTS  = 64;    // sanity bound
static constexpr size_t   IMAGE_HDR_SIZE       = 24;

// [p.437] Production builds refuse OTA when flash encryption is not
// provisioned. Escape hatch exists ONLY as an explicit, documented,
// non-production compile flag (Config.h) for a bench unit.
static bool _otaRequiresFlashEncryption() {
#if defined(PRODUCTION_BUILD) && !defined(PLTS_ALLOW_UNENCRYPTED_OTA)
  return true;
#else
  return false;
#endif
}

// [p.436] eFuse epoch burning is a one-way, production-only act. Development
// and staging builds NEVER burn (a dev board would be permanently pinned).
static bool _efuseBurnEnabled() {
#if defined(PRODUCTION_BUILD) && !defined(PLTS_DISABLE_EFUSE_BURN)
  return true;
#else
  return false;
#endif
}

size_t SecurityPosture::_imageLengthFromHeader(const uint8_t* hdr, size_t avail,
                                                uint16_t& segCount) {
  if (avail < IMAGE_HDR_SIZE) return 0;
  if (hdr[0] != ESP_IMAGE_MAGIC) return 0;
  uint8_t n = hdr[1];
  if (n == 0 || n > MAX_IMAGE_SEGMENTS) return 0;
  bool hashAppended = hdr[IMAGE_HDR_SIZE - 1] != 0;
  size_t off = IMAGE_HDR_SIZE;
  for (uint8_t i = 0; i < n; i++) {
    if (off + 8 > avail) return 0;              // segment header incomplete
    uint32_t dataLen;
    memcpy(&dataLen, hdr + off + 4, 4);          // little-endian len
    if (dataLen == 0 || dataLen > 0x200000) return 0;
    off += 8 + dataLen;
  }
  segCount = n;
  off += 1;                                     // checksum byte
  off = (off + 15) & ~((size_t)15);              // pad to 16
  if (hashAppended) off += 32;                   // appended SHA-256
  return off;
}

bool SecurityPosture::_loadLedger(uint32_t& epoch, uint16_t& maj, uint16_t& min) const {
  Preferences p;
  bool ok = false;
  epoch = 0; maj = 0; min = 0;
  if (p.begin("plts_sec", true)) {               // read-only open: exists?
    if (p.isKey("epoch")) {
      epoch = p.getUInt("epoch", 0);
      maj   = (uint16_t)p.getUInt("vmaj", 0);
      min   = (uint16_t)p.getUInt("vmin", 0);
      ok    = true;
    }
    p.end();
  }
  return ok;
}

void SecurityPosture::_storeLedger(uint32_t epoch, uint16_t maj, uint16_t min) {
  Preferences p;
  if (p.begin("plts_sec", false)) {
    p.putUInt("epoch", epoch);
    p.putUInt("vmaj", maj);
    p.putUInt("vmin", min);
    p.end();
  }
}

void SecurityPosture::_readHardwarePosture() {
  // Flash encryption: FLASH_CRYPT_CNT parity (7 bits, BLK0).
  uint32_t cryptCnt = 0;
  esp_efuse_read_field_blob(ESP_EFUSE_FLASH_CRYPT_CNT, &cryptCnt, 7);
  uint8_t ones = 0;
  for (uint8_t i = 0; i < 7; i++) if ((cryptCnt >> i) & 1) ones++;
  _s.flashEncryption = (ones & 1) == 1;          // odd parity = enabled

  // Secure boot markers.
  _s.secureBootV1 = esp_efuse_read_field_bit(ESP_EFUSE_ABS_DONE_0);
  _s.secureBootV2 = esp_efuse_read_field_bit(ESP_EFUSE_ABS_DONE_1);

  // Secure-version floor + capacity (same source as the IDF bootloader).
  _s.efuseFloor = esp_efuse_read_secure_version();          // popcount
  size_t bits = esp_efuse_get_field_size(ESP_EFUSE_SECURE_VERSION);
  _s.efuseFieldBits = (uint32_t)(bits ? bits : 32);
  // The block is taken from the field descriptor itself (the SDK's table
  // header does not export a SECURE_VERSION_NUM_BLOCK define).
  esp_efuse_block_t secVerBlock =
      (ESP_EFUSE_SECURE_VERSION[0] != nullptr)
          ? ESP_EFUSE_SECURE_VERSION[0]->efuse_block : EFUSE_BLK3;
  _s.efuseCodingNone =
      esp_efuse_get_coding_scheme(secVerBlock) == EFUSE_CODING_SCHEME_NONE;
  _s.burnSupported = _s.efuseCodingNone && _s.efuseFloor < _s.efuseFieldBits;
}

bool SecurityPosture::_parseRunningVersion(uint16_t& maj, uint16_t& min) const {
  // Core::FIRMWARE_VERSION is the compiled-in identity of THIS image.
  int a = 0, b = 0, c = 0;
  if (sscanf(Core::FIRMWARE_VERSION, "%u.%u.%u", &a, &b, &c) != 3) return false;
  maj = (uint16_t)a; min = (uint16_t)b;
  return true;
}

void SecurityPosture::_reconcileLedger() {
  uint32_t epoch; uint16_t maj, min;
  bool exists = _loadLedger(epoch, maj, min);
  uint32_t floor = _s.efuseFloor;

  if (exists && epoch == floor) {
    _s.ledgerStatus = LedgerStatus::Ok;
    _s.ledgerEpoch = epoch; _s.ledgerMaj = maj; _s.ledgerMin = min;
    return;
  }

  uint16_t runMaj, runMin;
  bool haveRun = _parseRunningVersion(runMaj, runMin);
  bool runNewer = haveRun && (runMaj > maj || (runMaj == maj && runMin > min));

  if (exists && epoch + 1 == floor && runNewer) {
    // Lost the ledger write between the eFuse burn and the NVS store during
    // the last activation (power loss in the microsecond window). The
    // running image itself proves the burn belonged to it: strictly newer
    // (major, minor) than the ledger record. Self-heal — provably safe.
    _storeLedger(floor, runMaj, runMin);
    _s.ledgerStatus = LedgerStatus::SelfHealed;
    _s.ledgerEpoch = floor; _s.ledgerMaj = runMaj; _s.ledgerMin = runMin;
    Log.append(Core::LogType::StorageError,
               "Security ledger self-healed: eFuse floor " + String(floor) +
               " advanced past ledger " + String(epoch) +
               " — recovered from running v" + Core::FIRMWARE_VERSION, 0);
    return;
  }

  if (!exists && floor == 0) {
    // Fresh device — nothing ever burned. Initialize the ledger at epoch 0.
    _storeLedger(0, 0, 0);
    _s.ledgerStatus = LedgerStatus::Ok;
    _s.ledgerEpoch = 0; _s.ledgerMaj = 0; _s.ledgerMin = 0;
    return;
  }

  if (!exists && floor > 0) {
    // NVS wiped/rolled back but the chip remembers advancing. The eFuse floor
    // is the hardware root of trust — its bits cannot be un-burned.
    _s.ledgerStatus = LedgerStatus::NoLedger;
    _s.ledgerEpoch = floor; _s.ledgerMaj = 0; _s.ledgerMin = 0;
    Log.append(Core::LogType::StorageError,
               "SECURITY: security ledger ABSENT but eFuse floor=" + String(floor) +
               " — NVS wiped/rolled back after security epochs were burned. " +
               "OTA will be refused until re-provisioned (docs/SECURE_PROVISIONING.md).", 0);
    return;
  }

  // exists && epoch < floor (gap > 1) or epoch > floor (impossible honestly).
  _s.ledgerStatus = LedgerStatus::Tamper;
  _s.ledgerEpoch = floor; _s.ledgerMaj = maj; _s.ledgerMin = min;
  Log.append(Core::LogType::StorageError,
             "SECURITY: ledger/eFuse MISMATCH (ledger=" + String(epoch) +
             ", floor=" + String(floor) + ") — possible rollback attack or "
             "NVS tampering. OTA will be refused.", 0);
}

bool SecurityPosture::otaProvisioningOk(String* whyNot) const {
  if (_otaRequiresFlashEncryption() && !_s.flashEncryption) {
    if (whyNot) *whyNot = "PRODUCTION: flash encryption not provisioned — "
                          "OTA refused (see docs/SECURE_PROVISIONING.md)";
    return false;
  }
  if (_s.ledgerStatus == LedgerStatus::Tamper ||
      _s.ledgerStatus == LedgerStatus::NoLedger) {
    if (whyNot) *whyNot = "SECURITY: anti-rollback ledger inconsistent with "
                          "eFuse floor — OTA refused until re-provisioned";
    return false;
  }
  return true;
}

bool SecurityPosture::onImageActivated() {
  uint16_t runMaj, runMin;
  if (!_parseRunningVersion(runMaj, runMin)) return false;
  bool advanced = (runMaj > _s.ledgerMaj) ||
                  (runMaj == _s.ledgerMaj && runMin > _s.ledgerMin);
  if (!advanced) return false;                    // patch-only update: no burn

  uint32_t newEpoch = _s.ledgerEpoch + 1;
  if (_efuseBurnEnabled()) {
    if (!_s.burnSupported) {
      // Report honestly, do not fail the already-committed image.
      Log.append(Core::LogType::StorageError,
                 "SECURITY: eFuse burn unsupported (BLK3 coding scheme or "
                 "field exhausted at floor " + String(_s.efuseFloor) +
                 ") — anti-rollback floor NOT advanced for v" +
                 Core::FIRMWARE_VERSION, 0);
      _s.ledgerStatus = LedgerStatus::BurnFail;
      return false;
    }
    esp_err_t err = esp_efuse_update_secure_version(newEpoch);
    if (err != ESP_OK) {
      Log.append(Core::LogType::StorageError,
                 "SECURITY: eFuse burn FAILED (err=" + String((int)err) +
                 ") — anti-rollback floor NOT advanced for v" +
                 Core::FIRMWARE_VERSION, 0);
      _s.ledgerStatus = LedgerStatus::BurnFail;
      return false;
    }
  }
  // Ledger advances even when burning is disabled (dev/staging): the ledger
  // then tracks intent without hardware teeth — reported as-is in /api/security.
  _storeLedger(newEpoch, runMaj, runMin);
  _s.ledgerEpoch = newEpoch; _s.ledgerMaj = runMaj; _s.ledgerMin = runMin;
  _s.efuseFloor = esp_efuse_read_secure_version();   // re-read the truth
  if (_s.ledgerStatus != LedgerStatus::BurnFail) _s.ledgerStatus = LedgerStatus::Ok;
  Log.append(Core::LogType::Boot,
             "Security epoch " + String(newEpoch) + " recorded for v" +
             String(runMaj) + "." + String(runMin) +
             (_efuseBurnEnabled() ? " (eFuse floor burned)" : " (no burn — non-production build)"), 0);
  return true;
}

bool SecurityPosture::_measureRunningImage() {
  RunningImageInfo& img = _s.image;
  img.measured = false;
  img.sha256[0] = '\0';
  img.lengthBytes = 0;
  img.partition[0] = '\0';
  img.state[0] = '\0';

  const esp_partition_t* run = esp_ota_get_running_partition();
  if (!run) return false;
  strncpy(img.partition, run->label, sizeof(img.partition) - 1);

  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
    strncpy(img.state, st == ESP_OTA_IMG_PENDING_VERIFY ? "PENDING_VERIFY"
            : st == ESP_OTA_IMG_VALID ? "VALID"
            : st == ESP_OTA_IMG_INVALID ? "INVALID"
            : st == ESP_OTA_IMG_ABORTED ? "ABORTED" : "UNKNOWN",
            sizeof(img.state) - 1);
    img.pendingVerify = (st == ESP_OTA_IMG_PENDING_VERIFY);
  } else {
    strncpy(img.state, "UNKNOWN", sizeof(img.state) - 1);
  }

  // Pass 1: map the header window, parse the segment table for the image
  // length. 8 KiB covers the header + far more segment records than any
  // real image has (bound: 64 segments = 512 bytes of headers).
  const size_t HDR_WINDOW = 8192;
  const uint8_t* hdrPtr = nullptr;
  spi_flash_mmap_handle_t hdrMap = 0;
  if (esp_partition_mmap(run, 0, HDR_WINDOW, SPI_FLASH_MMAP_DATA,
                         (const void**)&hdrPtr, &hdrMap) != ESP_OK) {
    return false;
  }
  uint16_t segCount = 0;
  size_t imageLen = _imageLengthFromHeader(hdrPtr, HDR_WINDOW, segCount);
  spi_flash_munmap(hdrMap);
  if (imageLen == 0 || imageLen > run->size) {
    Log.append(Core::LogType::StorageError,
               "Security self-measure: image header parse failed (len=0)", 0);
    return false;
  }

  // Pass 2: streaming SHA-256 over the image length in 32 KiB mmap windows.
  mbedtls_md_context_t sha;
  mbedtls_md_init(&sha);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_setup(&sha, info, 0) != 0 || mbedtls_md_starts(&sha) != 0) {
    mbedtls_md_free(&sha);
    return false;
  }
  const size_t CHUNK = 32768;
  uint8_t hash[32];
  for (size_t off = 0; off < imageLen; ) {
    size_t n = (imageLen - off < CHUNK) ? (imageLen - off) : CHUNK;
    const uint8_t* ptr = nullptr;
    spi_flash_mmap_handle_t map = 0;
    if (esp_partition_mmap(run, off, n, SPI_FLASH_MMAP_DATA,
                           (const void**)&ptr, &map) != ESP_OK) {
      mbedtls_md_free(&sha);
      return false;
    }
    mbedtls_md_update(&sha, ptr, n);
    spi_flash_munmap(map);
    off += n;
  }
  if (mbedtls_md_finish(&sha, hash) != 0) {
    mbedtls_md_free(&sha);
    return false;
  }
  mbedtls_md_free(&sha);
  Utils::bytesToHex(hash, 32, img.sha256);      // 64 hex + NUL
  img.lengthBytes = (uint32_t)imageLen;
  img.measured = true;
  return true;
}

const RunningImageInfo& SecurityPosture::runningImage() {
  if (!_imageMeasured) {
    _imageMeasured = _measureRunningImage();   // failure stays "unavailable"
  }
  return _s.image;
}

void SecurityPosture::begin() {
  // Build identity anchors.
#if defined(PRODUCTION_BUILD)
  _s.buildProfile = "PRODUCTION";
#elif defined(STAGING_BUILD)
  _s.buildProfile = "STAGING";
#else
  _s.buildProfile = "DEVELOPMENT";
#endif
  _s.firmwareVersion = Core::FIRMWARE_VERSION;
  _s.buildDate = Core::FIRMWARE_BUILD_DATE;
  _s.otaRequiresFlashEncryption = _otaRequiresFlashEncryption();
  _s.efuseBurnEnabled = _efuseBurnEnabled();

  _readHardwarePosture();
  _reconcileLedger();

  // Boot posture verdict — one line, loud when wrong.
  Serial.printf("[SECURITY] profile=%s enc=%d sbV1=%d sbV2=%d efuseFloor=%u/%u "
                "burn=%d ledger=%d\n",
                _s.buildProfile, _s.flashEncryption, _s.secureBootV1,
                _s.secureBootV2, (unsigned)_s.efuseFloor,
                (unsigned)_s.efuseFieldBits, _s.efuseBurnEnabled,
                (int)_s.ledgerStatus);
#ifdef PRODUCTION_BUILD
  if (!_s.flashEncryption) {
    // [p.437 fail-closed] Not a silent warning: production devices refuse
    // OTA until provisioned. The operator runbook is the remediation path.
    Log.append(Core::LogType::StorageError,
               "SECURITY: PRODUCTION device UNPROVISIONED — flash encryption "
               "is OFF. OTA will be refused until the device is provisioned "
               "(docs/SECURE_PROVISIONING.md).", 0);
  }
#endif
}

} // namespace Services
