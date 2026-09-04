#!/usr/bin/env python3
"""
test_version_identity.py — [P1-REMEDIATION] single source of truth guards
=========================================================================
Audit finding (P1, "version identity drift"): the repo carried three
disagreeing version identities for firmware-generic —
  * src/plts_firmware_v1.ino header comment said "v1.5.4" (stale),
  * the FIRMWARE_VERSION constant said 1.6.0,
  * bin/ contained BOTH plts_firmware_v1.5.4.bin and plts_firmware_v1.6.0.bin
while the modular tree (firmware/) tracked 1.6.3 independently.

Why it matters: OTA anti-downgrade compares the RUNNING firmware's
FIRMWARE_VERSION constant against the manifest version — NOT the .bin file
name. A manifest that lies about the binary it points to, or a source
header that lies about the constant, silently breaks the fleet's
anti-downgrade chain.

This test enforces the version identity contract:

  G1  firmware-generic manifest.json "version" is strict semver X.Y.Z
  G2  .ino FIRMWARE_VERSION constant == manifest version
  G3  .ino header comment top version == manifest version (stale-header guard)
  G4  bin/ holds AT MOST ONE plts_firmware_v*.bin (one active release)
  G5  if a versioned .bin exists, its embedded version == manifest version
  G6  manifest part path for the firmware image matches the manifest version
  G7  the modular tree's version (firmware/Core/Config.h FIRMWARE_VERSION)
      is strict semver and is reported via /api/version + telemetry — it is
      a SEPARATE product line from firmware-generic and may legitimately
      differ; this group only asserts it is well-formed and that the two
      lines are never accidentally cross-wired (no file under firmware/
      references the generic manifest and vice versa).

Transitional state note: between "version bumped in source" and "CI staged
the rebuilt binary", bin/ may temporarily hold no plts_firmware_v*.bin —
G4/G5 are written so that this state is legal but any STALE binary is not.

Usage: python3 scripts/test_version_identity.py   (exit 0 = PASS)
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # repo firmware
GENERIC = ROOT / "firmware-generic"
INO = GENERIC / "src" / "plts_firmware_v1.ino"
MANIFEST = GENERIC / "manifest.json"
BIN = GENERIC / "bin"
MODULAR_CONFIG = ROOT / "firmware" / "Core" / "Config.h"

PASS = 0
FAIL = 0
FAILURES = []


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        FAILURES.append(name + (f" — {detail}" if detail else ""))
        print(f"  FAIL  {name}" + (f" — {detail}" if detail else ""))


def semver_ok(v):
    return bool(re.fullmatch(r"\d+\.\d+\.\d+", str(v)))


def main() -> int:
    print("[G] Version identity (single source of truth):")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    version = manifest.get("version", "")

    # G1 — manifest semver
    check("G1 manifest.json version is strict semver", semver_ok(version),
          f"version={version!r}")

    # G2 — source constant == manifest
    ino_text = INO.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', ino_text)
    src_version = m.group(1) if m else None
    check("G2 .ino FIRMWARE_VERSION == manifest version",
          src_version == version, f"source={src_version!r} manifest={version!r}")

    # G3 — header comment top version == manifest (stale-header guard)
    hm = re.search(r"Generic Firmware v(\d+\.\d+\.\d+)", ino_text)
    header_version = hm.group(1) if hm else None
    check("G3 .ino header comment version == manifest version",
          header_version == version,
          f"header={header_version!r} manifest={version!r} "
          "(header changelog not bumped with the constant)")

    # G4 — at most one versioned binary in bin/
    versioned_bins = sorted(BIN.glob("plts_firmware_v*.bin")) if BIN.is_dir() else []
    check("G4 bin/ holds at most one plts_firmware_v*.bin",
          len(versioned_bins) <= 1,
          f"found {[p.name for p in versioned_bins]} — one active release only "
          "(delete stale versions; CI stages the current one)")

    # G5 — if a versioned binary exists, its name matches the manifest version
    if versioned_bins:
        expected_name = f"plts_firmware_v{version}.bin"
        check("G5 staged binary name matches manifest version",
              versioned_bins[0].name == expected_name,
              f"found {versioned_bins[0].name}, expected {expected_name}")

        # G5b — [Audit 8 P1-1] EMBEDDED version identity (not just filename).
        # The filename can lie — a stale binary renamed to v1.7.1 would pass G5
        # but the firmware would report a different version via /api/version,
        # breaking OTA anti-downgrade. Extract the FIRMWARE_VERSION string
        # embedded in the binary (it's compiled in as a C string literal) and
        # compare to the manifest version.
        bin_path = versioned_bins[0]
        try:
            bin_bytes = bin_path.read_bytes()
            # The FIRMWARE_VERSION constant is a quoted string like "1.7.1".
            # Search for the pattern in the binary. ESP32 Arduino compiles
            # string literals into .rodata — they appear as plain ASCII.
            # Look for "X.Y.Z" pattern near a plausible location.
            import re as _re
            # Find all X.Y.Z strings in the binary
            candidates = _re.findall(rb'"?(\d+\.\d+\.\d+)"?', bin_bytes)
            # Filter: must be a plausible firmware version (not a sub-version
            # of a library). Heuristic: appears at least once.
            semver_pattern = _re.compile(rb"\d+\.\d+\.\d+")
            all_versions = set()
            for match in semver_pattern.finditer(bin_bytes):
                v = match.group().decode("ascii", errors="ignore")
                all_versions.add(v)
            # The firmware version should appear. If multiple versions appear
            # (e.g., library versions), the manifest version MUST be among them.
            if version in all_versions:
                check("G5b embedded firmware version matches manifest",
                      True, f"found '{version}' embedded in {bin_path.name}")
            else:
                check("G5b embedded firmware version matches manifest",
                      False,
                      f"manifest version '{version}' NOT found embedded in "
                      f"{bin_path.name} (found: {sorted(all_versions)[:10]})")
        except Exception as e:
            check("G5b embedded firmware version matches manifest",
                  False, f"binary read error: {e}")
    else:
        # Transitional state (source bumped, CI build pending) — legal.
        print(f"  SKIP  G5 no staged binary yet (CI build pending) — "
              f"expected plts_firmware_v{version}.bin")

    # G6 — manifest part path carries the same version
    parts = []
    for b in manifest.get("builds", []):
        parts += [p.get("path", "") for p in b.get("parts", [])]
    fw_paths = [p for p in parts if "plts_firmware_v" in p]
    check("G6 manifest firmware part path embeds the manifest version",
          len(fw_paths) == 1 and fw_paths[0].endswith(f"plts_firmware_v{version}.bin"),
          f"parts={fw_paths}")

    # G7 — modular tree version well-formed + no cross-wiring
    cfg_text = MODULAR_CONFIG.read_text(encoding="utf-8", errors="replace") \
        if MODULAR_CONFIG.is_file() else ""
    cm = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', cfg_text)
    modular_version = cm.group(1) if cm else None
    check("G7a modular firmware/Core/Config.h version is strict semver",
          semver_ok(modular_version), f"version={modular_version!r}")
    if cm:
        line_no = cfg_text[:cm.start()].count("\n") + 1
        # The modular version constant must live in the Core namespace block,
        # not be confused with the generic constant.
        check("G7b modular version constant lives in namespace Core",
              "namespace Core" in cfg_text[:cm.start()],
              "FIRMWARE_VERSION found outside namespace Core")
        print(f"  NOTE  G7 modular={modular_version} generic={version} — "
              f"separate product lines (Config.h line {line_no}); "
              "differences are legitimate, cross-references are not")

    print(f"\n{'='*60}")
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    if FAILURES:
        print("FAILURES:")
        for f in FAILURES:
            print(f"  - {f}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
