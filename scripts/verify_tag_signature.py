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
    ap.add_argument("--expected-tagger-emails", type=str, default=None,
                    help="Comma-separated list of authorized tagger emails (e.g., 'desvandi101@gmail.com'). "
                         "If set, the tag's tagger email MUST match one of these. "
                         "Defense-in-depth for P0-1: prevents a valid signature from an unauthorized identity.")
    args = ap.parse_args()

    blockers = []

    # ------------------------------------------------------------------
    # 1. Tag must be annotated + signed.
    # ------------------------------------------------------------------
    # [CI fix] GitHub Actions checkout@v4 fetches tags as commit refs, not
    # tag objects. We need to explicitly fetch the tag object to verify
    # its type and signature. Use 'git fetch origin tag <tag>' to get the
    # actual annotated tag object.
    tag_ref = f"refs/tags/{args.tag}"
    # Try to fetch the tag object from origin (needed for CI)
    # [CI fix] GitHub Actions checkout@v4 resolves tag names to commits.
    # We need to fetch the actual tag object and update the local ref.
    subprocess.run(["git", "fetch", "origin", "tag", args.tag, "--force"],
                   capture_output=True, text=True, check=False)
    # Also update the local ref to point to the tag object, not the commit
    tag_obj_sha_raw = subprocess.run(
        ["git", "rev-parse", f"refs/tags/{args.tag}^{{tag}}"],
        capture_output=True, text=True, check=False
    ).stdout.strip()
    if tag_obj_sha_raw:
        # Force update the ref to point to the tag object
        subprocess.run(["git", "update-ref", f"refs/tags/{args.tag}", tag_obj_sha_raw],
                       capture_output=True, text=True, check=False)

    # Check the type of the OBJECT that the tag ref points to.
    # For annotated tags: 'git cat-file -t refs/tags/v1.8.0' returns 'tag'
    # For lightweight tags: returns 'commit'
    # But in CI, the ref might point to a commit even for annotated tags.
    # So also try: 'git rev-parse refs/tags/v1.8.0^{tag}' — this resolves
    # to the tag object SHA if it's annotated, or fails if lightweight.
    tag_obj_sha = ""
    try:
        tag_obj_sha = subprocess.run(
            ["git", "rev-parse", f"{tag_ref}^{{tag}}"],
            capture_output=True, text=True, check=False
        ).stdout.strip()
    except Exception:
        pass

    if tag_obj_sha and tag_obj_sha != tag_ref:
        # Annotated tag — tag_obj_sha is the SHA of the tag object
        tag_type = "tag"
    else:
        # Fallback: check the ref type directly
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
        # [CI fix] 'git tag -v' doesn't work with refs/tags/ prefix.
        # Use the tag name directly (without refs/tags/ prefix).
        verify_result = subprocess.run(
            ["git", "tag", "-v", args.tag],
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
            # GPG output format varies by version:
            #   - GPG 2.2.x: "Primary key fingerprint: 1234 5678 90AB CDEF ..."
            #   - GPG 2.4.x: "gpg:                using RSA key F3EA7A3F...5E0BB8EF44199645"
            #   - Some:      "Good signature from <name> <email> [key: ABCDEF12]"
            # [Audit 2026-09-04 P0-1 FIX] Added "using (RSA|ECDSA|EDDSA) key" pattern
            # because GPG 2.4.x omits the "Primary key fingerprint:" line.
            import re
            signer_key_id = None
            for line in verify_output.splitlines():
                line = line.strip()
                if line.startswith("Primary key fingerprint:"):
                    fp = line.split(":", 1)[1].strip().replace(" ", "")
                    signer_key_id = fp[-16:] if len(fp) >= 16 else fp
                    break
                # Pattern: "gpg: using RSA key F3EA7A3FC641EA0FE31FBB4C5E0BB8EF44199645"
                m = re.search(r"using\s+(?:RSA|ECDSA|EDDSA|DSA)\s+key\s+([0-9A-Fa-f]{16,40})", line)
                if m:
                    key_hex = m.group(1)
                    signer_key_id = key_hex[-16:]  # last 16 hex chars = long key ID
                    break
                # Pattern: [key: XXXX]
                m = re.search(r"\[key:\s*([0-9A-Fa-f]+)\]", line)
                if m:
                    signer_key_id = m.group(1)
                    break

            # Fallback: if extraction from `git tag -v` output failed, use
            # `gpg --list-packets` on the detached signature for a machine-
            # readable key ID. This is the most robust method.
            if not signer_key_id:
                try:
                    # Extract the signature block from the tag object and
                    # feed it to gpg --list-packets to find the issuer key ID.
                    tag_obj = subprocess.run(
                        ["git", "cat-file", "-p", args.tag],
                        capture_output=True, text=True, check=False
                    )
                    tag_text = tag_obj.stdout
                    # The PGP signature block is between "-----BEGIN PGP SIGNATURE-----"
                    # and "-----END PGP SIGNATURE-----"
                    sig_start = tag_text.find("-----BEGIN PGP SIGNATURE-----")
                    sig_end = tag_text.find("-----END PGP SIGNATURE-----")
                    if sig_start >= 0 and sig_end > sig_start:
                        sig_block = tag_text[sig_start:sig_end + len("-----END PGP SIGNATURE-----")]
                        pkt = subprocess.run(
                            ["gpg", "--list-packets", "--no-tty", "--batch"],
                            input=sig_block, capture_output=True, text=True, check=False
                        )
                        pkt_out = pkt.stdout + pkt.stderr
                        # Look for: ":signature packet: algo 1, keyid F3EA7A3FC641EA0FE31FBB4C5E0BB8EF44199645"
                        m = re.search(r"keyid\s+([0-9A-Fa-f]{16,40})", pkt_out)
                        if m:
                            signer_key_id = m.group(1)[-16:]
                except Exception:
                    pass

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
                # 3b. [Audit 2026-09-04 P0-1 DEFENSE-IN-DEPTH] Tagger email check.
                #
                # GitHub's signature verification requires the tagger email to
                # match a UID on the signing GPG key AND that UID's email must
                # be a verified email on the GitHub account. A valid GPG
                # signature from the correct key but with a mismatched tagger
                # email causes GitHub to report verification=false/bad_email.
                #
                # This check enforces the same invariant at CI level, so a
                # tag that would fail GitHub verification is caught locally
                # before the release is published.
                # ------------------------------------------------------------------
                if args.expected_tagger_emails:
                    expected_emails = [
                        e.strip().lower() for e in args.expected_tagger_emails.split(",")
                        if e.strip()
                    ]
                    tagger_email_raw = git("for-each-ref",
                                           f"refs/tags/{args.tag}",
                                           "--format=%(taggeremail)")
                    # tagger_email_raw looks like "<desvandi101@gmail.com>"
                    tagger_email = tagger_email_raw.strip().strip("<>").lower()
                    if not tagger_email:
                        blockers.append(
                            f"could not extract tagger email from tag {args.tag} — "
                            f"cannot verify email identity (P0-1 defense-in-depth)"
                        )
                        print(f"[FAIL] tagger-email: could not extract")
                    elif tagger_email not in expected_emails:
                        blockers.append(
                            f"tagger email '{tagger_email}' is not in the authorized list "
                            f"{expected_emails}. Even if the GPG signature is valid, GitHub "
                            f"will report verification=false/bad_email when the tagger email "
                            f"doesn't match a verified UID on the signing key. "
                            f"This check prevents releasing a tag that GitHub cannot verify."
                        )
                        print(f"[FAIL] tagger-email: '{tagger_email}' not in authorized list")
                    else:
                        print(f"[PASS] tagger-email: '{tagger_email}' is authorized")
                else:
                    print(f"[WARN] tagger-email: --expected-tagger-emails not set; skipping email identity check (P0-1 defense-in-depth)")

    # ------------------------------------------------------------------
    # 4. Tag's target commit MUST equal GITHUB_SHA.
    # ------------------------------------------------------------------
    tag_commit = git("rev-list", "-n", "1", args.tag)
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
