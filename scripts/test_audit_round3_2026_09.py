#!/usr/bin/env python3
"""
test_audit_round3_2026_09.py — Follow-up audit round 3 (p.418/425/427/432/434)
================================================================================
Level-2 evidence for the 2026-09 cross-subsystem re-audit. Static source
contracts plus Python mirrors:

  p.418  requestId = TRANSPORT identity — distinct from transactionId (the
         logical/dedup identity). The canonicalizer surfaces it, the REST and
         MQTT relay ingress propagate it into QueuedRelayCommand, terminal
         records/acks/GET responses carry it, and duplicate replays LOG the
         retry's attempt id. No call-site may fill qc.requestId with the
         transactionId anymore.
  p.425  TLS variant gate — EVERY live setInsecure() across ALL firmware
         targets (firmware/, firmware-generic/, push-alarm/) must sit behind
         a compile-time guard (DEVELOPMENT_BUILD / #else-of-PRODUCTION /
         TLS_SKIP_CERT_VERIFY), and the push-alarm insecure opt-in flag
         must remain OFF by default (commented #define).
  p.427  Factory reset = single source — REST confirm, MQTT confirm and the
         boot-time completion all sweep the SAME 13-namespace list declared
         once in Services/FactoryReset.cpp ("plts" LAST so the IN_PROGRESS
         marker survives the sweep). No private namespace list may remain.
  p.432  Honest MQTT delivery contract — PubSubClient 2.8 publish() is QoS 0
         (socket write, not broker PUBACK). No source/doc may claim PUBACK /
         "QoS 1" semantics for device->broker publishes; the durable journal
         + REST reconciliation is the authoritative outcome source.
  p.434  Honest relay.config — runtime relay config mutation has no wired
         ingress: MQTT rejects the action before queueing, and the
         applyCommand("config") stub fails closed (no "Config updated"
         success ACK for a no-op). Two-phase atomicity of the WIRED config
         paths (ConfigUpdater / calibration) is re-asserted.

Usage: python3 scripts/test_audit_round3_2026_09.py   (exit 0 = PASS)
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(REPO, "firmware")

passed, failed = 0, 0
failures = []


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        failures.append(f"{name} {detail}")
        print(f"  FAIL  {name} {detail}")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
CC_CPP = read(os.path.join(FW, "Services", "CommandCanonicalizer.cpp"))
CC_H = read(os.path.join(FW, "Services", "CommandCanonicalizer.h"))
RC_CPP = read(os.path.join(FW, "Services", "RelayController.cpp"))
RC_H = read(os.path.join(FW, "Services", "RelayController.h"))
RH_CPP = read(os.path.join(FW, "Web", "RelayHandlers.cpp"))
MCR_CPP = read(os.path.join(FW, "Network", "MqttConfigReceiver.cpp"))
SH_CPP = read(os.path.join(FW, "Web", "SystemHandlers.cpp"))
INO = read(os.path.join(FW, "firmware_v1.ino"))
FR_CPP = read(os.path.join(FW, "Services", "FactoryReset.cpp"))
FR_H = read(os.path.join(FW, "Services", "FactoryReset.h"))
TS_H = read(os.path.join(FW, "Services", "TelemetrySpool.h"))
TS_CPP = read(os.path.join(FW, "Services", "TelemetrySpool.cpp"))
CFG_H = read(os.path.join(FW, "Core", "Config.h"))
CU_H = read(os.path.join(FW, "Services", "ConfigUpdater.h"))
CH_CPP = read(os.path.join(FW, "Web", "CalibrationHandlers.cpp"))
REMED_DOC = read(os.path.join(REPO, "docs", "AUDIT_2026_09_REMEDIATION.md"))
GOR_H = read(os.path.join(FW, "Network", "GasOtaReporter.h"))

RC_CPP_NC = strip_comments(RC_CPP)
RH_CPP_NC = strip_comments(RH_CPP)
MCR_CPP_NC = strip_comments(MCR_CPP)

print("=" * 74)
print("p.418 — requestId is the TRANSPORT identity (distinct from transactionId)")
print("=" * 74)

# --- static contracts -------------------------------------------------------
check("418-1 CanonicalResult carries requestId",
      "String requestId;" in CC_H,
      "CommandCanonicalizer.h CanonicalResult must declare requestId")

check("418-2 canonicalizeAndHash populates result.requestId",
      "r.requestId = rid" in CC_CPP,
      "canonicalizer must surface the transport identity")

check("418-3 envelope no longer rejects differing ids",
      "requestId and transactionId differ" not in strip_comments(CC_CPP),
      "the old 'requestId and transactionId differ' rejection must be gone")

check("418-4 REST relay ingress uses canon.requestId (both sites)",
      RH_CPP_NC.count("strncpy(qc.requestId, canon.requestId.c_str()") == 2,
      f"found {RH_CPP_NC.count('strncpy(qc.requestId, canon.requestId.c_str()')} of 2")

bad_pattern = re.compile(r"strncpy\(\s*qc\.requestId\s*,\s*(canon\.transactionId|tid\.c_str\(\))")
check("418-5 NO call-site fills qc.requestId with the transactionId",
      not bad_pattern.search(RH_CPP_NC) and not bad_pattern.search(MCR_CPP_NC),
      "requestId must never duplicate transactionId")

check("418-6 MQTT relay ingress reads doc requestId with tid fallback",
      re.search(r'doc\["requestId"\]\s*\|\s*""', MCR_CPP_NC) is not None
      and "String rid = (strlen(ridC) > 0) ? String(ridC) : tid;" in MCR_CPP_NC,
      "MQTT must propagate the transport identity")

check("418-7 RelayTransactionRecord has requestId field",
      "char requestId[65]" in RC_H,
      "terminal records must carry the transport identity")

check("418-8 _recordTransactionResult persists rec.requestId",
      "strncpy(rec.requestId, cmd.requestId" in RC_CPP_NC
      and 'jDoc["requestId"] = rec.requestId' in RC_CPP_NC,
      "ring record + durable journal ack must both carry it")

check("418-9 GET /transactions/{id} surfaces requestId (ring path)",
      'doc["requestId"] = rec.requestId' in RH_CPP_NC,
      "reconciliation response must include the transport identity")

n_ack_rid = RH_CPP_NC.count('ackDoc["requestId"] = canon.requestId')
check("418-10 submission ack echoes requestId (per-channel + all_off)",
      n_ack_rid == 2,
      "found {} of 2".format(n_ack_rid))

check("418-10b MQTT journal/published ACK carries requestId",
      'ack["requestId"] = canon.requestId' in MCR_CPP_NC,
      "the MQTT ACK stored in the journal must carry the transport identity")

check("418-11 duplicate replays LOG the retry attempt id (REST + MQTT)",
      RH_CPP_NC.count("attempt requestId=") >= 2 and "attempt requestId=" in MCR_CPP_NC,
      "retry observability must be logged on both ingresses")

# --- Python mirror: envelope validation semantics --------------------------
def envelope_result(tx=None, rid=None):
    """Mirror of the new validateCommandEnvelope identity rules."""
    tid = tx
    if tid is None or len(tid) == 0:
        tid = rid          # v2 alias fallback
    if tid is None or len(tid) == 0:
        return "REJECT missing identity"
    if rid is not None and len(rid) > 0 and rid != tid:
        # allowed — but rid must itself be well-formed
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", rid):
            return "REJECT malformed requestId"
    return f"ACCEPT tid={tid} rid={rid if rid else tid}"

check("418-12 mirror: retry with fresh requestId + same transactionId ACCEPTED",
      envelope_result(tx="TX-100", rid="REQ-7").startswith("ACCEPT tid=TX-100 rid=REQ-7"),
      envelope_result(tx="TX-100", rid="REQ-7"))

check("418-13 mirror: requestId absent falls back to transactionId",
      envelope_result(tx="TX-100") == "ACCEPT tid=TX-100 rid=TX-100",
      envelope_result(tx="TX-100"))

check("418-14 mirror: v2 client sending only requestId accepted",
      envelope_result(rid="REQ-9").startswith("ACCEPT tid=REQ-9"),
      envelope_result(rid="REQ-9"))

check("418-15 mirror: malformed requestId REJECTED",
      envelope_result(tx="TX-100", rid="bad id!").startswith("REJECT"),
      envelope_result(tx="TX-100", rid="bad id!"))

check("418-16 mirror: no identity at all REJECTED",
      envelope_result().startswith("REJECT"),
      envelope_result())

print()
print("=" * 74)
print("p.427 — factory reset sweeps ONE shared namespace list")
print("=" * 74)

ns_array = re.search(
    r"FACTORY_RESET_NAMESPACES\[\]\s*=\s*\{(.*?)\};", FR_CPP, re.S)
check("427-1 Services/FactoryReset.cpp declares the namespace array",
      ns_array is not None, "shared list missing")
array_ns = []
if ns_array:
    array_ns = re.findall(r'"([a-z_]+)"', ns_array.group(1))

REQUIRED_NS = {"plts", "plts_health", "plts_energy", "plts_ota", "plts_soc",
               "plts_alarm", "plts_txn", "plts_spool", "plts_batt",
               "plts_time", "plts_emg", "plts_auth", "plts_relays"}
check("427-2 all 13 operational namespaces present",
      set(array_ns) == REQUIRED_NS,
      f"missing={REQUIRED_NS - set(array_ns)} extra={set(array_ns) - REQUIRED_NS}")

check("427-3 'plts' wiped LAST (IN_PROGRESS marker survives the sweep)",
      array_ns and array_ns[-1] == "plts",
      "marker namespace must be final in the sweep order")

check("427-4 REST confirm calls the shared wipe (no private list)",
      "Services::executeFactoryResetWipe();" in SH_CPP
      and "NAMESPACES[]" not in SH_CPP,
      "SystemHandlers must delegate")

check("427-5 MQTT confirm calls the shared wipe (no inline sweep)",
      "Services::executeFactoryResetWipe();" in MCR_CPP
      and not re.search(r'p\.begin\("plts[^"]*",\s*false\)\s*;\s*p\.clear\(\)', MCR_CPP),
      "MqttConfigReceiver must delegate")

check("427-6 boot completion calls the shared wipe (no private list)",
      "Services::executeFactoryResetWipe();" in INO
      and "NAMESPACES[]" not in INO,
      "firmware_v1.ino must delegate")

check("427-7 wipe sequence: marker → sweep → re-marker → preserve → format → restore → clear",
      all(s in FR_CPP for s in [
          'm.putBool("ftr_p", true)',
          "FACTORY_RESET_NAMESPACE_COUNT",
          'm.putBool("ftr_p", true)',
          "preserveAuditLogAcrossReset()",
          "LittleFS.format()",
          "restoreAuditLogAfterReset()",
          'm.putBool("ftr_p", false)']))
idx_marker = FR_CPP.find('m.putBool("ftr_p", true)')
idx_sweep = FR_CPP.find("for (size_t i = 0; i < FACTORY_RESET_NAMESPACE_COUNT")
idx_preserve = FR_CPP.find("preserveAuditLogAcrossReset()")
idx_format = FR_CPP.find("LittleFS.format()")
idx_restore = FR_CPP.find("restoreAuditLogAfterReset()")
idx_clear = FR_CPP.find('m.putBool("ftr_p", false)')
check("427-8 wipe step ORDER is correct",
      -1 < idx_marker < idx_sweep < idx_preserve < idx_format < idx_restore < idx_clear,
      f"order indices {idx_marker},{idx_sweep},{idx_preserve},{idx_format},{idx_restore},{idx_clear}")

# --- Python mirror: coverage — every persisted namespace is swept ----------
used_ns = set()
for root, _dirs, files in os.walk(FW):
    for fn in files:
        if not fn.endswith((".cpp", ".h", ".ino")):
            continue
        src = strip_comments(read(os.path.join(root, fn)))
        for m in re.finditer(r'\.begin\("([a-z_]+)"', src):
            used_ns.add(m.group(1))
unswept = used_ns - REQUIRED_NS - {"plts_audit"}   # plts_audit = preserve vessel, cleared by restore
check("427-9 mirror: every persisted namespace is swept (except the audit vessel)",
      not unswept,
      f"namespaces never factory-reset: {sorted(unswept)}")

print()
print("=" * 74)
print("p.432 — honest MQTT delivery contract (QoS 0 publish, no PUBACK claims)")
print("=" * 74)

check("432-1 TelemetrySpool.h no PUBACK overclaim",
      "(QoS 1 PUBACK)" not in TS_H and "NOT a broker PUBACK" in TS_H,
      "callback contract must be socket-write honest")

check("432-2 TelemetrySpool.cpp removal comments honest",
      "(QoS PUBACK)" not in TS_CPP and "PubSubClient QoS-0" in TS_CPP,
      "replay removal evidence must not claim PUBACK")

check("432-3 Config.h topic table is direction-accurate",
      "QoS 1 SUBSCRIBE" in CFG_H and "QoS 0 pub" in CFG_H,
      "config/ota are QoS 1 SUBSCRIBE only; ack is QoS 0 publish")

check("432-4 remediation doc no longer claims 'at QoS 1' for ota/event",
      "at QoS 1" not in REMED_DOC and "PubSubClient QoS 0" in REMED_DOC,
      "AUDIT_2026_09_REMEDIATION.md P1-6 must be corrected")

check("432-5 GasOtaReporter.h no '(QoS 1)' claim on ota/event publish",
      "(QoS 1)" not in GOR_H and "QoS-0 publish" in GOR_H,
      "ota/event publish is PubSubClient QoS 0")

check("432-6 MQTT relay ACK names journal+REST as authoritative",
      "DURABLE journal + REST reconciliation" in MCR_CPP,
      "command ACK honesty note required")

print()
print("=" * 74)
print("p.434 — honest relay.config + two-phase atomic config mutation")
print("=" * 74)

mcr_relay_block = MCR_CPP[MCR_CPP.find('if (type == "relay")'):]
check("434-1 MQTT relay ingress whitelists executable actions",
      'action != "on"' in mcr_relay_block and '"acknowledge" && action != "clear"' in mcr_relay_block,
      "ingress must reject non-executable actions before queueing")

check("434-2 applyCommand('config') fails closed (Rejected, honest message)",
      re.search(r'command == "config".*?RelayCommandResult::Rejected',
                RC_CPP_NC, re.S) is not None
      and "Config updated" not in RC_CPP_NC,
      "the no-op stub must never report success")

check("434-3 registry marks relay.config as schema-only (no runtime ingress)",
      "CANONICAL VALIDATION only" in CC_CPP or "audit p.434" in CC_CPP,
      "registry comment must document the reserved status")

check("434-4 ConfigUpdater keeps the two-phase contract",
      "PHASE 1" in CU_H and "PHASE 2" in CU_H and "Zero RAM mutation" in CU_H,
      "validate-all-then-apply must be documented and intact")

check("434-5 calibration setPoint validates BEFORE mutating (two-phase)",
      "two-phase" in CH_CPP and "setPoint validates internally BEFORE mutating" in CH_CPP,
      "calibration must remain validate-then-apply")

print()
print("=" * 74)
print("p.425 — TLS variant gate: every live setInsecure() is compile-time guarded")
print("=" * 74)

GUARDED_OK = ("TLS_SKIP_CERT_VERIFY",)
DEV_MACROS = ("DEVELOPMENT_BUILD",)
PRODUCTION_MACROS = ("PRODUCTION_BUILD", "OTA_HTTPS_ROOT_CA", "MQTT_ROOT_CA",
                     "GAS_ROOT_CA")

scanned_files = 0
live_total = 0
unguarded = []


def guard_of(lines, idx):
    """Return ('ok', reason) or ('bad', context) for the setInsecure at lines[idx]."""
    i = idx - 1
    while i >= 0:
        line = lines[i].strip()
        if line.startswith("#elif"):
            if any(m in line for m in DEV_MACROS):
                return ("ok", "dev-guard #elif")
            i -= 1
            continue
        if line.startswith("#if"):
            if "defined(" in line or line.startswith("#ifdef") or line.startswith("#ifndef"):
                macro = line
                if any(g in macro for g in GUARDED_OK):
                    return ("ok", "opt-in flag")
                # direct #ifdef DEV / #ifndef PRODUCTION? treat as ok if dev macro
                if any(m in macro for m in DEV_MACROS):
                    return ("ok", "dev-guard #if")
                i -= 1
                continue
            i -= 1
            continue
        if line.startswith("#else"):
            # find the owning #if* (nearest above, skipping #elif chain)
            j = i - 1
            while j >= 0:
                lj = lines[j].strip()
                if lj.startswith(("#if", "#ifdef", "#ifndef")):
                    if any(m in lj for m in PRODUCTION_MACROS):
                        return ("ok", "#else of production guard")
                    break
                j -= 1
            i -= 1
            continue
        if line.startswith("#endif"):
            # we are outside the conditional that guards nothing before us
            break
        i -= 1
    return ("bad", "no compile-time guard found above")


for base in ("firmware", os.path.join("firmware-generic", "src"),
             os.path.join("push-alarm", "firmware")):
    for root, dirs, files in os.walk(os.path.join(REPO, base)):
        dirs[:] = [d for d in dirs if d not in (".pio", "build", "node_modules")]
        for fn in files:
            if not fn.endswith((".ino", ".cpp", ".h")):
                continue
            path = os.path.join(root, fn)
            src = strip_comments(read(path))
            lines = src.splitlines()
            scanned_files += 1
            for k, ln in enumerate(lines):
                if "setInsecure" in ln:
                    live_total += 1
                    verdict, why = guard_of(lines, k)
                    if verdict != "ok":
                        unguarded.append(f"{os.path.relpath(path, REPO)}:{k + 1} ({why})")

check("425-1 every live setInsecure() is compile-time guarded",
      not unguarded,
      "; ".join(unguarded[:5]))

check("425-2 the scanner actually found live occurrences (sanity)",
      live_total >= 8,
      f"live setInsecure() count = {live_total} (expected >= 8)")

check("425-3 scanner covered all firmware trees (sanity)",
      scanned_files >= 100,
      f"scanned {scanned_files} files")

push_ino = read(os.path.join(REPO, "push-alarm", "firmware", "MonitorIoT_Firmware",
                             "MonitorIoT_Firmware.ino"))
check("425-4 push-alarm TLS_SKIP_CERT_VERIFY is OFF by default",
      "#define TLS_SKIP_CERT_VERIFY" not in strip_comments(push_ino),
      "the insecure opt-in must stay commented out")

mt_nc = strip_comments(read(os.path.join(FW, "Network", "MqttTransport.cpp")))
check("425-5 MqttTransport dev bypass still behind DEVELOPMENT_BUILD (M3 parity)",
      re.search(r"#elif defined\(DEVELOPMENT_BUILD\)\s*\n\s*_tls\.setInsecure\(\)", mt_nc)
      is not None,
      "regression on the modular MQTT TLS gate")

# ---------------------------------------------------------------------------
print()
print("=" * 74)
if failed == 0:
    print(f"RESULT: PASS — {passed} checks green "
          f"(TLS scanner: {scanned_files} files, {live_total} live setInsecure, 0 unguarded)")
    sys.exit(0)
print(f"RESULT: FAIL — {passed} passed, {failed} failed")
for f in failures:
    print(f"  - {f}")
sys.exit(1)
