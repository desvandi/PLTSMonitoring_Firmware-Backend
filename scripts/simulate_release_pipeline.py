#!/usr/bin/env python3
"""
simulate_release_pipeline.py — [Audit 10 Phase B] Local end-to-end release simulation.

Proves that the release gate chain works end-to-end without needing actual
hardware or CI. Creates synthetic artifacts, runs generate_provenance +
generate_canonical_manifest + sign_firmware, then runs release_gate.py in
STRICT mode to verify all invariants pass.

This does NOT replace the actual CI release — it proves the code path works.

Usage:  python3 scripts/simulate_release_pipeline.py
Exit:   0 = simulation PASS, 1 = simulation FAIL
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(cmd, cwd=None, env=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  CMD FAIL: {' '.join(cmd)}")
        print(f"  STDOUT: {r.stdout[:500]}")
        print(f"  STDERR: {r.stderr[:500]}")
    return r


def main():
    print("=" * 72)
    print("RELEASE PIPELINE SIMULATION (Audit 10 Phase B)")
    print("=" * 72)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        artifacts_dir = td / "ci-artifacts"
        mod_dir = artifacts_dir / "plts-firmware-modular-production"
        gen_dir = artifacts_dir / "plts-firmware-generic"
        mod_dir.mkdir(parents=True)
        gen_dir.mkdir(parents=True)

        # --- 1. Create synthetic production artifacts ---
        print("\n[1] Creating synthetic production artifacts...")
        for d in (mod_dir, gen_dir):
            (d / "bootloader.bin").write_bytes(os.urandom(1024))
            (d / "partitions.bin").write_bytes(os.urandom(512))
        (mod_dir / "firmware.bin").write_bytes(os.urandom(4096))
        (gen_dir / "plts_firmware_v1.7.1.bin").write_bytes(os.urandom(4096))

        # --- 2. Generate Ed25519 keypair ---
        print("[2] Generating Ed25519 keypair...")
        key_dir = td / "keys"
        key_dir.mkdir()
        r = run([sys.executable, str(ROOT / "scripts" / "sign_firmware.py"), "--gen-keys"],
                cwd=key_dir)
        if r.returncode != 0:
            print("FAIL: key generation failed")
            return 1
        priv_key = key_dir / "firmware_signing_private.pem"
        pub_key = key_dir / "firmware_signing_public.pem"
        if not priv_key.is_file() or not pub_key.is_file():
            print("FAIL: key files not created")
            return 1
        print("  PASS: Ed25519 keypair generated")

        # --- 3. Sign the modular production firmware ---
        print("[3] Signing modular + generic production firmware...")
        for fw_path in [mod_dir / "firmware.bin", gen_dir / "plts_firmware_v1.7.1.bin"]:
            r = run([sys.executable, str(ROOT / "scripts" / "sign_firmware.py"),
                     "--sign", str(fw_path)],
                    cwd=key_dir)
            if r.returncode != 0:
                print(f"FAIL: firmware signing failed for {fw_path.name}")
                return 1
        print("  PASS: both firmware binaries signed")

        # --- 4. Generate provenance for both targets ---
        print("[4] Generating provenance...")
        # Get the current git commit
        git_sha = subprocess.run(["git", "rev-parse", "HEAD"],
                                 cwd=ROOT, capture_output=True, text=True).stdout.strip()

        # Create toolchain.json for pinned toolchain info
        toolchain = {
            "platformio": "6.1.18",
            "espressif32_platform": "6.7.0",
            "arduinojson": "7.1.0",
            "pubsubclient": "2.8",
        }
        tc_file = td / "toolchain.json"
        tc_file.write_text(json.dumps(toolchain, indent=2))

        for target, d, fw_name in [
            ("modular", mod_dir, "firmware.bin"),
            ("generic", gen_dir, "plts_firmware_v1.7.1.bin"),
        ]:
            r = run([sys.executable, str(ROOT / "scripts" / "generate_provenance.py"),
                     "--target", target, "--version", "1.7.1",
                     "--bin-dir", str(d), "--firmware-name", fw_name,
                     "--out-dir", str(d),
                     "--toolchain-json", str(tc_file)])
            if r.returncode != 0:
                print(f"FAIL: provenance generation for {target}")
                return 1
        print("  PASS: provenance generated for both targets")

        # --- 5. Generate canonical manifests ---
        print("[5] Generating canonical manifests...")
        for d in (mod_dir, gen_dir):
            r = run([sys.executable, str(ROOT / "scripts" / "generate_canonical_manifest.py"),
                     "--release-json", str(d / "release.json"),
                     "--provenance-json", str(d / "provenance.json"),
                     "--bin-dir", str(d),
                     "--out", str(d / "manifest-canonical.json")])
            if r.returncode != 0:
                print(f"FAIL: canonical manifest for {d.name}")
                return 1
        print("  PASS: canonical manifests generated")

        # --- 6. Update release.json with correct gitCommit ---
        print("[6] Patching release.json with correct gitCommit...")
        for d in (mod_dir, gen_dir):
            rel = json.loads((d / "release.json").read_text())
            rel["gitCommit"] = git_sha
            (d / "release.json").write_text(json.dumps(rel, indent=2))
            prov = json.loads((d / "provenance.json").read_text())
            prov["release"]["gitCommit"] = git_sha
            (d / "provenance.json").write_text(json.dumps(prov, indent=2))
            # Regenerate canonical manifest with correct gitCommit
            r = run([sys.executable, str(ROOT / "scripts" / "generate_canonical_manifest.py"),
                     "--release-json", str(d / "release.json"),
                     "--provenance-json", str(d / "provenance.json"),
                     "--bin-dir", str(d),
                     "--out", str(d / "manifest-canonical.json")])
            if r.returncode != 0:
                print(f"FAIL: canonical manifest regeneration for {d.name}")
                return 1
        print(f"  PASS: gitCommit = {git_sha[:12]}")

        # --- 7. Run release gate (STRICT mode) ---
        print("[7] Running release gate (STRICT mode)...")
        # Create a minimal fake PWA checkout AND a fake firmware repo checkout
        # for cross-repo check. We can't use the real repos because their
        # committed/checked-out binaries differ from our synthetic ones.
        fake_pwa = td / "pwa-checkout" / "public" / "firmware"
        fake_pwa.mkdir(parents=True)
        fake_fw_repo = td / "fw-repo" / "firmware-generic" / "bin"
        fake_fw_repo.mkdir(parents=True)
        # Copy the synthetic artifacts + manifest so cross-repo SHA matches
        for f in ("bootloader.bin", "partitions.bin", "plts_firmware_v1.7.1.bin"):
            shutil.copy(gen_dir / f, fake_pwa / f)
            shutil.copy(gen_dir / f, fake_fw_repo / f)
        shutil.copy(gen_dir / "manifest-canonical.json", fake_pwa / "manifest.json")

        gate_cmd = [
            sys.executable, str(ROOT / "scripts" / "release_gate.py"),
            "--artifacts-dir", str(artifacts_dir),
            "--target", "generic",
            "--target", "modular",
            "--strict",
            "--ci-sha", git_sha,
            "--public-key", str(pub_key),
            "--pwa-path", str(td / "pwa-checkout"),
            "--firmware-repo-path", str(td / "fw-repo"),
        ]
        r = subprocess.run(gate_cmd, capture_output=True, text=True)
        print(r.stdout)
        if r.returncode != 0:
            print("FAIL: release gate BLOCKED")
            return 1
        print("  PASS: release gate PASSED (STRICT mode)")

        # --- 8. Verify Ed25519 signature independently ---
        print("[8] Verifying Ed25519 signature independently...")
        r = run([sys.executable, str(ROOT / "scripts" / "sign_firmware.py"),
                 "--verify", str(mod_dir / "firmware.bin.sig"),
                 str(mod_dir / "firmware.bin")],
                cwd=key_dir)
        if r.returncode != 0:
            print("FAIL: signature verification failed")
            return 1
        print("  PASS: Ed25519 signature verified")

        # --- 9. Negative test: tamper with firmware ---
        print("[9] Negative test: tamper with firmware → gate should BLOCK...")
        (mod_dir / "firmware.bin").write_bytes(os.urandom(4096))
        r = subprocess.run([
            sys.executable, str(ROOT / "scripts" / "release_gate.py"),
            "--artifacts-dir", str(artifacts_dir),
            "--target", "modular",
            "--strict",
            "--ci-sha", git_sha,
            "--public-key", str(pub_key),
        ], capture_output=True, text=True)
        if r.returncode == 0:
            print("FAIL: tampered firmware was NOT blocked!")
            return 1
        print("  PASS: tampered firmware correctly BLOCKED")

    print("\n" + "=" * 72)
    print("SIMULATION RESULT: PASS")
    print("=" * 72)
    print("The release gate chain works end-to-end:")
    print("  build → sign → provenance → canonical manifest → gate (STRICT) → PASS")
    print("  negative: tampered firmware → BLOCKED")
    print()
    print("This proves the CODE PATH is correct. The remaining blockers are:")
    print("  1. Operator must set up GitHub Environment 'production' with secrets")
    print("  2. Operator must add GPG key ID to .github/authorized-signers")
    print("  3. Operator must run hardware acceptance on real ESP32")
    print("  4. Operator must tag v1.7.1 to trigger the actual release pipeline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
