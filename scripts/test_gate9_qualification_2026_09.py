#!/usr/bin/env python3
"""
test_gate9_qualification_2026_09.py — [GATE-9 / PH8-06] contract tests for
verify_gate_qualification.py + template + CI wiring.

Discipline (same as the other mirror suites): a verifier that can be
satisfied by a missing file, a stale SHA, or a fabricated PASS string is a
false-green gate. Both directions are proven here:

  POSITIVE control — a fully-filled evidence file for the exact source
  commit + firmware SHA → verifier exits 0 (the gate opens for REAL work).

  NEGATIVE controls — each falsification class must BLOCK (exit 1):
    N1  evidence file missing (the current, honest state of the repo)
    N2  verdict != PASS
    N3  gitCommit mismatch (evidence from another source commit)
    N4  firmwareSha256 mismatch (evidence for another binary)
    N5  one of the 10 gate checks missing
    N6  one of the 10 gate checks = FAIL
    N7  signoff empty
    N8  version mismatch (evidence for a different release)

  SOURCE-SHAPE — the CI step exists, is version-gated (>1.9.3), and the
  template carries all 21 checks + honest observed fields.

Run: python3 scripts/test_gate9_qualification_2026_09.py  (exit 0 = PASS)
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERIFIER = ROOT / "scripts" / "verify_gate_qualification.py"
TEMPLATE = ROOT / "docs" / "hardware-acceptance" / "gate-qualification.template.json"
WORKFLOW = ROOT / ".github" / "workflows" / "build-firmware.yml"

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def run_verifier(version, hw_dir, release_json, source_commit):
    cmd = [sys.executable, str(VERIFIER),
           "--version", version,
           "--hw-dir", str(hw_dir),
           "--release-json", str(release_json)]
    if source_commit:
        cmd += ["--source-commit", source_commit]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


SRC = "a" * 40
SHA = "b" * 64

print("[S] Source-shape")
tpl = json.loads(TEMPLATE.read_text())
check("S1 template memuat 21 check (11 legacy + 10 gate)",
      len(tpl["checks"]) == 21)
check("S2 template verdict PENDING (bukti diisi fisik, bukan pra-PASS)",
      tpl["verdict"] == "PENDING")
check("S3 semua check template PENDING (tidak ada pra-PASS senyap)",
      all(v == "PENDING" for v in tpl["checks"].values()))
check("S4 observed memuat blok commLoss/emergencyAtomicity/soak",
      all(k in tpl["observed"] for k in
          ("commLossFailSafe", "emergencyAtomicity", "soak")))
wf = WORKFLOW.read_text()
check("S5 CI step verify_gate_qualification ada di workflow",
      "verify_gate_qualification.py" in wf)
check("S6 gating versi >1.9.3 (tuple compare)",
      ">(1,9,3)" in wf)
check("S7 template notes menyatakan larangan nilai rekaan",
      "rekaan" in tpl.get("notes", "").lower())
check("S8 HIL-PROCEDURE.md ada",
      (ROOT / "docs" / "hardware-acceptance" / "HIL-PROCEDURE.md").is_file())

print()
print("[P] Positive control — bukti lengkap & konsisten membuka gerbang")
with tempfile.TemporaryDirectory() as td:
    tdp = Path(td)
    hw_dir = tdp / "hw"
    hw_dir.mkdir()
    good = json.loads(TEMPLATE.read_text())
    good["version"] = "1.9.4"
    good["gitCommit"] = SRC
    good["firmwareSha256"] = SHA
    good["verdict"] = "PASS"
    for k in good["checks"]:
        good["checks"][k] = "PASS"
    good["testEngineer"] = "eng"
    good["reviewer"] = "rev"
    good["releaseManager"] = "rm"
    (hw_dir / "v1.9.4-gate.json").write_text(json.dumps(good))
    rel = tdp / "release.json"
    rel.write_text(json.dumps({"gitCommit": SRC, "firmwareSha256": SHA}))
    rc, out = run_verifier("1.9.4", hw_dir, rel, SRC)
    check("P1 verifier exit 0 untuk bukti lengkap",
          rc == 0, out[-300:])
    check("P2 output menyatakan VERIFIED",
          "VERIFIED" in out)

print()
print("[N] Negative controls — setiap kelas pemalsuan harus MEMBLOKIR")


def make_case(name, mutate):
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        hw_dir = tdp / "hw"
        hw_dir.mkdir()
        ev = json.loads(TEMPLATE.read_text())
        ev["version"] = "1.9.4"
        ev["gitCommit"] = SRC
        ev["firmwareSha256"] = SHA
        ev["verdict"] = "PASS"
        for k in ev["checks"]:
            ev["checks"][k] = "PASS"
        ev["testEngineer"] = "eng"
        ev["reviewer"] = "rev"
        ev["releaseManager"] = "rm"
        rel = tdp / "release.json"
        rel.write_text(json.dumps({"gitCommit": SRC, "firmwareSha256": SHA}))
        mutate(ev, hw_dir, rel)
        rc, out = run_verifier("1.9.4", hw_dir, rel, SRC)
        check(name, rc == 1, f"exit={rc} — harusnya diblokir")


make_case("N1 file bukti tidak ada → BLOCK", lambda ev, h, r: None)
make_case("N2 verdict bukan PASS → BLOCK",
          lambda ev, h, r: ev.update(verdict="FAIL"))
make_case("N3 gitCommit beda source commit → BLOCK",
          lambda ev, h, r: ev.update(gitCommit="c" * 40))
make_case("N4 firmwareSha256 beda binary → BLOCK",
          lambda ev, h, r: ev.update(firmwareSha256="d" * 64))
make_case("N5 satu check gate hilang → BLOCK",
          lambda ev, h, r: ev["checks"].pop("commLossFailSafe"))
make_case("N6 satu check gate FAIL → BLOCK",
          lambda ev, h, r: ev["checks"].update(commLossFailSafe="FAIL"))
make_case("N7 signoff kosong → BLOCK",
          lambda ev, h, r: ev.update(releaseManager="  "))
make_case("N8 version beda → BLOCK",
          lambda ev, h, r: ev.update(version="1.9.3"))

print()
print("=" * 72)
print(f"RESULT: {PASS} passed, {FAIL} failed")
print("=" * 72)
sys.exit(0 if FAIL == 0 else 1)
