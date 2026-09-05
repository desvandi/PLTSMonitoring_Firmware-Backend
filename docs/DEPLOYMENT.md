# Deployment & Release Governance

> **Consolidated from:** `P2_BRANCH_PROTECTION_AND_GPG_SIGNING.md`
> **Related:** `README.md` §6 (Panduan Deployment), `docs/AUDIT_2026_09_REMEDIATION.md` (P2-1, Audit 9 P0-1)

---

## Daftar Isi

1. [Branch Protection — `main`](#1-branch-protection--main)
2. [Tag Protection — Release Tags](#2-tag-protection--release-tags)
3. [GPG Signing Setup (Per Engineer)](#3-gpg-signing-setup-per-engineer)
4. [Release Tag Signing](#4-release-tag-signing)
5. [CI Enforcement](#5-ci-enforcement)
6. [Binary Distribution Policy](#6-binary-distribution-policy)
7. [CODEOWNERS](#7-codeowners-optional-but-recommended)

---

## 1. Branch Protection — `main`

> **Status: REQUIRED — Audit 9 P0-1 blocker.** Implementation requires GitHub repository settings (Settings → Branches → Branch protection rules). Cannot be enforced purely via files in the repo.

Open **GitHub → Settings → Branches → Add rule** for `main`:

| Setting | Value | Why |
|---------|-------|-----|
| Require pull request before merging | ON | No direct pushes to main |
| Required approvals | 0 (solo dev) / 1+ (team) | Peer review when available |
| Dismiss stale approvals on new push | ON | Re-review after changes |
| Require review from code owners | ON | If `CODEOWNERS` exists |
| Require status checks to pass | ON | CI gate is mandatory |
| Required status checks | `Python unit + property tests`, `PlatformIO build — env:staging`, `PlatformIO build — firmware-generic`, `Master release gate` | Every build job |
| Require branches to be up to date | ON | Catches merge-time regressions |
| Require conversation resolution | ON | No unresolved threads |
| Require linear history | ON | Merge commits, not rebase noise |
| Include administrators | ON | No bypassing the rules |
| Restrict who can push to matching branches | (no one — PRs only) | Force-push banned |

> **Without these settings, the CI `verify-tag-signature` job will still verify tag signatures at runtime, but GitHub itself won't prevent unauthorized users from pushing unsigned tags. Both layers are needed.**

---

## 2. Tag Protection — Release Tags

Open **GitHub → Settings → Tags → Add rule** for `v*`:

| Setting | Value |
|---------|-------|
| Tag name pattern | `v*` |
| Allowed to create tags | Release managers team only |
| Require signed tags | ON |

Alternative: use GitHub Rulesets API (as implemented in this repo — see scripts that manage ruleset IDs 22245583 + 22245588).

---

## 3. GPG Signing Setup (Per Engineer)

Each engineer who can merge PRs or create release tags MUST sign their commits:

```bash
# 1. Generate a GPG key (if you don't have one)
gpg --full-generate-key
#    Key type: RSA and RSA (default)
#    Bits: 4096
#    Real name: <your name>
#    Email: <your GitHub-verified email>
#    Expiration: 2 years

# 2. List your GPG keys and copy the KEY_ID (the long hex after 'rsa4096/')
gpg --list-secret-keys --keyid-format=long

# 3. Configure git to sign with this key
git config --global user.signingkey <KEY_ID>
git config --global commit.gpgsign true
git config --global tag.gpgsign true

# 4. Export the PUBLIC key and add it to GitHub
gpg --armor --export <KEY_ID> | pbcopy   # or xclip -selection clipboard
# Paste at: https://github.com/settings/gpg/new

# 5. Verify
echo "test" | gpg --clearsign | gpg --verify
git commit -S -m "test signed commit"   # should work without prompting
```

### Adding UID with GitHub-Verified Email

If your GPG key was created with a non-GitHub email, add a UID:

```bash
gpg --quick-add-uid <KEY_ID> 'Your Name <your-github-email@gmail.com>'
gpg --quick-set-primary-uid <KEY_ID> 'Your Name <your-github-email@gmail.com>'
gpg --armor --export <KEY_ID>  # re-export and re-upload to GitHub
```

---

## 4. Release Tag Signing

When tagging a release:

```bash
git tag -s v1.9.2 -m "Release v1.9.2 — INA219 dynamic gain (canonical fix)

Release chain (all verified):
  source → build → sign → gate → tag-verify → hw-verify → release

Hardware acceptance: PASSED (docs/hardware-acceptance/v1.9.2.json)
Release gate: PASS
Ed25519 firmware signing key fingerprint: <SHA-256 of public key>

Signed-off-by: PLTS Release Manager <release@plts-monitoring.local>"
git push origin v1.9.2
```

GitHub will show "Verified" next to the tag. Anyone cloning can verify:

```bash
git tag --verify v1.9.2
```

### Authorized Signers

Authorized GPG key IDs are listed in `.github/authorized-signers`. The CI `verify-tag-signature` job checks that the tag signer's key ID matches one of the authorized keys.

---

## 5. CI Enforcement

The CI pipeline (`.github/workflows/build-firmware.yml`) includes a `verify-tag-signature` job that runs on tag pushes:

```yaml
- name: Verify tag signature + identity
  env:
    AUTHORIZED_SIGNERS: ${{ secrets.AUTHORIZED_SIGNERS }}
    EXPECTED_TAGGER_EMAILS: ${{ secrets.EXPECTED_TAGGER_EMAILS }}
  run: |
    TAG="${GITHUB_REF#refs/tags/}"
    python3 scripts/verify_tag_signature.py \
      --tag "$TAG" \
      --ci-sha "$GITHUB_SHA" \
      --release-json ci-artifacts/modular/release.json \
      --authorized-signers-env "$AUTHORIZED_SIGNERS" \
      --expected-tagger-emails "$EXPECTED_TAGGER_EMAILS"
```

This verifies:
1. Tag is annotated + signed (GPG)
2. Signature verifies against an authorized signer
3. Tagger email matches expected identity (defense-in-depth)
4. Tag's target commit equals GITHUB_SHA
5. Tag name matches release.json version

---

## 6. Binary Distribution Policy

> **Source:** `firmware-generic/bin/README.md` (P0-4, P0-6)

Firmware binaries (`*.bin`) are **gitignored** in both repos — they are immutable release artifacts, NOT source code. Binaries are distributed via:

| Channel | Location | Purpose |
|---------|----------|---------|
| GitHub Actions artifacts | CI run artifacts (temporary, 90 days) | Build verification |
| GitHub Releases | `releases/download/v*/modular-firmware.bin` | Production OTA (canonical source) |
| ESP Web Tools | `public/firmware/*.bin` (PWA repo, synced at release time) | First-install via browser |

### Cross-Repo Integrity

The `prepush-audit.js` script enforces byte-identical copies of `Panduan_Deploy_Production_MonitorIoT.pdf` across both repos (firmware + PWA). This prevents documentation drift.

### Reproducibility

Release binaries include `buildTimestamp` + `gitCommit` in the binary metadata, making builds non-deterministic. Future v1.9.3+ will strip `buildTimestamp` for reproducible builds (target: `same source + same toolchain = same SHA`).

---

## 7. CODEOWNERS (Optional but Recommended)

Create `.github/CODEOWNERS` in each repo:

```
# Default owners — required reviewers on every PR
* @desvandi

# Security-sensitive areas — require additional review
/code.gs/         @desvandi
/firmware/Utils/  @desvandi
/scripts/sign_firmware.py  @desvandi

# CI / release engineering
/.github/workflows/  @desvandi
/scripts/release_gate.py  @desvandi
/scripts/release_firmware_generic.py  @desvandi
```
