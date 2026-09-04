#!/usr/bin/env python3
"""
verify_hardware_acceptance.py — [Audit 9 P0-2] Semantic hardware acceptance gate.

Replaces the previous "file exists → PASS" false-green check with a strict
JSON schema validation that verifies:
  1. File exists: docs/hardware-acceptance/v<X.Y.Z>.json
  2. JSON is parseable
  3. schemaVersion == 1
  4. version matches the tag version
  5. gitCommit matches GITHUB_SHA
  6. firmwareSha256 matches release.json.firmwareSha256
  7. verdict == "PASS"
  8. ALL required checks == "PASS" (boot, sensors, alarms, otaRest, otaMqtt,
     rollback, emergencyRelay, configPersistence, security, soak24h,
     factoryReset)
  9. ALL signoffs present (testEngineer, reviewer, releaseManager — non-empty)

If ANY check fails → BLOCKED. The release is NOT promoted to stable.

Usage (from CI):
  python3 scripts/verify_hardware_acceptance.py \\
      --version 1.7.1 \\
      --ci-sha "$GITHUB_SHA" \\
      --release-json ci-artifacts/plts-firmware-modular-production/release.json \\
      --hw-dir docs/hardware-acceptance

Exit:  0 = hardware acceptance verified (all checks PASS)
       1 = BLOCKED (with reason)
"""
import argparse
import json
import sys
from pathlib import Path


REQUIRED_CHECKS = [
    "boot",
    "sensors",
    "alarms",
    "otaRest",
    "otaMqtt",
    "rollback",
    "emergencyRelay",
    "configPersistence",
    "security",
    "soak24h",
    "factoryReset",
]

REQUIRED_SIGNOFFS = [
    "testEngineer",
    "reviewer",
    "releaseManager",
]


