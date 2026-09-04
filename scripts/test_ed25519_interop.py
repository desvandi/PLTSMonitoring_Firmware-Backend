#!/usr/bin/env python3
"""
test_ed25519_interop.py — P0-1 interop test for Ed25519 firmware signing.

Verifies that:
  1. sign_firmware.py produces a signature over the RAW 32-byte SHA-256
     digest (not the 64-byte hex string).
  2. The signature can be verified by an independent verifier that mirrors
     the firmware's `Utils::ed25519VerifyHash` semantics — i.e. PSA
     `psa_verify_message(publicKey, PSA_ALG_PURE_EDDSA, hashBytes=raw_sha256,
     signature)` which is equivalent to PyCA `pub.verify(signature, raw_sha256_digest)`.
  3. Negative cases fail-closed:
       (a) Binary modified after signing  -> INVALID
       (b) Signature bytes flipped         -> INVALID
       (c) Wrong public key                -> INVALID
       (d) Legacy "sign the hex string" form is NOT accepted (regression guard).

Run:  python3 scripts/test_ed25519_interop.py
Exit: 0 = PASS, 1 = FAIL
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SIGNER    = REPO_ROOT / "scripts" / "sign_firmware.py"


def _require_cryptography():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey, Ed25519PublicKey,
        )
        from cryptography.hazmat.primitives import serialization
        return True
    except ImportError:
        print("SKIP: 'cryptography' not installed (pip install cryptography)")
        return False


def _sign(binary_path: Path, cwd: Path) -> None:
    r = subprocess.run(
        [sys.executable, str(SIGNER), "--sign", str(binary_path)],
        cwd=cwd, capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise RuntimeError(f"signer failed:\nSTDOUT:{r.stdout}\nSTDERR:{r.stderr}")


def _verify_with_firmware_semantics(pub_pem: bytes, signature: bytes,
                                     raw_digest: bytes) -> bool:
    """
    Mirror firmware Utils::ed25519VerifyHash():
      psa_verify_message(key, PSA_ALG_PURE_EDDSA, hashBytes, hashLen=32, sig, 64)
    PureEdDSA: PSA hashes the "message" (here: the 32-byte SHA-256 digest)
    with SHA-512 internally, then verifies. PyCA's pub.verify(sig, msg) is
    the same operation.
    """
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.hazmat.primitives import serialization
    pub = serialization.load_pem_public_key(pub_pem)
    assert isinstance(pub, Ed25519PublicKey)
    try:
        pub.verify(signature, raw_digest)
        return True
    except Exception:
        return False


def _gen_keys(cwd: Path) -> None:
    r = subprocess.run(
        [sys.executable, str(SIGNER), "--gen-keys"],
        cwd=cwd, capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise RuntimeError(f"key-gen failed:\n{r.stderr}")


def run() -> int:
    if not _require_cryptography():
        return 0

    failures = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # 1. Generate keypair in work dir
        _gen_keys(td)
        priv_pem = (td / "firmware_signing_private.pem").read_bytes()
        pub_pem  = (td / "firmware_signing_public.pem").read_bytes()

        # 2. Create a fake "binary"
        binary = td / "fake_firmware.bin"
        payload = os.urandom(4096)
        binary.write_bytes(payload)

        # 3. Sign it
        _sign(binary, td)
        sig_hex = (binary.with_suffix(".bin.sig")).read_text().strip()
        sha_hex = (binary.with_suffix(".bin.sha256")).read_text().strip()
        meta    = json.loads((binary.with_suffix(".bin.ota.json")).read_text())
        signature = bytes.fromhex(sig_hex)
        raw_digest = hashlib.sha256(payload).digest()

        # ---- Positive: signature must verify against raw digest ----
        if not _verify_with_firmware_semantics(pub_pem, signature, raw_digest):
            failures.append("positive: signer signature rejected by firmware-semantics verifier")
        else:
            print("PASS positive: signer signature verified against raw SHA-256 digest")

        # ---- Algorithm marker in metadata ----
        if meta.get("algorithm") != "ed25519-sha256-raw-digest":
            failures.append(f"metadata.algorithm != ed25519-sha256-raw-digest (got {meta.get('algorithm')!r})")
        else:
            print("PASS metadata.algorithm marker correct")

        if meta.get("sha256") != sha_hex:
            failures.append("metadata.sha256 does not match *.sha256 side-file")
        else:
            print("PASS metadata.sha256 matches side-file")

        # ---- Negative (a): binary modified after signing ----
        modified_payload = bytearray(payload)
        modified_payload[0] ^= 0xFF
        modified_digest = hashlib.sha256(bytes(modified_payload)).digest()
        if _verify_with_firmware_semantics(pub_pem, signature, modified_digest):
            failures.append("negative(a): signature accepted on MODIFIED binary (must fail)")
        else:
            print("PASS negative(a): modified binary rejected")

        # ---- Negative (b): signature bytes flipped ----
        bad_sig = bytearray(signature)
        bad_sig[0] ^= 0xFF
        if _verify_with_firmware_semantics(pub_pem, bytes(bad_sig), raw_digest):
            failures.append("negative(b): flipped signature accepted (must fail)")
        else:
            print("PASS negative(b): flipped signature rejected")

        # ---- Negative (c): wrong public key ----
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
        other_key = Ed25519PrivateKey.generate()
        other_pub_pem = other_key.public_key().public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo,
        )
        if _verify_with_firmware_semantics(other_pub_pem, signature, raw_digest):
            failures.append("negative(c): signature accepted under WRONG public key (must fail)")
        else:
            print("PASS negative(c): wrong public key rejected")

        # ---- Negative (d): regression guard — legacy "sign hex string" form
        #                  must NOT verify under the firmware semantics ----
        legacy_sig = other_key.sign(raw_digest.hex().encode("ascii"))
        # Use the *original* key (the one matching pub_pem) for a fair regression test
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey as _EK
        priv = serialization.load_pem_private_key(priv_pem, password=None)
        assert isinstance(priv, _EK)
        legacy_sig = priv.sign(raw_digest.hex().encode("ascii"))
        if _verify_with_firmware_semantics(pub_pem, legacy_sig, raw_digest):
            failures.append("negative(d): legacy 'sign hex string' form was accepted — "
                            "regression: signer must sign RAW digest, not hex")
        else:
            print("PASS negative(d): legacy hex-string form rejected (regression guard)")

        # ---- Round-trip via the script's own --verify ----
        r = subprocess.run(
            [sys.executable, str(SIGNER), "--verify",
             str(binary.with_suffix(".bin.sig")), str(binary)],
            cwd=td, capture_output=True, text=True,
        )
        if r.returncode != 0:
            failures.append(f"script --verify failed:\n{r.stdout}\n{r.stderr}")
        else:
            print("PASS script --verify accepts its own signature")

    if failures:
        print("\nFAIL — interop test:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nPASS — all Ed25519 interop cases OK")
    return 0


if __name__ == "__main__":
    sys.exit(run())
