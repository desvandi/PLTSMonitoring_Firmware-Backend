#!/usr/bin/env python3
"""
generate_canonical_manifest.py — [Audit 8 P1-2] Canonical OTA manifest (schema 2.0).

The legacy manifest.json (schema 1.0) is for ESP Web Tools browser flashing —
it has NO SHA-256, NO signature, NO provenance. This generator produces a
canonical OTA manifest (schema 2.0) that carries the full chain of trust:

  manifest.json (schema 2.0)
    ├─ schemaVersion: "2.0"
    ├─ version
    ├─ target
    ├─ releaseId   (matches release.json.buildId)
    ├─ gitCommit   (matches release.json.gitCommit)
    ├─ artifacts:
    │   ├─ bootloader: { path, sha256, size }
    │   ├─ partitions: { path, sha256, size }
    │   └─ firmware:   { path, sha256, size, signature, signatureAlgorithm }
    ├─ toolchain   (from provenance.json)
    └─ generatedAt

The device's OTA verifier (firmware/Services/OtaManager) consumes this manifest
to validate each artifact BEFORE flashing:
  1. Download artifact
  2. Compute SHA-256
  3. Compare to manifest.artifacts.<name>.sha256
  4. Verify Ed25519 signature (firmware only) against OTA_ED25519_PUBLIC_KEY_HEX
  5. Flash only if ALL checks pass

Usage:
  python3 scripts/generate_canonical_manifest.py \\
      --release-json firmware-generic/bin/release.json \\
      --provenance-json firmware-generic/bin/provenance.json \\
      --bin-dir firmware-generic/bin \\
      --out firmware-generic/bin/manifest-canonical.json

The legacy manifest.json is preserved for ESP Web Tools compatibility.
The canonical manifest is the OTA authority.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


def sha256_of(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--release-json", required=True, type=Path,
                    help="release.json produced by generate_provenance.py")
    ap.add_argument("--provenance-json", required=True, type=Path,
                    help="provenance.json produced by generate_provenance.py")
    ap.add_argument("--bin-dir", required=True, type=Path,
                    help="directory containing bootloader.bin, partitions.bin, firmware.bin")
    ap.add_argument("--out", required=True, type=Path,
                    help="output manifest-canonical.json path")
    args = ap.parse_args()

    if not args.release_json.is_file():
        print(f"ERROR: release.json not found: {args.release_json}", file=sys.stderr)
        return 1
    if not args.provenance_json.is_file():
        print(f"ERROR: provenance.json not found: {args.provenance_json}", file=sys.stderr)
        return 1
    if not args.bin_dir.is_dir():
        print(f"ERROR: bin-dir not found: {args.bin_dir}", file=sys.stderr)
        return 1

    rel = json.loads(args.release_json.read_text())
    prov = json.loads(args.provenance_json.read_text())

    fw_name = rel.get("firmwareFilename", "firmware.bin")
    fw_path = args.bin_dir / fw_name
    bl_path = args.bin_dir / "bootloader.bin"
    pt_path = args.bin_dir / "partitions.bin"

    for label, p in [("bootloader", bl_path), ("partitions", pt_path), ("firmware", fw_path)]:
        if not p.is_file():
            print(f"ERROR: {label} artifact missing: {p}", file=sys.stderr)
            return 1

    # Read signature if present (produced by sign_firmware.py)
    sig_path = fw_path.with_suffix(".bin.sig")
    signature = None
    sig_algorithm = None
    if sig_path.is_file():
        signature = sig_path.read_text().strip()
        sig_algorithm = "ed25519-sha256-raw-digest"

    manifest = {
        "schemaVersion": "2.0",
        "version":       rel["version"],
        "target":        rel["target"],
        "releaseId":     rel["buildId"],
        "gitCommit":     rel["gitCommit"],
        "buildTimestamp": rel["buildTimestamp"],
        "artifacts": {
            "bootloader": {
                "path":   bl_path.name,
                "sha256": sha256_of(bl_path),
                "size":   bl_path.stat().st_size,
                "offset": 4096,
            },
            "partitions": {
                "path":   pt_path.name,
                "sha256": sha256_of(pt_path),
                "size":   pt_path.stat().st_size,
                "offset": 32768,
            },
            "firmware": {
                "path":              fw_path.name,
                "sha256":            sha256_of(fw_path),
                "size":              fw_path.stat().st_size,
                "offset":            65536,
                "signature":         signature,
                "signatureAlgorithm": sig_algorithm,
                "signatureSha256":   sha256_of(sig_path) if sig_path.is_file() else None,
            },
        },
        "toolchain": prov.get("toolchain", {}),
        "canonicalSource": "GitHub Release (immutable)",
        "generatedAt": rel["buildTimestamp"],
        "notes": [
            "Schema 2.0 canonical OTA manifest — carries full chain of trust.",
            "Each artifact has an immutable SHA-256; firmware also has Ed25519 signature.",
            "The device OTA verifier MUST validate SHA-256 + signature before flashing.",
            "This manifest is the OTA authority — the legacy manifest.json is for ESP Web Tools only.",
        ],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[canonical-manifest] PASS — wrote {args.out}")
    print(f"  schemaVersion: {manifest['schemaVersion']}")
    print(f"  version:       {manifest['version']}")
    print(f"  target:        {manifest['target']}")
    print(f"  releaseId:     {manifest['releaseId']}")
    print(f"  gitCommit:     {manifest['gitCommit'][:12]}...")
    print(f"  firmware sha:  {manifest['artifacts']['firmware']['sha256'][:16]}...")
    print(f"  firmware sig:  {'present' if signature else 'NONE (dev build)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
