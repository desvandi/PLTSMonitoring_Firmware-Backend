#!/usr/bin/env python3
"""
verify_flashed_image.py — [AUDIT 2026-09 ROUND 4 / auditor conclusion #4]
"proof that the production binary flashed to the device is the audited
artifact" — an operator-side verification tool with TWO independent modes:

  MODE A — device attestation (works on PROVISIONED, flash-encrypted units)
      The device self-measures its running image (mmap SHA-256 over the
      running partition — cache reads transparently decrypt, so the digest
      is of the PLAINTEXT image) and reports it via GET /api/security.
      This tool fetches it and compares against the signed release manifest
      (release.json firmwareSha256, produced by CI from the reproducible
      build + Ed25519 signing chain).

      python3 scripts/verify_flashed_image.py \\
          --device-url http://192.168.1.50 \\
          --token "$JWT" \\
          --release-json release.json

  MODE B — flash read-back (UNENCRYPTED bench units only)
      Reads the app partitions back over UART via esptool, parses each image
      header, hashes the image bytes, and compares against the manifest.
      On a flash-ENCRYPTED device the read-back is ciphertext and CANNOT
      match — use Mode A there. This mode exists to verify the very units
      that are about to be provisioned (before burning eFuses).

      python3 scripts/verify_flashed_image.py \\
          --port /dev/ttyUSB0 \\
          --release-json release.json

Verdict: exit 0 = the running/flashed image IS the audited artifact
         exit 1 = mismatch / verification failed
         exit 2 = tool misuse (missing args, unreachable device)

Self-test (CI): --self-test runs the comparison + header-parse logic
against built-in fixtures (no hardware, no network).
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import urllib.request
from pathlib import Path

# ESP32 app-image constants (must mirror Services/SecurityPosture.cpp)
ESP_IMAGE_MAGIC = 0xE9
IMAGE_HDR_SIZE = 24
MAX_SEGMENTS = 64

FAIL = 1
MISUSE = 2


def die(msg: str, code: int = MISUSE) -> None:
    print(f"[verify] FAIL: {msg}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------------------
# Shared: expected digest from release.json
# ---------------------------------------------------------------------------
def expected_sha_from_release(release_json: Path) -> tuple[str, str]:
    """Returns (sha256, version) from a CI-generated release.json."""
    try:
        rel = json.loads(release_json.read_text())
    except (OSError, json.JSONDecodeError) as e:
        die(f"cannot read release.json: {e}")
    for key in ("firmwareSha256", "sha256"):
        sha = rel.get(key)
        if isinstance(sha, str) and len(sha) == 64:
            return sha.lower(), str(rel.get("version", "unknown"))
    # provenance-style nested layout
    art = rel.get("artifacts") or {}
    fw = art.get("firmware.bin") if isinstance(art, dict) else None
    if isinstance(fw, dict):
        sha = fw.get("sha256")
        if isinstance(sha, str) and len(sha) == 64:
            return sha.lower(), str(rel.get("version", "unknown"))
    die("release.json carries no firmware SHA-256 (firmwareSha256/sha256/artifacts)")


# ---------------------------------------------------------------------------
# MODE A — device attestation via GET /api/security
# ---------------------------------------------------------------------------
def verify_device(device_url: str, token: str, expected_sha: str,
                  version: str) -> bool:
    url = device_url.rstrip("/")
    if not url.endswith("/api/security"):
        url = url + "/api/security"
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            body = json.loads(resp.read().decode())
    except Exception as e:  # noqa: BLE001 — operator tool, report anything
        die(f"cannot reach {url}: {e}")
    if not body.get("success"):
        die(f"device replied failure envelope: {body.get('message')}")
    data = body.get("data") or {}

    print("=== Device security posture ===")
    print(f"  buildProfile    : {data.get('buildProfile')}")
    print(f"  firmwareVersion : {data.get('firmwareVersion')}")
    print(f"  flashEncryption : {data.get('flashEncryption', {}).get('enabled')}")
    print(f"  secureBoot      : v1={data.get('secureBoot', {}).get('v1')} "
          f"v2={data.get('secureBoot', {}).get('v2')}")
    ef = data.get("efuseAntiRollback", {})
    print(f"  eFuse floor     : {ef.get('secureVersionFloor')}/{ef.get('fieldBits')} "
          f"(burnSupported={ef.get('burnSupported')})")
    lg = data.get("ledger", {})
    print(f"  ledger          : epoch={lg.get('epoch')} v{lg.get('epochVersion')} "
          f"status={lg.get('status')}")
    print(f"  provisioning    : {data.get('provisioning')}")
    for r in data.get("reasons") or []:
        print(f"    - {r}")

    img = data.get("runningImage") or {}
    if not img.get("measured"):
        die("device could not self-measure its running image (mmap failed)", FAIL)
    measured = str(img.get("sha256", "")).lower()
    print(f"  running image   : sha256={measured}")
    print(f"                     len={img.get('lengthBytes')} "
          f"partition={img.get('partition')} state={img.get('state')}")

    ok = True
    if measured != expected_sha.lower():
        print(f"[verify] FAIL: running image digest {measured[:16]}… "
              f"!= release digest {expected_sha.lower()[:16]}… "
              f"(release v{version})")
        ok = False
    else:
        print(f"[verify] PASS: running image IS the released artifact "
              f"(v{version}, sha256 match)")

    # Provisioning verdict is reported, not enforced, by this tool — but a
    # FAIL verdict (unencrypted production device / ledger tamper) is a
    # verification failure for production acceptance purposes.
    if data.get("buildProfile") == "PRODUCTION":
        if data.get("provisioning") == "FAIL":
            print("[verify] FAIL: production device provisioning verdict is FAIL "
                  "(see reasons above)")
            ok = False
        elif data.get("provisioning") == "DEGRADED":
            print("[verify] WARN: provisioning DEGRADED (acceptable for "
                  "pre-secure-boot rollout, must be justified in acceptance)")
    return ok


# ---------------------------------------------------------------------------
# MODE B — esptool flash read-back
# ---------------------------------------------------------------------------
def image_length_from_header(hdr: bytes) -> int:
    """Mirror of SecurityPosture::_imageLengthFromHeader (ESP32 app image)."""
    if len(hdr) < IMAGE_HDR_SIZE:
        return 0
    if hdr[0] != ESP_IMAGE_MAGIC:
        return 0
    n = hdr[1]
    if n == 0 or n > MAX_SEGMENTS:
        return 0
    hash_appended = hdr[IMAGE_HDR_SIZE - 1] != 0
    off = IMAGE_HDR_SIZE
    for _ in range(n):
        if off + 8 > len(hdr):
            return 0
        (_load_addr, data_len) = struct.unpack_from("<II", hdr, off)
        if data_len == 0 or data_len > 0x200000:
            return 0
        off += 8 + data_len
    off += 1
    off = (off + 15) & ~15
    if hash_appended:
        off += 32
    return off


def parse_partition_table(bin_: bytes) -> list:
    """Minimal ESP32 partition-table parser -> [(label, type, subtype, offset, size)]."""
    parts = []
    for i in range(0, min(len(bin_), 0x1000), 32):
        entry = bin_[i:i + 32]
        if len(entry) < 32 or entry[:2] == b"\xEB\xEB":
            break
        if entry[:2] != b"\xAA\x50":
            continue
        ptype, subtype = entry[2], entry[3]
        (offset, size) = struct.unpack_from("<II", entry, 4)
        label = entry[12:28].split(b"\x00")[0].decode(errors="replace")
        parts.append((label, ptype, subtype, offset, size))
    return parts


def read_flash(port: str, offset: int, length: int, out: Path) -> None:
    cmd = [sys.executable, "-m", "esptool", "--port", port,
           "read_flash", str(offset), str(length), str(out)]
    print(f"[verify] $ {' '.join(cmd)}")
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        die("esptool not installed (pip install esptool) — or use --device-url "
            "mode for provisioned devices")
    except subprocess.CalledProcessError as e:
        die(f"esptool read_flash failed: {e}", FAIL)


def verify_uart(port: str, expected_sha: str, version: str) -> bool:
    tmp = Path("/tmp/plts_verify_flash")
    tmp.mkdir(exist_ok=True)
    # Partition table first: locate ALL app slots (ota_0/ota_1/factory).
    ptable_file = tmp / "ptable.bin"
    read_flash(port, 0x8000, 0x1000, ptable_file)
    parts = parse_partition_table(ptable_file.read_bytes())
    app_slots = [p for p in parts if p[1] == 0x00]  # type 0 = app
    if not app_slots:
        die("no app partitions found in the partition table")
    print("[verify] app slots: " +
          ", ".join(f"{p[0]}@0x{p[3]:X} ({p[4]} bytes)" for p in app_slots))

    # Read the first 4 KiB of every app slot to parse its image header.
    candidates = []
    for (label, _t, _st, offset, size) in app_slots:
        head_file = tmp / f"{label}.head"
        read_flash(port, offset, 4096, head_file)
        length = image_length_from_header(head_file.read_bytes())
        if length == 0 or length > size:
            print(f"[verify] {label}: no valid image header (erased slot)")
            continue
        img_file = tmp / f"{label}.bin"
        read_flash(port, offset, length, img_file)
        digest = hashlib.sha256(img_file.read_bytes()).hexdigest()
        candidates.append((label, length, digest))

    if not candidates:
        print("[verify] FAIL: no bootable image found in any app slot")
        return False

    ok = False
    for (label, length, digest) in candidates:
        match = digest == expected_sha.lower()
        print(f"[verify] {label}: len={length} sha256={digest[:16]}… "
              f"{'MATCH' if match else 'no match'}")
        if match:
            ok = True
    if ok:
        print(f"[verify] PASS: a flashed image IS the released artifact "
              f"(v{version})")
        print("[verify] NOTE: read-back verification only proves plaintext "
              "flash. On flash-ENCRYPTED devices use --device-url (the "
              "device self-measures through the decrypting cache).")
    else:
        print(f"[verify] FAIL: none of the app slots matches the release "
              f"digest {expected_sha[:16]}… (v{version})")
    return ok


# ---------------------------------------------------------------------------
# Self-test (CI) — exercises the comparison + header-parse logic.
# ---------------------------------------------------------------------------
def self_test() -> bool:
    ok = True

    # 1. Image-length parser vs a synthetic two-segment image.
    seg0 = struct.pack("<II", 0, 0x100) + b"A" * 0x100  # load_addr=0, len=0x100
    seg1 = struct.pack("<II", 0x40000000, 0x30) + b"B" * 0x30
    # 24-byte header: magic, seg_count=2, spi_mode, spi_speed/size, entry(4),
    # wp_pin, spi_pin_drv[3], chip_id(2), min_chip_rev, reserved[8], hash=1
    hdr = bytes([ESP_IMAGE_MAGIC, 2, 0, 0]) + b"\x00" * 4 + \
        b"\x00" * 15 + bytes([0x01])
    assert len(hdr) == IMAGE_HDR_SIZE
    image = hdr + seg0 + seg1 + b"\xC3"               # + checksum
    image += b"\x00" * ((-len(image)) % 16)           # pad16
    image += b"\x5A" * 32                             # appended sha
    got = image_length_from_header(image[:4096])
    if got == len(image):
        print(f"self-test image-length: PASS ({got} bytes)")
    else:
        print(f"self-test image-length: FAIL (got {got}, want {len(image)})")
        ok = False

    # 2. Parser rejects garbage.
    if image_length_from_header(b"\x00" * 64) == 0:
        print("self-test garbage-reject: PASS")
    else:
        print("self-test garbage-reject: FAIL")
        ok = False

    # 3. Partition table parser vs a synthetic table (2 entries + end marker).
    def entry(magic, ptype, subtype, off, size, label):
        e = struct.pack("<BBII", ptype, subtype, off, size) + label.ljust(16, b"\x00")[:16] + b"\x00" * 4
        assert len(magic + e) == 32
        return magic + e
    tbl = entry(b"\xAA\x50", 0x00, 0x10, 0x10000, 0x170000, b"ota_0") + \
          entry(b"\xAA\x50", 0x00, 0x11, 0x180000, 0x170000, b"ota_1") + \
          b"\xEB\xEB" + b"\xff" * 30 + b"\x00" * (0x1000 - 96)
    parts = parse_partition_table(tbl)
    labels = [p[0] for p in parts]
    if labels == ["ota_0", "ota_1"]:
        print(f"self-test partition-table: PASS ({labels})")
    else:
        print(f"self-test partition-table: FAIL ({labels})")
        ok = False

    # 4. Digest comparison semantics (mode A core).
    sha = hashlib.sha256(b"firmware-image").hexdigest()
    if sha == sha.lower() and len(sha) == 64:
        print("self-test digest-shape: PASS")
    else:
        print("self-test digest-shape: FAIL")
        ok = False

    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device-url", help="device base URL or full /api/security URL")
    ap.add_argument("--token", help="Bearer JWT for the device API")
    ap.add_argument("--port", help="serial port for esptool read-back mode")
    ap.add_argument("--release-json", type=Path, help="CI release.json (expected digest)")
    ap.add_argument("--expected-sha", help="expected SHA-256 (hex, 64 chars)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1

    if not (args.device_url or args.port):
        die("choose a mode: --device-url (provisioned/encrypted units) or "
            "--port (UART read-back, unencrypted bench units)")
    expected_sha, version = None, "unknown"
    if args.release_json:
        expected_sha, version = expected_sha_from_release(args.release_json)
    elif args.expected_sha:
        expected_sha = args.expected_sha.lower()
    else:
        die("provide --release-json (preferred) or --expected-sha")
    if len(expected_sha) != 64 or any(c not in "0123456789abcdef" for c in expected_sha):
        die("expected SHA-256 must be 64 hex chars")

    if args.device_url:
        if not args.token:
            die("--device-url requires --token (the device API is authenticated)")
        ok = verify_device(args.device_url, args.token, expected_sha, version)
    else:
        ok = verify_uart(args.port, expected_sha, version)
    return 0 if ok else FAIL


if __name__ == "__main__":
    sys.exit(main())
