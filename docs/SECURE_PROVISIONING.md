# Secure Provisioning Runbook — PLTS Monitor (ESP32-WROOM-32)

**Status: ACTIVE RUNBOOK — audit 2026-09 round 4 response (p.436/p.437)**
**Audience:** provisioning operator (bench). **Scope:** per-device, one-way,
irreversible acts. Read the whole document before touching a device.

---

## 0. Why this runbook exists

The auditor's round-4 verdict is correct and important:

> Ed25519 menjawab "apakah firmware ini ditandatangani pihak yang dipercaya?"
> Secure Boot menjawab "apakah boot ROM/bootloader hardware mau menjalankan
> hanya firmware yang dipercaya?"

— these are DIFFERENT questions. Until now the firmware answered only the
first. This runbook is the operational path to answer the second, and the
accompanying firmware release (see §2) makes the device itself enforce and
self-report the answer.

## 1. Layer map — what stops which attack

| # | Layer | Answers | State in this repo |
|---|-------|---------|--------------------|
| 0 | Ed25519 OTA signature (SHA-256 + Ed25519, fail-closed in production) | "Is this IMAGE from the trusted releaser?" | ✅ shipped (OtaManager) |
| 1 | **Flash Encryption** (eFuse FLASH_CRYPT_CNT + BLK1 key) | "Is the DEVICE's flash ciphertext at rest?" | **Gate A — this runbook** |
| 2 | **Secure Boot** V1/V2 (eFuse ABS_DONE_*) | "Will the boot chain run untrusted code?" | **Gate B — roadmap, §6** |
| 3 | **eFuse SECURE_VERSION floor** (BLK3, one-way) | "Can this DEVICE ever run a lower security epoch, even past the app OTA path?" | ✅ app-enforced now (Layer 2A); bootloader-enforced under Gate B |

Defense-in-depth is the design: each layer assumes the others may fail.
Even with Layers 0+3 fully in place, an attacker who has the *released,
legitimately-signed old binary* and physical UART access can flash it —
only Layers 1+2 stop that. Conversely, Layer 1+2 without Layer 0 would allow
an attacker-controlled image. All four are needed for "device production
grade" (the auditor's words).

## 2. What the firmware now does by itself (no operator action required)

Shipped in this release (see `Services/SecurityPosture.*`):

1. **Reads and reports the hardware posture** — flash-encryption parity
   (FLASH_CRYPT_CNT), secure-boot bits (ABS_DONE_0/ABS_DONE_1), the eFuse
   secure-version floor (BLK3 SECURE_VERSION popcount), BLK3 coding scheme —
   via **`GET /api/security`** (authenticated). This is the per-device
   *evidence endpoint* the auditor asked for.
2. **Enforces, fail-closed:**
   - PRODUCTION builds **refuse every OTA while flash encryption is off**
     (compile escape hatch `PLTS_ALLOW_UNENCRYPTED_OTA` exists for a sealed
     bench unit only — see `firmware/Core/Config.h`).
   - **[AUDIT ROUND 5 / p.443]** The same wrong-order state is now also
     *operationally loud*: a PRODUCTION build on unencrypted flash raises the
     persistent `SECURITY_PROVISIONING` Critical alarm at boot. The device
     says it in telemetry until Gate A completes — it is no longer a fact
     discoverable only via `/api/security`.
   - ANY build **refuses OTA when the anti-rollback ledger is inconsistent
     with the eFuse floor** (NVS rolled back / wiped after epochs burned —
     rollback evidence).
   - ANY build **refuses OTA candidates whose (major, minor) is below the
     burned security epoch** — even when the running image is itself older
     (e.g. after a bootloader rollback). Patch-level updates within the
     epoch remain free.
2b. **[AUDIT ROUND 5 / p.444] Commissioning credential boundary:**
   the one-time UART reveal of the generated admin password stays (F-G18
   commissioning contract) but is now an *auditable, closable* window:
   - the generation moment is logged with an explicit SECURITY marker;
   - the `DEFAULT_CREDENTIALS_ACTIVE` Warning alarm stays raised until the
     operator changes the password (PWA Settings → Security), mirrored as
     `credentialBoundary.defaultCredentialActive` on `/api/security`;
   - the flag is persisted (`credProv` in config.json); configs written
     before the flag existed are treated as already provisioned.
