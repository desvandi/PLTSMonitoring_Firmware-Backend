#!/usr/bin/env python3
"""
test_audit_round4_2026_09.py — [AUDIT 2026-09 ROUND 4] security provisioning
+ hardware anti-rollback + build-attestation contracts.

Auditor round-4 findings this gate pins:
  p.436  OTA anti-rollback is software-only (no eFuse secure_version use)
  p.437  Secure Boot + Flash Encryption unproven/unenforced in production
  item4  INA219_SIGN_CORRECTION still ASSUMED (must STAY until INA-004 passes)
  concl4 proof that the flashed binary is the audited artifact

What this gate enforces (static, on every push):
  A. OtaManager runs BOTH new hardware-security gates in BOTH OTA paths,
     BEFORE any bytes are accepted.
  B. The anti-rollback floor burns ONLY in production builds, ONLY on
     activation, using IDF-bootloader-compatible unary semantics.
  C. The eFuse posture is READ-ONLY everywhere else (no stray burns).
  D. /api/security + /api/diagnostics/ina219 exist, authenticated, and
     report (never fake) the posture.
  E. The security ledger survives factory reset (not in the wipe list).
  F. The INA219 sign constant KEEPS its ASSUMED honesty flag.
  G. verify_flashed_image.py exists and its self-test passes.
  H. Python mirrors: ledger reconcile decision table, candidate floor gate,
     epoch advance rule, parity rule, provisioning verdict rule.

Run: python3 scripts/test_audit_round4_2026_09.py   (exit 0 = PASS)
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

results = []


def check(name: str, cond: bool, detail: str = "") -> None:
    results.append((name, cond, detail))
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}" + (f" — {detail}" if detail and not cond else ""))


def read(p: Path) -> str:
    return p.read_text(errors="replace")


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


print("=" * 78)
print("AUDIT ROUND 4 — security provisioning / anti-rollback / attestation")
print("=" * 78)

# ---------------------------------------------------------------------------
# A. OtaManager gate wiring
# ---------------------------------------------------------------------------
print("\n[A] OtaManager hardware-security gates")
ota_cpp = read(FW / "Services" / "OtaManager.cpp")
ota_cpp_nc = strip_comments(ota_cpp)
ota_h = read(FW / "Services" / "OtaManager.h")

check("A1 beginUpload calls _validateProvisioning before Update.begin",
      "_validateProvisioning()" in ota_cpp_nc.split("bool OtaManager::beginUpload")[1]
      .split("bool OtaManager::feedChunk")[0]
      .split("Update.begin(")[0])
check("A2 beginUpload calls _validateSecurityFloor before Update.begin",
      "_validateSecurityFloor(String(expectedVersion))" in
      ota_cpp_nc.split("bool OtaManager::beginUpload")[1]
      .split("bool OtaManager::feedChunk")[0]
      .split("Update.begin(")[0])
check("A3 beginDownload calls both gates before allowlist/CA",
      "_validateProvisioning()" in ota_cpp_nc.split("bool OtaManager::beginDownload")[1]
      .split("void OtaManager::tickDownload")[0]
      .split("_validateUrlAllowlist(")[0]
      and "_validateSecurityFloor(String(expectedVersion))" in
      ota_cpp_nc.split("bool OtaManager::beginDownload")[1]
      .split("void OtaManager::tickDownload")[0]
      .split("_validateUrlAllowlist(")[0])
check("A4 manifest path (ota.check) inherits gates via beginDownload handoff",
      "beginDownload(url, ver, size, sha, sig)" in ota_cpp_nc)
check("A5 activation burns floor: markBootHealthyIfPending calls onImageActivated",
      "securityPosture.onImageActivated()" in
      ota_cpp_nc.split("void OtaManager::markBootHealthyIfPending")[1]
      .split("void OtaManager::markBootHealthy()")[0])
check("A6 gate decls present in header",
      "bool _validateProvisioning();" in ota_h and
      "bool _validateSecurityFloor(const String& newVer);" in ota_h)
check("A7 floor rejection logs (rollback evidence)",
      "OTA REFUSED by hardware security floor" in ota_cpp)

# ---------------------------------------------------------------------------
# B. SecurityPosture semantics
# ---------------------------------------------------------------------------
print("\n[B] SecurityPosture — read, enforce, burn")
sp_h = read(FW / "Services" / "SecurityPosture.h")
sp_cpp = read(FW / "Services" / "SecurityPosture.cpp")

check("B1 reads IDF-canonical floor (popcount API)",
      "esp_efuse_read_secure_version()" in sp_cpp)
check("B2 burns with IDF-canonical unary API",
      "esp_efuse_update_secure_version(" in sp_cpp)
check("B3 checks bootloader rule parity (candidate >= floor is delegated to "
      "candidateAllowedByFloor)",
      "candidateAllowedByFloor" in sp_h and ">=" in sp_h.split("candidateAllowedByFloor")[1][:200])
check("B4 flash encryption via FLASH_CRYPT_CNT parity (not a runtime flag)",
      "ESP_EFUSE_FLASH_CRYPT_CNT" in sp_cpp and "ones & 1" in strip_comments(sp_cpp))
check("B5 secure boot bits read: ABS_DONE_0 + ABS_DONE_1",
      "ESP_EFUSE_ABS_DONE_0" in sp_cpp and "ESP_EFUSE_ABS_DONE_1" in sp_cpp)
check("B6 burn requires PRODUCTION_BUILD (dev/staging never burn)",
      "#if defined(PRODUCTION_BUILD) && !defined(PLTS_DISABLE_EFUSE_BURN)" in sp_cpp)
check("B7 OTA-requires-encryption defaults ON in production",
      "#if defined(PRODUCTION_BUILD) && !defined(PLTS_ALLOW_UNENCRYPTED_OTA)" in sp_cpp)
check("B8 burn failure reported honestly (BurnFail, no fake success)",
      "LedgerStatus::BurnFail" in sp_cpp and "NOT advanced" in sp_cpp)
check("B9 self-measurement uses mmap (cache-decrypt path, works encrypted)",
      "esp_partition_mmap" in sp_cpp and "spi_flash_munmap" in sp_cpp)
check("B10 burn call sites are ONLY onImageActivated (no stray burns)",
      len(re.findall(r"esp_efuse_update_secure_version\(", strip_comments(sp_cpp))) == 1)
check("B11 burn is preceded by burnSupported check",
      "_s.burnSupported" in
      sp_cpp.split("bool SecurityPosture::onImageActivated")[1]
      .split("esp_efuse_update_secure_version(")[0])

# ---------------------------------------------------------------------------
# C. Ledger + factory reset survival
# ---------------------------------------------------------------------------
print("\n[C] Ledger durability")
fr_cpp = read(FW / "Services" / "FactoryReset.cpp")
ns_block = fr_cpp.split("FACTORY_RESET_NAMESPACES[] = {")[1].split("};")[0]
check("C1 plts_sec NOT in factory-reset sweep (survives reset)",
      "plts_sec" not in ns_block)
ns_count = len(re.findall(r'"plts', ns_block))
check("C2 13 operational namespaces unchanged",
      ns_count == 13, f"found {ns_count}")
check("C3 preservation is documented at the sweep site",
      "plts_sec" in fr_cpp.split("const char* const FACTORY_RESET_NAMESPACES")[0].split("namespace Services {")[-1])
check("C4 ledger namespace literal used consistently",
      'p.begin("plts_sec"' in strip_comments(sp_cpp))
check("C5 ledger self-heal only when running image is exactly one epoch newer",
      "epoch + 1 == floor" in sp_cpp and "runNewer" in sp_cpp)

# ---------------------------------------------------------------------------
# D. Endpoints
# ---------------------------------------------------------------------------
print("\n[D] Evidence endpoints")
http_cpp = read(FW / "Web" / "HttpServer.cpp")
sec_cpp = read(FW / "Web" / "SecurityHandlers.cpp")
diag_cpp = read(FW / "Web" / "DiagnosticsHandlers.cpp")

check("D1 /api/security route registered",
      'http.on("/api/security"' in sec_cpp and
      "SecurityHandlers::registerRoutes();" in http_cpp)
check("D2 /api/security is authenticated",
      "requireAuth()" in sec_cpp)
check("D3 /api/security reports verdict, not assertion (computed PASS/FAIL)",
      "provisioning" in sec_cpp and "DEGRADED" in sec_cpp and "FAIL" in sec_cpp)
check("D4 /api/security never fakes unmeasured values",
      'img.measured ? img.sha256 : ""' in sec_cpp)
check("D5 /api/diagnostics/ina219 registered + authenticated",
      'http.on("/api/diagnostics/ina219"' in diag_cpp and
      "requireAuth()" in diag_cpp.split("handleGetIna219")[1][:400])
check("D6 INA219 endpoint reports the ASSUMED sign status verbatim",
      "ASSUMED" in diag_cpp and "NOT HARDWARE VERIFIED" in diag_cpp)
check("D7 INA219 endpoint exposes raw registers + constant chain",
      "readDiagnostic()" in diag_cpp and "signCorrection" in diag_cpp)
check("D8 BMS cross-check included (sign verdict path)",
      "bmsCrossCheck" in diag_cpp and "signAgreement" in diag_cpp)
check("D9 driver exposes read-only diagnostic (no PGA side effects)",
      "Ina219Diag readDiagnostic();" in read(FW / "Drivers" / "Ina219Driver.h"))
check("D10 version identity intact (no silent bump this round)",
      re.search(r'FIRMWARE_VERSION\s*=\s*"1\.9\.3"', read(FW / "Core" / "Config.h")) is not None)

# ---------------------------------------------------------------------------
# E. INA219 honesty contract (auditor item 4)
# ---------------------------------------------------------------------------
print("\n[E] INA219 honesty")
cfg = read(FW / "Core" / "Config.h")
sign_block = cfg.split("INA219_SIGN_CORRECTION")[0][-400:]
check("E1 ASSUMED flag retained until INA-004 evidence lands",
      "ASSUMED" in sign_block and "NOT HARDWARE VERIFIED" in sign_block)
check("E2 constant unchanged (-1.0f)",
      re.search(r"INA219_SIGN_CORRECTION\s*=\s*-1\.0f", cfg) is not None)

# ---------------------------------------------------------------------------
# F. Operator tooling + docs
# ---------------------------------------------------------------------------
print("\n[F] Tooling + runbook")
vfi = ROOT / "scripts" / "verify_flashed_image.py"
check("F1 verify_flashed_image.py exists", vfi.is_file())
if vfi.is_file():
    r = subprocess.run([sys.executable, str(vfi), "--self-test"],
                       capture_output=True, text=True, timeout=60)
    check("F2 verify_flashed_image.py --self-test exits 0", r.returncode == 0,
          r.stdout + r.stderr)
    vfi_src = vfi.read_text()
    check("F3 device mode compares against release.json digest",
          "firmwareSha256" in vfi_src and "runningImage" in vfi_src)
    check("F4 UART mode hashes parsed image (not raw slot)",
          "image_length_from_header" in vfi_src)

# ---------------------------------------------------------------------------
# G. Python mirrors — decision tables
# ---------------------------------------------------------------------------
print("\n[G] Python mirrors")


def mirror_candidate_allowed(cand_maj, cand_min, led_maj, led_min):
    """Mirror of SecurityPosture::candidateAllowedByFloor."""
    return cand_maj > led_maj or (cand_maj == led_maj and cand_min >= led_min)


floor_cases = [
    # (cand, ledger, want) — ledger at 1.10 (epoch burned)
    ((1, 9, 4), (1, 10), False, "1.9.4 below burned epoch 1.10 → REJECT"),
    ((1, 9, 99), (1, 10), False, "1.9.99 still below → REJECT"),
    ((1, 10, 0), (1, 10), True, "same epoch re-flash (post-rollback recovery) → ALLOW"),
    ((1, 10, 7), (1, 10), True, "patch bump within epoch → ALLOW"),
    ((1, 11, 0), (1, 10), True, "minor advance → ALLOW"),
    ((2, 0, 0), (1, 10), True, "major advance → ALLOW"),
    ((1, 10, 0), (0, 0), True, "fresh device (epoch 0) → ALLOW"),
    ((1, 9, 3), (0, 0), True, "fresh device, old-style version → ALLOW (semver gate rules)"),
]
for (cand, led, want, why) in floor_cases:
    got = mirror_candidate_allowed(cand[0], cand[1], led[0], led[1])
    check(f"G1 floor gate: {why}", got == want)


def mirror_reconcile(exists, epoch, floor, run_maj, run_min, led_maj, led_min):
    """Mirror of SecurityPosture::_reconcileLedger decision table."""
    run_newer = run_maj > led_maj or (run_maj == led_maj and run_min > led_min)
    if exists and epoch == floor:
        return "OK"
    if exists and epoch + 1 == floor and run_newer:
        return "SELF_HEALED"
    if not exists and floor == 0:
        return "OK_INIT"
    if not exists and floor > 0:
        return "NO_LEDGER"
    return "TAMPER"


ledger_cases = [
    # clean boot
    (dict(exists=True, epoch=2, floor=2, run_maj=1, run_min=11, led_maj=1, led_min=11),
     "OK", "ledger matches floor"),
    # lost ledger write mid-activation (power loss between burn and NVS store)
    (dict(exists=True, epoch=1, floor=2, run_maj=1, run_min=11, led_maj=1, led_min=10),
     "SELF_HEALED", "running image proves the burn belonged to it"),
    # same gap but running image NOT newer → cannot attribute the burn
    (dict(exists=True, epoch=1, floor=2, run_maj=1, run_min=10, led_maj=1, led_min=10),
     "TAMPER", "extra burned bit, no proof of ownership"),
    # NVS wiped on a fresh device
    (dict(exists=False, epoch=0, floor=0, run_maj=1, run_min=9, led_maj=0, led_min=0),
     "OK_INIT", "fresh device initializes ledger"),
    # NVS wiped after epochs burned (rollback evidence)
    (dict(exists=False, epoch=0, floor=3, run_maj=1, run_min=9, led_maj=0, led_min=0),
     "NO_LEDGER", "floor>0 without ledger"),
    # gap > 1
    (dict(exists=True, epoch=0, floor=3, run_maj=1, run_min=9, led_maj=0, led_min=0),
     "TAMPER", "multi-epoch gap impossible honestly"),
    # ledger ahead of floor (NVS forged) — impossible honestly
    (dict(exists=True, epoch=3, floor=1, run_maj=1, run_min=9, led_maj=1, led_min=9),
     "TAMPER", "ledger ahead of hardware"),
]
for (case, want, why) in ledger_cases:
    got = mirror_reconcile(**case)
    check(f"G2 reconcile: {why}", got == want)


def mirror_epoch_advance(run_maj, run_min, led_maj, led_min):
    """Mirror of SecurityPosture::onImageActivated advance rule."""
    return run_maj > led_maj or (run_maj == led_maj and run_min > led_min)


advance_cases = [
    ((1, 10, 1, 10), False, "patch-only update: no burn"),
    ((1, 11, 1, 10), True, "minor advance: burn 1 bit"),
    ((2, 0, 1, 11), True, "major advance: burn 1 bit"),
    ((1, 9, 1, 10), False, "running older than ledger (rollback state): no burn"),
]
for ((rm, rn, lm, ln), want, why) in advance_cases:
    check(f"G3 epoch advance: {why}", mirror_epoch_advance(rm, rn, lm, ln) == want)


def mirror_parity(bits7):
    """Mirror of FLASH_CRYPT_CNT parity rule (odd = enabled)."""
    return bin(bits7).count("1") % 2 == 1


parity_cases = [
    # ESP32 rule: enabled iff an ODD NUMBER OF BITS is set (value ≠ parity!
    # 0b10 = one bit = ON; 0b11 = two bits = OFF — verified against the
    # IDF v4.4 efuse table description of FLASH_CRYPT_CNT).
    (0, False), (1, True), (2, True), (3, False), (5, False),
    (6, False), (0x7F, True), (0x40, True), (0x7E, False),
]
for (val, want) in parity_cases:
    check(f"G4 parity 0b{val:07b} ({bin(val).count('1')} bits) -> {'on' if want else 'off'}",
          mirror_parity(val) == want)


def mirror_provisioning_verdict(profile, enc, sb1, sb2, ledger_ok):
    """Mirror of SecurityHandlers verdict computation."""
    if profile != "PRODUCTION":
        return "N/A"
    verdict = "PASS"
    if not enc:
        verdict = "FAIL"
    if not (sb1 or sb2):
        if enc:
            verdict = "DEGRADED"
    if not ledger_ok:
        verdict = "FAIL"
    return verdict


verdict_cases = [
    (("PRODUCTION", True, True, False, True), "PASS", "fully provisioned"),
    (("PRODUCTION", True, False, False, True), "DEGRADED", "encrypted, secure boot off"),
    (("PRODUCTION", False, False, False, True), "FAIL", "unencrypted production device"),
    (("PRODUCTION", True, True, False, False), "FAIL", "ledger tamper overrides all"),
    (("DEVELOPMENT", False, False, False, False), "N/A", "dev build reports facts only"),
    (("STAGING", True, False, False, True), "N/A", "staging reports facts only"),
]
for ((prof, enc, sb1, sb2, lok), want, why) in verdict_cases:
    check(f"G5 verdict: {why}",
          mirror_provisioning_verdict(prof, enc, sb1, sb2, lok) == want)


def mirror_ota_provisioning(profile_requires_enc, enc_enabled, ledger_ok):
    """Mirror of SecurityPosture::otaProvisioningOk."""
    if profile_requires_enc and not enc_enabled:
        return False
    if not ledger_ok:
        return False
    return True


check("G6 OTA gate: production + unencrypted → refuse",
      mirror_ota_provisioning(True, False, True) is False)
check("G7 OTA gate: dev + unencrypted → allowed (facts reported)",
      mirror_ota_provisioning(False, False, True) is True)
check("G8 OTA gate: encrypted + tampered ledger → refuse",
      mirror_ota_provisioning(True, True, False) is False)
check("G9 OTA gate: encrypted + OK ledger → allowed",
      mirror_ota_provisioning(True, True, True) is True)

# ---------------------------------------------------------------------------
print("\n" + "=" * 78)
failed = [r for r in results if not r[1]]
total = len(results)
print(f"ROUND-4 GATE: {total - len(failed)}/{total} PASS")
if failed:
    print("\nFAILED checks:")
    for (name, _cond, detail) in failed:
        print(f"  - {name}" + (f" — {detail}" if detail else ""))
    sys.exit(1)
print("ALL ROUND-4 CONTRACTS HOLD")
sys.exit(0)
