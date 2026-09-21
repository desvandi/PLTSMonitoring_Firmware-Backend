#!/usr/bin/env python3
"""
test_governance_2026_09.py — [F18/F19 regresi tata kelola permukaan 2026-09]
Suite regresi untuk scripts/check_surface_policy.py.

Disiplin: setiap asersi negatif HARUS terbukti menolak (fail-closed),
bukan sekadar diasumsikan. Semua skenario berjalan pada SALINAN temp —
repo asli tidak pernah dimodifikasi oleh suite ini.

Skenario:
  T1  repo apa adanya ......................... exit 0 (positive)
  T2  file ter-pin dimodifikasi ............... exit 1 (drift konten)
  T3  file BARU masuk direktori beku .......... exit 1 (pinset dilanggar)
  T4  marker hardening dihapus dari aktif .... exit 1 (drift hardening)
  T5  lastRotated fabrikasi tanpa bukti ....... exit 1 (anti-fabrikasi)
  T6  lastRotated + bukti file NYATA ......... exit 0 (jalur jujur diterima)
  T7  registry JSON rusak ..................... exit 2 (precondition)
  T8  status surface tak dikenal .............. exit 2 (precondition)
  T9  registry hilang .......................... exit 2 (precondition)

Exit: 0 = semua PASS; 1 = ada test GAGAL.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CHECKER = os.path.join(HERE, "check_surface_policy.py")
REGISTRY_REL = "docs/surface-governance/SURFACE-STATUS.json"

results = []


def run_checker(root, registry_rel=REGISTRY_REL):
    p = subprocess.run(
        [sys.executable, CHECKER, "--repo-root", root,
         "--registry", registry_rel],
        capture_output=True, text=True, timeout=120)
    out = {}
    try:
        out = json.loads(p.stdout)
    except json.JSONDecodeError:
        pass
    return p.returncode, out


def load_registry():
    with open(os.path.join(REPO, *REGISTRY_REL.split("/")),
              encoding="utf-8") as f:
        return json.load(f)


def build_tree(dst, declare_inventory=False, inventory_name=None):
    """Salinan minimal: registry + file ter-pin + file marker (+ fixture)."""
    reg = load_registry()
    os.makedirs(os.path.dirname(os.path.join(
        dst, *REGISTRY_REL.split("/"))), exist_ok=True)
    if declare_inventory:
        reg.setdefault("governanceFiles", {})[
            "credentialInventory"] = inventory_name
    with open(os.path.join(dst, *REGISTRY_REL.split("/")), "w",
              encoding="utf-8") as f:
        json.dump(reg, f, indent=2, ensure_ascii=False)
    copied = []
    for s in reg["surfaces"]:
        if s["status"] in ("frozen", "deprecated"):
            for relp in s.get("pins", {}):
                _copy_repo_file(dst, relp)
                copied.append(relp)
        elif s["status"] == "active":
            for m in s.get("markers", []):
                _copy_repo_file(dst, m["file"])
                copied.append(m["file"])
    return reg, copied


def _copy_repo_file(dst, relp):
    src = os.path.join(REPO, *relp.split("/"))
    tgt = os.path.join(dst, *relp.split("/"))
    os.makedirs(os.path.dirname(tgt), exist_ok=True)
    shutil.copy2(src, tgt)


def inventory_fixture(dst, name, last_rotated, evidence_ref):
    entries = [{
        "id": "C-TEST", "kind": "test", "storage": "NVS",
        "consumers": ["firmware"], "rotationProcedure": "§runbook",
        "verification": "healthcheck", "blastRadius": "1 device",
        "cadenceDays": 90,
        "lastRotated": last_rotated, "evidenceRef": evidence_ref,
    }]
    inv = {"schemaVersion": 1, "credentials": entries}
    path = os.path.join(dst, *name.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(inv, f, indent=2)
    return path


def record(tid, desc, want_rc, got_rc, extra=""):
    okk = want_rc == got_rc
    results.append((tid, desc, want_rc, got_rc, okk, extra))
    print(f"[{'PASS' if okk else 'FAIL'}] {tid} — {desc} "
          f"(harus exit {want_rc}, aktual {got_rc}){extra}")
    return okk


def main():
    tmp = tempfile.mkdtemp(prefix="gov-test-")
    try:
        # T1 — repo apa adanya (positif)
        t1 = os.path.join(tmp, "t1")
        os.makedirs(t1)
        build_tree(t1)
        rc, out = run_checker(t1)
        record("T1", "repo as-is lolos semua kebijakan", 0, rc)

        # T2 — file ter-pin dimodifikasi (drift konten)
        t2 = os.path.join(tmp, "t2")
        os.makedirs(t2)
        build_tree(t2)
        pino = os.path.join(t2, "push-alarm", "gas", "Code.gs")
        with open(pino, "a", encoding="utf-8") as f:
            f.write("\n// tamper-test: satu baris ilegal\n")
        rc, _ = run_checker(t2)
        record("T2", "modifikasi file beku ditolak (drift SHA-256)", 1, rc)

        # T3 — file BARU di direktori beku (pinset dilanggar)
        t3 = os.path.join(tmp, "t3")
        os.makedirs(t3)
        build_tree(t3)
        with open(os.path.join(t3, "push-alarm", "gas",
                               "EXTRA-Baru.gs"), "w") as f:
            f.write("// file tidak ter-pin\n")
        rc, _ = run_checker(t3)
        record("T3", "file baru di permukaan beku ditolak", 1, rc)

        # T4 — marker hardening dihapus dari surface aktif
        t4 = os.path.join(tmp, "t4")
        os.makedirs(t4)
        build_tree(t4)
        gen = os.path.join(t4, "firmware-generic", "src",
                           "plts_firmware_v1.ino")
        with open(gen, encoding="utf-8") as f:
            body = f.read()
        body = body.replace("getOrCreateApPassword", "ApPassword_HILANG")
        with open(gen, "w", encoding="utf-8") as f:
            f.write(body)
        rc, _ = run_checker(t4)
        record("T4", "marker [FW-A3] hilang dari firmware-generic", 1, rc)

        # T5 — lastRotated fabrikasi tanpa evidenceRef
        t5 = os.path.join(tmp, "t5")
        os.makedirs(t5)
        inv_name = "docs/security/credential-inventory.json"
        build_tree(t5, declare_inventory=True, inventory_name=inv_name)
        inventory_fixture(t5, inv_name, "2026-08-01", None)
        rc, _ = run_checker(t5)
        record("T5", "lastRotated tanpa bukti ditolak (anti-fabrikasi)",
               1, rc)

        # T6 — lastRotated DENGAN bukti file nyata (jalur jujur diterima)
        t6 = os.path.join(tmp, "t6")
        os.makedirs(t6)
        build_tree(t6, declare_inventory=True, inventory_name=inv_name)
        inventory_fixture(t6, inv_name, "2026-08-01",
                           "docs/security/evidence/C-TEST-2026-08-01.json")
        ev = os.path.join(t6, "docs", "security", "evidence",
                          "C-TEST-2026-08-01.json")
        os.makedirs(os.path.dirname(ev), exist_ok=True)
        with open(ev, "w") as f:
            f.write('{"rotatedAt": "2026-08-01", "by": "operator"}')
        rc, _ = run_checker(t6)
        record("T6", "rotasi terbukti + bukti nyata diterima", 0, rc)

        # T7 — registry JSON rusak
        t7 = os.path.join(tmp, "t7")
        os.makedirs(os.path.join(t7, "docs", "surface-governance"))
        with open(os.path.join(t7, *REGISTRY_REL.split("/")), "w") as f:
            f.write('{"surfaces": [ TIDAK VALID')
        rc, _ = run_checker(t7)
        record("T7", "registry JSON rusak = precondition", 2, rc)

        # T8 — status surface tak dikenal
        t8 = os.path.join(tmp, "t8")
        os.makedirs(os.path.join(t8, "docs", "surface-governance"))
        with open(os.path.join(t8, *REGISTRY_REL.split("/")), "w") as f:
            json.dump({"schemaVersion": 1, "surfaces": [
                {"id": "x", "path": "x/", "status": "tidak-dikenal"}
            ]}, f)
        rc, _ = run_checker(t8)
        record("T8", "status tak dikenal = precondition", 2, rc)

        # T9 — registry hilang
        t9 = os.path.join(tmp, "t9")
        os.makedirs(t9)
        rc, _ = run_checker(t9)
        record("T9", "registry hilang = precondition", 2, rc)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    n_fail = sum(1 for r in results if not r[4])
    print(f"\n== {len(results) - n_fail}/{len(results)} PASS, "
          f"{n_fail} FAIL ==")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
