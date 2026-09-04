# Audit 2026-09 — Remediation Summary

> This document tracks the implementation status of every P0 / P1 / P2 item
> from the September 2026 production-readiness audit. It is the authoritative
> reference for the "production-hardening in progress" status banner in the
> README.

## TL;DR

**Audit iterasi 1 (P0/P1/P2 asli):**
- **6 P0 items**: all implemented. Critical release-engineering gaps closed.
- **10 P1 items**: all implemented (P1-6 iterasi 1 hanya REST path; iterasi 2 menutup MQTT path).
- **3 P2 items**: documented; branch protection / GPG signing require GitHub
  repository settings (operator action).

**Audit iterasi 2 (self-review + area lain):**
- **3 blocker bugs** ditemukan di iterasi 1: getDeviceKeyForMqtt undefined
  (linker error), unused type imports di api.ts (lint error), build-production
  job tanpa `if:` guard (CI failure setelah branch protection). **FIXED.**
- **P1-6 tuntas**: lifecycle events untuk MQTT path + jobId management +
  bootloader-revert path. **FIXED.**
- **7 KRITIS baru** ditemukan di area lain: OOB read diagnostik, NVS journal
  size mismatch, AuthManager refresh token bugs, GAS HMAC timing attack,
  push-alarm subscribe DoS, PWA XSS PDF, PWA JWT alg confusion. **ALL FIXED.**
- **17 SEDANG** ditemukan: factory reset namespace incomplete, CSRF
  double-submit weak, cookie Secure+SameSite missing, handleRefresh no CSRF,
  config range validation silent-drop, doPost error leak, push-alarm batch
  loop, prune stale subscriptions, PWA HTTPS-only validation, MQTT payload
  validation, URL encode alarmId, login progressive backoff, factory reset
  token cleanup, MQTT reconnect backoff, deprecated escape() functions.
  **ALL FIXED.**
- **14 RINGAN + missed items**: dead code cleanup, doc drift, naming — **DONE.**

**Remaining work**: Hardware acceptance on real hardware (P1-10 checklist)
+ Master Release Gate running green in CI for one full release cycle.

**Audit iterasi 3 (Auditor Audit 8 — release gate enforcement):**
- **4 P0 blocker** ditemukan oleh Auditor terkait release gate enforcement:
  - **P0-A**: production signature masih optional di release_gate.py (unsigned
    production artifact bisa PASS). **FIXED** — gate sekarang production-aware,
    signature REQUIRED untuk production target, FAIL jika unsigned.
  - **P0-B**: cross-repo verification masih optional (skip→PASS). **FIXED** —
    mandatory untuk production, FAIL jika --pwa-path/--firmware-repo-path
    tidak diberikan.
  - **P0-C**: Master Release Gate belum ada release-publishing enforcement.
    **FIXED** — new release-publish job creates immutable GitHub Release
    ONLY on tag push AND only if release-gate PASSED.
  - **P0-D**: GitHub Release belum ada. **FIXED** — release-publish job
    creates the release with all artifacts attached.
- **9 P1 open items** dari Auditor:
  - **P1-1**: embedded firmware identity test. **FIXED** — G5b scans binary
    for X.Y.Z semver, verifies manifest version is embedded.
  - **P1-2**: SHA-256 + signature in canonical manifest. **FIXED** — new
    generate_canonical_manifest.py produces schema 2.0 manifest-canonical.json.
  - **P1-3+4**: full artifact inventory + exact artifact selection. **FIXED**
    — CANONICAL_ARTIFACTS set, firmware identified by release.json SHA, not glob.
  - **P1-5**: provenance commit binding. **FIXED** — --ci-sha binds
    provenance.gitCommit == GITHUB_SHA, FAIL on mismatch.
  - **P1-7**: pin PlatformIO in ALL CI jobs. **FIXED** — staging + generic
    now use platformio==6.1.18.
  - **P1-8**: CHANGE_ME in test fixtures. **FIXED** — replaced with
    TEST_ONLY_AUTH_TOKEN_32_BYTES_FIXTURE.
  - **P1-9**: hardware acceptance. **PARTIALLY FIXED** — template v1.7.1.md
    created, CI gating implemented (pre-release if absent). Physical test
    still pending operator action.
  - **P1-6 (residual)**: OTA lifecycle physical verification. **OPEN** —
    source-level implementation complete (Audit 2), physical validation
    pending hardware acceptance test.

**Remaining work (operator action required)**:
1. Set GitHub Environment `production` secrets (PIO_MQTT_*, PIO_OTA_*,
   PIO_FIRMWARE_SIGNING_PRIVATE_KEY).
2. Apply branch protection (docs/P2_BRANCH_PROTECTION_AND_GPG_SIGNING.md).
3. Run hardware acceptance on real ESP32 (docs/hardware-acceptance/v1.7.1.md).
4. Tag v1.7.1 (after hardware acceptance PASS) to trigger immutable release.
5. Revoke + rotate the exposed GitHub PAT.