3. **Burns the anti-rollback floor on activation** — when a freshly updated
   image survives its healthy window (`esp_ota_mark_app_valid_cancel_
   rollback`), a PRODUCTION build burns one more eFuse bit (BLK3
   SECURE_VERSION, unary/IDF-bootloader-compatible semantics:
   `esp_efuse_update_secure_version`) and records the epoch ledger in NVS
   namespace `plts_sec` (survives factory reset by design). Dev/staging
   builds never burn.
4. **Self-measures the running image** — SHA-256 over the running partition
   via `esp_partition_mmap` (cache reads are transparently decrypted under
   flash encryption, so this equals the *plaintext* release digest) and
   reports it in `/api/security`. Combined with
   `scripts/verify_flashed_image.py --device-url`, this is the
   **"proof the flashed binary is the audited artifact"** tool.

### eFuse budget (honest limit)

SECURE_VERSION is 32 bits → **32 security epochs** per device. One epoch is
burned per (major, minor) advance. At the project's cadence that is decades
of headroom, and exhaustion is *reported* (`/api/security` →
`efuseAntiRollback.burnSupported=false` + DEGRADED verdict), never silent.

## 3. Gate A — Flash Encryption (per production device)

**Prerequisites:** a sacrificial bench device validated first; production
image (signed, from CI release artifacts); `esptool` + `espefuse` +
`espsecure` installed (`pip install esptool`); the release `release.json` at
hand; serial console open. All commands verified against ESP-IDF v4.4
documentation (`docs/en/security/flash-encryption.rst`, release/v4.4) —
the IDF the prebuilt arduino-esp32 2.0.17 core was built with.

> ⚠️ **ONE-WAY.** Burning eFuses cannot be undone. On classic ESP32 flash
> encryption can be toggled off at most ~3 times (FLASH_CRYPT_CNT is 7 bits,
> odd parity = on). Practice the whole sequence on a bench unit first.
>
> ⚠️ **Key loss = un-reflashable via UART in release mode.** In Gate A
> (development-mode encryption, 1 bit) re-flashing works with
> `esptool write_flash --encrypt` without the host key. If you later burn
> all 7 bits + DISABLE_DL_* (§3.6), keep the key in the same vault as the
> firmware signing key or the device is OTA-only forever.

### 3.1 Baseline (before touching anything)

```bash
# Archive the current eFuse state (forensic baseline, one file per device)
espefuse.py --port PORT summary | tee plts-<serial>-efuse-baseline.txt
espefuse.py --port PORT dump --file plts-<serial>-efuse-baseline.bin

# Confirm the device posture as the firmware sees it
# (needs an auth token — see docs/AUDIT_2026_09_FOLLOWUP.md round 2 §auth)
curl -s -H "Authorization: Bearer $JWT" http://<device>/api/security | jq .data
# Expect BEFORE provisioning: flashEncryption.enabled=false, provisioning=FAIL (production)
```

Also record that the flashed image is the audited artifact
(**plaintext read-back mode — only valid before encryption is enabled**):

```bash
python3 scripts/verify_flashed_image.py \
  --port /dev/ttyUSB0 --release-json release.json
```

### 3.2 Generate and burn the per-device flash-encryption key

```bash
# 256-bit key for classic ESP32 flash encryption (AES-256, non-XTS variant)
espsecure.py generate_flash_encryption_key plts-<serial>-flashkey.bin

# Vault the key (same handling as the Ed25519 firmware signing key).
# ONE-TIME action — BLK1 can only be written once:
espefuse.py --port PORT burn_key flash_encryption plts-<serial>-flashkey.bin
```

### 3.3 Enable encryption (1 bit = development-mode parity)

```bash
espefuse.py --port PORT burn-efuse FLASH_CRYPT_CNT
```

### 3.4 Re-flash everything encrypted

Between 3.3 and 3.4 the on-flash images are effectively "wrong" for the new
decryption mode — the device will not boot normally. That is expected; the
ROM download mode does not execute flash, so `esptool` still works. Flash a
clean, fully-encrypted image set:

```bash
esptool.py --port PORT erase_flash

# Partition table stays PLAINTEXT (ROM reads it before decryption is active)
esptool.py --port PORT write_flash 0x8000 partitions.bin

# Bootloader + app must be flashed ENCRYPTED (ROM download-mode encrypts
# on the fly using the eFuse key — the host never sees the key)
esptool.py --port PORT write_flash --encrypt \
  0x1000 bootloader.bin \
  0x20000 firmware.bin          # app0 (ota_0) — our active slot for fresh units
```

