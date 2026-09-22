#!/usr/bin/env python3
"""
check_surface_policy.py — [F18/F19 surface & governance gate 2026-09]
Penegak tata kelola permukaan (temuan #18 audit baseline 2026-09-18)
dan anti-fabrikasi inventaris kredensial (temuan #19).

Apa yang diperiksa (semua fail-closed):
  1. Registry docs/surface-governance/SURFACE-STATUS.json valid schema.
  2. Permukaan berstatus frozen/deprecated:
     - himpunan file di bawah path HARUS persis = kunci pins
       (file hilang ATAU baru = FAIL),
     - SHA-256 setiap file HARUS cocok dengan pin.
  3. Permukaan berstatus active: setiap marker hardening
     (file + pattern regex) HARUS ditemukan. Refactor yang
     menghilangkan marker = FAIL (registrasi ulang harus disengaja).
  4. Jika governanceFiles.credentialInventory dideklarasikan:
     inventaris kredensial WAJIB lolos anti-fabrikasi —
     lastRotated non-null HANYA boleh bila evidenceRef menunjuk
     file bukti yang benar-benar ada di repo.

Apa yang TIDAK dilakukan (batas scope, jujur):
  - Tidak membandingkan semantik/baris-per-baris antar permukaan.
  - Tidak mengeksekusi rotasi kredensial (runbook = dokumen).
  - Tidak memeriksa repo lain (external-ref hanya dicatat).

Exit:  0 = PASS semua kebijakan
       1 = ada PELANGGARAN kebijakan (pin/mark/inventaris)
       2 = precondition error (registry/inventaris rusak/hilang —
           checker tidak dapat mengevaluasi secara berarti)

READ-ONLY: tidak menulis apa pun ke repo. Output JSON ke stdout.
"""
import argparse
import hashlib
import json
import os
import re
import sys

ALLOWED_STATUSES = {"active", "frozen", "deprecated", "external-ref"}
PINNED_STATUSES = {"frozen", "deprecated"}
REQUIRED_SURFACE_FIELDS = ("id", "path", "status")
REQUIRED_INVENTORY_FIELDS = (
    "id", "kind", "storage", "consumers",
    "rotationProcedure", "verification", "blastRadius", "cadenceDays",
)

REGISTRY_DEFAULT = "docs/surface-governance/SURFACE-STATUS.json"


class PreconditionError(Exception):
    pass


