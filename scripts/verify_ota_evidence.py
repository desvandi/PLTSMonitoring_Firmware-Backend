#!/usr/bin/env python3
"""
verify_ota_evidence.py — [Audit 2026-09-04] OTA physical test evidence gate.

Verifies that the OTA physical test evidence JSON exists, is valid, and records
a PASS verdict for all 16 criteria. This is the final gate before fleet OTA
authorization.

Usage (from CI or local):
  python3 scripts/verify_ota_evidence.py \\
      --version 1.8.0 \\
      --canonical-release-json ci-artifacts/modular/release.json \\
      --ota-dir docs/ota-physical-test

Exit:  0 = OTA evidence verified (all 16 checks PASS, fleet OTA eligible)
       1 = BLOCKED (with reason)
"""
import argparse
import json
import sys
from pathlib import Path


REQUIRED_CHECKS = [
    "preOtaDeviceRunning",
    "pwaFetchesCanonicalRelease",
    "pwaResolvesVersionAndSha",
    "pwaTriggersOta",
    "deviceReceivesArtifact",
    "signatureVerificationPassed",
    "sha256VerificationPassed",
    "downloadComplete",
    "bootRebootSucceeded",
    "deviceRunning180",
    "deviceReportsCorrectIdentity",
    "relaySafeAfterReboot",
    "sensorsAlarmsNormal",
    "otaTerminalState",
    "pwaBackendSeeSameState",
    "rollbackRecoveryWorks",
]

REQUIRED_SIGNOFFS = [
    "testEngineer",
    "reviewer",
    "releaseManager",
]