Notes:
- NVS (0x9000) and otadata (0x19000) need nothing: the app recreates NVS
  at runtime, and NVS is *deliberately* not flash-encrypted (it is accessed
  via direct SPI reads that bypass the decrypting cache — see §5.3).
- Later OTA updates write encrypted automatically — `esp_partition_write`
  handles it (verified against IDF v4.4 OTA docs; no firmware change needed).
- If you must re-flash a plaintext build later (development mode only),
  repeat the two `write_flash` commands with `--encrypt` on the same
  offsets.

### 3.5 Verify (the acceptance evidence)

```bash
# Device self-report — flashEncryption must now be true, and the
# running-image digest must equal the release manifest digest:
curl -s -H "Authorization: Bearer $JWT" http://<device>/api/security | jq .data
#   .flashEncryption.enabled == true
#   .runningImage.sha256 == release.json firmwareSha256
#   .efuseAntiRollback.burnSupported == true (BLK3 coding scheme NONE)
#   .provisioning == "DEGRADED" (encryption on, secure boot still off — expected until Gate B)

# Automated check (exit 0 = the running image IS the audited artifact):
python3 scripts/verify_flashed_image.py \
  --device-url http://<device> --token "$JWT" --release-json release.json

# Independent eFuse confirmation:
espefuse.py --port PORT summary | grep -A2 "FLASH_CRYPT_CNT"
```

Archive the post-state exactly like the baseline
(`summary | tee plts-<serial>-efuse-provisioned.txt`). The pair
(baseline, provisioned) + the `/api/security` dump + the
`verify_flashed_image.py` PASS output is the per-device provisioning
evidence the auditor's item 2 asks for.

### 3.6 Optional hardening — lock encryption (only after fleet processes are proven)

Once OTA-on-encrypted has been proven on the fleet for a full cycle:

```bash
# Burn the remaining 6 bits: FLASH_CRYPT_CNT = 0x7F → encryption permanent,
# cannot be toggled off even by the operator:
espefuse.py --port PORT burn-efuse FLASH_CRYPT_CNT   # repeat until 7 bits read 1

# Close the UART read holes (release-mode semantics):
espefuse.py --port PORT burn-efuse DISABLE_DL_DECRYPT
espefuse.py --port PORT burn-efuse DISABLE_DL_CACHE
```

After this point the device can only be updated via OTA (which is signed +
floor-gated) — plan RMA accordingly.

## 4. Anti-rollback epochs — operator contract

- The device burns **one eFuse bit per confirmed (major, minor) advance**,
  automatically, in production builds only. No operator action needed.
- The OTA floor blocks downgrades **across** a burned epoch; patch-level
  moves within the epoch are always allowed. Example: after 1.9 → 1.10
  burns epoch 1, OTA to 1.9.x is refused forever (that is the point);
  1.10.0 → 1.10.7 stays free.
- `/api/security` → `ledger` shows the epoch, the version it represents and
  its status (`OK` / `SELF_HEALED` / `TAMPER` / `NO_LEDGER` / `BURN_FAIL`).
  Any status other than OK/SELF_HEALED refuses OTA — re-provision the unit
  per §4.1 before returning it to service.
- Physical escape hatch: a UART re-flash of a signed newer image is always
  possible for recovery (Gate A mode) — the eFuse floor is about the OTA
  path; physical flash paths are closed by Gate A/B instead.

### 4.1 Ledger recovery (NVS loss / tamper)

`NO_LEDGER` (NVS wiped but floor > 0) cannot be auto-recovered safely — the
chip cannot map burned bits back to versions. Return the unit to the bench
and re-flash the CURRENT release over UART; the boot reconcile will
self-heal only when the running image is exactly one epoch newer than the
ledger (the provable mid-activation power-loss case). If it stays
`NO_LEDGER`/`TAMPER`, quarantine the device — that state is rollback
evidence, not a bug.

## 5. Honest limits of Gate A (do not over-claim)

1. **Gate A does not stop execution of unsigned code.** Flash encryption
   protects data at rest; the prebuilt Arduino bootloader still happily
   boots any correctly-formatted image (see Gate B).
2. **The bootloader is not secure-boot-verified.** Someone with UART access
   can flash their own bootloader; the attacker model Gate A covers is
   flash-theft/read-out and casual tampering, not full re-provisioning.
