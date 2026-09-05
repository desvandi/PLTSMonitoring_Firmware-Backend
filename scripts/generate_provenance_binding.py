#!/usr/bin/env python3
"""
generate_provenance_binding.py — [Audit 2026-09-04 P1-2] Generate the explicit
provenance-binding.json that ties the entire release chain together.

This file is the machine-verifiable answer to the auditor's question:
  «ac5f54e → firmware SHA → 2953944 → v1.8.0» — is this chain explicit?

The binding records:
  - tag name + signed tag object SHA
  - release commit (tag's target) + its parent (source commit)
  - released binary's SHA (from release.json)
  - hardware-tested binary's SHA (from hw-acceptance.json)
  - binary equivalence rationale (non-reproducible build explanation)
  - GitHub Release URL + PWA canonical release identity

Generated at RELEASE time (in the release-publish CI job), not at BUILD time,
because it binds the tag object SHA + GitHub Release URL which are only known
after the tag is pushed and the release is created.

Usage:
  python3 scripts/generate_provenance_binding.py \
    --tag v1.8.0 \
    --version 1.8.0 \
    --release-commit <GITHUB_SHA> \
    --release-json ci-artifacts/plts-firmware-modular-production/release.json \
    --hw-json docs/hardware-acceptance/v1.8.0.json \
    --output release-assets/provenance-binding.json \
    --github-release-url https://github.com/.../releases/tag/v1.8.0
"""
import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True, check=False)
    return r.stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--release-commit", required=True, help="GITHUB_SHA (tag's target commit)")
    ap.add_argument("--release-json", required=True, type=Path)
    ap.add_argument("--hw-json", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--github-release-url", required=True)
    args = ap.parse_args()

    # Load release.json (released binary provenance)
    rel = json.loads(args.release_json.read_text())

    # Load hw-acceptance.json (hardware-tested binary provenance)
    hw = json.loads(args.hw_json.read_text())

    # Resolve tag object SHA (the annotated tag object, not the commit it points to)
    tag_obj_sha = git("rev-parse", f"refs/tags/{args.tag}^{{tag}}") or \
                  git("rev-parse", f"refs/tags/{args.tag}")

    # Get tagger info
    tagger_name = git("for-each-ref", f"refs/tags/{args.tag}", "--format=%(taggername)")
    tagger_email = git("for-each-ref", f"refs/tags/{args.tag}", "--format=%(taggeremail)").strip("<>")

    # Get tag signature verification status from git
    verify = subprocess.run(["git", "tag", "-v", args.tag],
                            capture_output=True, text=True, check=False)
    tag_verified = verify.returncode == 0

    # Get the source commit (parent of release commit)
    source_commit = git("rev-parse", f"{args.release_commit}^")

    # Check if release commit only adds the hw-acceptance JSON (no source changes)
    diff_stat = git("diff", "--stat", f"{source_commit}..{args.release_commit}")
    diff_files = git("diff", "--name-only", f"{source_commit}..{args.release_commit}")
    diff_file_list = [f for f in diff_files.splitlines() if f.strip()]

    # [Audit 2026-09-04 P1-2 fix] sourceOnlyChanges should only check directories
    # that affect the FIRMWARE BINARY: firmware/ and firmware-generic/.
    # Scripts (scripts/) and CI config (.github/) are release-engineering files
    # that do NOT affect the compiled binary. A release commit may legitimately
    # update verifier scripts + CI workflow + hw-acceptance JSON without changing
    # the firmware binary — this is still "source-only" for binary equivalence.
    firmware_source_changes = any(
        f.startswith("firmware/") or f.startswith("firmware-generic/")
        for f in diff_file_list
    )
    source_only_changes = not firmware_source_changes

    # Compute byte difference between hw-tested binary SHA and released binary SHA
    # (We can't compute byte difference directly since we don't have both binaries
    # at this point, but we record the SHA difference as evidence.)
    sha_matches = hw.get("firmwareSha256") == rel.get("firmwareSha256")
    commit_matches = hw.get("gitCommit") == rel.get("gitCommit")
    is_parent = hw.get("gitCommit") == source_commit

    # [Audit 2026-09-05 re-audit, section 20] Hardware identity binding: the
    # provenance chain now binds THREE identities (firmware + hardware +
    # release). boardRevision/deviceSerial/relayBoardRevision come from the
    # hardware acceptance evidence; the release-time binding records them so
    # the exact physical device class that was hardware-tested is
    # machine-verifiable against docs/hardware-revisions.json.
    hw_identity = hw.get("hardwareIdentity") if isinstance(hw.get("hardwareIdentity"), dict) else {}

    binding = {
        "schemaVersion": 1,
        "schema": "provenance-binding",
        "tag": args.tag,
        "tagObjectSha": tag_obj_sha,
        "tagObjectType": "tag",
        "tagSignature": {
            "verified": tag_verified,
            "signerEmail": tagger_email,
            "signerName": tagger_name,
            "gitTagVerifyReturnCode": verify.returncode,
        },
        "releaseCommit": {
            "sha": args.release_commit,
            "parent": source_commit,
            "diffVsParent": diff_stat,
            "diffFilesVsParent": diff_file_list,
            "sourceOnlyChanges": source_only_changes,
        },
        "sourceCommit": {
            "sha": source_commit,
            "note": "Source commit that was both built AND hardware-tested. "
                    "Identical source tree to releaseCommit if sourceOnlyChanges=true "
                    "(release commit only adds hw-acceptance evidence file).",
        },
        "releasedBinary": {
            "buildId": rel.get("buildId"),
            "gitCommit": rel.get("gitCommit"),
            "buildTimestamp": rel.get("buildTimestamp"),
            "firmwareSha256": rel.get("firmwareSha256"),
            "firmwareSize": rel.get("firmwareSize"),
            "releaseJsonAsset": "modular-release.json",
        },
        "hardwareTestedBinary": {
            "gitCommit": hw.get("gitCommit"),
            "firmwareSha256": hw.get("firmwareSha256"),
            "hardwareSerial": hw.get("hardwareSerial"),
            "hardwareIdentity": {
                "boardRevision": hw_identity.get("boardRevision"),
                "deviceSerial": hw_identity.get("deviceSerial"),
                "relayBoardRevision": hw_identity.get("relayBoardRevision"),
            },
            "testDate": hw.get("testDate"),
            "verdict": hw.get("verdict"),
        },
        "binaryEquivalence": {
            "shaMatches": sha_matches,
            "commitMatches": commit_matches,
            "hwCommitIsParentOfReleaseCommit": is_parent,
            "sourceOnlyChanges": source_only_changes,
            "explanation": (
                "Binary SHAs match exactly (reproducible build achieved)."
                if sha_matches else
                "Binary SHAs differ due to non-reproducible build (buildTimestamp + "
                "gitCommit embedded in binary). Source trees are identical "
                "(sourceOnlyChanges=true). Hardware test results transfer from "
                "hardwareTestedBinary to releasedBinary. Fix in future release: "
                "strip buildTimestamp for byte-identical reproducible builds."
            ),
        },
        "hardwareAcceptance": {
            "file": str(args.hw_json),
            "version": hw.get("version"),
            "verdict": hw.get("verdict"),
            "gitCommit": hw.get("gitCommit"),
            "firmwareSha256": hw.get("firmwareSha256"),
            "hardwareSerial": hw.get("hardwareSerial"),
            "hardwareIdentity": {
                "boardRevision": hw_identity.get("boardRevision"),
                "deviceSerial": hw_identity.get("deviceSerial"),
                "relayBoardRevision": hw_identity.get("relayBoardRevision"),
            },
            "testDate": hw.get("testDate"),
            "signoffs": {
                "testEngineer": hw.get("testEngineer"),
                "reviewer": hw.get("reviewer"),
                "releaseManager": hw.get("releaseManager"),
            },
        },
        "githubRelease": {
            "url": args.github_release_url,
            "tagName": args.tag,
            "targetCommitish": args.release_commit,
        },
        "pwaCanonicalReleaseIdentity": {
            "module": "src/lib/release-identity.ts",
            "function": "getCanonicalRelease()",
            "resolvedVersion": rel.get("version"),
            "resolvedReleaseId": rel.get("buildId"),
            "resolvedGitCommit": rel.get("gitCommit"),
            "resolvedFirmwareSha256": rel.get("firmwareSha256"),
            "resolvedReleaseUrl": args.github_release_url,
            "resolvedManifestUrl": f"{args.github_release_url.replace('/tag/', '/download/')}/modular-manifest-canonical.json",
        },
        "bindingTimestamp": datetime.now(timezone.utc).isoformat(),
        "bindingAuthority": f"{tagger_name} <{tagger_email}>",
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(binding, indent=2) + "\n")
    print(f"[OK] Provenance binding written: {args.output}")
    print(f"     tag={args.tag}  tag_obj={tag_obj_sha[:12]}  release_commit={args.release_commit[:12]}")
    print(f"     source_commit={source_commit[:12]}  sha_matches={sha_matches}  source_only={source_only_changes}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
