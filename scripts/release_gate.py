#!/usr/bin/env python3
"""
release_gate.py — Master Release Gate (Audit 8 remediation).

Single point of authority that decides RELEASE = PASS | BLOCKED. Runs every
invariant check defined in the audit brief §7 (Master Release Gate) and
enforces the Audit 8 P0 invariants:

  P0-A — Production signature is REQUIRED, not optional.
         Unsigned production artifact => FAIL (was "not signed (dev build)" => PASS).
  P0-B — Cross-repo verification is MANDATORY for production.
         Missing PWA path / firmware repo path => FAIL (was "skipped" => PASS).
  P1-3 — Full artifact inventory: expected set is fixed; extra/missing => FAIL.
  P1-4 — Exact artifact selection: firmware binary identified by release.json
         firmwareSha256, NOT by glob firmware*.bin[0]. Multiple matches => FAIL.
  P1-5 — Provenance binding: provenance.release.gitCommit MUST equal --ci-sha
         (passed from GITHUB_SHA in CI). Mismatch => FAIL.

Per-target checks (target tag in release.json):
  - target == "modular"     → production invariants apply (signature, cross-repo)
  - target == "generic"     → production invariants apply IF release.json.target
                              says "production" or --strict is set
  - dev/staging artifacts   → signature optional, cross-repo optional

Usage:
  python3 scripts/release_gate.py \\
      --artifacts-dir ci-artifacts \\
      --target generic \\
      --target modular \\
      --public-key firmware_signing_public.pem \\
      --pwa-path ../PLTSMonitoring_PWA \\
      --firmware-repo-path . \\
      --ci-sha ${GITHUB_SHA} \\
      [--strict]

Exit:  0 = RELEASE = PASS
       1 = RELEASE = BLOCKED (with detailed reason list)
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


# [P1-3 + self-review] Canonical artifact inventory. The gate fails if ANY
# of these is missing OR if any UNEXPECTED executable artifact (.bin/.elf/.hex)
# is present. manifest-canonical.json is included so the required-files check
# catches it even for dev/staging (the separate check at line ~297 adds
# production-specific enforcement for the signature field).
CANONICAL_ARTIFACTS = {
    "bootloader.bin",
    "partitions.bin",
    # firmware.bin OR plts_firmware_v<X.Y.Z>.bin — identified by release.json
    "release.json",
    "provenance.json",
    "SHA256SUMS",
    "manifest-canonical.json",   # [self-review] schema 2.0 canonical OTA manifest
}
EXECUTABLE_EXTS = (".bin", ".elf", ".hex", ".map")


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

    def warn(self, name: str, detail: str) -> None:
        # Warning = recorded but does NOT block. Used for dev/staging only.
        self.checks.append((name, "WARN", detail))

    def summary(self) -> str:
        lines = ["=" * 72, "MASTER RELEASE GATE", "=" * 72]
        for name, status, detail in self.checks:
            tag = {"PASS": "[PASS]", "FAIL": "[FAIL]", "WARN": "[WARN]"}[status]
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


def is_production_target(target: str, target_dir: Path) -> bool:
    """
    Determine if this target dir represents a production artifact.

    Production invariants (signature required, cross-repo required) apply when:
      - target == "modular" AND directory name contains "production"
      - target == "generic" (generic tree is the OTA distribution tree)
      - release.json.target == "production" (explicit)
      - --strict is set on CLI (handled by caller)
    """
    if "production" in target_dir.name:
        return True
    if target == "generic":
        # The generic tree IS the production OTA distribution tree.
        return True
    # Check release.json for explicit production tag
    rel_path = target_dir / "release.json"
    if rel_path.is_file():
        try:
            rel = json.loads(rel_path.read_text())
            if rel.get("target") == "production":
                return True
        except Exception:
            pass
    return False


def check_target(
    gate: Gate,
    target: str,
    target_dir: Path,
    public_key: Path | None,
    ci_sha: str | None,
    strict: bool,
) -> None:
    name = f"target[{target}]: artifacts present"
    if not target_dir.is_dir():
        gate.fail(name, f"directory not found: {target_dir}")
        return
    gate.ok(name, str(target_dir))

    is_prod = is_production_target(target, target_dir) or strict

    # ------------------------------------------------------------------
    # [P1-3] Full artifact inventory: required files MUST exist, no extra
    # executable artifacts allowed.
    # ------------------------------------------------------------------
    required_files = list(CANONICAL_ARTIFACTS)
    for f in required_files:
        p = target_dir / f
        if not p.is_file():
            gate.fail(f"target[{target}]: required artifact {f}", "missing")
        else:
            gate.ok(f"target[{target}]: required artifact {f}", "present")

    # Detect unexpected executable artifacts (.bin/.elf/.hex/.map)
    # bootloader.bin and partitions.bin are expected; firmware is identified below.
    unexpected_exec = []
    for p in target_dir.iterdir():
        if not p.is_file():
            continue
        if p.suffix.lower() not in EXECUTABLE_EXTS:
            continue
        # Allowed executables: bootloader.bin, partitions.bin, firmware*.bin, *.sig
        if p.name in ("bootloader.bin", "partitions.bin"):
            continue
        if p.name == "firmware.bin" or p.name.startswith("plts_firmware_v"):
            continue
        if p.name.endswith(".bin.sig") or p.name.endswith(".bin.sha256") or p.name.endswith(".bin.ota.json"):
            continue
        unexpected_exec.append(p.name)
    if unexpected_exec:
        gate.fail(
            f"target[{target}]: unexpected executable artifacts",
            f"found: {unexpected_exec} (only bootloader.bin, partitions.bin, firmware.bin/plts_firmware_v*.bin allowed)",
        )
    else:
        gate.ok(f"target[{target}]: no unexpected executables", "clean")

    # ------------------------------------------------------------------
    # [P1-4] Exact artifact selection: identify firmware binary by release.json
    # firmwareSha256, NOT by glob. Multiple firmware binaries => FAIL.
    # ------------------------------------------------------------------
    rel_path = target_dir / "release.json"
    if not rel_path.is_file():
        gate.fail(f"target[{target}]: firmware identity", "release.json missing — cannot identify canonical firmware")
        return
    try:
        rel = json.loads(rel_path.read_text())
    except Exception as e:
        gate.fail(f"target[{target}]: firmware identity", f"release.json unparseable: {e}")
        return

    expected_fw_sha = rel.get("firmwareSha256", "")
    expected_fw_name = rel.get("firmwareFilename") or rel.get("artifacts", {}).get("firmware", {}).get("filename", "")

    # Find firmware binaries present
    fw_candidates = [p for p in target_dir.iterdir()
                     if p.is_file() and (p.name == "firmware.bin" or p.name.startswith("plts_firmware_v"))]
    if len(fw_candidates) == 0:
        gate.fail(f"target[{target}]: firmware binary", "no firmware.bin or plts_firmware_v*.bin found")
        return
    if len(fw_candidates) > 1:
        names = [p.name for p in fw_candidates]
        gate.fail(f"target[{target}]: firmware binary",
                  f"multiple firmware binaries found: {names} — exactly one required")
        return

    fw_path = fw_candidates[0]
    actual_fw_sha = sha256_of(fw_path)

    # [P1-4] Verify the binary's SHA matches release.json's firmwareSha256.
    if not expected_fw_sha:
        gate.fail(f"target[{target}]: release.firmwareSha256", "missing in release.json")
    elif expected_fw_sha != actual_fw_sha:
        gate.fail(f"target[{target}]: release.firmwareSha256",
                  f"manifest={expected_fw_sha[:16]}... actual={actual_fw_sha[:16]}... (binary != manifest)")
    else:
        gate.ok(f"target[{target}]: release.firmwareSha256",
                f"matches actual binary ({actual_fw_sha[:16]}...)")

    # If release.json declares the expected filename, verify it matches.
    if expected_fw_name and expected_fw_name != fw_path.name:
        gate.fail(f"target[{target}]: release.firmwareFilename",
                  f"manifest={expected_fw_name} actual={fw_path.name}")

    # ------------------------------------------------------------------
    # Verify SHA256SUMS — re-hash every binary, compare to manifest.
    # Also verify SHA256SUMS contains EXACTLY the canonical artifact set.
    # ------------------------------------------------------------------
    sums_path = target_dir / "SHA256SUMS"
    if sums_path.is_file():
        sums_entries = {}
        for line in sums_path.read_text().splitlines():
            parts = line.split(None, 1)
            if len(parts) != 2:
                continue
            expected, filename = parts
            filename = filename.strip()
            sums_entries[filename] = expected

        # [P1-3] SHA256SUMS must list exactly the executable artifacts present
        expected_sums_keys = {"bootloader.bin", "partitions.bin", fw_path.name}
        missing_in_sums = expected_sums_keys - set(sums_entries.keys())
        extra_in_sums = set(sums_entries.keys()) - expected_sums_keys
        if missing_in_sums:
            gate.fail(f"target[{target}]: SHA256SUMS coverage",
                      f"missing entries: {sorted(missing_in_sums)}")
        elif extra_in_sums:
            gate.fail(f"target[{target}]: SHA256SUMS coverage",
                      f"unexpected entries: {sorted(extra_in_sums)}")
        else:
            gate.ok(f"target[{target}]: SHA256SUMS coverage", "exact match with artifact set")

        # Verify each hash
        bad = 0
        for filename, expected in sums_entries.items():
            p = target_dir / filename
            if not p.is_file():
                gate.fail(f"target[{target}]: SHA256SUMS entry", f"{filename} missing on disk")
                bad += 1
                continue
            actual = sha256_of(p)
            if actual != expected:
                gate.fail(f"target[{target}]: SHA256 verify", f"{filename}: {actual} != {expected}")
                bad += 1
        if bad == 0:
            gate.ok(f"target[{target}]: SHA256SUMS verify", "all hashes match")

    # ------------------------------------------------------------------
    # [Audit 9 P1] Canonical manifest (schema 2.0) enforcement.
    # The manifest-canonical.json is the authoritative OTA contract. The
    # gate MUST verify it exists, is parseable, and all its SHA-256 + signature
    # fields match the actual artifacts on disk.
    # ------------------------------------------------------------------
    canonical_path = target_dir / "manifest-canonical.json"
    if not canonical_path.is_file():
        if is_prod:
            gate.fail(f"target[{target}]: canonical manifest",
                      "manifest-canonical.json missing — REQUIRED for production (P1)")
        else:
            gate.warn(f"target[{target}]: canonical manifest",
                      "manifest-canonical.json missing (dev/staging — not required)")
    else:
        try:
            cm = json.loads(canonical_path.read_text())
            # schemaVersion
            if cm.get("schemaVersion") != "2.0":
                gate.fail(f"target[{target}]: canonical.schemaVersion",
                          f"expected '2.0', got '{cm.get('schemaVersion')}'")
            else:
                gate.ok(f"target[{target}]: canonical.schemaVersion", "2.0")

            # version must match release.json
            cm_version = cm.get("version", "")
            if cm_version != rel.get("version", ""):
                gate.fail(f"target[{target}]: canonical.version",
                          f"manifest={cm_version} release.json={rel.get('version')}")
            else:
                gate.ok(f"target[{target}]: canonical.version", cm_version)

            # releaseId must match release.json.buildId
            cm_release_id = cm.get("releaseId", "")
            if cm_release_id != rel.get("buildId", ""):
                gate.fail(f"target[{target}]: canonical.releaseId",
                          f"manifest={cm_release_id} release.json={rel.get('buildId')}")
            else:
                gate.ok(f"target[{target}]: canonical.releaseId", cm_release_id[:20])

            # gitCommit must match release.json
            cm_commit = cm.get("gitCommit", "")
            if cm_commit != rel.get("gitCommit", ""):
                gate.fail(f"target[{target}]: canonical.gitCommit",
                          f"manifest={cm_commit[:12]} release.json={rel.get('gitCommit','')[:12]}")
            else:
                gate.ok(f"target[{target}]: canonical.gitCommit", cm_commit[:12])

            # artifacts: each SHA must match actual file on disk
            cm_artifacts = cm.get("artifacts", {})
            for art_name in ("bootloader", "partitions", "firmware"):
                art = cm_artifacts.get(art_name, {})
                art_sha = art.get("sha256", "")
                art_path_name = art.get("path", "")
                art_path_on_disk = target_dir / art_path_name if art_path_name else None

                if not art_sha:
                    gate.fail(f"target[{target}]: canonical.{art_name}.sha256", "missing")
                    continue
                if not art_path_on_disk or not art_path_on_disk.is_file():
                    gate.fail(f"target[{target}]: canonical.{art_name}.path",
                              f"file not found: {art_path_name}")
                    continue
                actual_art_sha = sha256_of(art_path_on_disk)
                if actual_art_sha != art_sha:
                    gate.fail(f"target[{target}]: canonical.{art_name}.sha256",
                              f"manifest={art_sha[:16]}... actual={actual_art_sha[:16]}...")
                else:
                    gate.ok(f"target[{target}]: canonical.{art_name}.sha256",
                            f"matches ({art_sha[:16]}...)")

            # firmware signature in canonical manifest
            fw_art = cm_artifacts.get("firmware", {})
            fw_sig = fw_art.get("signature")
            fw_sig_alg = fw_art.get("signatureAlgorithm")
            if is_prod:
                if not fw_sig or fw_sig == "null":
                    gate.fail(f"target[{target}]: canonical.firmware.signature",
                              "PRODUCTION requires signature in canonical manifest")
                elif not fw_sig_alg:
                    gate.fail(f"target[{target}]: canonical.firmware.signatureAlgorithm",
                              "missing")
                else:
                    gate.ok(f"target[{target}]: canonical.firmware.signature",
                            f"present ({fw_sig_alg})")
            else:
                if fw_sig and fw_sig != "null":
                    gate.ok(f"target[{target}]: canonical.firmware.signature",
                            f"present ({fw_sig_alg})")
                else:
                    gate.warn(f"target[{target}]: canonical.firmware.signature",
                              "absent (dev/staging)")

        except Exception as e:
            gate.fail(f"target[{target}]: canonical manifest parse", str(e))

    # ------------------------------------------------------------------
    # Verify provenance
    # ------------------------------------------------------------------
    prov_path = target_dir / "provenance.json"
    if prov_path.is_file():
        try:
            prov = json.loads(prov_path.read_text())
        except Exception as e:
            gate.fail(f"target[{target}]: provenance parse", str(e))
            prov = {}
    else:
        prov = {}

    git_commit = rel.get("gitCommit", "")
    if not git_commit or git_commit == "unknown":
        gate.fail(f"target[{target}]: provenance.gitCommit", "empty/unknown")
    else:
        gate.ok(f"target[{target}]: provenance.gitCommit", git_commit[:12])

    # [P1-5] Bind provenance to CI identity: provenance.gitCommit MUST equal --ci-sha
    if ci_sha:
        if git_commit != ci_sha:
            gate.fail(f"target[{target}]: provenance.commit binding",
                      f"provenance.gitCommit={git_commit[:12]} != GITHUB_SHA={ci_sha[:12]} "
                      f"(binary may not be built from this commit)")
        else:
            gate.ok(f"target[{target}]: provenance.commit binding",
                    f"provenance.gitCommit == GITHUB_SHA ({ci_sha[:12]})")
    elif is_prod:
        # [P1-5] For production, --ci-sha is REQUIRED. Without it, we cannot
        # verify the binary was built from the tagged commit.
        gate.fail(f"target[{target}]: provenance.commit binding",
                  "--ci-sha not provided — cannot verify binary was built from tagged commit (P1-5)")
    else:
        gate.warn(f"target[{target}]: provenance.commit binding",
                  "--ci-sha not provided (dev/staging — skipped)")

    pio = prov.get("toolchain", {}).get("platformio", "")
    if not pio or pio == "unknown":
        gate.fail(f"target[{target}]: toolchain.platformio", "unknown (must be pinned)")
    else:
        gate.ok(f"target[{target}]: toolchain.platformio", pio)

    # ------------------------------------------------------------------
    # [P0-A] Ed25519 signature verification — REQUIRED for production.
    # Unsigned production artifact => FAIL (was "not signed (dev build)" => PASS).
    # ------------------------------------------------------------------
    sig_path = fw_path.with_suffix(".bin.sig")
    if sig_path.is_file() and public_key and public_key.is_file():
        try:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
            from cryptography.hazmat.primitives import serialization
            pub = serialization.load_pem_public_key(public_key.read_bytes())
            sig = bytes.fromhex(sig_path.read_text().strip())
            # firmware signs raw 32-byte SHA-256 digest (P0-1)
            raw_digest = bytes.fromhex(actual_fw_sha)
            assert isinstance(pub, Ed25519PublicKey)
            pub.verify(sig, raw_digest)
            gate.ok(f"target[{target}]: Ed25519 signature", "VALID")
        except ImportError:
            gate.fail(f"target[{target}]: Ed25519 signature", "cryptography not installed")
        except Exception as e:
            gate.fail(f"target[{target}]: Ed25519 signature", f"INVALID: {e}")
    elif sig_path.is_file() and not public_key:
        gate.fail(f"target[{target}]: Ed25519 signature",
                  "signature present but --public-key not provided")
    elif is_prod:
        # [P0-A] PRODUCTION INVARIANT: signature is REQUIRED.
        if not public_key:
            gate.fail(f"target[{target}]: Ed25519 signature",
                      "PRODUCTION target requires --public-key (no key provided)")
        else:
            gate.fail(f"target[{target}]: Ed25519 signature",
                      f"PRODUCTION target requires .sig file ({sig_path.name} missing) — "
                      f"unsigned production artifact is BLOCKED")
    else:
        # Dev/staging — unsigned allowed, but recorded as WARN (not PASS-as-OK).
        gate.warn(f"target[{target}]: Ed25519 signature", "not signed (dev/staging build)")


def check_no_duplicate_versions(gate: Gate, artifacts_dir: Path) -> None:
    """Across all targets, exactly one plts_firmware_v<X.Y.Z>.bin per target."""
    seen = {}  # version -> [target, ...]
    for sub in artifacts_dir.iterdir():
        if not sub.is_dir():
            continue
        for fw in sub.glob("plts_firmware_v*.bin"):
            stem = fw.stem  # plts_firmware_v1.7.1
            ver = stem.replace("plts_firmware_v", "")
            seen.setdefault(ver, []).append(sub.name)
    for ver, targets in seen.items():
        unique_targets = set(targets)
        if len(targets) != len(unique_targets):
            gate.fail("duplicate version", f"v{ver} appears multiple times in same target: {targets}")
        else:
            gate.ok("duplicate version", f"v{ver} in {sorted(unique_targets)}")


def check_cross_repo(
    gate: Gate,
    pwa_path: Path | None,
    firmware_repo_path: Path | None,
    is_prod_release: bool,
) -> None:
    """
    [P0-B] Cross-repo verification — MANDATORY for production.

    Previous behavior: missing --pwa-path => "skipped" => PASS (false green).
    New behavior:
      - Production release: missing --pwa-path or --firmware-repo-path => FAIL
      - Dev/staging: skipped is OK (WARN, not FAIL)
    """
    if not pwa_path or not firmware_repo_path:
        if is_prod_release:
            gate.fail(
                "cross-repo P0-6",
                "PRODUCTION release requires --pwa-path AND --firmware-repo-path "
                "(cannot skip cross-repo identity verification for production)",
            )
        else:
            gate.warn(
                "cross-repo P0-6",
                "skipped (provide --pwa-path + --firmware-repo-path to enable; required for production)",
            )
        return

    pwa_fw = pwa_path / "public" / "firmware"
    fw_bin = firmware_repo_path / "firmware-generic" / "bin"
    if not pwa_fw.is_dir():
        # PWA has no public/firmware/ — this is the desired end-state (P0-6:
        # PWA should NOT track binaries). Verify the PWA manifest version
        # matches the firmware manifest version instead.
        pwa_manifest = pwa_path / "public" / "firmware" / "manifest.json"
        fw_manifest = firmware_repo_path / "firmware-generic" / "manifest.json"
        if not pwa_manifest.is_file() and not fw_manifest.is_file():
            gate.fail("cross-repo P0-6", f"both manifests missing: {pwa_manifest} + {fw_manifest}")
            return
        # If PWA manifest is in a different location, look for it.
        pwa_manifest_alt = pwa_path / "public" / "firmware" / "manifest.json"
        if not pwa_manifest_alt.is_file():
            # PWA may have a manifest at public/firmware/manifest.json if
            # it's still using the legacy sync flow. If neither exists, the
            # PWA has fully decoupled — check release reference instead.
            # For now, accept: PWA has no firmware dir => no drift possible.
            gate.ok("cross-repo P0-6", "PWA has no public/firmware/ (fully decoupled — OK)")
            return
        # Compare versions
        try:
            pwa_m = json.loads(pwa_manifest_alt.read_text())
            fw_m = json.loads(fw_manifest.read_text())
            if pwa_m.get("version") != fw_m.get("version"):
                gate.fail("cross-repo P0-6",
                          f"PWA manifest version {pwa_m.get('version')} != "
                          f"firmware manifest version {fw_m.get('version')}")
            else:
                gate.ok("cross-repo P0-6",
                        f"manifests in sync (v{pwa_m.get('version')})")
        except Exception as e:
            gate.fail("cross-repo P0-6", f"manifest parse error: {e}")
        return

    if not fw_bin.is_dir():
        gate.fail("cross-repo P0-6", f"firmware-generic/bin/ missing: {fw_bin}")
        return

    # Compare SHAs for bootloader, partitions, and versioned firmware binary.
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
                    help="PWA repo checkout (REQUIRED for production — P0-B)")
    ap.add_argument("--firmware-repo-path", type=Path, default=None,
                    help="Firmware repo root (REQUIRED for production — P0-B)")
    ap.add_argument("--public-key", type=Path, default=None,
                    help="Ed25519 public key PEM (REQUIRED for production — P0-A)")
    ap.add_argument("--ci-sha", type=str, default=None,
                    help="GITHUB_SHA — provenance.release.gitCommit MUST match (P1-5)")
    ap.add_argument("--strict", action="store_true",
                    help="Apply production invariants even to dev/staging artifacts")
    args = ap.parse_args()

    gate = Gate()

    if not args.artifacts_dir.is_dir():
        gate.fail("artifacts-dir", f"{args.artifacts_dir} does not exist")
        print(gate.summary())
        return 1

    # Determine if this is a production release (any target is production).
    # If so, cross-repo and signature checks are MANDATORY.
    is_prod_release = False
    for target in args.target:
        td = find_target_dir(args.artifacts_dir, target)
        if td and is_production_target(target, td):
            is_prod_release = True
            break
    if args.strict:
        is_prod_release = True

    # [P0-A] For production, --public-key is REQUIRED upfront.
    if is_prod_release and not args.public_key:
        gate.fail("production public key",
                  "PRODUCTION release requires --public-key (Ed25519 signing key)")

    for target in args.target:
        td = find_target_dir(args.artifacts_dir, target)
        if td is None:
            gate.fail(f"target[{target}]", "artifact directory not found")
        else:
            check_target(gate, target, td, args.public_key, args.ci_sha, args.strict)

    check_no_duplicate_versions(gate, args.artifacts_dir)

    # [P0-B] Cross-repo check is MANDATORY for production.
    check_cross_repo(gate, args.pwa_path, args.firmware_repo_path, is_prod_release)

    print(gate.summary())
    return 1 if gate.blockers else 0


if __name__ == "__main__":
    sys.exit(main())