3. **NVS is not encrypted.** On classic ESP32, NVS is accessed by direct SPI
   reads (bypassing the decrypting cache), so it stays plaintext on flash.
   Secrets stored in NVS (Wi-Fi credentials, refresh tokens, device secret)
   are readable by desoldering the chip. Mitigation options: NVS encryption
   (needs an `nvs_keys` partition → partition-table change → release-gated,
   tracked as follow-up) and minimizing NVS-resident secret value.
4. **The eFuse floor is app-enforced, not bootloader-enforced, until Gate
   B.** Layer 2A closes the OTA downgrade path *through this firmware*; a
   full re-flash bypasses it (that is Gate A/B's job).

## 6. Gate B — Secure Boot + bootloader-enforced anti-rollback (roadmap)

Classic-ESP32 Secure Boot V1 (AES-256 digest, ABS_DONE_0) requires the
bootloader to be **built with secure boot enabled** (`CONFIG_SECURE_BOOT_
ENABLED=y` + self-signing flow: bootloader digest burned into BLK2, image
signature blocks appended). The prebuilt arduino-esp32 2.0.17 bootloader has
`CONFIG_SECURE_BOOT` **not set** — it cannot be enabled after the fact.

The migration that unlocks it (no application code changes):

1. Move the PlatformIO build to mixed mode (`framework = arduino, espidf`),
   which compiles the Arduino core as an IDF component and builds the
   bootloader from project-local `sdkconfig.defaults`.
2. Set in `sdkconfig.defaults`:
   - `CONFIG_SECURE_BOOT=y`, `CONFIG_SECURE_BOOTLOADER_MODE_RELEASING=y`
     (after bench validation with SIGNED/DEVELOPING modes),
   - `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (already the effective
     runtime contract — W13-1),
   - `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` +
     `CONFIG_BOOTLOADER_APP_SECURE_VERSION_SIZE=32` — this makes the
     BOOTLOADER enforce the exact eFuse floor the app already burns
     (app-enforced Layer 2A becomes bootloader-enforced Layer 3).
3. Extend the release pipeline to sign app images with the secure-boot key
   (in addition to the existing Ed25519 OTA signature) — key ceremony in the
   same vault, per fleet.
4. Then burn per-device: secure-boot digest (BLK2) + `ABS_DONE_0`, and for
   rev-3+ chips consider Secure Boot V2 (RSA-PSS, ABS_DONE_1).

⚠️ Do NOT enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` before the release
pipeline stamps a real `secure_version` into the app descriptor — Arduino
images today carry 0, so a burned floor of 1 would make the bootloader
reject the only image on the device. The app-level ledger already uses
IDF-compatible semantics precisely so this migration is a config change,
not a redesign.

## 7. Per-device acceptance checklist (archive with the device record)

| # | Check | Evidence |
|---|-------|----------|
| 1 | Running image = audited artifact | `verify_flashed_image.py --device-url` exit 0 |
| 2 | Flash encryption ON | `/api/security` `flashEncryption.enabled=true` + `espefuse summary` |
| 3 | Secure boot state recorded | `/api/security` `secureBoot.v1/v2` (expected false until Gate B) |
| 4 | Anti-rollback floor sane | `/api/security` `ledger.status` ∈ {OK, SELF_HEALED} |
| 5 | Provisioning verdict acceptable | `provisioning` ∈ {PASS, DEGRADED-with-justification} |
| 6 | eFuse baseline + provisioned dumps archived | `plts-<serial>-efuse-*.txt` |
| 7 | Key escrowed | flash-encryption key + (future) secure-boot key in vault |

## 8. Command cheat-sheet

```bash
espefuse.py --port PORT summary                       # read everything (safe)
espefuse.py --port PORT dump --file f.bin             # binary backup (safe)
espsecure.py generate_flash_encryption_key key.bin    # 256-bit key (host)
espefuse.py --port PORT burn_key flash_encryption key.bin   # ONE-WAY
espefuse.py --port PORT burn-efuse FLASH_CRYPT_CNT    # ONE-WAY (parity on)
esptool.py --port PORT write_flash --encrypt 0x1000 bootloader.bin 0x20000 firmware.bin
esptool.py --port PORT write_flash 0x8000 partitions.bin  # stays plaintext
```

References: ESP-IDF v4.4 `docs/en/security/flash-encryption.rst`,
`docs/en/security/secure-boot-v1.rst`, `components/efuse/esp32/esp_efuse_table.csv`
(SECURE_VERSION @ BLK3 bit 128, 32 bits; ABS_DONE_0/1 @ BLK0; FLASH_CRYPT_CNT
@ BLK0 bit 20, 7 bits, odd parity).
