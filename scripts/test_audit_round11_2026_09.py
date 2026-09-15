#!/usr/bin/env python3
# =============================================================================
# test_audit_round11_2026_09.py — ROUND-11 static gate (p.476 remediation:
# factory-reset one-time-token race in AuthManager).
#
# Auditor finding (verdict 2026-09-15, HEAD 989ee48):
#   p.476  P1 — prepareFactoryReset()/confirmFactoryReset() ran fully
#          unsynchronized on shared mutable state (_factoryResetToken +
#          _factoryResetTokenTime) while BOTH are cross-task (REST = web
#          task, MQTT = network task). Two concurrent confirms could both
#          pass the constant-time compare before either cleared the token —
#          a "one-time" authorization confirmed TWICE, both callers free to
#          enter the destructive factory wipe; a prepare racing a confirm
#          could observe a torn token/time pair.
#
# What this gate enforces (static, on every push):
#   [A] prepareFactoryReset(): fail-closed ("" when the lock is unavailable)
#       + the whole generate+store inside ONE critical section + success log
#       AFTER unlock (auth→log order, P3 hold time).
#   [B] confirmFactoryReset(): fail-closed (false, token NOT consumed) + the
#       WHOLE check → TTL → compare → consume chain inside ONE critical
#       section + the WAVE-5/FW-B1 length guard retained.
#   [C] Honest call-site contracts: REST prepare → 503 on empty token
#       (never a 200 with an empty token); MQTT prepare → REJECTED on empty
#       token (never an ACCEPTED that lies "token issued"); both confirm
#       sites keep the conservative refusal.
#   [D] Header contract documents the one-time invariant + the fail-closed
#       per-function values; boot guard and authLockFailures observability
#       not regressed.
#   [E] The native harness carries Phase F (p.476) in all three build modes,
#       the P476 sentinels (single-winner + outage), and the Phase W
#       recovery single-confirm.
#   [F] The old unlocked fingerprints are gone from the comment-stripped
#       source.
# =============================================================================
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
NATIVE = ROOT / "scripts" / "native"

results = []


def check(name: str, cond: bool, detail: str = "") -> None:
    results.append((name, bool(cond), detail))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail and not cond else ""))


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_comments(src: str) -> str:
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return src


def brace_body(src: str, signature: str) -> str:
    i = src.find(signature)
    if i < 0:
        return ""
    j = src.find("{", i)
    if j < 0:
        return ""
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j + 1:k]
    return ""


auth_cpp = read(FW / "Services" / "AuthManager.cpp")
auth_h = read(FW / "Services" / "AuthManager.h")
sh_cpp = read(FW / "Web" / "SystemHandlers.cpp")
mcr_cpp = read(FW / "Network" / "MqttConfigReceiver.cpp")
diag_cpp = read(FW / "Web" / "DiagnosticsHandlers.cpp")
harness = read(NATIVE / "verify_service_lock_concurrency.cpp")
runner = read(NATIVE / "run-native-tests.sh")
audit_doc = read(ROOT / "docs" / "AUDIT_2026_09_FOLLOWUP.md")

auth_nc = strip_comments(auth_cpp)

print("== [A] prepareFactoryReset — p.476: fail-closed + satu critical section ==")

prep = brace_body(auth_cpp, "String AuthManager::prepareFactoryReset()")
prep_nc = brace_body(auth_nc, "String AuthManager::prepareFactoryReset()")
check("A1 prepare fail-closed: _lockAuth() → \"\" (tanpa kunci TIDAK ada token)",
      "if (!_lockAuth())" in prep_nc and "return String();" in prep_nc)
check("A2 prepare menolak jujur (log AuthFail + 'no token issued')",
      "Core::LogType::AuthFail" in prep and "no token issued" in prep)
