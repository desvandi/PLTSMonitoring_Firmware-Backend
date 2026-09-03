#!/usr/bin/env python3
"""
sign_firmware.py — Ed25519 Firmware Signing Tool
Brief §72: signs SHA-256 hash of firmware binary (not full binary — ESP32 RAM constraint)
Usage:
  python3 sign_firmware.py --gen-keys                    # generate keypair
  python3 sign_firmware.py --sign firmware.bin           # sign binary
  python3 sign_firmware.py --verify firmware.bin.sig firmware.bin
"""
import argparse
import hashlib
import json
import os
import sys
import subprocess

def gen_keys():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        print("ERROR: pip install cryptography")
        sys.exit(1)
    key = Ed25519PrivateKey.generate()
    private_pem = key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    public_pem = key.public_key().public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo
    )
    # Raw hex for Config.h
    raw_pub = key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw
    )
    with open('firmware_signing_private.pem', 'wb') as f:
        f.write(private_pem)
    os.chmod('firmware_signing_private.pem', 0o600)
    with open('firmware_signing_public.pem', 'wb') as f:
        f.write(public_pem)
    print(f"Private key: firmware_signing_private.pem (chmod 0600)")
    print(f"Public key:  firmware_signing_public.pem")
    print(f"\nAdd to Config.h:")
    print(f'  #define OTA_ED25519_PUBLIC_KEY_HEX "{raw_pub.hex()}"')

def sign_binary(binary_path):
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        print("ERROR: pip install cryptography")
        sys.exit(1)
    with open(binary_path, 'rb') as f:
        data = f.read()
    # [P0-1 AUDIT 2026-09] Compute SHA-256 RAW DIGEST (32 bytes) and sign
    # THAT, not its hex-string encoding. The firmware verifier
    # (Utils::ed25519VerifyHash in firmware/Utils/Crypto.cpp) calls
    # psa_verify_message() with the 32-byte raw digest as the "message" —
    # signing 64 ASCII bytes of hex (the previous behavior) made every
    # OTA signature fail on the device.
    #   old (broken): signature = key.sign(hash_hex.encode("ascii"))
    #   new (correct): signature = key.sign(hash_digest_bytes)
    hash_digest = hashlib.sha256(data).digest()      # 32 raw bytes
    hash_hex    = hash_digest.hex()                  # for *.sha256 side-file
    # Load private key
    with open('firmware_signing_private.pem', 'rb') as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    # Sign the RAW 32-byte SHA-256 digest (matches firmware verifier)
    signature = key.sign(hash_digest)
    sig_hex = signature.hex()
    # Write outputs
    with open(binary_path + '.sha256', 'w') as f:
        f.write(hash_hex)
    with open(binary_path + '.sig', 'w') as f:
        f.write(sig_hex)
    metadata = {
        'binary': os.path.basename(binary_path),
        'sha256': hash_hex,
        'signature': sig_hex,
        'algorithm': 'ed25519-sha256-raw-digest',
        'signedMessage': 'raw-sha256-digest-32-bytes',
        'signedAt': int(__import__('datetime').datetime.now().timestamp())
    }
    with open(binary_path + '.ota.json', 'w') as f:
        json.dump(metadata, f, indent=2)
    print(f"Signed {binary_path}")
    print(f"  SHA-256: {hash_hex}")
    print(f"  Ed25519 signature: {sig_hex}")
    print(f"  Signed payload: 32-byte raw SHA-256 digest (matches firmware Crypto.cpp)")
    print(f"  Metadata: {binary_path}.ota.json")

def verify_signature(sig_path, binary_path):
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        print("ERROR: pip install cryptography")
        sys.exit(1)
    with open(binary_path, 'rb') as f:
        data = f.read()
    # [P0-1] Verify against RAW 32-byte SHA-256 digest — same form signed.
    hash_digest = hashlib.sha256(data).digest()
    hash_hex    = hash_digest.hex()
    with open(sig_path, 'r') as f:
        sig_hex = f.read().strip()
    with open('firmware_signing_public.pem', 'rb') as f:
        pub = serialization.load_pem_public_key(f.read())
    try:
        pub.verify(bytes.fromhex(sig_hex), hash_digest)
        print(f"OK Signature VALID — {binary_path}")
        print(f"  SHA-256: {hash_hex}")
        print(f"  Verified payload: 32-byte raw SHA-256 digest")
    except Exception as e:
        print(f"FAIL Signature INVALID — {e}")
        sys.exit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='PLTS Monitor Ed25519 Firmware Signer')
    parser.add_argument('--gen-keys', action='store_true', help='Generate Ed25519 keypair')
    parser.add_argument('--sign', metavar='BINARY', help='Sign firmware binary')
    parser.add_argument('--verify', nargs=2, metavar=('SIG', 'BINARY'), help='Verify signature')
    args = parser.parse_args()
    if args.gen_keys:
        gen_keys()
    elif args.sign:
        sign_binary(args.sign)
    elif args.verify:
        verify_signature(args.verify[0], args.verify[1])
    else:
        parser.print_help()
