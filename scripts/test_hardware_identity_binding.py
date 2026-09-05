#!/usr/bin/env python3
"""
test_hardware_identity_binding.py — [Audit 2026-09-05 re-audit, sections 17+20]

Self-test for the three-identity (firmware + hardware + release) evidence
binding introduced for v1.9.3+:

  verify_ina219_hardware_acceptance.py   — hardwareIdentity required for
                                           v1.9.3+; boardRevision must be
                                           release-eligible per
                                           docs/hardware-revisions.json.
  verify_ota_evidence.py                 — same rules for OTA evidence.
  generate_provenance_binding.py         — binds hardwareIdentity into the
                                           release provenance chain.

IMPORTANT: this test builds SYNTHETIC evidence files in a TEMPORARY directory.
Nothing here fabricates acceptance evidence — the files exist only to prove
the verifiers' BLOCK/PASS behavior. The audit's rule stands: template /
synthetic data can never become committed evidence
(docs/hardware-acceptance/v1.9.3.json must come from a real device).

Usage: python3 scripts/test_hardware_identity_binding.py
Exit:  0 = all verifier behaviors correct
       1 = a verifier no longer blocks (or no longer passes) as designed
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY = sys.executable

FAILURES = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not cond else ""))
    if not cond:
        FAILURES.append(f"{name}: {detail}")


def run(script, args, cwd=None):
    return subprocess.run(
        [PY, str(REPO / "scripts" / script)] + [str(a) for a in args],
        capture_output=True, text=True, cwd=str(cwd) if cwd else None,
    )


# ---------------------------------------------------------------- fixtures ---

def valid_ina219_evidence(version="1.9.3", hardware_identity=None, serial="ESP32-A1B2C3D4"):
    hw = {
        "schemaVersion": 1,
        "version": version,
        "gitCommit": "a" * 40,
        "firmwareSha256": "f" * 64,
        "verdict": "PASS",
        "testEngineer": "Test Engineer",
        "reviewer": "Reviewer",
        "releaseManager": "Release Manager",
        "testDate": "2026-09-05",
        "hardwareSerial": serial,
        "configReadbackHex": "0x0FFF",
        "configReadbackPgaBits": 1,
        "adcFineTuneApplied": 1.0,
        "checks": {
            "configReadback": "PASS",
            "lowCurrentAccuracy": "PASS",
            "midCurrentAccuracy": "PASS",
            "preTransitionCurrent": "PASS",
            "pgaUpTransition": "PASS",
            "peakCurrentMeasurement": "PASS",
            "pgaDownTransition": "PASS",
            "hysteresisStability": "PASS",
            "currentSignCorrectness": "PASS",
            "voltageDividerAccuracy": "PASS",
            "powerCalculation": "PASS",
            "telemetryPgaModeCorrect": "PASS",
        },
        "observed": {
            "lowCurrentPwa": 1.6, "lowCurrentRef": 1.5,
            "midCurrentPwa": 50.4, "midCurrentRef": 50.0,
            "preTransitionPwa": 90.3, "preTransitionRef": 90.0,
            "transitionUpObserved": True, "transitionUpPwa": 100.2, "transitionUpRef": 100.0,
            "peakCurrent120Pwa": 120.5, "peakCurrent120Ref": 120.0,
            "peakCurrent150Pwa": 149.8, "peakCurrent150Ref": 150.0,
            "transitionDownObserved": True, "transitionDownPwa": 89.7, "transitionDownRef": 90.0,
            "hysteresisChatterCount": 0,
            "dischargeSignCorrect": True, "chargeSignCorrect": True,
            "voltagePwa": 53.2, "voltageRef": 53.4, "voltageDelta": 0.2,
            "powerPwa": 2660.0, "powerExpected": 2670.0, "powerDeltaPct": 0.37,
            "telemetryPgaModeMatchesHardware": True,
        },
        "notes": "synthetic self-test fixture (never committed as evidence)",
    }
    if hardware_identity is not None:
        hw["hardwareIdentity"] = hardware_identity
    return hw


def valid_identity(serial="ESP32-A1B2C3D4", board="bench-prototype"):
    return {
        "boardRevision": board,
        "deviceSerial": serial,
        "relayBoardRevision": "none",
        "notes": "",
    }


def valid_release_json():
    return {
        "version": "1.9.3",
        "buildId": "selftest-build",
        "gitCommit": "a" * 40,
        "firmwareSha256": "f" * 64,
        "firmwareSize": 1234567,
    }


def valid_ota_evidence(hardware_identity=None, serial="ESP32-A1B2C3D4"):
    ota = {
        "schemaVersion": 1,
        "version": "1.9.3",
        "testDate": "2026-09-05",
        "hardwareSerial": serial,
        "deviceStartingVersion": "1.8.0",
        "canonicalReleaseVersion": "1.9.3",
        "canonicalReleaseFirmwareSha256": "f" * 64,
        "canonicalReleaseGitCommit": "a" * 40,
        "canonicalReleaseUrl": "https://github.com/desvandi/PLTSMonitoring_Firmware-Backend/releases/tag/v1.9.3",
        "verdict": "PASS",
        "testEngineer": "Test Engineer",
        "reviewer": "Reviewer",
        "releaseManager": "Release Manager",
        "checks": {name: "PASS" for name in [
            "preOtaDeviceRunning", "pwaFetchesCanonicalRelease", "pwaResolvesVersionAndSha",
            "pwaTriggersOta", "deviceReceivesArtifact", "signatureVerificationPassed",
            "sha256VerificationPassed", "downloadComplete", "bootRebootSucceeded",
            "deviceRunning180", "deviceReportsCorrectIdentity", "relaySafeAfterReboot",
            "sensorsAlarmsNormal", "otaTerminalState", "pwaBackendSeeSameState",
            "rollbackRecoveryWorks",
        ]},
        "observed": {
            "preOtaVersion": "1.8.0", "preOtaUptimeSec": 600,
            "pwaCanonicalVersion": "1.9.3", "pwaCanonicalSha256": "f" * 64,
            "pwaCanonicalShaMatchedClient": True,
            "pwaFailClosedPrePublish": True,
            "pwaLatestMismatchWarningShown": True,
            "uploadDurationSec": 95, "uploadBytes": 1234567,
            "deviceLifecycleEvents": [], "postOtaVersion": "1.9.3",
            "postOtaBuildProfile": "production", "postOtaUptimeSec": 30,
            "postOtaHeapFreeKB": 120, "relayStateAfterReboot": "SAFE",
            "sensorReadingsNormal": True, "alarmsActiveAfterOta": False,
            "otaHistoryEntryStatus": "ACTIVATED", "otaHistorySourceBadge": "canonical",
            "rollbackTestResult": "PASS",
        },
        "notes": "synthetic self-test fixture (never committed as evidence)",
    }
    if hardware_identity is not None:
        ota["hardwareIdentity"] = hardware_identity
    return ota


# ------------------------------------------------------------ test driver ---

def ina219_cases(tmp: Path):
    release_json = tmp / "release.json"
    release_json.write_text(json.dumps(valid_release_json(), indent=2))
    hw_dir = tmp / "hw"
    hw_dir.mkdir()

    def run_case(name, evidence, expect_rc, expect_msg):
        hw_dir.mkdir(exist_ok=True)
        target = hw_dir / f"v{evidence.get('version', '1.9.3')}.json"
        target.write_text(json.dumps(evidence, indent=2))
        r = run(
            "verify_ina219_hardware_acceptance.py",
            ["--version", evidence.get("version", "1.9.3"),
             "--source-commit", "a" * 40,
             "--release-json", release_json,
             "--hw-dir", hw_dir],
        )
        out = r.stdout + r.stderr
        check(name, r.returncode == expect_rc,
              f"expected rc={expect_rc}, got {r.returncode}\n{out[-1500:]}")
        if expect_msg:
            check(f"{name} :: message", expect_msg in out, f"missing '{expect_msg}'\n{out[-1500:]}")
        target.unlink()

    run_case("INA219: valid v1.9.3 evidence + bench-prototype identity → PASS",
             valid_ina219_evidence(hardware_identity=valid_identity()), 0, None)

    e = valid_ina219_evidence(hardware_identity=valid_identity(board="S12"))
    run_case("INA219: boardRevision=S12 (dev-only) → BLOCK", e, 1, "development-only")

    e = valid_ina219_evidence(hardware_identity=valid_identity(board="S10"))
    run_case("INA219: boardRevision=S10 (dev-only) → BLOCK", e, 1, "development-only")

    e = valid_ina219_evidence(hardware_identity=None)
    run_case("INA219: hardwareIdentity missing → BLOCK", e, 1, "hardwareIdentity block is MISSING")

    e = valid_ina219_evidence(hardware_identity=valid_identity(board="S99"))
    run_case("INA219: boardRevision not in registry → BLOCK", e, 1, "is NOT declared")

    e = valid_ina219_evidence(hardware_identity=valid_identity(serial="OTHER-SERIAL"))
    run_case("INA219: deviceSerial != hardwareSerial → BLOCK", e, 1, "mismatch with hardwareSerial")

    hi = valid_identity()
    hi["deviceSerial"] = ""
    e = valid_ina219_evidence(hardware_identity=hi)
    run_case("INA219: deviceSerial empty → BLOCK", e, 1, "deviceSerial is empty")

    # Version gate: v1.9.2 evidence (old schema, no hardwareIdentity) must
    # still PASS — the identity requirement is strictly v1.9.3+.
    e = valid_ina219_evidence(version="1.9.2", hardware_identity=None)
    run_case("INA219: v1.9.2 old-schema evidence (no hardwareIdentity) → PASS (version gate)",
             e, 0, None)


def ota_cases(tmp: Path):
    release_json = tmp / "release.json"
    release_json.write_text(json.dumps(valid_release_json(), indent=2))
    ota_dir = tmp / "ota"
    ota_dir.mkdir()

    def run_case(name, evidence, expect_rc, expect_msg):
        ota_dir.mkdir(exist_ok=True)
        target = ota_dir / f"v{evidence.get('version', '1.9.3')}.json"
        target.write_text(json.dumps(evidence, indent=2))
        r = run(
            "verify_ota_evidence.py",
            ["--version", evidence.get("version", "1.9.3"),
             "--canonical-release-json", release_json,
             "--ota-dir", ota_dir],
        )
        out = r.stdout + r.stderr
        check(name, r.returncode == expect_rc,
              f"expected rc={expect_rc}, got {r.returncode}\n{out[-1500:]}")
        if expect_msg:
            check(f"{name} :: message", expect_msg in out, f"missing '{expect_msg}'\n{out[-1500:]}")
        target.unlink()

    run_case("OTA: valid v1.9.3 evidence + bench-prototype identity → PASS",
             valid_ota_evidence(hardware_identity=valid_identity()), 0, None)

    e = valid_ota_evidence(hardware_identity=valid_identity(board="S12"))
    run_case("OTA: boardRevision=S12 (dev-only) → BLOCK", e, 1, "development-only")

    e = valid_ota_evidence(hardware_identity=None)
    run_case("OTA: hardwareIdentity missing → BLOCK", e, 1, "hardwareIdentity block is MISSING")

    e = valid_ota_evidence(hardware_identity=valid_identity(serial="OTHER-SERIAL"))
    run_case("OTA: deviceSerial != hardwareSerial → BLOCK", e, 1, "mismatch with hardwareSerial")


def provenance_case(tmp: Path):
    """generate_provenance_binding.py must bind hardwareIdentity into the
    provenance chain (three-identity rule)."""
    repo = tmp / "repo"
    repo.mkdir()
    env_git = ["git", "-C", str(repo)]

    def git(*args):
        return subprocess.run(list(env_git) + list(args), capture_output=True, text=True, check=False)

    git("init", "-q")
    git("config", "user.name", "Selftest")
    git("config", "user.email", "selftest@example.invalid")
    # source commit
    (repo / "fw.txt").write_text("source\n")
    git("add", "-A")
    git("commit", "-q", "-m", "source")
    source_sha = git("rev-parse", "HEAD").stdout.strip()
    # release commit (evidence file only)
    hw = valid_ina219_evidence(hardware_identity=valid_identity())
    hw["gitCommit"] = source_sha
    hw_file = repo / "hw.json"
    hw_file.write_text(json.dumps(hw, indent=2))
    git("add", "-A")
    git("commit", "-q", "-m", "evidence")
    release_sha = git("rev-parse", "HEAD").stdout.strip()
    git("tag", "-a", "v1.9.3-selftest", "-m", "selftest tag")

    rel = valid_release_json()
    rel["gitCommit"] = source_sha
    rel_file = repo / "release.json"
    rel_file.write_text(json.dumps(rel, indent=2))

    out_file = repo / "binding.json"
    r = run(
        "generate_provenance_binding.py",
        ["--tag", "v1.9.3-selftest",
         "--version", "1.9.3",
         "--release-commit", release_sha,
         "--release-json", rel_file,
         "--hw-json", hw_file,
         "--output", out_file,
         "--github-release-url", "https://github.com/x/y/releases/tag/v1.9.3-selftest"],
        cwd=repo,
    )
    check("provenance: generator exits 0", r.returncode == 0, r.stdout + r.stderr)
    if out_file.is_file():
        binding = json.loads(out_file.read_text())
        hi = binding.get("hardwareTestedBinary", {}).get("hardwareIdentity", {})
        check("provenance: hardwareTestedBinary.hardwareIdentity bound",
              hi.get("boardRevision") == "bench-prototype"
              and hi.get("deviceSerial") == "ESP32-A1B2C3D4"
              and hi.get("relayBoardRevision") == "none",
              json.dumps(binding.get("hardwareTestedBinary", {}), indent=2))
        check("provenance: hardwareAcceptance.hardwareIdentity bound",
              binding.get("hardwareAcceptance", {}).get("hardwareIdentity", {}).get("boardRevision") == "bench-prototype")
    else:
        check("provenance: binding file written", False, "binding.json not created")


def registry_case():
    """The registry itself must be well-formed and consumed by verifiers."""
    reg = json.loads((REPO / "docs" / "hardware-revisions.json").read_text())
    ids = [r.get("id") for r in reg.get("revisions", [])]
    check("registry: bench-prototype declared release-eligible",
          any(r.get("id") == "bench-prototype" and r.get("releaseEligible") for r in reg["revisions"]))
    check("registry: S10 and S12 declared development-only",
          all(any(r.get("id") == rev and not r.get("releaseEligible") for r in reg["revisions"])
              for rev in ("S10", "S12")))
    check("registry: no duplicate ids", len(ids) == len(set(ids)))
    for version_file in ("1.9.3",):
        tpl = json.loads((REPO / f"docs/hardware-acceptance/v{version_file}.template.json").read_text())
        check(f"template hw v{version_file}: hardwareIdentity present",
              isinstance(tpl.get("hardwareIdentity"), dict)
              and tpl["hardwareIdentity"].get("boardRevision") == "bench-prototype")
        ota_tpl = json.loads((REPO / f"docs/ota-physical-test/v{version_file}.template.json").read_text())
        check(f"template ota v{version_file}: hardwareIdentity present",
              isinstance(ota_tpl.get("hardwareIdentity"), dict)
              and ota_tpl["hardwareIdentity"].get("boardRevision") == "bench-prototype")


def main():
    print("=" * 72)
    print("test_hardware_identity_binding.py — three-identity rule self-test")
    print("(synthetic fixtures in a temp dir; NEVER committed as evidence)")
    print("=" * 72)
    with tempfile.TemporaryDirectory(prefix="plts-hwid-") as td:
        tmp = Path(td)
        registry_case()
        ina219_cases(tmp)
        ota_cases(tmp)
        provenance_case(tmp)
    print()
    if FAILURES:
        print(f"HARDWARE IDENTITY BINDING SELF-TEST = FAIL ({len(FAILURES)} failures)")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("HARDWARE IDENTITY BINDING SELF-TEST = PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
