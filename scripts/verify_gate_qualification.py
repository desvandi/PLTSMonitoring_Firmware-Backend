#!/usr/bin/env python3
"""
verify_gate_qualification.py — [GATE-9 / PH8-06 2026-09] Gate-remediation
hardware qualification gate.

Closes the Phase 10 gap: since v1.9.2 the release chain only enforced the
INA219 measurement-chain schema (verify_ina219_hardware_acceptance.py);
the PH8-06 safety matrix (E-WAVE atomicity, comm-loss fail-safe, OTA
durability, …) was NOT required by any release gate. This verifier demands
the physical HIL evidence for every gate remediation BEFORE a release that
contains them may be promoted.

Validates docs/hardware-acceptance/v{version}-gate.json:
  1. File exists
  2. JSON parseable, schemaVersion == 1
  3. version matches the tag version
  4. gitCommit == the SOURCE commit (release.json.gitCommit)
  5. firmwareSha256 == release.json.firmwareSha256
  6. verdict == "PASS"
  7. ALL 22 checks == "PASS" — 11 legacy safety checks (v1.8.0 protocol:
     boot/sensors/alarms/otaRest/otaMqtt/rollback/emergencyRelay/
     configPersistence/security/soak24h/factoryReset) + 10 gate checks
     (emergencyAtomicity, watchdogStarvationBank, clockInvalidReject,
     safetyConfigLockdown, commLossFailSafe, pinChangeRefused,
     mqttTopicBinding, otaDurableReservation, qosTimeoutBudget,
     staleCommandBlocked)
  8. ALL signoffs present (testEngineer, reviewer, releaseManager)

If ANY check fails → BLOCKED. The release is NOT promoted.

Usage (from CI):
  python3 scripts/verify_gate_qualification.py \\
      --version 1.9.4 \\
      --source-commit "$GITHUB_SHA" \\
      --release-json ci-artifacts/modular/release.json

Exit:  0 = qualification verified
       1 = BLOCKED (with reason)
"""
import argparse
import json
import sys
from pathlib import Path

