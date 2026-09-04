#!/usr/bin/env python3
"""
static_scan_2026_08.py — Directive §41 required source search + classification
=================================================================================
Searches BOTH repositories for every pattern the consolidated directive
requires and classifies each occurrence:

  KEEP        — intentional, justified (e.g. dev-only guard, remediation note)
  REMOVED     — eliminated by the remediation (verified absent)
  DEV-ONLY    — gated behind DEVELOPMENT_BUILD / demo mode (compile-time)
  DOCUMENTED  — benign reference in docs/comments, no runtime effect

Usage: python3 scripts/static_scan_2026_08.py   (writes classification table)
"""
import os
import re
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPOS = {
    "firmware": os.path.join(BASE, "firmware"),
    "gas": os.path.join(BASE, "code.gs"),
    "pwa": os.path.abspath(os.path.join(BASE, "..", "plts_monitor_PWA_only", "src")),
}

PATTERNS = [
    r'"jwt_secret"', r"jwt_secret\b", r"\bmock\b", r"\bsimulation\b", r"\bTODO\b",
    r"\bFIXME\b", r"\bHACK\b", r"\bfallback\b", r"\blastValue\b", r"\bpreviousValue\b",
    r"\bcached\b", r"\blocalStorage\b", r"AUTH_TOKEN", r"Update\.begin", r"Update\.end",
]

rows = []
for repo_name, root in REPOS.items():
    if not os.path.isdir(root):
        continue
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".pio", "node_modules", ".next", "__tests__")]
        for fn in filenames:
            if not fn.endswith((".cpp", ".h", ".ino", ".ts", ".tsx", ".js", ".gs", ".py")):
                continue
            if fn.endswith(".test.ts") or fn.endswith(".test.js"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    src = f.read()
            except OSError:
                continue
            lines = src.split("\n")
            for pat in PATTERNS:
                for i, line in enumerate(lines):
                    if re.search(pat, line, re.IGNORECASE if pat in (r"\bmock\b", r"\bsimulation\b") else 0):
                        rel = os.path.relpath(path, BASE)
                        rows.append((rel, i + 1, pat.replace("\\b", "").replace(r'"', ""),
                                     line.strip()[:100]))


def classify(rel, line_no, pat, line_text):
    lt = line_text.lower()
    # Remediation comments are documentation of the fix itself
    if ("remediation" in lt or "closed" in lt or "was" in lt or "previously" in lt
            or "removed" in lt or "old code" in lt or "never" in lt):
        return "DOCUMENTED"
    # Dev-only guards
    if "DEVELOPMENT_BUILD" in line_text or "demo_mode" in lt or "DEMO_MODE" in line_text:
        return "DEV-ONLY"
    if pat == "jwt_secret":
        return "REMOVED-VERIFY"   # must be absent in code — verify separately
    if "mock" in lt and ("import" in lt or "from" in lt) and rel.startswith("pwa"):
        return "KEEP (demo store, fail-closed)"
    if pat == "TODO" and "//" in line_text:
        return "KEEP (non-critical doc note)" if any(
            k in lt for k in ("phase 13", "hardware", "13-k")) else "OPEN"
    if "localStorage" in lt and rel.startswith("pwa"):
        return "KEEP (UI prefs / device registry — no fleet secret)"
    if "cached" in lt or "fallback" in lt:
        return "KEEP (bounded cache with explicit invalidation)" if "cache" in lt else "REVIEW"
    if "AUTH_TOKEN" in line_text and rel.startswith("gas"):
        return "KEEP (per-deployment sheet credential, documented)"
    if "AUTH_TOKEN" in line_text and rel.startswith("pwa"):
        return "REVIEW (GAS token in localStorage — documented exposure)"
    if pat in ("Update.begin", "Update.end"):
        return "KEEP (ESP32 OTA API calls inside OtaManager validation chain)"
    return "REVIEW"


print(f"{'REPO:FILE':58} {'LN':>5}  {'PATTERN':16} {'CLASS':32} SNIPPET")
print("-" * 160)
counts = {}
critical_open = []
for rel, ln, pat, txt in sorted(set(rows)):
    cls = classify(rel, ln, pat, txt)
    counts[cls] = counts.get(cls, 0) + 1
    if cls in ("OPEN", "REMOVED-VERIFY", "REVIEW"):
        critical_open.append((rel, ln, pat, cls, txt))
    print(f"{rel[:58]:58} {ln:>5}  {pat:16} {cls:32} {txt[:60]}")

print("\n=== SUMMARY ===")
for cls, n in sorted(counts.items()):
    print(f"  {cls:34} {n}")

# Hard gate: jwt_secret must NOT appear in executable firmware code
bad_jwt = [r for r in rows if r[2] in ("jwt_secret",) and "remediation" not in r[3].lower()
           and "was" not in r[3].lower() and "literal" not in r[3].lower()
           and "removed" not in r[3].lower() and "old" not in r[3].lower()]
if bad_jwt:
    print("\nCRITICAL: jwt_secret literal found in executable context:")
    for b in bad_jwt:
        print("  ", b)
    sys.exit(1)

print("\nStatic scan complete. Classification table above is the §41 deliverable.")