lock_p = prep_nc.find("_lockAuth()")
gen_p = prep_nc.find("Utils::generateToken(32)")
cp_p = prep_nc.find("strncpy(_factoryResetToken")
ts_p = prep_nc.find("_factoryResetTokenTime = millis()")
unl_p = prep_nc.find("_unlockAuth()")
check("A3 seluruh generate+store DI DALAM satu critical section (lock < generate < copy < timestamp < unlock)",
      0 <= lock_p < gen_p < cp_p < ts_p < unl_p,
      f"lock@{lock_p} gen@{gen_p} copy@{cp_p} ts@{ts_p} unlock@{unl_p}")
log_prep_p = prep_nc.find('"Factory reset prepared')
check("A4 log sukses SETELAH unlock (urutan auth→log satu arah, hold time P3)",
      0 <= unl_p < log_prep_p,
      f"unlock@{unl_p} log@{log_prep_p}")

print("== [B] confirmFactoryReset — p.476: rantai check→TTL→compare→consume SATU critical section ==")

conf = brace_body(auth_cpp, "bool AuthManager::confirmFactoryReset(const String& token)")
conf_nc = brace_body(auth_nc, "bool AuthManager::confirmFactoryReset(const String& token)")
check("B1 confirm fail-closed: _lockAuth() → false, token TIDAK dikonsumsi",
      "if (!_lockAuth())" in conf_nc and "return false;" in conf_nc)
check("B2 confirm menolak jujur (log AuthFail + 'token NOT consumed')",
      "Core::LogType::AuthFail" in conf and "token NOT consumed" in conf)
check("B3 length guard WAVE-5/FW-B1 dipertahankan sebelum compare fixed-length",
      "token.length() != 32" in conf)
cl = conf_nc.find("_lockAuth()")
ttl = conf_nc.find("_factoryResetTokenTime > Core::FACTORY_RESET_TOKEN_TTL_MS")
cmp_ = conf_nc.find("constantTimeMemEquals")
cu = conf_nc.find("_unlockAuth()")
# Dua jalur bersih: TTL-discard (cabang if) mendahului compare secara sah;
# CONSUME (cabang else-if, setelah compare) adalah mutasi yang harus berada
# SETELAH compare dan sebelum unlock. Keduanya harus di dalam [cl, cu].
muts = [mm.start() for mm in re.finditer(re.escape("_factoryResetToken[0] = '\\0'"), conf_nc)]
consume_after_cmp = [m for m in muts if m > cmp_]
check("B4 rantai LENGKAP di bawah kunci (lock < TTL < compare < consume < unlock)",
      0 <= cl < ttl and cl < cmp_ < cu and len(consume_after_cmp) >= 1 and
      max(consume_after_cmp) < cu and all(cl < m < cu for m in muts),
      f"lock@{cl} ttl@{ttl} cmp@{cmp_} muts={muts} unlock@{cu}")
n_mut = conf_nc.count("_factoryResetToken[0] = '\\0'")
check("B5 SEMUA mutasi _factoryResetToken berada di dalam kunci (2 jalur: TTL-discard + consume)",
      n_mut == 2 and all(cl < m < cu for m in
                         [mm.start() for mm in re.finditer(re.escape("_factoryResetToken[0] = '\\0'"), conf_nc)]),
      f"{n_mut} mutasi, window lock=[{cl},{cu}]")

print("== [C] Call-site jujur ==")

prep_h = brace_body(sh_cpp, "void handleFactoryResetPrepare()")
check("C1 REST prepare: token kosong → 503 jujur (tidak pernah 200 + token kosong)",
      "token.length() != 32" in prep_h and "sendError(503" in prep_h)
mcr_src = mcr_cpp
check("C2 MQTT prepare: token kosong → REJECTED jujur (ACK tidak berbohong 'token issued')",
      re.search(r"factory_reset_prepare[\s\S]{0,600}token\.length\(\) != 32[\s\S]{0,200}REJECTED", mcr_src) is not None)
check("C3 kedua situs confirm tetap konservatif (false → tolak, tanpa jalur sukses palsu)",
      "confirmFactoryReset(token))) {" not in sh_cpp and
      sh_cpp.count("confirmFactoryReset(token)") == 1 and
      mcr_cpp.count("confirmFactoryReset(token)") == 1)

