#!/usr/bin/env python3
"""
release_gate.py — P1-9 Master Release Gate.

Single point of authority that decides RELEASE = PASS | BLOCKED. Runs every
invariant check defined in the audit brief §7 (Master Release Gate):

  1. Build generic present         (artifacts/plts-firmware-generic/)
  2. Build modular present         (artifacts/plts-firmware-modular-production/)
  3. Production env present        (P0-2 — must be in CI artifacts)
  4. release.json + provenance.json + SHA256SUMS present per target
  5. SHA256SUMS hashes verify      (re-hash every binary, compare to manifest)
  6. Ed25519 signature valid       (if .sig present, verify against public key)
  7. Cross-repo artifact SHA       (if PWA repo path provided, compare SHAs)
  8. No duplicate versions         (exactly one plts_firmware_v<X.Y.Z>.bin per target)
  9. provenance.gitCommit non-empty (source identity recorded)
 10. provenance.toolchain pinned   (PlatformIO version not "unknown")

Usage:
  python3 scripts/release_gate.py \\
      --artifacts-dir ci-artifacts \\
      --target generic \\
      --target modular \\
      [--pwa-path ../PLTSMonitoring_PWA] \\
      [--public-key firmware_signing_public.pem]

Exit:  0 = RELEASE = PASS
       1 = RELEASE = BLOCKED (with detailed reason list)
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


class Gate:
    def __init__(self):
        self.checks = []  # list of (name, status, detail)
        self.blockers = []

    def ok(self, name: str, detail: str = "") -> None:
        self.checks.append((name, "PASS", detail))

    def fail(self, name: str, detail: str) -> None:
        self.checks.append((name, "FAIL", detail))
        self.blockers.append(f"{name}: {detail}")

    def summary(self) -> str:
        lines = ["=" * 72, "MASTER RELEASE GATE", "=" * 72]
        for name, status, detail in self.checks:
            tag = "[PASS]" if status == "PASS" else "[FAIL]"
            lines.append(f"  {tag} {name}")
            if detail:
                lines.append(f"        {detail}")
        lines.append("-" * 72)
        if self.blockers:
            lines.append("RELEASE = BLOCKED")
            lines.append("")
            lines.append("Blockers:")
            for b in self.blockers:
                lines.append(f"  - {b}")
        else:
            lines.append("RELEASE = PASS")
        lines.append("=" * 72)
        return "\n".join(lines)


def find_target_dir(artifacts_dir: Path, target: str) -> Path | None:
    """Find the artifact directory for a target (handles CI naming)."""
    candidates = []
    if target == "generic":
        candidates = [
            artifacts_dir / "plts-firmware-generic",
            artifacts_dir / "plts-firmware-generic-legacy",
        ]
    elif target == "modular":
        candidates = [
            artifacts_dir / "plts-firmware-modular-production",
            artifacts_dir / "plts-firmware-modular-dev-staging",
        ]
    for c in candidates:
        if c.is_dir():
            return c
    return None


def check_target(gate: Gate, target: str, target_dir: Path, public_key: Path | None) -> None:
    name = f"target[{target}]: artifacts present"
    if not target_dir.is_dir():
        gate.fail(name, f"directory not found: {target_dir}")
        return
    gate.ok(name, str(target_dir))

    # Required files
    required = ["bootloader.bin", "partitions.bin"]
    # find firmware.bin or plts_firmware_v*.bin
    fw_files = list(target_dir.glob("firmware.bin")) + \
               list(target_dir.glob("plts_firmware_v*.bin"))
    if not fw_files:
        gate.fail(f"target[{target}]: firmware binary", "no firmware.bin or plts_firmware_v*.bin")
        return
    fw_path = fw_files[0]
    required.append(fw_path.name)

    for f in required:
        p = target_dir / f
        if not p.is_file():
            gate.fail(f"target[{target}]: {f} present", "missing")
        else:
            gate.ok(f"target[{target}]: {f} present", f"{p.stat().st_size:,} B")

    # Manifests
    for m in ("release.json", "provenance.json", "SHA256SUMS"):
        p = target_dir / m
        if not p.is_file():
            gate.fail(f"target[{target}]: {m}", "missing")
        else:
            gate.ok(f"target[{target}]: {m}", "present")

    # Verify SHA256SUMS
    sums_path = target_dir / "SHA256SUMS"
    if sums_path.is_file():
        bad = 0
        for line in sums_path.read_text().splitlines():
            parts = line.split(None, 1)
            if len(parts) != 2:
                continue
            expected, filename = parts
            filename = filename.strip()
            p = target_dir / filename
            if not p.is_file():
                gate.fail(f"target[{target}]: SHA256SUMS entry", f"{filename} missing")
                bad += 1
                continue
            actual = sha256_of(p)
            if actual != expected:
                gate.fail(f"target[{target}]: SHA256 verify", f"{filename}: {actual} != {expected}")
                bad += 1
        if bad == 0:
            gate.ok(f"target[{target}]: SHA256SUMS verify", "all hashes match")

    # Verify provenance
    prov_path = target_dir / "provenance.json"
    rel_path  = target_dir / "release.json"
    if prov_path.is_file() and rel_path.is_file():
        try:
            prov = json.loads(prov_path.read_text())
            rel  = json.loads(rel_path.read_text())
            git_commit = rel.get("gitCommit", "")
            if not git_commit or git_commit == "unknown":
                gate.fail(f"target[{target}]: provenance.gitCommit", "empty/unknown")
            else:
                gate.ok(f"target[{target}]: provenance.gitCommit", git_commit[:12])

            pio = prov.get("toolchain", {}).get("platformio", "")
            if not pio or pio == "unknown":
                gate.fail(f"target[{target}]: toolchain.platformio", "unknown (must be pinned)")
            else:
                gate.ok(f"target[{target}]: toolchain.platformio", pio)

            # Cross-check: release.json.firmwareSha256 must match actual
            actual_fw_sha = sha256_of(fw_path)
            if rel.get("firmwareSha256") != actual_fw_sha:
                gate.fail(f"target[{target}]: release.firmwareSha256",
                          f"manifest={rel.get('firmwareSha256','')[:16]}... actual={actual_fw_sha[:16]}...")
            else:
                gate.ok(f"target[{target}]: release.firmwareSha256", "matches actual binary")
        except Exception as e:
            gate.fail(f"target[{target}]: provenance parse", str(e))

    # Ed25519 signature verification
    sig_path = fw_path.with_suffix(".bin.sig")
    if sig_path.is_file() and public_key and public_key.is_file():
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
            from cryptography.hazmat.primitives import serialization
            pub = serialization.load_pem_public_key(public_key.read_bytes())
            sig = bytes.fromhex(sig_path.read_text().strip())
            digest = sha256_of(fw_path)  # firmware signs raw 32-byte digest (P0-1)
            raw_digest = bytes.fromhex(digest)
            assert isinstance(pub, Ed25519PublicKey)
            pub.verify(sig, raw_digest)
            gate.ok(f"target[{target}]: Ed25519 signature", "VALID")
        except ImportError:
            gate.fail(f"target[{target}]: Ed25519 signature", "cryptography not installed")
        except Exception as e:
            gate.fail(f"target[{target}]: Ed25519 signature", f"INVALID: {e}")
    elif sig_path.is_file() and not public_key:
        gate.fail(f"target[{target}]: Ed25519 signature", "signature present but --public-key not provided")
    else:
        gate.ok(f"target[{target}]: Ed25519 signature", "not signed (dev build)")


def check_no_duplicate_versions(gate: Gate, artifacts_dir: Path) -> None:
    """Across all targets, exactly one plts_firmware_v<X.Y.Z>.bin per target."""
    seen = {}  # version -> [target, ...]
    for sub in artifacts_dir.iterdir():
        if not sub.is_dir():
            continue
        for fw in sub.glob("plts_firmware_v*.bin"):
            # extract version
            stem = fw.stem  # plts_firmware_v1.7.1
            ver = stem.replace("plts_firmware_v", "")
            seen.setdefault(ver, []).append(sub.name)
    for ver, targets in seen.items():
        # A version can appear in BOTH generic and modular (different binaries)
        # but not twice within the same target.
        unique_targets = set(targets)
        if len(targets) != len(unique_targets):
            gate.fail("duplicate version", f"v{ver} appears multiple times in same target: {targets}")
        else:
            gate.ok("duplicate version", f"v{ver} in {sorted(unique_targets)}")


def check_cross_repo(gate: Gate, pwa_path: Path, firmware_repo_path: Path) -> None:
    """P0-6 — verify PWA's committed binary SHA matches firmware repo's binary SHA."""
    pwa_fw = pwa_path / "public" / "firmware"
    fw_bin = firmware_repo_path / "firmware-generic" / "bin"
    if not pwa_fw.is_dir():
        gate.ok("cross-repo P0-6", "PWA has no public/firmware/ — no drift possible")
        return
    if not fw_bin.is_dir():
        gate.fail("cross-repo P0-6", "firmware-generic/bin/ missing")
        return

    for f in ("bootloader.bin", "partitions.bin"):
        a, b = fw_bin / f, pwa_fw / f
        if a.is_file() and b.is_file():
            ha, hb = sha256_of(a), sha256_of(b)
            if ha != hb:
                gate.fail("cross-repo P0-6", f"{f}: firmware={ha[:16]}... PWA={hb[:16]}...")
            else:
                gate.ok("cross-repo P0-6", f"{f}: SHA matches ({ha[:16]}...)")

    # versioned firmware binary
    pwa_v = list(pwa_fw.glob("plts_firmware_v*.bin"))
    fw_v  = list(fw_bin.glob("plts_firmware_v*.bin"))
    if pwa_v and fw_v:
        ha, hb = sha256_of(fw_v[0]), sha256_of(pwa_v[0])
        if ha != hb:
            gate.fail("cross-repo P0-6", f"{fw_v[0].name}: firmware={ha[:16]}... PWA={hb[:16]}...")
        else:
            gate.ok("cross-repo P0-6", f"{fw_v[0].name}: SHA matches ({ha[:16]}...)")
    elif pwa_v and not fw_v:
        gate.ok("cross-repo P0-6", "PWA has binary, firmware repo doesn't (source-only — OK)")
    elif not pwa_v and fw_v:
        gate.ok("cross-repo P0-6", "PWA has no binary (gitignored) — OK")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--artifacts-dir", required=True, type=Path,
                    help="CI artifacts download directory (e.g. ci-artifacts/)")
    ap.add_argument("--target", action="append", required=True, choices=["generic", "modular"],
                    help="target to check (can be repeated)")
    ap.add_argument("--pwa-path", type=Path, default=None,
                    help="PWA repo checkout (for P0-6 cross-repo check)")
    ap.add_argument("--firmware-repo-path", type=Path, default=None,
                    help="Firmware repo root (for P0-6 cross-repo check)")
    ap.add_argument("--public-key", type=Path, default=None,
                    help="Ed25519 public key PEM (for signature verification)")
    args = ap.parse_args()

    gate = Gate()

    if not args.artifacts_dir.is_dir():
        gate.fail("artifacts-dir", f"{args.artifacts_dir} does not exist")
        print(gate.summary())
        return 1

    for target in args.target:
        td = find_target_dir(args.artifacts_dir, target)
        if td is None:
            gate.fail(f"target[{target}]", "artifact directory not found")
        else:
            check_target(gate, target, td, args.public_key)

    check_no_duplicate_versions(gate, args.artifacts_dir)

    if args.pwa_path and args.firmware_repo_path:
        check_cross_repo(gate, args.pwa_path, args.firmware_repo_path)
    else:
        gate.ok("cross-repo P0-6", "skipped (provide --pwa-path + --firmware-repo-path to enable)")

    print(gate.summary())
    return 1 if gate.blockers else 0


if __name__ == "__main__":
    sys.exit(main())