## P0 — Critical (Release Integrity)

| ID | Title | Implementation | Verification |
|----|-------|----------------|--------------|
| P0-1 | Ed25519 signer/verifier interoperability | `scripts/sign_firmware.py` now signs the raw 32-byte SHA-256 digest (was: 64-byte hex string). Firmware `Crypto.cpp::ed25519VerifyHash` already verified the raw digest — both sides now match. | `scripts/test_ed25519_interop.py` (8 test cases: positive + 4 negative + script round-trip) |
| P0-2 | Production build in CI | `.github/workflows/build-firmware.yml` `build-production` job runs `pio run -e production` against the `production` GitHub Environment. `assert_production_secrets.py` enforces the fail-closed contract. | Job is `needs: test` and gates `release-gate`. |
| P0-3 | Disable `--skip-build` for production | `scripts/release_firmware_generic.py` rejects `--skip-build` in `--production` mode and requires `--allow-skip-build` for dev-only use. | Manual test: `--production --skip-build` exits 1. |
| P0-4 | Stop binary auto-commit to main | `.gitignore` removed the `!firmware-generic/bin/*.bin` exception. CI workflow removed the auto-commit step. Binaries are now Actions artifacts (immutable). | `git status` shows binaries as deleted from tracking. |
| P0-5 | release.json + provenance.json + SHA256SUMS | `scripts/generate_provenance.py` produces all three files for every CI build. Includes gitCommit, buildTimestamp, SHA-256 per artifact, toolchain version. | `release_gate.py` verifies hashes match. |
| P0-6 | Cross-repo binary identity | `release_firmware_generic.py` performs SHA-256 cross-repo verification (firmware repo ↔ PWA repo). PWA `.gitignore` ignores binaries — single source of truth is the firmware repo's GitHub Release. | `release_gate.py --pwa-path --firmware-repo-path` enforces. |

## P1 — Important (Hardening)

| ID | Title | Implementation | Verification |
|----|-------|----------------|--------------|
| P1-1 | PWA CI workflow | `.github/workflows/ci.yml` runs lint + typecheck + test + build on every push/PR. Cross-repo manifest sync check included. | Workflow file present. |
| P1-2 | DeviceApiClient vs BackendApiClient split | `src/lib/deviceApi.ts` + `src/lib/backendApi.ts` + `src/lib/apiShared.ts`. Legacy `api.ts` is a façade that delegates. | Type-only split; backward-compatible. |
| P1-3 | Canonical Alarm ACK contract | Firmware: `AlarmHandlers.cpp` now registers `/api/alarms/` POST catch-all → `handleAcknowledge` (parses `/api/alarms/{code}/acknowledge`); 404 if code not found. Non-canonical `/api/alarms/acknowledge` removed. | `scripts/test_alarm_ack_contract.py` verifies both sides. |
| P1-4 | Reports ownership = Backend | Firmware already returns honest 501. PWA `backendApi.reports()` routes to backend (separate `NEXT_PUBLIC_BACKEND_API_BASE_URL`). Legacy `api.reports()` delegates. | Source inspection. |
| P1-5 | Demo OTA endpoint separation | `src/app/api/demo/ota/route.ts` (new) — explicitly `simulated: true, flashed: false`. `/api/ota/route.ts` rewritten as production proxy that 503s when `API_BASE_URL` not configured. | Production guard: returns 404 in `NODE_ENV=production` unless `NEXT_PUBLIC_DEMO_MODE=true`. |
| P1-6 | OTA modular observability → GAS | `MqttTelemetryPublisher::publishOtaLifecycle()` emits ACCEPTED/DOWNLOADING/VERIFIED/FLASHED/ACTIVATED/ROLLBACK/FAILED on `plts/<deviceId>/ota/event` at QoS 1. `OtaManager` calls it at every transition. | Code review. GAS dedupes on (deviceId, jobId, state). |
| P1-7 | Pin dependency & toolchain | `firmware-generic/platformio.ini` and `firmware/platformio.ini` pinned to exact versions (`espressif32@6.7.0`, `ArduinoJson@7.1.0`, `PubSubClient@2.8`). `firmware/toolchain.json` records the full toolchain. CI uses `pip install platformio==6.1.18`. | `release_gate.py` verifies `provenance.toolchain.platformio != unknown`. |
| P1-8 | Remove CHANGE_ME, fail-closed GAS | `code.gs/Code.gs`: `AUTH_TOKEN` default is now `''` (was `plts_sec_CHANGE_ME`). `setupMasterTemplate()` refuses to deploy if AUTH_TOKEN is empty / contains CHANGE_ME / shorter than 16 chars. `doPost()` runtime guard returns 503 if AUTH_TOKEN is unconfigured. | Manual inspection. |
| P1-9 | Master release gate script | `scripts/release_gate.py` runs 10 invariant checks across both targets (generic + modular) and emits `RELEASE = PASS` or `RELEASE = BLOCKED` with reasons. Wired into CI as the final `release-gate` job. | Tested locally with sample artifacts. |
| P1-10 | Hardware acceptance gate | `docs/hardware-acceptance/README.md` — 9-section checklist (boot, sensors, alarms, OTA, rollback, emergency, persistence, security, 24h soak) + sign-off table + release decision template. | Documentation complete; physical test pending. |

