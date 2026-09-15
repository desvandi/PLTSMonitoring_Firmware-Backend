#!/usr/bin/env python3
# =============================================================================
# test_audit_round10_2026_09.py — ROUND-10 static gate (p.472–p.475
# remediation: the fail-open mutex FAMILY in LogService, TransactionJournal,
# BatteryCommManager and AuthManager).
#
# Auditor findings (baseline f724e9c, verdict 2026-09-15):
#   p.472  P1 — LogService: readers (getActivityJson/getAuditText) read the
#          ring/audit buffer WITHOUT the mutex; append()/audit()/
#          flushToDisk() were fail-open when _mutex == nullptr.
#   p.473  P1/P2 — TransactionJournal::_lock() was a no-op when the handle
#          was null → storeTransaction (networkTask) and updateAck
#          (relayTask) interleaved over the RAM mirror + NVS slots.
#   p.474  P1 — BatteryCommManager: crossCheckShunt() read _data.current
#          BEFORE the mutex check and never acquired it (reader
#          synchronization gap); getData() depended on the handle existing.
#   p.475  P1/P2 — AuthManager::_lockAuth() failed open → the whole
#          refresh-token rotation critical section could run
#          unsynchronized (the A→B AND A→C replay race AUTH-GATE-05 closed).
#
# What this gate enforces (static, on every push):
#   [A] LogService: reader-side serialization + fail-closed writers +
#       boot-guard begin() + lock-free lockFailures().
#   [B] TransactionJournal: fail-closed _lock() + boot guard +
#       TransactionDecision::Unavailable with a REJECT branch at EVERY
#       decide() consumer (REST 503 / MQTT JOURNAL_UNAVAILABLE).
#   [C] BatteryCommManager: crossCheckShunt() takes the lock BEFORE reading
#       _data; getData()/socAuthoritative()/isMismatchActive()/
#       getLastMismatchA() are lock-guarded; boot guard; timeout-vs-
#       unavailability distinction preserved.
#   [D] AuthManager: fail-closed _lockAuth() + boot guard + every refresh
#       operation refuses; issueRefreshToken persists INSIDE the lock (the
#       race the native harness caught under TSAN).
#   [E] Observability: the four lockFailures counters on /api/diagnostics
#       and the honest-500 login guard when no refresh token could issue.
#   [F] The native harness mirrors all four services with THREE build modes
#       (treatment / SVC_NO_LOCK / LOCK_FAIL_OPEN) and the runner REQUIRES
#       both negative controls to trip.
#   [G] No old fail-open fingerprint survives anywhere in the four services.
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


log_cpp = read(FW / "Services" / "LogService.cpp")
log_h = read(FW / "Services" / "LogService.h")
txn_cpp = read(FW / "Services" / "TransactionJournal.cpp")
txn_h = read(FW / "Services" / "TransactionJournal.h")
bms_cpp = read(FW / "Comm" / "BatteryCommManager.cpp")
bms_h = read(FW / "Comm" / "BatteryCommManager.h")
auth_cpp = read(FW / "Services" / "AuthManager.cpp")
auth_h = read(FW / "Services" / "AuthManager.h")
diag_cpp = read(FW / "Web" / "DiagnosticsHandlers.cpp")
authhandlers_cpp = read(FW / "Web" / "AuthHandlers.cpp")
harness = read(NATIVE / "verify_service_lock_concurrency.cpp")
runner = read(NATIVE / "run-native-tests.sh")

# Comment-stripped views — structural/ordering checks must not be fooled by
# documentation quoting the OLD shapes.
log_nc = strip_comments(log_cpp)
txn_nc = strip_comments(txn_cpp)
bms_nc = strip_comments(bms_cpp)
auth_nc = strip_comments(auth_cpp)

print("== [A] LogService — p.472: reader serialization + fail-closed ==")

lock_body = brace_body(log_cpp, "bool LogService::_lock() const")
check("A1 _lock() bool fail-closed dengan retry-create sekali",
      "bool LogService::_lock() const" in log_cpp and
      "if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();" in lock_body and
      "_lockFailures.fetch_add" in lock_body and
      "return false;" in lock_body)
check("A2 jalur kegagalan _lock() TIDAK memanggil Log.append (anti rekursi-dir)",
      "Log.append" not in lock_body and "Serial.printf" in lock_body)

begin_body = brace_body(log_cpp, "void LogService::begin()")
check("A3 begin() boot guard FATAL (halt tanpa feed WDT)",
      "xSemaphoreCreateMutex" in begin_body and
      "while (true)" in begin_body and
      "delay(10000)" in begin_body and
      "FATAL" in begin_body)

for fn, sig in [("append", "void LogService::append(Core::LogType type"),
                ("audit", "void LogService::audit(const char* action"),
                ("flushToDisk", "void LogService::flushToDisk()")]:
    body = brace_body(log_nc, sig)
    check(f"A4 {fn}() fail-closed (guard di awal)",
          body.lstrip().startswith("if (!_lock())"))

