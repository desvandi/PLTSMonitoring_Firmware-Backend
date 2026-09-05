#!/usr/bin/env python3
"""
verify_ina219_hardware_acceptance.py — [v1.9.1 / HW-02] INA219 hardware
acceptance gate for measurement-integrity changes.

This is a SEPARATE gate from verify_hardware_acceptance.py (which covers the
general boot/sensors/alarms/OTA checklist). This gate specifically validates
the INA219 dynamic gain switching + voltage divider changes introduced in
v1.9.0/v1.9.1.

Why a separate gate:
  The v1.9.0 INA219 PGA register bug (inverted bit-mapping → both modes used
  ±320mV) passed the general hardware acceptance because the general checklist
  doesn't test measurement accuracy across the full 1A-150A range. This gate
  enforces 12 INA219-specific criteria that would have caught the bug:
    - Config register readback (proves PGA bits are correct in hardware)
    - Low/mid/peak current accuracy (proves measurement correctness)
    - PGA UP/DOWN transitions (proves dynamic switching works)
    - Hysteresis stability (proves no chattering)
    - Current sign (proves VIN+/VIN- wiring)
    - Voltage divider accuracy (proves 190k/10k calibration)
    - Power calculation (proves V×I consistency)
    - Telemetry pga_mode matches hardware (proves end-to-end chain)

Usage (from CI or local):
  python3 scripts/verify_ina219_hardware_acceptance.py \\
      --version 1.9.1 \\
      --ci-sha "$GITHUB_SHA" \\
      --release-json ci-artifacts/modular/release.json \\
      --hw-dir docs/hardware-acceptance

Exit:  0 = INA219 hardware acceptance verified (all 12 checks PASS)
       1 = BLOCKED (with reason)
"""
import argparse
import json
import sys
from pathlib import Path


REQUIRED_CHECKS = [
    "configReadback",
    "lowCurrentAccuracy",
    "midCurrentAccuracy",
    "preTransitionCurrent",
    "pgaUpTransition",
    "peakCurrentMeasurement",
    "pgaDownTransition",
    "hysteresisStability",
    "currentSignCorrectness",
    "voltageDividerAccuracy",
    "powerCalculation",
    "telemetryPgaModeCorrect",
]

REQUIRED_SIGNOFFS = [
    "testEngineer",
    "reviewer",
    "releaseManager",
]

# INA219 config register constants — CORRECTED per TI datasheet SBOS448G (v1.9.2)
# These are the CANONICAL correct values. The v1.9.1 values (0x152B/0x252B) were
# wrong — they used an incorrect bit-field layout that caused the register to
# actually decode to ±160mV/±40mV with 10-bit SADC and triggered (one-shot) mode.
# Verified via scripts/test_ina219_config_registers.py (canonical cross-check
# with datasheet reset value 0x399F confirms the bit-field layout is correct).
EXPECTED_CONFIG_80MV  = 0x0FFF   # ±80mV,  12b/128s, shunt+bus cont, 16V FSR
EXPECTED_CONFIG_160MV = 0x17FF   # ±160mV, 12b/128s, shunt+bus cont, 16V FSR

# Known buggy values — if the readback matches any of these, BLOCK immediately
# with a clear message that the firmware is running old/wrong constants.
KNOWN_BUGGY_VALUES = {
    0x152B: "v1.9.1 wrong bit-field layout (actually ±160mV/10-bit-SADC/triggered)",
    0x252B: "v1.9.1 wrong bit-field layout (actually ±40mV/32V/10-bit-SADC/triggered)",
    0x3BFF: "v1.9.0 inverted PGA mapping (actually ±320mV)",
    0x39FF: "v1.9.0 inverted PGA mapping (actually ±320mV)",
    0x3FFB: "v1.9.0 legacy (actually ±320mV + triggered mode)",
}


