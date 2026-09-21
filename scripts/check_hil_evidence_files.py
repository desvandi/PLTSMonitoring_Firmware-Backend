#!/usr/bin/env python3
"""
check_hil_evidence_files.py — [GATE-9 evidence-hygiene 2026-09]
CI/lokus-repo validator untuk SEMUA file bukti hardware-acceptance gate
yang ter-commit (dipakai job hil-evidence-gate; verifikasi rilis tetap
di verify_gate_qualification.py pada build tag).

Yang diperiksa untuk tiap file v*-gate*.json + gate-qualification.template.json:
  1. JSON parseable, schemaVersion == 1
  2. keys "checks" == PERSIS 22 check wajib (LEGACY + GATE dari
     verify_gate_qualification.REQUIRED_CHECKS — single source of truth)
  3. File *.DRAFT.json / template: verdict WAJIB "PENDING" dan SEMUA
     check WAJIB "PENDING". DRAFT yang sudah berisi PASS = bukti rekaan
     tanpa pengukuran → gagal pipeline (anti-false-closure).
  4. File bukti final (vX-gate.json tanpa DRAFT): verdict harus "PASS"
     atau "PENDING" (FAIL tidak bolej di-commit sbg bukti rilis; PENDING
     non-DRAFT diizinkan saat prosedur berjalan — gerbang rilis tetap
     mem-BLOK PENDING).

Exit:  0 = semua file jujur
       1 = ada pelanggaran (daftar dicetak)
       2 = precondition (direktori/file tidak ada)
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_gate_qualification import REQUIRED_CHECKS  # noqa: E402


def main() -> int:
    hw = Path(__file__).resolve().parent.parent / "docs" / "hardware-acceptance"
    if not hw.is_dir():
        print("[PRECONDITION] docs/hardware-acceptance tidak ditemukan. Exit 2.")
        return 2

    files = sorted(hw.glob("v*-gate*.json"))
    tpl = hw / "gate-qualification.template.json"
    if tpl.is_file():
        files.append(tpl)
    if not files:
        print("[PRECONDITION] tidak ada file gate-qualification. Exit 2.")
        return 2

    req = set(REQUIRED_CHECKS)
    violations = []

    for f in files:
        is_template = f.name == "gate-qualification.template.json"
        is_draft = f.name.endswith(".DRAFT.json")
        try:
            d = json.loads(f.read_text(encoding="utf-8"))
        except Exception as e:
            violations.append(f"{f.name}: JSON parse error: {e}")
            continue

        if d.get("schemaVersion") != 1:
            violations.append(f"{f.name}: schemaVersion != 1 ({d.get('schemaVersion')})")

        checks = d.get("checks") or {}
        keys = set(checks.keys())
        if keys != req:
            missing = sorted(req - keys)
            extra = sorted(keys - req)
            violations.append(
                f"{f.name}: checks != 22 wajib (missing={missing}, extra={extra})")

        verdict = d.get("verdict", "")
        if is_template or is_draft:
            if verdict != "PENDING":
                violations.append(
                    f"{f.name}: DRAFT/template verdict harus PENDING, dapat '{verdict}'")
            filled = sorted(k for k, v in checks.items() if v != "PENDING")
            if filled:
                violations.append(
                    f"{f.name}: DRAFT/template berisi nilai terisi {filled} — "
                    f"finalisasi hanya lewat prosedur manual (HIL-PROCEDURE.md)")
        else:
            if verdict not in ("PASS", "PENDING"):
                violations.append(
                    f"{f.name}: verdict bukti final '{verdict}' tidak sah "
                    f"(hanya PASS/PENDING; FAIL tidak di-commit sbg bukti)")
            filled = {k: v for k, v in checks.items() if v not in ("PASS", "FAIL", "PENDING")}
            if filled:
                violations.append(f"{f.name}: nilai check tidak sah: {filled}")

        print(f"[{'OK' if not any(v.startswith(f.name) for v in violations) else 'VIOLATION'}]"
              f" {f.name}: {len(checks)} check, verdict={verdict!r}"
              f"{' (DRAFT)' if is_draft else ''}{' (template)' if is_template else ''}")

    print()
    print("=" * 72)
    if violations:
        print("HIL EVIDENCE HYGIENE = VIOLATION")
        for v in violations:
            print(f"  - {v}")
        print("=" * 72)
        return 1
    print(f"HIL EVIDENCE HYGIENE = OK ({len(files)} file jujur)")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