for fn, sig, empty in [
    ("getActivityJson", "String LogService::getActivityJson(uint16_t limit, int8_t filterType) const",
     '{"logs":[],"lockUnavailable":true}'),
    ("getAuditText", "String LogService::getAuditText(uint16_t maxBytes) const", ""),
    ("getActivityCount", "uint16_t LogService::getActivityCount() const", ""),
    ("getAuditBytes", "uint16_t LogService::getAuditBytes() const", ""),
]:
    body = brace_body(log_cpp, sig)
    check(f"A5 {fn}() reader terkunci + nilai jujur saat unavailable",
          "if (!_lock())" in body and "_unlock()" in body and
          ("return" in body.split("_unlock()")[0]))

check("A6 aksesor lockFailures() lock-free di header",
      "uint32_t lockFailures() const" in log_h and
      "_lockFailures.load(std::memory_order_relaxed)" in log_h)

check("A7 fingerprint fail-open lama hilang (if (_mutex && ... take)",
      "if (_mutex && xSemaphoreTake" not in log_cpp and
      "if (_mutex) xSemaphoreGive" not in log_cpp)

print("== [B] TransactionJournal — p.473: fail-closed + Unavailable ==")

tlock = brace_body(txn_cpp, "bool TransactionJournal::_lock()")
check("B1 _lock() bool fail-closed",
      "bool TransactionJournal::_lock()" in txn_cpp and
      "_lockFailures.fetch_add" in tlock and "return false;" in tlock)
tbegin = brace_body(txn_cpp, "void TransactionJournal::begin()")
check("B2 begin() boot guard FATAL",
      "while (true)" in tbegin and "delay(10000)" in tbegin and "FATAL" in tbegin)
check("B3 enum TransactionDecision::Unavailable ada",
      "Unavailable = 3" in txn_h)

dec = brace_body(txn_cpp, "TransactionDecision TransactionJournal::decide(const String& requestId,")
check("B4 decide() → Unavailable saat lock tidak tersedia",
      "if (!_lock())" in dec and "TransactionDecision::Unavailable" in dec)
check("B5 storeTransaction/updateAck → false; isProcessed → true (aman-replay); getter → \"\"",
      "if (!_lock()) return false;" in brace_body(txn_cpp, "bool TransactionJournal::storeTransaction(const String& requestId,") and
      "if (!_lock()) return false;" in brace_body(txn_cpp, "bool TransactionJournal::updateAck(const String& requestId,") and
      "if (!_lock()) return true;" in brace_body(txn_cpp, "bool TransactionJournal::isProcessed(const String& requestId)") and
      "if (!_lock()) return String();" in brace_body(txn_cpp, "String TransactionJournal::getAckJson(const String& requestId)"))
check("B6 lockFailures() lock-free",
      "uint32_t lockFailures() const" in txn_h)

consumers = {
    "Web/RelayHandlers.cpp": 2, "Web/ExtraHandlers.cpp": 3, "Web/AlarmHandlers.cpp": 1,
    "Web/CalibrationHandlers.cpp": 2, "Web/ConfigHandlers.cpp": 1,
    "Network/MqttOtaHandler.cpp": 1, "Network/MqttConfigReceiver.cpp": 1,
}
total_branches = 0
for path, expected in consumers.items():
    src = read(FW / path)
    n = len(re.findall(r"TransactionDecision::Unavailable", src))
    total_branches += n
    check(f"B7 cabang Unavailable di {path} ({n}/{expected})", n >= expected)
check("B7 total cabang Unavailable >= 11 di seluruh konsumen decide()",
      total_branches >= 11)

print("== [C] BatteryCommManager — p.474: urutan kunci + snapshot ==")

cross = brace_body(bms_nc, "float BatteryCommManager::crossCheckShunt(float shuntCurrentA, uint32_t nowMs)")
lock_pos = cross.find("if (!_lock(50))")
data_pos = cross.find("_data.current")
check("C1 crossCheckShunt() mengambil kunci SEBELUM membaca _data.current",
      0 <= lock_pos < data_pos,
      f"lock@{lock_pos} vs data@{data_pos}")
check("C2 crossCheckShunt() mengembalikan NAN saat lock tidak tersedia, state tak tersentuh",
      "if (!_lock(50)) return NAN;" in cross)

bbegin = brace_body(bms_cpp, "void BatteryCommManager::begin()")
check("C3 begin() boot guard FATAL",
      "while (true)" in bbegin and "delay(10000)" in bbegin and "FATAL" in bbegin)

gd = brace_body(bms_cpp, "BmsData BatteryCommManager::getData() const")
check("C4 getData() fail-closed (reset + kunci + salin)",
      "copy.reset();" in gd and "_lock(50)" in gd)
sa = brace_body(bms_cpp, "bool BatteryCommManager::socAuthoritative() const")
check("C5 socAuthoritative() membaca snapshot di bawah kunci, fail-closed → false",
      "if (!_lock(50)) return false;" in sa and "_unlock();" in sa)
check("C6 isMismatchActive/getLastMismatchA terkunci (bukan inline tanpa kunci)",
      "if (!_lock(50)) return false;" in brace_body(bms_cpp, "bool BatteryCommManager::isMismatchActive() const") and
      "if (!_lock(50)) return NAN;" in brace_body(bms_cpp, "float BatteryCommManager::getLastMismatchA() const"))