def run() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="Release version (e.g., 1.9.1)")
    ap.add_argument("--ci-sha", help="GITHUB_SHA — the commit CI built from (DEPRECATED: use --source-commit)")
    ap.add_argument("--source-commit", help="Source commit that was built AND hardware-tested")
    ap.add_argument("--release-json", required=True, type=Path,
                    help="release.json from the production artifact")
    ap.add_argument("--hw-dir", type=Path,
                    default=Path("docs/hardware-acceptance"),
                    help="Directory containing hardware acceptance files")
    args = ap.parse_args()

    blockers = []
    warnings = []

    # --- 1. File exists ---
    hw_json = args.hw_dir / f"v{args.version}.json"
    if not hw_json.is_file():
        blockers.append(
            f"INA219 hardware acceptance file not found: {hw_json}\n"
            f"  Execute the test protocol in {args.hw_dir}/v{args.version}.md "
            f"on real ESP32 hardware with INA219 + shunt + voltage divider, "
            f"then export the verdict to {hw_json}"
        )
        print(f"[FAIL] file-exists: {hw_json} not found")
        print()
        print("=" * 72)
        print("INA219 HARDWARE ACCEPTANCE = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print(f"[PASS] file-exists: {hw_json}")

    # --- 2. JSON parseable ---
    try:
        hw = json.loads(hw_json.read_text())
    except Exception as e:
        blockers.append(f"hardware acceptance JSON parse error: {e}")
        print(f"[FAIL] json-parse: {e}")
        return 1
    print(f"[PASS] json-parse: valid JSON")

    # --- 3. schemaVersion ---
    sv = hw.get("schemaVersion")
    if sv != 1:
        blockers.append(f"schemaVersion must be 1, got {sv}")
        print(f"[FAIL] schemaVersion: {sv} (expected: 1)")
    else:
        print(f"[PASS] schemaVersion: {sv}")

    # --- 4. version matches ---
    hw_version = hw.get("version", "")
    if hw_version != args.version:
        blockers.append(f"hardware acceptance version '{hw_version}' != expected '{args.version}'")
        print(f"[FAIL] version: '{hw_version}' != '{args.version}'")
    else:
        print(f"[PASS] version: '{hw_version}'")

    # --- 5. gitCommit (source commit, not release commit — same chicken-and-egg fix as v1.8.0) ---
    rel = None
    if args.release_json.is_file():
        try:
            rel = json.loads(args.release_json.read_text())
        except Exception as e:
            blockers.append(f"release.json parse error: {e}")
            print(f"[FAIL] release-json: parse error: {e}")

    expected_source_commit = args.source_commit or args.ci_sha
    if not expected_source_commit and rel is not None:
        expected_source_commit = rel.get("gitCommit", "")
    if not expected_source_commit:
        blockers.append("could not determine expected source commit")
        print(f"[FAIL] gitCommit: no source commit to compare against")
    else:
        hw_commit = hw.get("gitCommit", "")
        if not hw_commit:
            blockers.append("gitCommit is empty in hardware acceptance JSON")
            print(f"[FAIL] gitCommit: empty")
        elif hw_commit == expected_source_commit:
            print(f"[PASS] gitCommit: matches source commit {expected_source_commit[:12]}")
        else:
            # Check parent-child relationship (release commit only adds hw-acceptance JSON)
            try:
                import subprocess
                parent_check = subprocess.run(
                    ["git", "rev-parse", "--verify", f"{expected_source_commit}^"],
                    capture_output=True, text=True, check=False
                )
                parent_sha = parent_check.stdout.strip()
                if parent_sha == hw_commit:
                    print(f"[PASS] gitCommit: {hw_commit[:12]} is the parent of release commit {expected_source_commit[:12]}")
                else:
                    blockers.append(
                        f"hardware acceptance gitCommit '{hw_commit[:12]}' does not match the expected source commit "
                        f"'{expected_source_commit[:12]}' and is not its git parent"
                    )
                    print(f"[FAIL] gitCommit: '{hw_commit[:12]}' != source '{expected_source_commit[:12]}' and not parent")
            except Exception as e:
                blockers.append(f"could not verify gitCommit parent relationship: {e}")
                print(f"[FAIL] gitCommit: parent check error: {e}")

    # --- 6. firmwareSha256 (WARNING if mismatch — non-reproducible build, documented) ---
    if rel is not None:
        rel_fw_sha = rel.get("firmwareSha256", "")
        hw_fw_sha = hw.get("firmwareSha256", "")
        if not hw_fw_sha:
            blockers.append("firmwareSha256 is empty in hardware acceptance JSON")
            print(f"[FAIL] firmwareSha256: empty")
        elif hw_fw_sha == rel_fw_sha:
            print(f"[PASS] firmwareSha256: matches release.json (exact binary identity)")
        else:
            warnings.append(
                f"firmwareSha256 mismatch: hw-acceptance='{hw_fw_sha[:16]}...' != "
                f"release.json='{rel_fw_sha[:16]}...'. Expected with non-reproducible builds. "
                f"Functional equivalence assumed because source commits match."
            )
            print(f"[WARN] firmwareSha256: mismatch (non-reproducible build — see provenance-binding.json)")

    # --- 7. configReadbackHex — CRITICAL for INA219 measurement integrity ---
    readback_hex = hw.get("configReadbackHex", "").lower().replace("0x", "")
    if not readback_hex:
        blockers.append("configReadbackHex is empty — must record the Serial Monitor readback value")
        print(f"[FAIL] configReadbackHex: empty")
    else:
        try:
            readback_val = int(readback_hex, 16)
            if readback_val == EXPECTED_CONFIG_80MV:
                print(f"[PASS] configReadbackHex: 0x{readback_hex} == expected 0x{EXPECTED_CONFIG_80MV:04X} (±80mV, 12b/128s, continuous)")
            elif readback_val == EXPECTED_CONFIG_160MV:
                print(f"[PASS] configReadbackHex: 0x{readback_hex} == expected 0x{EXPECTED_CONFIG_160MV:04X} (±160mV, 12b/128s, continuous)")
            elif readback_val in KNOWN_BUGGY_VALUES:
                bug_reason = KNOWN_BUGGY_VALUES[readback_val]
                blockers.append(
                    f"configReadbackHex 0x{readback_hex} is a KNOWN BUGGY value ({bug_reason}). "
                    f"The firmware is still running old/wrong constants. Reflash with v1.9.2+ "
                    f"(correct values: 0x{EXPECTED_CONFIG_80MV:04X} for ±80mV, 0x{EXPECTED_CONFIG_160MV:04X} for ±160mV)."
                )
                print(f"[FAIL] configReadbackHex: 0x{readback_hex} is a KNOWN BUGGY value!")
                print(f"       Reason: {bug_reason}")
            else:
                blockers.append(
                    f"configReadbackHex 0x{readback_hex} does not match any expected value "
                    f"(0x{EXPECTED_CONFIG_80MV:04X} for ±80mV or 0x{EXPECTED_CONFIG_160MV:04X} for ±160mV) "
                    f"and is not a known buggy value. Unknown register configuration."
                )
                print(f"[FAIL] configReadbackHex: 0x{readback_hex} is unexpected")
        except ValueError:
            blockers.append(f"configReadbackHex '{readback_hex}' is not valid hex")
            print(f"[FAIL] configReadbackHex: not valid hex")

    # --- 8. configReadbackPgaBits — must be 1 (±80mV) at boot ---
    pga_bits = hw.get("configReadbackPgaBits")
    if pga_bits is None:
        blockers.append("configReadbackPgaBits is empty — must record the PGA bits from Serial Monitor readback")
        print(f"[FAIL] configReadbackPgaBits: empty")
    elif pga_bits == 1:
        print(f"[PASS] configReadbackPgaBits: 1 (0b01 = ±80mV at boot — correct)")
    elif pga_bits == 3:
        blockers.append(
            f"configReadbackPgaBits=3 (0b11 = ±320mV) — this is the BUGGY v1.9.0 value! "
            f"The firmware is still running old constants. Reflash with v1.9.1+."
        )
        print(f"[FAIL] configReadbackPgaBits: 3 (BUGGY ±320mV)")
    else:
        blockers.append(f"configReadbackPgaBits={pga_bits} — expected 1 (±80mV at boot)")
        print(f"[FAIL] configReadbackPgaBits: {pga_bits} (expected 1)")

    # --- 9. verdict == PASS ---
    verdict = hw.get("verdict", "")
    if verdict != "PASS":
        blockers.append(
            f"verdict is '{verdict}', not 'PASS' — v1.9.1 INA219 measurement chain NOT accepted"
        )
        print(f"[FAIL] verdict: '{verdict}' (expected: PASS)")
    else:
        print(f"[PASS] verdict: PASS")

    # --- 10. ALL 12 required checks == PASS ---
    checks = hw.get("checks", {})
    for check_name in REQUIRED_CHECKS:
        check_val = checks.get(check_name)
        if check_val is None:
            blockers.append(f"check '{check_name}' is missing")
            print(f"[FAIL] check-{check_name}: missing")
        elif check_val != "PASS":
            blockers.append(f"check '{check_name}' is '{check_val}', not 'PASS'")
            print(f"[FAIL] check-{check_name}: '{check_val}'")
        else:
            print(f"[PASS] check-{check_name}: PASS")

    # --- 11. ALL signoffs present ---
    for signoff_field in REQUIRED_SIGNOFFS:
        signoff_val = hw.get(signoff_field, "")
        if not signoff_val or not str(signoff_val).strip():
            blockers.append(f"signoff '{signoff_field}' is empty")
            print(f"[FAIL] signoff-{signoff_field}: empty")
        else:
            print(f"[PASS] signoff-{signoff_field}: '{signoff_val}'")

    # --- 12. Measurement accuracy spot-checks (CRITICAL — P0-B: MANDATORY, not optional) ---
    # [v1.9.2 / P0-B] These fields are MANDATORY. Missing → BLOCK (not WARN).
    # The v1.9.1 verifier allowed empty measurement fields to pass with a warning,
    # creating a false-green path where checks=all PASS + verdict=PASS + signoffs
    # complete but NO actual measurement evidence existed. This is unacceptable
    # for measurement certification — the auditor correctly identified that
    # "only Criteria 2, 6, and 10 can prove measurement correctness" and these
    # are NOT optional.
    obs = hw.get("observed", {})

    # Define mandatory measurement fields with their validators
    MANDATORY_MEASUREMENTS = [
        # (field_name, display_name, tolerance_check_fn)
        ("lowCurrentPwa", "lowCurrentPwa (PWA reading at 1.5A)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("lowCurrentRef", "lowCurrentRef (reference meter at 1.5A)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("midCurrentPwa", "midCurrentPwa (PWA reading at 50A)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("midCurrentRef", "midCurrentRef (reference meter at 50A)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("peakCurrent150Pwa", "peakCurrent150Pwa (PWA reading at 150A)",
         lambda v: v is not None and isinstance(v, (int, float)) and v >= 140),
        ("peakCurrent150Ref", "peakCurrent150Ref (reference meter at 150A)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("voltagePwa", "voltagePwa (PWA battery voltage)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("voltageRef", "voltageRef (reference meter voltage)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("voltageDelta", "voltageDelta (|PWA - ref|)",
         lambda v: v is not None and isinstance(v, (int, float)) and v <= 0.5),
        ("powerPwa", "powerPwa (PWA power reading)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("powerExpected", "powerExpected (V×I calculated)",
         lambda v: v is not None and isinstance(v, (int, float))),
        ("powerDeltaPct", "powerDeltaPct (|PWA - expected| / expected × 100)",
         lambda v: v is not None and isinstance(v, (int, float)) and v <= 5.0),
        ("hysteresisChatterCount", "hysteresisChatterCount (PGA switches in 30s at 95A)",
         lambda v: v is not None and isinstance(v, int) and v <= 2),
        ("transitionUpObserved", "transitionUpObserved (PGA UP at 100A)",
         lambda v: v is True),
        ("transitionDownObserved", "transitionDownObserved (PGA DOWN at 90A)",
         lambda v: v is True),
        ("dischargeSignCorrect", "dischargeSignCorrect (discharge = negative)",
         lambda v: v is True),
        ("telemetryPgaModeMatchesHardware", "telemetryPgaModeMatchesHardware",
         lambda v: v is True),
    ]

    print()
    print("--- MANDATORY measurement evidence (P0-B: missing = BLOCK) ---")
    for field_name, display_name, validator in MANDATORY_MEASUREMENTS:
        val = obs.get(field_name)
        if val is None:
            blockers.append(
                f"MANDATORY measurement field '{field_name}' is MISSING. "
                f"Measurement evidence is NOT optional (P0-B). "
                f"Record the {display_name} in the evidence JSON."
            )
            print(f"[FAIL] {field_name}: MISSING (mandatory — BLOCK)")
        elif not validator(val):
            blockers.append(
                f"MANDATORY measurement field '{field_name}' = {val} FAILED validation. "
                f"Expected: {display_name}"
            )
            print(f"[FAIL] {field_name}: {val} (validation failed)")
        else:
            print(f"[PASS] {field_name}: {val}")

    # Additional accuracy cross-checks (delta calculations)
    low_pwa = obs.get("lowCurrentPwa")
    low_ref = obs.get("lowCurrentRef")
    if low_pwa is not None and low_ref is not None:
        delta = abs(low_pwa - low_ref)
        if delta > 0.5:
            blockers.append(f"lowCurrent accuracy FAILED: |PWA({low_pwa}) - ref({low_ref})| = {delta:.2f}A > 0.5A tolerance")
            print(f"[FAIL] lowCurrent-accuracy: delta={delta:.2f}A > 0.5A")
        else:
            print(f"[PASS] lowCurrent-accuracy: delta={delta:.2f}A ≤ 0.5A")

    mid_pwa = obs.get("midCurrentPwa")
    mid_ref = obs.get("midCurrentRef")
    if mid_pwa is not None and mid_ref is not None:
        delta = abs(mid_pwa - mid_ref)
        if delta > 1.0:
            blockers.append(f"midCurrent accuracy FAILED: |PWA({mid_pwa}) - ref({mid_ref})| = {delta:.2f}A > 1.0A tolerance")
            print(f"[FAIL] midCurrent-accuracy: delta={delta:.2f}A > 1.0A")
        else:
            print(f"[PASS] midCurrent-accuracy: delta={delta:.2f}A ≤ 1.0A")

    # Peak current 150A: must NOT cap at ~106A (that would indicate ±80mV mode is still active)
    peak150 = obs.get("peakCurrent150Pwa")
    if peak150 is not None:
        if peak150 < 140:
            blockers.append(
                f"peakCurrent150Pwa={peak150}A — reading caps below 150A. "
                f"If near 106A, the PGA switch to ±160mV is NOT working (v1.9.0/v1.9.1 bug symptom). "
                f"Verify criterion 5 (PGA UP transition) actually occurred."
            )
            print(f"[FAIL] peakCurrent150: {peak150}A (capped — PGA switch may not be working)")
        else:
            print(f"[PASS] peakCurrent150: {peak150}A (no saturation — ±160mV mode active)")

    # Voltage accuracy
    v_pwa = obs.get("voltagePwa")
    v_ref = obs.get("voltageRef")
    if v_pwa is not None and v_ref is not None:
        v_delta = abs(v_pwa - v_ref)
        if v_delta > 0.5:
            blockers.append(f"voltage accuracy FAILED: |PWA({v_pwa}) - ref({v_ref})| = {v_delta:.2f}V > 0.5V tolerance")
            print(f"[FAIL] voltage-accuracy: delta={v_delta:.2f}V > 0.5V")
        else:
            print(f"[PASS] voltage-accuracy: delta={v_delta:.2f}V ≤ 0.5V")

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
        print("INA219 HARDWARE ACCEPTANCE = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print("=" * 72)
    print("INA219 HARDWARE ACCEPTANCE = PASS")
    print("=" * 72)
    print(f"  All {len(REQUIRED_CHECKS)} criteria PASS, all {len(REQUIRED_SIGNOFFS)} signoffs present.")
    print(f"  Config register readback verified (0x{EXPECTED_CONFIG_80MV:04X} = ±80mV at boot).")
    print(f"  Measurement accuracy verified against reference meter.")
    if warnings:
        print(f"  {len(warnings)} warning(s) recorded (non-blocking).")
    print(f"  v{args.version} INA219 measurement chain is ACCEPTED.")
    return 0


if __name__ == "__main__":
    sys.exit(run())