def norm(*parts):
    """Gabung path registry (slash) ke path OS lokal, relatif repo root.

    Bagian PERTAMA dipertahankan apa adanya (root boleh absolut dan
    harus tetap absolut — jangan strip('/') bagian pertama, itu pernah
    mengubah '/home/…' menjadi relatif 'home/…'). Bagian berikutnya
    dibersihkan slash pengantar/penutupnya.
    """
    parts = [p for p in parts if p]
    if not parts:
        return ""
    rest = [p.strip("/") for p in parts[1:] if p.strip("/")]
    return os.path.join(parts[0], *rest)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def list_files_under(root, surf_path):
    """Semua file (relatif root) di bawah surf_path — untuk pin set."""
    base = norm(root, surf_path)
    if not os.path.isdir(base):
        raise PreconditionError(f"path surface bukan direktori: {surf_path}")
    found = set()
    for dirpath, _dirnames, filenames in os.walk(base):
        for fn in filenames:
            rel = os.path.relpath(os.path.join(dirpath, fn), root)
            found.add(rel.replace(os.sep, "/"))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", default=".",
                    help="root repo (default: direktori kerja)")
    ap.add_argument("--registry", default=REGISTRY_DEFAULT,
                    help="path registry relatif repo root")
    args = ap.parse_args()

    root = os.path.abspath(args.repo_root)
    checks = []

    def record(cid, status, detail, target=""):
        checks.append({"id": cid, "target": target,
                       "status": status, "detail": detail})

    def fail(cid, detail, target=""):
        record(cid, "FAIL", detail, target)

    def ok(cid, detail, target=""):
        record(cid, "PASS", detail, target)

    # ------------------------------------------------------------------
    # 0. Precondition: registry ada + parse + schema dasar
    # ------------------------------------------------------------------
    reg_path = norm(root, args.registry)
    try:
        with open(reg_path, "r", encoding="utf-8") as f:
            registry = json.load(f)
    except FileNotFoundError:
        print(json.dumps({
            "tool": "check_surface_policy.py", "verdict": "ERROR",
            "exit": 2, "checks": [], "summary": {},
            "preconditionError": f"registry tidak ditemukan: {args.registry}",
        }, indent=2))
        return 2
    except json.JSONDecodeError as e:
        print(json.dumps({
            "tool": "check_surface_policy.py", "verdict": "ERROR",
            "exit": 2, "checks": [], "summary": {},
            "preconditionError": f"registry bukan JSON valid: {e}",
        }, indent=2))
        return 2

    try:
        surfaces = registry["surfaces"]
        assert isinstance(surfaces, list) and surfaces, "surfaces kosong"
        assert registry.get("schemaVersion") == 1, "schemaVersion != 1"
        for s in surfaces:
            for fld in REQUIRED_SURFACE_FIELDS:
                assert s.get(fld), f"surface tanpa {fld}"
            if s["status"] not in ALLOWED_STATUSES:
                raise PreconditionError(
                    f"status tak dikenal '{s['status']}' (surface {s['id']})")
            if s["status"] in PINNED_STATUSES:
                pins = s.get("pins")
                assert isinstance(pins, dict) and pins, \
                    f"surface {s['id']} ({s['status']}) tanpa pins"
                for relp in pins:
                    assert relp.startswith(s["path"].rstrip("/") + "/"), \
                        f"pin di luar path surface {s['id']}: {relp}"
            if s["status"] == "active":
                markers = s.get("markers")
                assert isinstance(markers, list) and markers, \
                    f"surface {s['id']} (active) tanpa markers"
                for m in markers:
                    for fld in ("file", "pattern", "desc"):
                        assert m.get(fld), f"marker tanpa {fld} ({s['id']})"
                    assert m["file"].startswith(
                        s["path"].rstrip("/") + "/"), \
                        f"marker di luar path surface {s['id']}: {m['file']}"
    except (KeyError, AssertionError, PreconditionError, TypeError) as e:
        print(json.dumps({
            "tool": "check_surface_policy.py", "verdict": "ERROR",
            "exit": 2, "checks": [], "summary": {},
            "preconditionError": f"schema registry rusak: {e}",
        }, indent=2))
        return 2

    ok("schema", f"registry valid: {len(surfaces)} surface, "
       f"{sum(1 for s in surfaces if s['status'] in PINNED_STATUSES)} "
       f"ter-pin, {sum(1 for s in surfaces if s['status'] == 'active')} "
       f"aktif", target=args.registry)

    # ------------------------------------------------------------------
    # 1. Permukaan frozen/deprecated: pin set + SHA-256
    # ------------------------------------------------------------------
    for s in surfaces:
        if s["status"] not in PINNED_STATUSES:
            continue
        sid, surf_path, pins = s["id"], s["path"], s["pins"]
        try:
            present = list_files_under(root, surf_path)
        except PreconditionError as e:
            fail(f"surface:{sid}:pinset", f"precondition: {e}",
                 target=surf_path)
            continue
        expected = set(pins.keys())
        extra = present - expected
        missing = expected - present
        if extra or missing:
            parts = []
            if extra:
                parts.append("file TIDAK ter-pin muncul: " +
                             ", ".join(sorted(extra)))
            if missing:
                parts.append("file ter-pin HILANG: " +
                             ", ".join(sorted(missing)))
            fail(f"surface:{sid}:pinset",
                 "; ".join(parts) +
                 " — perubahan permukaan beku butuh pembaruan pin "
                 "eksplisit di SURFACE-STATUS.json (PR + review)",
                 target=surf_path)
        else:
            ok(f"surface:{sid}:pinset",
               f"{len(expected)} file persis seperti registry",
               target=surf_path)
        for relp, want in sorted(pins.items()):
            fpath = norm(root, relp)
            if not os.path.isfile(fpath):
                fail(f"surface:{sid}:pin:{relp}", "file tidak ada",
                     target=relp)
                continue
            got = sha256_file(fpath)
            if got != want:
                fail(f"surface:{sid}:pin:{relp}",
                     f"drift konten: pin={want[:16]}… aktual={got[:16]}… "
                     "(modifikasi senyap ditolak — perbarui pin via PR)",
                     target=relp)
            else:
                ok(f"surface:{sid}:pin:{relp}", "SHA-256 cocok",
                   target=relp)

    # ------------------------------------------------------------------
    # 2. Permukaan active: marker hardening
    # ------------------------------------------------------------------
    for s in surfaces:
        if s["status"] != "active":
            continue
        sid = s["id"]
        for m in s["markers"]:
            fpath = norm(root, m["file"])
            cid = f"surface:{sid}:marker:{m['file']}:{m['pattern']}"
            if not os.path.isfile(fpath):
                fail(cid, f"file marker tidak ada: {m['file']}",
                     target=m["file"])
                continue
            try:
                with open(fpath, "r", encoding="utf-8",
                          errors="replace") as f:
                    content = f.read()
            except OSError as e:
                fail(cid, f"gagal membaca: {e}", target=m["file"])
                continue
            if re.search(m["pattern"], content):
                ok(cid, f"marker ada ({m['desc']})", target=m["file"])
            else:
                fail(cid,
                     f"marker hardening HILANG: pattern '{m['pattern']}' "
                     f"tidak ditemukan di {m['file']} ({m['desc']}). "
                     "Refactor boleh — tapi registrasi marker harus "
                     "diperbarui secara sengaja di registry.",
                     target=m["file"])

    # ------------------------------------------------------------------
    # 3. Inventaris kredensial (anti-fabrikasi) — bila dideklarasikan
    # ------------------------------------------------------------------
    inv_rel = (registry.get("governanceFiles") or {}).get(
        "credentialInventory")
    if inv_rel:
        inv_path = norm(root, inv_rel)
        try:
            with open(inv_path, "r", encoding="utf-8") as f:
                inventory = json.load(f)
            entries = inventory["credentials"]
            assert isinstance(entries, list) and entries, \
                "credentials kosong"
        except FileNotFoundError:
            fail("inventory:file",
                 f"inventaris dideklarasikan tapi tidak ada: {inv_rel}",
                 target=inv_rel)
            entries = None
        except (json.JSONDecodeError, KeyError, AssertionError,
                TypeError) as e:
            fail("inventory:schema", f"inventaris rusak: {e}",
                 target=inv_rel)
            entries = None
        if entries is not None:
            ok("inventory:schema",
               f"{len(entries)} entri kredensial ter-parse", target=inv_rel)
            for c in entries:
                cid = f"inventory:{c.get('id', '?')}"
                missing_fields = [f for f in REQUIRED_INVENTORY_FIELDS
                                  if f not in c or c[f] in (None, "", [])]
                if missing_fields:
                    fail(f"{cid}:fields",
                         "field wajib kosong/hilang: " +
                         ", ".join(missing_fields), target=inv_rel)
                    continue
                last_rot = c.get("lastRotated")
                ev_ref = c.get("evidenceRef")
                if last_rot is None:
                    ok(f"{cid}:evidence",
                       "lastRotated=null (jujur: belum ada bukti rotasi)",
                       target=inv_rel)
                else:
                    if not ev_ref:
                        fail(f"{cid}:evidence",
                             f"lastRotated='{last_rot}' TANPA evidenceRef "
                             "— riwayat rotasi tidak boleh diklaim tanpa "
                             "bukti (anti-fabrikasi)", target=inv_rel)
                    elif not os.path.isfile(norm(root, ev_ref)):
                        fail(f"{cid}:evidence",
                             f"evidenceRef '{ev_ref}' tidak menunjuk file "
                             "yang ada — bukti rotasi fabrikasi/hilang",
                             target=inv_rel)
                    else:
                        ok(f"{cid}:evidence",
                           f"rotasi {last_rot} + bukti {ev_ref} ada",
                           target=inv_rel)

    # ------------------------------------------------------------------
    # 4. Verdict
    # ------------------------------------------------------------------
    n_fail = sum(1 for c in checks if c["status"] == "FAIL")
    n_pass = sum(1 for c in checks if c["status"] == "PASS")
    verdict = "FAIL" if n_fail else "PASS"
    report = {
        "tool": "check_surface_policy.py",
        "repoRoot": root,
        "registry": args.registry,
        "verdict": verdict,
        "exit": 1 if n_fail else 0,
        "summary": {
            "surfaces": len(surfaces),
            "checksPass": n_pass,
            "checksFail": n_fail,
            "inventoryDeclared": bool(inv_rel),
        },
        "checks": checks,
        "preconditionError": None,
    }
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