print("== [D] Kontrak header + observabilitas ==")

check("D1 header mendokumentasikan invariant one-time p.476 + nilai fail-closed per-fungsi",
      "ROUND 11 / p.476" in auth_h and "ONE-TIME CONFIRMATION INVARIANT" in auth_h and
      "prepareFactoryReset() → \"\"" in auth_h and "confirmFactoryReset() → false" in auth_h)
check("D2 anggota _factoryResetToken dianotasikan ter-guard _authMutex (lintas task)",
      re.search(r"Guarded by _authMutex[\s\S]{0,200}_factoryResetToken\[33\]", auth_h) is not None)
abegin = brace_body(auth_cpp, "void AuthManager::begin()")
check("D3 boot guard FATAL p.475 tidak terregresi (menutup p.476 juga — mutex yang sama)",
      "while (true)" in abegin and "delay(10000)" in abegin and "FATAL" in abegin)
check("D4 authLockFailures tetap terekspos di /api/diagnostics (mencakup refusal p.476)",
      'doc["authLockFailures"]' in diag_cpp)

print("== [E] Harness native round-11 ==")

check("E1 fase F (p.476) ada dan dipanggil di main",
      "static bool phaseF()" in harness and "anyTrip |= !phaseF();" in harness)
check("E2 sentinel P476 ada di dua titik (single-winner + outage)",
      harness.count('SENTINEL_TRIP("476"') == 2)
check("E3 mirror prepare/confirm 3 mode build (treatment fail-closed; kedua NC unlocked = bentuk pra-r11)",
      "std::string prepareFactoryReset()" in harness and
      "bool confirmFactoryReset(const std::string& token)" in harness and
      harness.count("#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)\n    return prepareFactoryResetBody();") == 1 and
      harness.count("#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)\n    return confirmFactoryResetBody(token);") == 1)
check("E4 Phase X meng-storm jalur factory + Phase W konfirmasi tepat-sekali pasca-recovery",
      "frPrepares.fetch_add(1)" in harness and "frConfirms.fetch_add(1)" in harness and
      "confirm exactly ONCE after recovery" in harness)
check("E5 invarian single-winner fase F (8 konfirmer konkuren)",
      "exactly ONE concurrent confirm may consume the one-time token" in harness)
check("E6 runner masih mewajibkan kedua negative control trip (regresi round-10)",
      "NEGATIVE CONTROL FAILED: unlocked services ran clean" in runner and
      "NEGATIVE CONTROL FAILED (p.472-p.475): fail-open services ran clean" in runner)

print("== [F] Fingerprint lama hilang (source tanpa komentar) ==")

check("F1 prepare tidak lagi berjalan tanpa kunci (fingerprint pra-r11: body langsung generate)",
      "_lockAuth()" in prep_nc and prep_nc.lstrip().find("Utils::generateToken") > 0)
check("F2 confirm tidak lagi dibuka dengan baca state tanpa kunci",
      "_lockAuth()" in conf_nc and not conf_nc.lstrip().startswith("if (_factoryResetToken[0]"))
check("F3 tidak ada mutasi _factoryResetToken di luar critical section di seluruh file",
      all(m > auth_nc.find("_lockAuth()") for m in
          [mm.start() for mm in re.finditer(re.escape("_factoryResetToken[0] = '\\0'"), auth_nc)]))

print("== [G] Dokumentasi ==")

check("G1 §17 round-11 terdokumentasi di AUDIT_2026_09_FOLLOWUP.md",
      "## 17. Round 11" in audit_doc and "p.476" in audit_doc and
      "ONE-TIME" in audit_doc.upper())

fails = [r for r in results if not r[1]]
print()
for name, ok, detail in fails:
    print(f"  FAIL: {name} — {detail}")
print(f"ROUND-11 GATE: {len(results) - len(fails)}/{len(results)} PASS")
sys.exit(1 if fails else 0)
