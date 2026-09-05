# RELEASE EXECUTION RUNBOOK — v1.9.3

> **Purpose:** step-by-step closure of the five P0 blockers from the
> 2026-09-05 re-audit. Everything the *agent* could do without physical
> hardware and push credentials is already done (P1 hardening + this
> runbook). The remaining P0s are **yours to execute** — they require the
> physical ESP32 bench, the PWA deployment, and your GPG signing key.
>
> **State of this runbook (2026-09-06):**
> - PWA P1 hardening commit: `46b9d37` (branch `fix/pwa-audit2-p1-release-policy`)
> - Firmware three-identity commit: `3a661bb` (branch `fix/audit2-hardware-identity`)
> - v1.9.3 software release line tip: `96cb34b` (main before PCB S12/S10 work)
> - Only tag on origin: `v1.8.0`. `GET /releases/tags/v1.9.3` → 404 (correct — not yet released).

---

## 0. Ground rules (from the re-audit — read first)

1. **No synthetic evidence.** `docs/hardware-acceptance/v1.9.3.json` and
   `docs/ota-physical-test/v1.9.3.json` must come from the ACTUAL device.
   Never copy a template and fill PASS.
2. **Do not build v1.9.3 from current `main`.** main now contains PCB S12/S10
   development work (`9cbd58c`). The release must be cut from the release
   line (`96cb34b`) + the release-engineering commit (`3a661bb`, docs/scripts
   only — zero firmware-source changes, so the binary is byte-unaffected).
3. **The PWA is the OTA authority instrument.** Deploy the PWA with the
   audit-fix + P1 hardening commits BEFORE Phase A.
4. **Hardware identity is mandatory (v1.9.3+).** Evidence must record
   `hardwareIdentity.boardRevision = "bench-prototype"` + the actual device
   serial. S10/S12 citations are BLOCKED (see `docs/HARDWARE_REVISIONS.md`).
5. **CI PASS ≠ hardware acceptance.** The physical sessions below are the
   acceptance.

Order of execution (why): you cannot publish before the hardware acceptance
exists (the release must carry it), and Phase B of the OTA test cannot run
before the release is published. Hence:
**merge code → build+flash → hardware acceptance → Phase A → publish →
Phase B → final provenance.**

---

## 1. Merge the prepared branches (15 min)

### 1a. PWA

```bash
cd PLTSMonitoring_PWA
git fetch origin
# apply the prepared patch (delivered as 0001-*.patch) on a branch, or add
# the remote branch if you pushed it:
git checkout -b fix/pwa-audit2-p1-release-policy origin/main
git am 0001-fix-v1.9.3-PWA-audit-re-audit-P1-hardening.patch   # = commit 46b9d37
git push -u origin fix/pwa-audit2-p1-release-policy
# open PR → CI must be 7/7 green (lint, typecheck, lockfile-integrity,
# test, build, production-config-gate, cross-repo-contract) → merge to main
```

Local verification already performed for this commit: **lint 0 errors ·
typecheck PASS · 179/179 tests · production build PASS · wrong-tag build
FAILS** (fail-closed proof). CI repeats all of it.

> Vercel: after merge, deploy production. Do NOT set
> `NEXT_PUBLIC_EXPECTED_FIRMWARE_TAG` in production (the invariant is
> pinned by `release-policy.json` = v1.9.3; a differing value now FAILS the
> build). Leave `NEXT_PUBLIC_RELEASE_CHANNEL` unset.

### 1b. Firmware

```bash
cd PLTSMonitoring_Firmware-Backend
git fetch origin
git checkout -b fix/audit2-hardware-identity origin/main
git am 0001-feat-v1.9.3-three-identity-evidence-binding.patch   # = commit 3a661bb
git push -u origin fix/audit2-hardware-identity
# open PR → CI green (python tests now include
# scripts/test_hardware_identity_binding.py — 28 checks) → merge to main
```

