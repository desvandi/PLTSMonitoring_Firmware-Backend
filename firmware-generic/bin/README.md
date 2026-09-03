# Firmware Binaries — Distribution Policy

> **P0-4 / P0-6 AUDIT 2026-09**: This directory is **gitignored**. Binaries are
> **NOT source code** and must not be committed to the repository.

## Where binaries live

| Channel | Purpose | Location |
|---------|---------|----------|
| **GitHub Actions artifacts** | Every CI build (transient, immutable per run) | `Actions → Run → plts-firmware-*` |
| **GitHub Releases** | Tagged releases (persistent, immutable) | `https://github.com/desvandi/PLTSMonitoring_Firmware-Backend/releases/tag/v<X.Y.Z>` |
| **Local `.pio/build`** | Developer build output (private) | `firmware-generic/.pio/build/esp32dev/` |

## How to populate this directory locally

For ESP Web Tools (browser-based flashing) you need the binaries on disk so
the PWA can serve them. Run the release script:

```bash
# from firmware repo root
python3 scripts/release_firmware_generic.py --pwa-path ../PLTSMonitoring_PWA
```

This script:
1. Builds `firmware-generic` from source (clean checkout — no `--skip-build`)
2. Stages `bootloader.bin`, `partitions.bin`, `plts_firmware_v{version}.bin` here
3. Generates `release.json`, `provenance.json`, `SHA256SUMS`
4. Syncs the SAME binaries (with cross-repo SHA verification) to `PWA/public/firmware/`

## Cross-repo integrity

`release_firmware_generic.py` enforces P0-6: after sync, SHA-256 of each binary
in `Firmware-Backend/firmware-generic/bin/` MUST equal the SHA-256 of the
matching file in `PWA/public/firmware/`. Any drift aborts the release.

## Reproducibility

To verify a binary's provenance, compare its SHA-256 against the
`SHA256SUMS` file published in the GitHub Release for that version:

```bash
sha256sum bootloader.bin partitions.bin plts_firmware_v*.bin
# must match SHA256SUMS from the GitHub Release
```