def run() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="Release version (e.g., 1.8.0)")
    ap.add_argument("--canonical-release-json", required=True, type=Path,
                    help="modular-release.json from the GitHub Release")
    ap.add_argument("--ota-dir", type=Path,
                    default=Path("docs/ota-physical-test"),
                    help="Directory containing OTA physical test evidence files")
    args = ap.parse_args()

    blockers = []
    warnings = []

    # --- 1. File exists ---
    ota_json = args.ota_dir / f"v{args.version}.json"
    if not ota_json.is_file():
        blockers.append(
            f"OTA physical test evidence file not found: {ota_json}\n"
            f"  Execute the test protocol in {args.ota_dir}/v{args.version}.md "
            f"on real ESP32 hardware with the PWA, then export the verdict to "
            f"{ota_json}"
        )
        print(f"[FAIL] file-exists: {ota_json} not found")
        print()
        print("=" * 72)
        print("OTA PHYSICAL TEST = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print(f"[PASS] file-exists: {ota_json}")

    # --- 2. JSON parseable ---
    try:
        ota = json.loads(ota_json.read_text())
    except Exception as e:
        blockers.append(f"OTA evidence JSON parse error: {e}")
        print(f"[FAIL] json-parse: {e}")
        return 1
    print(f"[PASS] json-parse: valid JSON")

    # --- 3. schemaVersion ---
    sv = ota.get("schemaVersion")
    if sv != 1:
        blockers.append(f"schemaVersion must be 1, got {sv}")
        print(f"[FAIL] schemaVersion: {sv} (expected: 1)")
    else:
        print(f"[PASS] schemaVersion: {sv}")

    # --- 4. version matches ---
    ota_version = ota.get("version", "")
    if ota_version != args.version:
        blockers.append(f"OTA evidence version '{ota_version}' != expected '{args.version}'")
        print(f"[FAIL] version: '{ota_version}' != '{args.version}'")
    else:
        print(f"[PASS] version: '{ota_version}'")

    # --- 5. canonicalReleaseVersion matches ---
    canonical_version = ota.get("canonicalReleaseVersion", "")
    if canonical_version != args.version:
        blockers.append(f"canonicalReleaseVersion '{canonical_version}' != '{args.version}'")
        print(f"[FAIL] canonicalReleaseVersion: '{canonical_version}'")
    else:
        print(f"[PASS] canonicalReleaseVersion: '{canonical_version}'")

    # --- 6. canonicalReleaseFirmwareSha256 matches release.json ---
    if not args.canonical_release_json.is_file():
        blockers.append(f"canonical release.json not found: {args.canonical_release_json}")
        print(f"[FAIL] canonical-sha: release.json missing")
    else:
        try:
            rel = json.loads(args.canonical_release_json.read_text())
            rel_sha = rel.get("firmwareSha256", "")
            ota_sha = ota.get("canonicalReleaseFirmwareSha256", "")
            if not ota_sha:
                blockers.append("canonicalReleaseFirmwareSha256 is empty")
                print(f"[FAIL] canonical-sha: empty")
            elif ota_sha != rel_sha:
                blockers.append(
                    f"canonicalReleaseFirmwareSha256 '{ota_sha[:16]}...' != "
                    f"release.json firmwareSha256 '{rel_sha[:16]}...'"
                )
                print(f"[FAIL] canonical-sha: mismatch")
            else:
                print(f"[PASS] canonical-sha: matches release.json")
        except Exception as e:
            blockers.append(f"release.json parse error: {e}")
            print(f"[FAIL] canonical-sha: parse error: {e}")

    # --- 7. verdict == PASS ---
    verdict = ota.get("verdict", "")
    if verdict != "PASS":
        blockers.append(
            f"OTA evidence verdict is '{verdict}', not 'PASS' — "
            f"fleet OTA CANNOT be authorized without a PASS verdict"
        )
        print(f"[FAIL] verdict: '{verdict}' (expected: PASS)")
    else:
        print(f"[PASS] verdict: PASS")

    # --- 8. ALL 16 required checks == PASS ---
    checks = ota.get("checks", {})
    for check_name in REQUIRED_CHECKS:
        check_val = checks.get(check_name)
        if check_val is None:
            blockers.append(f"check '{check_name}' is missing from OTA evidence JSON")
            print(f"[FAIL] check-{check_name}: missing")
        elif check_val != "PASS":
            blockers.append(
                f"check '{check_name}' is '{check_val}', not 'PASS' — "
                f"ALL 16 criteria must PASS for fleet OTA authorization"
            )
            print(f"[FAIL] check-{check_name}: '{check_val}'")
        else:
            print(f"[PASS] check-{check_name}: PASS")

    # --- 9. ALL signoffs present ---
    for signoff_field in REQUIRED_SIGNOFFS:
        signoff_val = ota.get(signoff_field, "")
        if not signoff_val or not str(signoff_val).strip():
            blockers.append(
                f"signoff '{signoff_field}' is empty — "
                f"ALL three signoffs are required for fleet OTA authorization"
            )
            print(f"[FAIL] signoff-{signoff_field}: empty")
        else:
            print(f"[PASS] signoff-{signoff_field}: '{signoff_val}'")

    # --- 10. deviceStartingVersion < canonicalReleaseVersion (anti-downgrade) ---
    start_ver = ota.get("deviceStartingVersion", "")
    if start_ver:
        try:
            start_parts = [int(x) for x in start_ver.split(".")]
            canonical_parts = [int(x) for x in args.version.split(".")]
            if start_parts >= canonical_parts:
                blockers.append(
                    f"deviceStartingVersion '{start_ver}' >= canonicalReleaseVersion '{args.version}' — "
                    f"OTA test must start from an OLDER firmware (anti-downgrade verification)"
                )
                print(f"[FAIL] anti-downgrade: starting version {start_ver} >= {args.version}")
            else:
                print(f"[PASS] anti-downgrade: starting version {start_ver} < {args.version}")
        except ValueError:
            warnings.append(f"deviceStartingVersion '{start_ver}' is not valid SemVer")
            print(f"[WARN] anti-downgrade: cannot parse starting version '{start_ver}'")
    else:
        warnings.append("deviceStartingVersion is empty — cannot verify anti-downgrade")
        print(f"[WARN] anti-downgrade: deviceStartingVersion empty")

    # --- Summary ---
    print()
    if warnings:
        print("=" * 72)
        print(f"WARNINGS ({len(warnings)}):")
        print("=" * 72)
        for w in warnings:
            print(f"  ! {w}")
        print()
    if blockers:
        print("=" * 72)
        print("OTA PHYSICAL TEST = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print("=" * 72)
    print("OTA PHYSICAL TEST = PASS")
    print("=" * 72)
    print(f"  All {len(REQUIRED_CHECKS)} criteria PASS, all {len(REQUIRED_SIGNOFFS)} signoffs present.")
    if warnings:
        print(f"  {len(warnings)} warning(s) recorded (non-blocking).")
    print(f"  Fleet OTA is AUTHORIZED for v{args.version}.")
    return 0


if __name__ == "__main__":
    sys.exit(run())