This commit touches **docs/ and scripts/ only** — `git diff --stat` shows
zero `firmware/` / `firmware-generic/` changes, so the v1.9.3 binary is
byte-identical with or without it (`sourceOnlyChanges=true`).

---

## 2. Cut the release branch from the RELEASE LINE (5 min)

```bash
cd PLTSMonitoring_Firmware-Backend
git fetch origin
git checkout -b release/v1.9.3 96cb34b          # release line tip (pre-PCB)
git cherry-pick 3a661bb                          # + the three-identity commit
# (equivalently: git cherry-pick origin/fix/audit2-hardware-identity)
git push -u origin release/v1.9.3
```

`96cb34b` = last v1.9.3 software release-candidate state (PR #17 merge:
INA219 canonical fix + reproducible build + verifier dispatch + OTA
protocol docs). Everything after it on main (PCB S12/S10) is excluded.

Record the branch tip — this is **SRC**, the source commit whose binary you
will hardware-test:

```bash
SRC=$(git rev-parse HEAD)          # release/v1.9.3 tip after cherry-pick
SRC_TS=$(git log -1 --format=%ct "$SRC")
echo "SRC=$SRC  SRC_TS=$SRC_TS"    # keep these for steps 3-5
```

Pushing the branch triggers the `build-firmware.yml` pipeline (scripts/**
path matches): build-staging + build-generic-tree + release-gate run; the
production build runs on push. Wait for the run to go green and download
the **`plts-firmware-modular-production`** artifact — it contains
`firmware.bin`, `release.json` (with `gitCommit == $SRC` and its
`firmwareSha256`), `provenance.json`, `SHA256SUMS`.

> The reproducible-build job on the tag push will rebuild this binary and
> require identical SHA — that is what makes the hardware-tested binary
> equal the released binary.

---

## 3. P0-01 — Execute the v1.9.3 hardware acceptance (half a day on the bench)

Follow `docs/hardware-acceptance/v1.9.3.md` (12 criteria) on the
**bench-prototype** hardware class. Checklist of the non-negotiables:

1. Flash the **CI-built** binary from step 2 (byte-exact guarantee):
   `pio run -e production -t upload` is fine ONLY if your local toolchain
   matches CI pins (PlatformIO 6.1.18); otherwise flash the artifact's
   `firmware.bin` via esptool.
2. Serial monitor must show: `[INA219 0x40] config readback OK: 0x0FFF`.
   If not 0x0FFF — STOP (wiring/Config.h problem).
3. Record, with the reference meter: 1.5A / 50A / 90A / 100A-transition /
   120–150A peak / 95A hysteresis-30s / sign / voltage / V×I.
4. Fill `docs/hardware-acceptance/v1.9.3.json` from the template. **All 17
   `observed` fields + all signoffs + `hardwareIdentity`:**
   - `boardRevision: "bench-prototype"`
   - `deviceSerial`: the real ESP32 module serial (must equal `hardwareSerial`)
   - `relayBoardRevision: "none"` (or the real one)
   - `gitCommit: $SRC`, `firmwareSha256`: from the artifact's `release.json`
5. Run the verifier locally:

```bash
python3 scripts/verify_ina219_hardware_acceptance.py \
  --version 1.9.3 \
  --source-commit "$SRC" \
  --release-json <downloaded-artifact>/release.json \
  --hw-dir docs/hardware-acceptance
# exit 0 = PASS
```

6. Commit the evidence **with the committer date pinned to SRC** (byte-exact
   recipe, REL-03):

```bash
git add docs/hardware-acceptance/v1.9.3.json
GIT_AUTHOR_DATE="@$SRC_TS +0000" GIT_COMMITTER_DATE="@$SRC_TS +0000" \
  git commit -m "docs(v1.9.3): hardware acceptance evidence — PASS (bench-prototype, 12/12 criteria)"
```

> Why the pinned date: the build embeds SOURCE_DATE_EPOCH from HEAD's
> committer timestamp. Same timestamp → the tag-time CI rebuild is
> byte-identical to the binary you tested → `provenance-binding.json`
> reports `binaryEquivalence.shaMatches = true`.

---

## 4. P0-02a — OTA Phase A, BEFORE publishing (30 min)

State now: authorized = v1.9.3 (pinned by the deployed PWA), GitHub
latest = v1.8.0, tag v1.9.3 does not exist.

1. Open the deployed PWA → OTA view → **Fetch Authorized Release**.
2. Expected: REFUSAL — “Authorized release v1.9.3 is not published yet …
   refuses to fall back to GitHub latest (v1.8.0)” plus the
   latest-mismatch warning. **No binary is offered for upload.**
3. Record in `docs/ota-physical-test/v1.9.3.json` (create from template when
   you start the session; keep it locally until step 6):
   - `observed.pwaFailClosedPrePublish = true`
   - `observed.pwaLatestMismatchWarningShown = true`
   - checks C1–C4 as observed.

If the PWA instead offers v1.8.0 for upload — STOP: the fail-closed policy
is broken; file it before proceeding.

---

## 5. P0-03 + P0-04 — Signed tag → CI → GitHub Release (1 h, mostly CI time)

```bash
cd PLTSMonitoring_Firmware-Backend
git checkout release/v1.9.3
git config user.name  "PLTS Release Manager"
git config user.email "desvandi101@gmail.com"     # MUST be this UID (key 5E0BB8EF44199645)

git tag -a v1.9.3 -s -m "Release v1.9.3 — Reproducible build + INA219 canonical chain (REL-03/REL-04 closed)

Signed-off-by: PLTS Release Manager <desvandi101@gmail.com>" \
  $EVIDENCE_COMMIT     # the evidence commit from step 3.6

git push origin release/v1.9.3 v1.9.3
```

The tag push runs the full chain — all must be green for the release to
publish:

```text
build-production (sign) → reproducible-build (byte-identical 2×)
→ release-gate (STRICT: signature + cross-repo + provenance)
→ verify-tag-signature (GPG + authorized signer + tagger email)
→ verify-hardware-acceptance (INA219 verifier, 12 criteria + hardwareIdentity)
→ release-publish → immutable GitHub Release v1.9.3 (20+ assets incl.
   provenance-binding.json with the three identities bound)
```

Acceptance for this step:

```bash
curl -s -o /dev/null -w "%{http_code}\n" \
  https://api.github.com/repos/desvandi/PLTSMonitoring_Firmware-Backend/releases/tags/v1.9.3
# expect 200 (was 404)
```

- Release is NOT draft/prerelease; assets include `modular-firmware.bin`,
  `.sig`, `modular-release.json`, `modular-manifest-canonical.json`,
  `hardware-acceptance.json`, `provenance-binding.json`.
- `releases/latest` now points to v1.9.3 (mismatch warning in the PWA disappears).

---

## 6. P0-02b — OTA Phase B + rollback, AFTER publishing (1–2 h)

Follow `docs/ota-physical-test/v1.9.3.md` (16 criteria) end-to-end on the
device that is still running pre-1.9.3 firmware:

```text
PWA fetch → authorized v1.9.3 resolves → SHA client-verify → Ed25519 headers
→ ESP32 upload → device SHA verify → signature verify → flash → reboot
→ boot health → ACTIVATED → sensors/alarms normal → PWA/backend agree
```

Then the rollback criterion (C16): upload a deliberately corrupted/truncated
image (or wrong-SHA artifact) and prove the device rejects it / rolls back
and reports the failure.

**Note on the new hardening:** the PWA now re-authorizes the release FRESH
(cache bypassed) at the moment you press Push, and aborts if the identity
changed since you fetched it. If you see “Authorized release identity
changed since it was fetched” — re-fetch and review; this is the intended
guard, not a bug.

Fill `docs/ota-physical-test/v1.9.3.json` (all 16 checks + `observed` +
signoffs + `hardwareIdentity` = the same bench-prototype device), then:

```bash
python3 scripts/verify_ota_evidence.py \
  --version 1.9.3 \
  --canonical-release-json <downloaded release asset>/modular-release.json \
  --ota-dir docs/ota-physical-test
# exit 0 = PASS → fleet OTA authorized

git add docs/ota-physical-test/v1.9.3.json
git commit -m "docs(v1.9.3): OTA physical test evidence — PASS (Phase A fail-closed + Phase B activation + rollback)"
git push origin release/v1.9.3
git checkout main && git merge --no-ff release/v1.9.3 -m "merge: v1.9.3 evidence complete" && git push origin main
```

---

## 7. P0-05 — Final provenance verification (30 min)

Download `provenance-binding.json` from the release assets and verify the
chain end-to-end:

```text
signed tag v1.9.3 → tag object SHA → release commit (evidence commit)
→ source commit $SRC → reproducible binary (shaMatches = true)
→ hardwareIdentity (bench-prototype + deviceSerial) → hardware acceptance
→ OTA evidence → GitHub Release → PWA authorized identity (release-policy.json = v1.9.3)
```

Concretely:

```bash
# 1. tag exists, signed, expected signer
git tag -v v1.9.3

# 2. binding file checks
python3 - <<'PY'
import json
b = json.load(open("provenance-binding.json"))
assert b["tag"] == "v1.9.3"
assert b["tagSignature"]["verified"] is True
assert b["tagSignature"]["signerEmail"] == "desvandi101@gmail.com"
assert b["binaryEquivalence"]["shaMatches"] is True
assert b["releaseCommit"]["sourceOnlyChanges"] is True
hi = b["hardwareTestedBinary"]["hardwareIdentity"]
assert hi["boardRevision"] == "bench-prototype" and hi["deviceSerial"]
print("PROVENANCE BINDING = PASS")
PY

# 3. PWA identity in sync (cross-repo)
#    PWA release-policy.json authorizedProductionTag == v1.9.3
#    firmware manifest version == 1.9.3  (CI cross-repo-contract already enforces)

# 4. evidence files exist and verify (steps 3.5 / 6 re-run)
```

Then perform the **one final delta audit** against the exact signed v1.9.3
release (not moving main) per the re-audit's closing instruction. When all
five P0s are genuinely closed, the target final state is:

> **PRODUCTION GRADE — RELEASE APPROVED**

---

## Appendix A — what the two prepared commits contain (already verified)

| Repo | Commit | Content | Local verification |
|------|--------|---------|--------------------|
| PWA | `46b9d37` | P1-5 policy invariant (release-policy.json + build-time throw), P1-7 legacy manifest de-ambiguation, P1-8 production-config gate (+ CI gate-the-gate), P1-9 cross-repo contract extension, P2 fresh-authorization before OTA upload | lint 0 errors · typecheck · 179/179 tests · build PASS · wrong-tag build FAILS |
| Firmware | `3a661bb` | §17/§20 hardware revision registry, hardwareIdentity in v1.9.3 templates/protocols, verifier hardening (dev-only revisions BLOCK), provenance binding carries hardwareIdentity, 28-check self-test | all `test_*.py` PASS (incl. new self-test) |

## Appendix B — known-good values quick reference

```text
INA219 ±80mV  = 0x0FFF      (boot default, PGA bits = 1)
INA219 ±160mV = 0x17FF      (transition at 100A up / 90A down)
GPG key        = 5E0BB8EF44199645 (PLTS Release Manager <desvandi101@gmail.com>)
Release line   = 96cb34b (pre-PCB)   PCB S12/S10 = development-only
CI toolchain   = PlatformIO 6.1.18, pip 24.2, cryptography 43.0.1
OTA headers    = X-Expected-SHA256 / X-Signature / X-Firmware-Version
```