bms_lock = brace_body(bms_cpp, "bool BatteryCommManager::_lock(uint32_t timeoutMs) const")
check("C7 timeout TIDAK dihitung sebagai lockFailure (hanya unavailability)",
      "return xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;" in bms_lock and
      "_lockFailures.fetch_add" in bms_lock)
check("C8 fingerprint lama hilang (baca _data sebelum cek mutex)",
      "float bmsI = _data.current;\n  if (!_mutex) return NAN;" not in bms_cpp)

print("== [D] AuthManager — p.475: critical section refresh fail-closed ==")

alock = brace_body(auth_cpp, "bool AuthManager::_lockAuth()")
check("D1 _lockAuth() bool fail-closed",
      "bool AuthManager::_lockAuth()" in auth_cpp and
      "_lockFailures.fetch_add" in alock and "return false;" in alock)
abegin = brace_body(auth_cpp, "void AuthManager::begin()")
check("D2 begin() boot guard FATAL",
      "while (true)" in abegin and "delay(10000)" in abegin and "FATAL" in abegin)
check("D3 tidak ada deklarasi ganda _lockAuth (void lama)",
      auth_h.count("_lockAuth();") == 1 and "bool _lockAuth();" in auth_h)

consume = brace_body(auth_cpp, "bool AuthManager::consumeRefreshToken(const String& oldToken, String& outNewToken,")
check("D4 consumeRefreshToken() menolak tanpa kunci (no rotation)",
      consume.lstrip().find("if (!_lockAuth()) return false;") < consume.find("for (int i = 0"))
issue = brace_body(auth_cpp, "String AuthManager::issueRefreshToken(WebServer& server, const String& username)")
check("D5 issueRefreshToken() di bawah kunci + persist DI DALAM kunci (race yang ditangkap harness)",
      "if (!_lockAuth()) return String();" in issue and
      issue.find("_persistRefreshTokens();") < issue.find("_unlockAuth();"))
revoke = brace_body(auth_cpp, "void AuthManager::revokeAllRefreshTokens()")
check("D6 revokeAllRefreshTokens() menolak jujur saat lock tidak tersedia",
      "if (!_lockAuth())" in revoke and "REFUSED" in revoke)
verify = brace_body(auth_cpp, "bool AuthManager::verifyRefreshToken(const String& token, String& outUsername)")
check("D7 verifyRefreshToken() terkunci + fail-closed",
      "if (!_lockAuth()) return false;" in verify)
check("D8 lockFailures() lock-free",
      "uint32_t lockFailures() const" in auth_h)

print("== [E] Observabilitas & handler ==")

check("E1 /api/diagnostics memaparkan 4 counter lockFailures baru",
      'doc["logLockFailures"]' in diag_cpp and
      'doc["journalLockFailures"]' in diag_cpp and
      'doc["battCommLockFailures"]' in diag_cpp and
      'doc["authLockFailures"]' in diag_cpp)
check("E2 login handler menolak jujur (500) saat refresh token gagal terbit",
      "Session persistence unavailable (auth lock)" in authhandlers_cpp)

print("== [F] Harness native round-10 ==")

check("F1 harness memuat 3 mode build (treatment / SVC_NO_LOCK / LOCK_FAIL_OPEN)",
      "-DSVC_NO_LOCK" in harness and "-DLOCK_FAIL_OPEN" in harness and
      "verify_service_lock_concurrency" in harness)
check("F2 harness memuat fase V/R/S/T/U/X/W + sentinel per-temuan",
      all(f"phase{p}()" in harness for p in ["V", "R", "S", "T", "U", "X"]) and
      'SENTINEL_TRIP("472"' in harness and 'SENTINEL_TRIP("473"' in harness and
      'SENTINEL_TRIP("474"' in harness and 'SENTINEL_TRIP("475"' in harness)
check("F3 runner menjalankan treatment TSAN+ASAN dan MEWAJIBKAN kedua negative control trip",
      "verify_service_lock_concurrency (TREATMENT, ThreadSanitizer)" in runner and
      "NEGATIVE CONTROL FAILED: unlocked services ran clean" in runner and
      "NEGATIVE CONTROL FAILED (p.472-p.475): fail-open services ran clean" in runner)

print("== [G] Fingerprint fail-open lama hilang dari keempat service ==")

check("G1 tidak ada 'if (handle) take' tanpa else-reject di 4 service (source tanpa komentar)",
      "if (_mutex) xSemaphoreTake" not in txn_nc and
      "if (_authMutex) xSemaphoreTake" not in auth_nc and
      "if (_mutex) xSemaphoreGive" not in txn_nc and
      "if (_mutex && xSemaphoreTake" not in log_nc)

fails = [r for r in results if not r[1]]
print()
for name, ok, detail in fails:
    print(f"  FAIL: {name} — {detail}")
print(f"ROUND-10 GATE: {len(results) - len(fails)}/{len(results)} PASS")
sys.exit(1 if fails else 0)