def run() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="Release version (e.g., 1.7.1)")
    ap.add_argument("--ci-sha", required=True, help="GITHUB_SHA — the commit CI built from")
    ap.add_argument("--release-json", required=True, type=Path,
                    help="release.json from the production artifact")
    ap.add_argument("--hw-dir", type=Path,
                    default=Path("docs/hardware-acceptance"),
                    help="Directory containing hardware acceptance files")
    args = ap.parse_args()

    blockers = []

    # ------------------------------------------------------------------
    # 1. File exists.
    # ------------------------------------------------------------------
    hw_json = args.hw_dir / f"v{args.version}.json"
    if not hw_json.is_file():
        blockers.append(
            f"hardware acceptance file not found: {hw_json}\n"
            f"  Create it by completing docs/hardware-acceptance/v{args.version}.md "
            f"on real ESP32 hardware, then export the verdict to "
            f"docs/hardware-acceptance/v{args.version}.json"
        )
        print(f"[FAIL] file-exists: {hw_json} not found")
        print()
        print("=" * 72)
        print("HARDWARE ACCEPTANCE = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print(f"[PASS] file-exists: {hw_json}")

    # ------------------------------------------------------------------
    # 2. JSON parseable.
    # ------------------------------------------------------------------
    try:
        hw = json.loads(hw_json.read_text())
    except Exception as e:
        blockers.append(f"hardware acceptance JSON parse error: {e}")
        print(f"[FAIL] json-parse: {e}")
        print()
        print("=" * 72)
        print("HARDWARE ACCEPTANCE = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print(f"[PASS] json-parse: valid JSON")

    # ------------------------------------------------------------------
    # 3. schemaVersion.
    # ------------------------------------------------------------------
    sv = hw.get("schemaVersion")
    if sv != 1:
        blockers.append(f"schemaVersion must be 1, got {sv}")
        print(f"[FAIL] schemaVersion: {sv} (expected: 1)")
    else:
        print(f"[PASS] schemaVersion: {sv}")

    # ------------------------------------------------------------------
    # 4. version matches tag.
    # ------------------------------------------------------------------
    hw_version = hw.get("version", "")
    if hw_version != args.version:
        blockers.append(
            f"hardware acceptance version '{hw_version}' != tag version '{args.version}'"
        )
        print(f"[FAIL] version: '{hw_version}' != '{args.version}'")
    else:
        print(f"[PASS] version: '{hw_version}'")

    # ------------------------------------------------------------------
    # 5. gitCommit matches GITHUB_SHA.
    # ------------------------------------------------------------------
    hw_commit = hw.get("gitCommit", "")
    if not hw_commit:
        blockers.append("gitCommit is empty in hardware acceptance JSON")
        print(f"[FAIL] gitCommit: empty")
    elif hw_commit != args.ci_sha:
        blockers.append(
            f"hardware acceptance gitCommit '{hw_commit[:12]}' != GITHUB_SHA '{args.ci_sha[:12]}' "
            f"— the hardware test must be run on the exact commit being released"
        )
        print(f"[FAIL] gitCommit: '{hw_commit[:12]}' != GITHUB_SHA '{args.ci_sha[:12]}'")
    else:
        print(f"[PASS] gitCommit: matches GITHUB_SHA")

    # ------------------------------------------------------------------
    # 6. firmwareSha256 matches release.json.
    # ------------------------------------------------------------------
    if not args.release_json.is_file():
        blockers.append(f"release.json not found: {args.release_json}")
        print(f"[FAIL] release-json: missing")
    else:
        try:
            rel = json.loads(args.release_json.read_text())
            rel_fw_sha = rel.get("firmwareSha256", "")
            hw_fw_sha = hw.get("firmwareSha256", "")
            if not hw_fw_sha:
                blockers.append("firmwareSha256 is empty in hardware acceptance JSON")
                print(f"[FAIL] firmwareSha256: empty")
            elif hw_fw_sha != rel_fw_sha:
                blockers.append(
                    f"hardware acceptance firmwareSha256 '{hw_fw_sha[:16]}...' != "
                    f"release.json firmwareSha256 '{rel_fw_sha[:16]}...' — "
                    f"the hardware test must be run on the exact binary being released"
                )
                print(f"[FAIL] firmwareSha256: mismatch")
            else:
                print(f"[PASS] firmwareSha256: matches release.json")
        except Exception as e:
            blockers.append(f"release.json parse error: {e}")
            print(f"[FAIL] release-json: parse error: {e}")

    # ------------------------------------------------------------------
    # 7. verdict == PASS.
    # ------------------------------------------------------------------
    verdict = hw.get("verdict", "")
    if verdict != "PASS":
        blockers.append(
            f"hardware acceptance verdict is '{verdict}', not 'PASS' — "
            f"release CANNOT be promoted to stable without a PASS verdict"
        )
        print(f"[FAIL] verdict: '{verdict}' (expected: PASS)")
    else:
        print(f"[PASS] verdict: PASS")

    # ------------------------------------------------------------------
    # 8. ALL required checks == PASS.
    # ------------------------------------------------------------------
    checks = hw.get("checks", {})
    for check_name in REQUIRED_CHECKS:
        check_val = checks.get(check_name)
        if check_val is None:
            blockers.append(f"check '{check_name}' is missing from hardware acceptance JSON")
            print(f"[FAIL] check-{check_name}: missing")
        elif check_val != "PASS":
            blockers.append(
                f"check '{check_name}' is '{check_val}', not 'PASS' — "
                f"ALL checks must PASS for stable release"
            )
            print(f"[FAIL] check-{check_name}: '{check_val}'")
        else:
            print(f"[PASS] check-{check_name}: PASS")

    # Detect unexpected extra checks
    extra_checks = set(checks.keys()) - set(REQUIRED_CHECKS)
    if extra_checks:
        print(f"[WARN] extra-checks: {sorted(extra_checks)} (allowed but not required)")

    # ------------------------------------------------------------------
    # 9. ALL signoffs present (non-empty strings).
    # ------------------------------------------------------------------
    for signoff_field in REQUIRED_SIGNOFFS:
        signoff_val = hw.get(signoff_field, "")
        if not signoff_val or not str(signoff_val).strip():
            blockers.append(
                f"signoff '{signoff_field}' is empty — "
                f"ALL three signoffs (testEngineer, reviewer, releaseManager) are required"
            )
            print(f"[FAIL] signoff-{signoff_field}: empty")
        else:
            print(f"[PASS] signoff-{signoff_field}: '{signoff_val}'")

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    if blockers:
        print("=" * 72)
        print("HARDWARE ACCEPTANCE = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print("=" * 72)
    print("HARDWARE ACCEPTANCE = PASS")
    print("=" * 72)
    print(f"  All {len(REQUIRED_CHECKS)} checks PASS, all {len(REQUIRED_SIGNOFFS)} signoffs present.")
    print(f"  Release is eligible for STABLE promotion.")
    return 0


if __name__ == "__main__":
    sys.exit(run())
