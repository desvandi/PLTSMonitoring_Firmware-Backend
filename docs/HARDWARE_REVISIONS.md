# Hardware Revision Registry — Acceptance Boundary

> **Status: ACTIVE POLICY** — [Audit 2026-09-05 re-audit, sections 17 + 20]
>
> Machine-readable registry: [`docs/hardware-revisions.json`](hardware-revisions.json)
> (consumed by `scripts/verify_ina219_hardware_acceptance.py` and
> `scripts/verify_ota_evidence.py`).

---

## 1. Why this document exists

`main` has moved past the v1.9.3 software release line into PCB work
(S12 relay-expansion board routing + S10 fabrication outputs, PR #18). The
re-audit requires an **explicit boundary** between:

- the **v1.9.3 software release candidate** (release line tip `96cb34b`,
  which includes the INA219 canonical fix, reproducible build, and OTA
  physical-test protocol), and
- the **PCB revision work** (S12/S10), which is **development-only** and is
  NOT part of the v1.9.3 release.

This document declares that boundary and the rule that keeps it honest.

## 2. The three-identity rule (v1.9.3+)

Starting with v1.9.3, hardware acceptance and OTA physical-test evidence must
bind THREE identities — not just the firmware:

| Identity | Fields in evidence JSON | Proven by |
|---|---|---|
| **Firmware** | `version`, `gitCommit`, `firmwareSha256` | reproducible build + release.json |
| **Hardware** | `hardwareIdentity.boardRevision`, `hardwareIdentity.deviceSerial`, `hardwareIdentity.relayBoardRevision` | this registry + physical device serial |
| **Release** | the signed tag `v1.9.3` → release commit → this evidence file | provenance-binding.json |

Every v1.9.3+ evidence file MUST contain a `hardwareIdentity` block:

```json
"hardwareIdentity": {
  "boardRevision": "bench-prototype",
  "deviceSerial": "<actual ESP32 module serial, e.g. from the boot log / chip cap>",
  "relayBoardRevision": "none",
  "notes": ""
}
```

Verifiers BLOCK (exit 1) when, for versions >= 1.9.3:

- `hardwareIdentity` is missing;
- `boardRevision` is empty or not declared in the registry;
- `boardRevision` is declared `releaseEligible: false` (development-only);
- `deviceSerial` is empty or inconsistent with the top-level
  `hardwareSerial` field;
- `relayBoardRevision` is empty (use `"none"` when no relay expansion board
  is present).

## 3. Registry

| Revision | Class | Release eligible | Notes |
|---|---|---|---|
| `bench-prototype` | Hand-wired module carrier (DevKit + INA219 breakout + external shunt + divider) | **YES** — designated v1.9.3 release-target hardware class | Acceptance basis: v1.8.0 evidence; measurement chain unchanged in v1.9.2/v1.9.3 |
| `S10` | PCB module carrier board | NO — development-only | KiCad fab outputs (PR #18). New acceptance required before any release cites S10 |
| `S12` | PCB relay-expansion board | NO — development-only | Routed, DRC 0 errors (PR #18). Contains BSS138 emergency sense/drive circuitry; revision-specific acceptance (incl. emergency criteria) required |

## 4. Boundary statement (explicit)

1. **v1.9.3 production release hardware target = `bench-prototype` class.**
   The v1.9.3 hardware acceptance and OTA physical test must be executed on a
   device of this class, and the evidence must record
   `boardRevision: "bench-prototype"` plus the actual device serial.
2. **S10 and S12 are development-only.** Acceptance evidence citing S10 or
   S12 will BLOCK the v1.9.3 release chain. They are not release targets.
3. **Acceptance does not transfer across hardware revisions.** Any future
   release that deploys on S10/S12 (or any later PCB revision) requires a NEW
   hardware acceptance executed on that revision, with new evidence, new
   signoffs, and a new provenance binding. A firmware-version match alone is
   not sufficient.
4. **Firmware + hardware + release identity are bound together** in
   `provenance-binding.json` at release time (see
   `scripts/generate_provenance_binding.py` — it embeds the
   `hardwareIdentity` block from the acceptance evidence).
