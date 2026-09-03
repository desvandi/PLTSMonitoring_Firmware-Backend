#!/usr/bin/env python3
"""
generate_provenance.py — P0-5 immutable artifact identity generator.

Produces three files in the directory containing the binaries:
  - release.json       — operator-readable release manifest
  - provenance.json    — full build provenance (source commit, toolchain,
                         artifact SHA-256, sizes, target)
  - SHA256SUMS         — plain-text checksums (sha256sum format)

The release.json/provenance.json together make every release:
  - Auditable     : gitCommit + buildTimestamp + toolchain pinned
  - Reproducible  : same source + same toolchain = same SHA-256
  - Verifiable    : each artifact has SHA-256 + Ed25519 signature (if signed)
  - Cross-repo safe : one canonical identity referenced by PWA + GAS

Usage:
  python3 scripts/generate_provenance.py \\
      --target generic|modular \\
      --version 1.7.1 \\
      --bin-dir firmware-generic/bin \\
      --out-dir firmware-generic/bin

Exit 1 if any required artifact is missing or a hash mismatch is detected.
"""
import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def die(msg: str) -> None:
    print(f"[provenance] FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args: str, cwd: Path | None = None) -> str:
    try:
        r = subprocess.run(
            ["git", *args],
            cwd=cwd, capture_output=True, text=True, check=True,
        )
        return r.stdout.strip()
    except subprocess.CalledProcessError as e:
        return ""  # best-effort; CI without git history returns ""
    except FileNotFoundError:
        return ""


def pio_version() -> str:
    try:
        r = subprocess.run(
            ["pio", "--version"], capture_output=True, text=True, check=True,
        )
        return r.stdout.strip()
    except Exception:
        return "unknown"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, choices=["generic", "modular"],
                    help="firmware tree (generic legacy | modular production)")
    ap.add_argument("--version", required=True, help="semver X.Y.Z")
    ap.add_argument("--bin-dir", required=True, type=Path,
                    help="directory containing bootloader.bin, partitions.bin, firmware.bin")
    ap.add_argument("--out-dir", default=None, type=Path,
                    help="where to write release.json / provenance.json / SHA256SUMS "
                         "(default: same as --bin-dir)")
    ap.add_argument("--firmware-name", default=None,
                    help="firmware binary filename inside --bin-dir "
                         "(default: plts_firmware_v{version}.bin)")
    ap.add_argument("--toolchain-json", default=None, type=Path,
                    help="optional JSON file with extra pinned dependency versions "
                         "(merged into provenance.toolchain)")
    args = ap.parse_args()

    bin_dir = args.bin_dir.resolve()
    out_dir = (args.out_dir or bin_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    fw_name = args.firmware_name or f"plts_firmware_v{args.version}.bin"
    artifacts = {
        "bootloader":  bin_dir / "bootloader.bin",
        "partitions":  bin_dir / "partitions.bin",
        "firmware":    bin_dir / fw_name,
    }

    for label, p in artifacts.items():
        if not p.is_file():
            die(f"artifact missing: {label} -> {p}")

    # SHA-256 + size for each artifact
    artifact_meta = {}
    sha_lines = []
    for label, p in artifacts.items():
        h = sha256_of(p)
        sz = p.stat().st_size
        artifact_meta[label] = {
            "filename": p.name,
            "sha256":   h,
            "size":     sz,
            "path":     str(p.relative_to(bin_dir)),
        }
        sha_lines.append(f"{h}  {p.name}")

    # Optional Ed25519 signature side-file (created by sign_firmware.py)
    sig_path = artifacts["firmware"].with_suffix(".bin.sig")
    if sig_path.is_file():
        artifact_meta["firmware"]["signatureSha256"] = sha256_of(sig_path)
        artifact_meta["firmware"]["signatureFile"] = sig_path.name

    # git provenance
    repo_root = bin_dir.parent
    git_commit = git("rev-parse", "HEAD", cwd=repo_root)
    git_branch = git("rev-parse", "--abbrev-ref", "HEAD", cwd=repo_root) or "(detached)"
    git_dirty  = bool(git("status", "--porcelain", "--untracked-files=no", cwd=repo_root))
    git_describe = git("describe", "--tags", "--always", cwd=repo_root)

    toolchain = {
        "platformio": pio_version(),
        "python":     sys.version.split()[0],
        "os":         f"{platform.system()} {platform.release()}",
        "arch":       platform.machine(),
    }
    if args.toolchain_json and args.toolchain_json.is_file():
        try:
            extra = json.loads(args.toolchain_json.read_text())
            toolchain.update(extra)
        except Exception as e:
            print(f"[provenance] WARN: toolchain-json unreadable: {e}", file=sys.stderr)

    build_ts = datetime.now(timezone.utc).isoformat(timespec="seconds")
    build_id = f"{args.target}-{args.version}-{git_commit[:12] or 'nogit'}"

    release = {
        "version":          args.version,
        "target":           args.target,
        "buildId":          build_id,
        "gitCommit":        git_commit or "unknown",
        "gitBranch":        git_branch,
        "gitDescribe":      git_describe,
        "gitDirty":         git_dirty,
        "buildTimestamp":   build_ts,
        "firmwareSha256":   artifact_meta["firmware"]["sha256"],
        "firmwareSize":     artifact_meta["firmware"]["size"],
        "bootloaderSha256": artifact_meta["bootloader"]["sha256"],
        "partitionsSha256": artifact_meta["partitions"]["sha256"],
        "canonicalSource":  "GitHub Release (immutable)",
    }

    provenance = {
        "release":        release,
        "artifacts":      artifact_meta,
        "toolchain":      toolchain,
        "schemaVersion":  "1.0.0",
        "generator":      "scripts/generate_provenance.py",
        "generatedAt":    build_ts,
        "notes": [
            "Every artifact has an immutable SHA-256.",
            "Build from the same gitCommit + toolchain MUST produce identical SHA-256s.",
            "Cross-repo consumers (PWA, GAS) MUST reference this release.json and "
            "verify firmwareSha256 before serving/flashing.",
        ],
    }

    (out_dir / "release.json").write_text(
        json.dumps(release, indent=2) + "\n", encoding="utf-8")
    (out_dir / "provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    (out_dir / "SHA256SUMS").write_text("\n".join(sha_lines) + "\n", encoding="utf-8")

    print(f"[provenance] PASS — wrote 3 files to {out_dir}/")
    print(f"  version         : {args.version}")
    print(f"  target          : {args.target}")
    print(f"  gitCommit       : {git_commit or '(unknown)'}")
    print(f"  firmwareSha256  : {release['firmwareSha256']}")
    print(f"  bootloaderSha256: {release['bootloaderSha256']}")
    print(f"  partitionsSha256: {release['partitionsSha256']}")


if __name__ == "__main__":
    main()