## P2 — Governance & Optimization

| ID | Title | Implementation | Verification |
|----|-------|----------------|--------------|
| P2-1 | GPG commit + tag signing | `docs/P2_BRANCH_PROTECTION_AND_GPG_SIGNING.md` documents branch protection rules, tag protection rules, per-engineer GPG setup, release tag signing procedure, CI verification step, and CODEOWNERS template. | Documentation; requires GitHub Settings application. |
| P2-2 | Documentation status fix | `README.md` §0 banner now says "PRODUCTION-HARDENING IN PROGRESS" with reference to this audit doc. | Visual inspection. |
| P2-3 | CI cache determinism | `.github/workflows/build-firmware.yml` cache keys are exact (`hashFiles('firmware/platformio.ini')-v1`) — no broad `restore-keys:` that could pull stale packages. Pinned PlatformIO version. | YAML inspection. |

## Files Added / Modified

### Firmware-Backend repo
- `scripts/sign_firmware.py` (modified) — P0-1
- `scripts/test_ed25519_interop.py` (new) — P0-1 verification
- `scripts/generate_provenance.py` (new) — P0-5
- `scripts/release_firmware_generic.py` (modified) — P0-3, P0-5, P0-6
- `scripts/release_gate.py` (new) — P1-9
- `scripts/test_alarm_ack_contract.py` (new) — P1-3 verification
- `.github/workflows/build-firmware.yml` (rewritten) — P0-2, P0-4, P0-5, P1-7, P1-9
- `.gitignore` (modified) — P0-4
- `firmware-generic/bin/README.md` (new) — P0-4
- `firmware-generic/platformio.ini` (modified) — P1-7
- `firmware/platformio.ini` (modified) — P1-7
- `firmware/toolchain.json` (new) — P1-7
- `firmware/Web/AlarmHandlers.cpp` (modified) — P1-3
- `firmware/Web/ExtraHandlers.cpp` (modified) — P1-3 (removed non-canonical route)
- `firmware/Network/MqttTelemetryPublisher.{h,cpp}` (modified) — P1-6
- `firmware/Services/OtaManager.{h,cpp}` (modified) — P1-6
- `code.gs/Code.gs` (modified) — P1-8
- `docs/hardware-acceptance/README.md` (new) — P1-10
- `docs/P2_BRANCH_PROTECTION_AND_GPG_SIGNING.md` (new) — P2-1
- `docs/AUDIT_2026_09_REMEDIATION.md` (new — this file)
- `README.md` (modified) — P2-2

### PWA repo
- `.github/workflows/ci.yml` (new) — P1-1
- `.gitignore` (modified) — P0-6
- `src/lib/apiShared.ts` (new) — P1-2
- `src/lib/deviceApi.ts` (new) — P1-2
- `src/lib/backendApi.ts` (new) — P1-2, P1-4
- `src/lib/api.ts` (rewritten as façade) — P1-2
- `src/app/api/ota/route.ts` (rewritten as production proxy) — P1-5
- `src/app/api/demo/ota/route.ts` (new) — P1-5

## Remaining Work (out of scope for code)

These items require operator action and cannot be completed in code:

1. **GitHub Environment `production`** — create in repo Settings → Environments
   with the required secrets (`PIO_MQTT_BROKER_HOST`, `PIO_MQTT_BROKER_PORT`,
   `PIO_MQTT_USERNAME`, `PIO_MQTT_PASSWORD`, `PIO_MQTT_ROOT_CA`,
   `PIO_OTA_ED25519_PUBLIC_KEY_HEX`, `PIO_OTA_HTTPS_ROOT_CA`). Add required
   reviewers + branch restriction to `main`.

2. **Branch protection** — apply the rules in `docs/P2_BRANCH_PROTECTION_AND_GPG_SIGNING.md`.

3. **GPG signing** — each release manager sets up GPG per the doc; upload
   public key to GitHub.

4. **Hardware acceptance** — run the checklist in
   `docs/hardware-acceptance/README.md` on real ESP32 hardware with sensors,
   battery, and relay attached. Sign off and attach to the release.

5. **Token rotation** — the GitHub PAT used to clone/push during this
   remediation MUST be revoked and rotated. It was exposed in plaintext
   during the agent run.