LEGACY_CHECKS = [
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

GATE_CHECKS = [
    # [GATE-1] PH8-01: emergency OFF atomic vs in-flight relay ON
    "emergencyAtomicity",
    # [GATE-1] PH8-02: supervisor starvation drives the relay BANK off
    "watchdogStarvationBank",
    # [GATE-1] PH8-03: invalid clock rejects energizing commands
    "clockInvalidReject",
    # [GATE-1] PH8-04: safety-config immutable over remote CONFIG
    "safetyConfigLockdown",
    # [GATE-1b] PH8-05: per-channel comm-loss fail-safe (lease + policy)
    "commLossFailSafe",
    # [GATE-1] PH8-08: pin change refused while energized
    "pinChangeRefused",
    # [GATE-3] S1-02: foreign MQTT topic rejected + counted
    "mqttTopicBinding",
    # [GATE-4] S1-03: OTA durable reservation survives partial failure
    "otaDurableReservation",
    # [GATE-5] delivery semantics: honest QoS + timeout budget + jitter
    "qosTimeoutBudget",
    # [audit PH8-06 matrix] expired/stale command after reconnect blocked
    "staleCommandBlocked",
    # [GATE-7b] P7-S1-02: journal retention honest capacity + outage
    # drain + power-cycle crash recovery + bounded mid-drain duplicates
    "telemetryJournalRetention",
]

REQUIRED_CHECKS = LEGACY_CHECKS + GATE_CHECKS

REQUIRED_SIGNOFFS = [
    "testEngineer",
    "reviewer",
    "releaseManager",
]


def run() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="Release version (e.g., 1.9.4)")
    ap.add_argument("--source-commit", help="Source commit that was built AND hardware-tested")
    ap.add_argument("--ci-sha", help=argparse.SUPPRESS)
    ap.add_argument("--release-json", required=True, type=Path)
    ap.add_argument("--hw-dir", type=Path,
                    default=Path("docs/hardware-acceptance"))
    args = ap.parse_args()

    blockers = []
    hw_json = args.hw_dir / f"v{args.version}-gate.json"

    def blocked():
        print()
        print("=" * 72)
        print("GATE QUALIFICATION = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        print("A release containing the gate remediations may NOT be promoted")
        print("until the HIL qualification evidence exists and is complete.")
        print("Procedure: docs/hardware-acceptance/HIL-PROCEDURE.md")
        return 1

    # 1. File exists.
    if not hw_json.is_file():
        blockers.append(
            f"gate qualification file not found: {hw_json}\n"
            f"  Complete the HIL procedure (docs/hardware-acceptance/HIL-PROCEDURE.md) "
            f"on real ESP32 hardware, then export the verdict to {hw_json} "
            f"(schema: gate-qualification.template.json)"
        )
        print(f"[FAIL] file-exists: {hw_json} not found")
        return blocked()
    print(f"[PASS] file-exists: {hw_json}")

    # 2. JSON parseable.
    try:
        hw = json.loads(hw_json.read_text())
    except Exception as e:
        blockers.append(f"gate qualification JSON parse error: {e}")
        print(f"[FAIL] json-parse: {e}")
        return blocked()
    print("[PASS] json-parse: valid JSON")

    # 3. schemaVersion.
    sv = hw.get("schemaVersion")
    if sv != 1:
        blockers.append(f"schemaVersion must be 1, got {sv}")
        print(f"[FAIL] schemaVersion: {sv}")
    else:
        print(f"[PASS] schemaVersion: {sv}")

    # 4. version.
    hw_version = hw.get("version", "")
    if hw_version != args.version:
        blockers.append(f"version '{hw_version}' != tag version '{args.version}'")
        print(f"[FAIL] version: '{hw_version}' != '{args.version}'")
    else:
        print(f"[PASS] version: {hw_version}")

    # release.json (for source commit + firmware sha)
    rel = None
    if args.release_json.is_file():
        try:
            rel = json.loads(args.release_json.read_text())
        except Exception as e:
            blockers.append(f"release.json parse error: {e}")
            print(f"[FAIL] release-json: parse error: {e}")
    else:
        blockers.append(f"release.json not found: {args.release_json}")
        print(f"[FAIL] release-json: not found")

    # 5. gitCommit == source commit.
    expected = args.source_commit or args.ci_sha or (rel or {}).get("gitCommit", "")
    hw_commit = hw.get("gitCommit", "")
    if not expected:
        blockers.append("could not determine expected source commit")
        print("[FAIL] gitCommit: no source commit to compare against")
    elif not hw_commit:
        blockers.append("gitCommit is empty in gate qualification JSON")
        print("[FAIL] gitCommit: empty")
    elif hw_commit == expected:
        print(f"[PASS] gitCommit: matches source commit {expected[:12]}")
    else:
        blockers.append(f"gitCommit '{hw_commit[:12]}' != source commit '{expected[:12]}'")
        print(f"[FAIL] gitCommit: mismatch")

    # 6. firmwareSha256 == release.json.firmwareSha256.
    if rel is not None:
        rel_sha = rel.get("firmwareSha256", "")
        hw_sha = hw.get("firmwareSha256", "")
        if not hw_sha:
            blockers.append("firmwareSha256 is empty in gate qualification JSON")
            print("[FAIL] firmwareSha256: empty")
        elif hw_sha == rel_sha:
            print(f"[PASS] firmwareSha256: matches release artifact {hw_sha[:16]}…")
        else:
            blockers.append(f"firmwareSha256 '{hw_sha[:16]}' != release '{rel_sha[:16]}'")
            print("[FAIL] firmwareSha256: mismatch (evidence is for a different binary!)")

    # 7. verdict.
    verdict = hw.get("verdict", "")
    if verdict != "PASS":
        blockers.append(f"verdict must be 'PASS', got '{verdict}'")
        print(f"[FAIL] verdict: {verdict}")
    else:
        print("[PASS] verdict: PASS")

    # 8. ALL required checks PASS.
    checks = hw.get("checks", {})
    missing = [c for c in REQUIRED_CHECKS if c not in checks]
    # [HARDENING 2026-09-18 hil-runner session] null-leniency ditembus: nilai
    # null sebelumnya DIKECUALIKAN dari not_pass (checks.get(c) not in (None,))
    # sehingga 22 check null + verdict PASS + signoff bisa lolos sebagai
    # VERIFIED. Check yang tidak BUKAN "PASS" eksplisit = blocker (fail-closed).
    not_pass = [c for c in REQUIRED_CHECKS if checks.get(c) != "PASS"]
    for c in missing:
        blockers.append(f"check '{c}' missing — HIL procedure incomplete")
        print(f"[FAIL] check: {c} missing")
    for c in not_pass:
        blockers.append(f"check '{c}' = '{checks.get(c)}' — must be PASS")
        print(f"[FAIL] check: {c} = {checks.get(c)}")
    n_pass = sum(1 for c in REQUIRED_CHECKS if checks.get(c) == "PASS")
    if not missing and not not_pass:
        print(f"[PASS] checks: {len(REQUIRED_CHECKS)}/{len(REQUIRED_CHECKS)} PASS "
              f"(11 legacy safety + 10 gate remediation)")
    else:
        print(f"[FAIL] checks: {n_pass}/{len(REQUIRED_CHECKS)} PASS")
    extra = set(checks.keys()) - set(REQUIRED_CHECKS)
    if extra:
        print(f"[WARN] extra-checks: {sorted(extra)} (allowed, not required)")

    # 9. Signoffs.
    for role in REQUIRED_SIGNOFFS:
        val = hw.get(role, "")
        if not val or not str(val).strip():
            blockers.append(f"signoff '{role}' missing/empty")
            print(f"[FAIL] signoff: {role} empty")
        else:
            print(f"[PASS] signoff: {role}")

    if blockers:
        return blocked()
    print()
    print("=" * 72)
    print("GATE QUALIFICATION = VERIFIED")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(run())
