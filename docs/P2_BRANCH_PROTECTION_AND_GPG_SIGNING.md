# Branch Protection & GPG Signing Recommendations (P2-1)

> **Status: RECOMMENDED** — implementation requires GitHub repository settings
> (Settings → Branches → Branch protection rules) and per-engineer GPG setup.
> These cannot be enforced purely via files in the repo. This document is the
> authoritative reference for the configuration operators must apply.

## 1. Branch Protection — `main`

Open **GitHub → Settings → Branches → Add rule** for `main`:

| Setting | Value | Why |
|---------|-------|-----|
| Require pull request before merging | ON | No direct pushes to main |
| Required approvals | 1+ | Peer review mandatory |
| Dismiss stale approvals on new push | ON | Re-review after changes |
| Require review from code owners | ON | If `CODEOWNERS` exists |
| Require status checks to pass | ON | CI gate is mandatory |
| Required status checks | `test`, `build-production`, `build-staging`, `build-generic-tree`, `release-gate`, `ci` (PWA) | Every build job |
| Require branches to be up to date | ON | Catches merge-time regressions |
| Require conversation resolution | ON | No unresolved threads |
| Require signed commits | ON | **P2-1 — GPG signing** |
| Require linear history | ON | Merge commits, not rebase noise |
| Include administrators | ON | No bypassing the rules |
| Restrict who can push to matching branches | (no one — PRs only) | Force-push banned |

## 2. Tag Protection — release tags

Open **GitHub → Settings → Tags → Add rule** for `v*`:

| Setting | Value |
|---------|-------|
| Tag name pattern | `v*` |
| Allowed to create tags | Release managers team only |
| Require signed tags | ON |

## 3. GPG Signing Setup (per engineer)

Each engineer who can merge PRs or create release tags MUST sign their
commits. Steps:

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

## 4. Release Tag Signing

When tagging a release:

```bash
git tag -s v1.7.2 -m "Release v1.7.2

Hardware acceptance: PASSED (docs/hardware-acceptance/v1.7.2.md)
Release gate: PASS
Ed25519 firmware signing key fingerprint: <SHA-256 of public key>
"
git push origin v1.7.2
```

GitHub will show "Verified" next to the tag. Anyone cloning can verify:

```bash
git tag --verify v1.7.2
```

## 5. CI Enforcement

Add to `.github/workflows/build-firmware.yml` `release-gate` job (already
exists; this is the additional verification step):

```yaml
- name: Verify release tag signature (if running on a tag)
  if: startsWith(github.ref, 'refs/tags/v')
  run: |
    # GitHub verifies the tag signature server-side; this is a defense-in-depth
    # check that the tag was actually signed (unsigned tags would have been
    # rejected by branch protection, but we re-verify here).
    if ! git tag --verify "${GITHUB_REF##*/}"; then
      echo "FAIL: release tag is not GPG-signed — refusing to publish"
      exit 1
    fi
```

## 6. CODEOWNERS (optional but recommended)

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
