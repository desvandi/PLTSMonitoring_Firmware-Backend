#!/usr/bin/env python3
"""
verify_tag_signature.py — [Audit 9 P0-1] Verify Git tag signature + identity.

Enforces the production release tag invariants that the Auditor demanded:
  1. Tag MUST be annotated + signed (GPG)
  2. Tag signature MUST verify against an authorized signer
  3. Tag's target commit MUST equal GITHUB_SHA (the CI build commit)
  4. Tag name MUST match the version in release.json

This script is called by the CI `release-publish` job BEFORE creating a
GitHub Release. If ANY check fails, the release is BLOCKED.

Authorized signers are configured via:
  - File: .github/authorized-signers (one GPG key ID per line, # comments)
  - Or CI secret: AUTHORIZED_SIGNERS (newline-separated key IDs)

Usage (from CI):
  python3 scripts/verify_tag_signature.py \\
      --tag v1.7.1 \\
      --ci-sha "$GITHUB_SHA" \\
      --release-json ci-artifacts/plts-firmware-modular-production/release.json \\
      [--authorized-signers-file .github/authorized-signers]

Exit:  0 = tag is authenticated + matches release identity
       1 = BLOCKED (with reason)
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path


def git(*args: str, cwd: str | None = None) -> str:
    r = subprocess.run(
        ["git", *args],
        cwd=cwd, capture_output=True, text=True, check=False,
    )
    return r.stdout.strip()


def run() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True, help="Git tag name (e.g., v1.7.1)")
    ap.add_argument("--ci-sha", required=True, help="GITHUB_SHA — the commit CI built from")
    ap.add_argument("--release-json", required=True, type=Path,
                    help="release.json from the production artifact")
    ap.add_argument("--authorized-signers-file", type=Path,
                    default=Path(".github/authorized-signers"),
                    help="File with authorized GPG key IDs (one per line)")
    ap.add_argument("--authorized-signers-env", type=str, default=None,
                    help="Newline-separated authorized key IDs (from CI secret)")
    args = ap.parse_args()

    blockers = []

    # ------------------------------------------------------------------
    # 1. Tag must be annotated + signed.
    # ------------------------------------------------------------------
    # `git cat-file -t <tag>` returns "tag" for annotated tags, "commit" for lightweight tags.
    # [CI fix] Use refs/tags/ prefix to ensure git resolves the tag OBJECT,
    # not the commit it points to. Without the prefix, some git configurations
    # resolve the tag name to the commit (especially in shallow clones or
    # CI checkouts where refs are set up differently).
    tag_ref = args.tag if args.tag.startswith("refs/tags/") else f"refs/tags/{args.tag}"
    tag_type = git("cat-file", "-t", tag_ref)
    if tag_type != "tag":
        blockers.append(
            f"tag {args.tag} is '{tag_type}', not 'tag' — "
            f"production release tags MUST be annotated + signed "
            f"(use: git tag -s -a v1.7.1 -m 'Release v1.7.1')"
        )
        print(f"[FAIL] tag-type: {tag_type} (expected: tag)")
    else:
        print(f"[PASS] tag-type: annotated tag")

    # ------------------------------------------------------------------
    # 2. Tag signature must verify.
    # ------------------------------------------------------------------
    if tag_type == "tag":
        verify_result = subprocess.run(
            ["git", "tag", "-v", tag_ref],
            capture_output=True, text=True, check=False,
        )
        # git tag -v exits 0 if signature is valid, non-zero otherwise.
        # The output contains "Good signature from ..." or "BAD signature".
        verify_output = (verify_result.stdout + verify_result.stderr).strip()
        if verify_result.returncode != 0:
            blockers.append(
                f"tag {args.tag} signature verification FAILED (exit {verify_result.returncode}). "
                f"Output: {verify_output[:500]}"
            )
            print(f"[FAIL] tag-signature: verification failed")
        else:
            print(f"[PASS] tag-signature: valid GPG signature")

            # Extract the signer key ID from the verification output.
            # GPG output format: "Good signature from <name> <email> [key: ABCDEF12]"
            # or: "Primary key fingerprint: 1234 5678 90AB CDEF ..."
            signer_key_id = None
            for line in verify_output.splitlines():
                line = line.strip()
                if line.startswith("Primary key fingerprint:"):
                    # Extract the fingerprint (last 16 chars without spaces)
                    fp = line.split(":", 1)[1].strip().replace(" ", "")
                    signer_key_id = fp[-16:] if len(fp) >= 16 else fp
                    break
                if "key " in line.lower() and "[" in line:
                    # Try to extract [key: XXXX] format
                    import re
                    m = re.search(r"\[key:\s*([0-9A-Fa-f]+)\]", line)
                    if m:
                        signer_key_id = m.group(1)
                        break

            if not signer_key_id:
                # [Audit 10 P0-1 FIX] FAIL-CLOSED: if we cannot extract the
                # signer's key ID, we CANNOT verify authorization. The
                # signature may be valid, but we don't know WHO signed it.
                # This is a security-critical gap — BLOCK the release.
                # Previous behavior was WARN (continue without authorization
                # check), which allowed any valid GPG signature to pass.
                tag_info = git("for-each-ref", f"refs/tags/{args.tag}",
                               "--format=%(taggername) %(taggeremail)")
                blockers.append(
                    f"could not extract signer key ID from GPG output — "
                    f"cannot verify authorization. tagger: {tag_info}. "
                    f"This is FAIL-CLOSED: a valid signature from an unknown "
                    f"signer is BLOCKED (Audit 10 P0-1)."
                )
                print(f"[FAIL] tag-signer: could not extract signer key ID — BLOCKED (fail-closed)")
            else:
                print(f"[PASS] tag-signature: signer key ID = {signer_key_id}")

                # ------------------------------------------------------------------
                # 3. Signer must be authorized.
                # ------------------------------------------------------------------
                authorized = set()
                if args.authorized_signers_env:
                    authorized.update(
                        line.strip().upper()
                        for line in args.authorized_signers_env.splitlines()
                        if line.strip() and not line.strip().startswith("#")
                    )
                if args.authorized_signers_file and args.authorized_signers_file.is_file():
                    for line in args.authorized_signers_file.read_text().splitlines():
                        line = line.strip()
                        if line and not line.startswith("#"):
                            authorized.add(line.upper())

                if authorized:
                    # Check if signer_key_id matches any authorized key (suffix match)
                    is_authorized = any(
                        signer_key_id.upper().endswith(auth) or auth.endswith(signer_key_id.upper())
                        for auth in authorized
                    )
                    if not is_authorized:
                        blockers.append(
                            f"tag {args.tag} signed by unauthorized key {signer_key_id}. "
                            f"Authorized keys: {sorted(authorized)}"
                        )
                        print(f"[FAIL] tag-signer: unauthorized key {signer_key_id}")
                    else:
                        print(f"[PASS] tag-signer: key {signer_key_id} is authorized")
                else:
                    # [Audit 10 P0-1 FIX] FAIL-CLOSED: empty authorized-signers
                    # list = NO authorization enforcement. A valid signature
                    # from ANY key would pass. This is unacceptable for
                    # production — BLOCK.
                    blockers.append(
                        f"no authorized-signers configured (neither "
                        f"--authorized-signers-file nor --authorized-signers-env has entries). "
                        f"Production release tags MUST have at least one authorized GPG key ID. "
                        f"Configure .github/authorized-signers or the AUTHORIZED_SIGNERS CI secret."
                    )
                    print(f"[FAIL] tag-signer: no authorized-signers configured — BLOCKED (fail-closed)")

    # ------------------------------------------------------------------
    # 4. Tag's target commit MUST equal GITHUB_SHA.
    # ------------------------------------------------------------------
    tag_commit = git("rev-list", "-n", "1", tag_ref)
    if not tag_commit:
        blockers.append(f"could not resolve tag {args.tag} to a commit")
        print(f"[FAIL] tag-commit: could not resolve")
    elif tag_commit != args.ci_sha:
        blockers.append(
            f"tag {args.tag} points to commit {tag_commit[:12]} but CI built from "
            f"GITHUB_SHA={args.ci_sha[:12]} — tag MUST point to the exact commit "
            f"that was built and tested"
        )
        print(f"[FAIL] tag-commit: {tag_commit[:12]} != GITHUB_SHA {args.ci_sha[:12]}")
    else:
        print(f"[PASS] tag-commit: {tag_commit[:12]} == GITHUB_SHA")

    # ------------------------------------------------------------------
    # 5. Tag name MUST match release.json version.
    # ------------------------------------------------------------------
    if not args.release_json.is_file():
        blockers.append(f"release.json not found: {args.release_json}")
        print(f"[FAIL] tag-version: release.json missing")
    else:
        try:
            rel = json.loads(args.release_json.read_text())
            rel_version = rel.get("version", "")
            expected_tag = f"v{rel_version}"
            if args.tag != expected_tag:
                blockers.append(
                    f"tag name '{args.tag}' does not match release.json version "
                    f"'{rel_version}' (expected tag: '{expected_tag}')"
                )
                print(f"[FAIL] tag-version: '{args.tag}' != expected '{expected_tag}'")
            else:
                print(f"[PASS] tag-version: '{args.tag}' matches release.json version '{rel_version}'")

            # Also verify release.json.gitCommit == GITHUB_SHA
            rel_commit = rel.get("gitCommit", "")
            if rel_commit and rel_commit != args.ci_sha:
                blockers.append(
                    f"release.json.gitCommit={rel_commit[:12]} != GITHUB_SHA={args.ci_sha[:12]} "
                    f"— provenance mismatch (binary may not be from this commit)"
                )
                print(f"[FAIL] provenance-commit: release.json.gitCommit != GITHUB_SHA")
            elif rel_commit:
                print(f"[PASS] provenance-commit: release.json.gitCommit == GITHUB_SHA")
        except Exception as e:
            blockers.append(f"release.json parse error: {e}")
            print(f"[FAIL] release-json: parse error: {e}")

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    if blockers:
        print("=" * 72)
        print("TAG AUTHENTICATION = BLOCKED")
        print("=" * 72)
        for b in blockers:
            print(f"  - {b}")
        return 1
    print("=" * 72)
    print("TAG AUTHENTICATION = PASS")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(run())
