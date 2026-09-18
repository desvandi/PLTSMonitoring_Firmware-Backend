#!/usr/bin/env python3
"""
test_gate3_gate4_mqtt_2026_09.py — MQTT device-topic binding (S1-02) +
OTA transaction durability ordering (S1-03) contract tests.

Structural + logic mirrors (repo discipline): each check asserts the
POST-remediation source shape on the actual files, plus a logic mirror of
the OTA ordering state machine (decide -> RESERVE -> beginDownload ->
updateAck) including the failure-injection branches.

Run: python3 scripts/test_gate3_gate4_mqtt_2026_09.py   (exit 0 = PASS)
"""
import re
import sys

PASS = 0
FAIL = 0
FAILURES = []


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        FAILURES.append(name)
        print(f"  FAIL  {name}  {detail}")


ROOT = "firmware/"


def read(p):
    with open(ROOT + p, encoding="utf-8", errors="replace") as f:
        return f.read()


ota_cpp = read("Network/MqttOtaHandler.cpp")
cfg_cpp = read("Network/MqttConfigReceiver.cpp")
transport_h = read("Network/MqttTransport.h")
ino = read("firmware_v1.ino")

print("========================================================================")
print("GATE-3 — MQTT DEVICE-TOPIC AUTHORIZATION (audit S1-02 / F4-09 / P3-S1-04)")
print("========================================================================")

# --- Router: exact topic match before dispatch -------------------------------
check("router: exact-match dispatch against canonical device topics",
      "getDeviceTopic(\"config\")" in ino and "getDeviceTopic(\"ota\")" in ino and
      "t.equals(expectedConfig)" in ino and "t.equals(expectedOta)" in ino)
check("router: suffix-only dispatch REMOVED (no lastIndexOf routing)",
      "int lastSlash = t.lastIndexOf('/');" not in ino.split("Install message router")[1].split("Subscribe the config")[0])
check("router: foreign topic security log + counter",
      "MQTT SECURITY: topic rejected" in ino and "countForeignTopic()" in ino)

# --- Handler-level exact binding (second layer) ------------------------------
check("MqttConfigReceiver: (void)topic removed",
      "(void)topic;" not in cfg_cpp)
check("MqttConfigReceiver: exact expected-topic guard",
      'getDeviceTopic("config")' in cfg_cpp and "FOREIGN topic rejected" in cfg_cpp)
check("MqttConfigReceiver: foreign topic -> no parse, no mutation, no ACK (early return)",
      cfg_cpp.find("FOREIGN topic rejected") < cfg_cpp.find("Deserialize"))

check("MqttOtaHandler: (void)topic removed",
      "(void)topic;" not in ota_cpp)
check("MqttOtaHandler: exact expected-topic guard",
      'getDeviceTopic("ota")' in ota_cpp and "FOREIGN topic rejected" in ota_cpp)
check("MqttOtaHandler: foreign topic -> early return before deserialize",
      ota_cpp.find("FOREIGN topic rejected") < ota_cpp.find("Deserialize"))

check("MqttTransport: foreign-topic counter exposed",
      "foreignTopicCount" in transport_h)

print("\n========================================================================")
print("GATE-4 — OTA TRANSACTION DURABILITY ORDERING (audit S1-03 / F4-06)")
print("========================================================================")

# --- Source-shape assertions --------------------------------------------------
# Search for ACTUAL CALLS, not comment mentions:
res_call = ota_cpp.find("Services::journal.storeTransaction(")
dl_call = ota_cpp.find("Services::ota.beginDownload(")
term_call = ota_cpp.find("Services::journal.updateAck(canon.transactionId")
check("ordering: durable reservation (storeTransaction) BEFORE beginDownload",
      0 <= res_call < dl_call,
      f"res_call={res_call} dl_call={dl_call}")
check("ordering: terminal updateAck AFTER beginDownload",
      0 <= dl_call < term_call,
      f"dl_call={dl_call} term_call={term_call}")
check("reservation: return value CHECKED (DURABILITY_FAILURE path)",
      "DURABILITY_FAILURE" in ota_cpp and
      "could not be made durable" in ota_cpp)
check("reservation: OTA NOT started on reservation failure",
      re.search(r"if \(!Services::journal\.storeTransaction\([\s\S]{0,700}return;", ota_cpp) is not None)
check("reservation: RESERVED phase recorded durably",
      '"RESERVED"' in ota_cpp)
check("terminal: updateAck return value observed (log on failure)",
      re.search(r"if \(!Services::journal\.updateAck", ota_cpp) is not None)
check("terminal: journal failure no longer silently swallowed",
      "terminal journal update FAILED" in ota_cpp)

# --- Logic mirror: the ordering state machine incl. failure injection --------
print("\n[logic mirror] OTA ordering state machine (with journal failure injection)")


class JournalMirror:
    """Mirrors the ORDERING contract: decide / reserve / updateAck."""

    def __init__(self):
        self.records = {}          # tid -> (hash, ack_json)
        self.fail_reserve = False
        self.fail_update = False

    def decide(self, tid, chash):
        if tid in self.records:
            h, _ = self.records[tid]
            if h != chash:
                return "CONFLICT"
            return "DUPLICATE"
        return "NEW"

    def reserve(self, tid, chash, ack):
        if self.fail_reserve:
            return False
        self.records[tid] = (chash, ack)
        return True

    def update_ack(self, tid, chash, ack):
        if tid not in self.records:
            return False
        h, _ = self.records[tid]
        if h != chash:
            return False
        if self.fail_update:
            return False          # reserved record stands
        self.records[tid] = (chash, ack)
        return True


class OtaManagerMirror:
    def __init__(self):
        self.jobs_started = 0

    def begin_download(self):
        self.jobs_started += 1
        return True


def run_ota_command(journal, ota, tid, chash):
    """Mirror of the NEW MqttOtaHandler flow. Returns (code, started)."""
    decision = journal.decide(tid, chash)
    if decision == "UNAVAILABLE":
        return "JOURNAL_UNAVAILABLE", False
    if decision == "DUPLICATE":
        return "DUPLICATE", False
    if decision == "CONFLICT":
        return "CONFLICT", False
    # GATE-4: durable reservation FIRST — failure => NO side effect.
    if not journal.reserve(tid, chash, {"phase": "RESERVED"}):
        return "DURABILITY_FAILURE", False
    # Only after durability: the side effect.
    ok = ota.begin_download()
    journal.update_ack(tid, chash, {"phase": "ACCEPTED"})
    return ("ACCEPTED" if ok else "REJECTED"), ok


j, m = JournalMirror(), OtaManagerMirror()
code, started = run_ota_command(j, m, "TX-1", "H1")
check("T1 normal flow: ACCEPTED, one job started", code == "ACCEPTED" and started and m.jobs_started == 1)

code, started = run_ota_command(j, m, "TX-1", "H1")
check("T2 idempotent retry: DUPLICATE, no second job",
      code == "DUPLICATE" and m.jobs_started == 1)

code, started = run_ota_command(j, m, "TX-1", "H2")
check("T3 same tid different hash: CONFLICT, no job",
      code == "CONFLICT" and m.jobs_started == 1)

j2, m2 = JournalMirror(), OtaManagerMirror()
j2.fail_reserve = True
code, started = run_ota_command(j2, m2, "TX-9", "H9")
check("T4 reservation failure: DURABILITY_FAILURE, OTA NOT started",
      code == "DURABILITY_FAILURE" and not started and m2.jobs_started == 0)
check("T4-b no durable record left behind", "TX-9" not in j2.records)

j3, m3 = JournalMirror(), OtaManagerMirror()
j3.fail_update = True
code, started = run_ota_command(j3, m3, "TX-A", "HA")
check("T5 terminal update failure: job still settles, RESERVED record stands",
      code == "ACCEPTED" and m3.jobs_started == 1 and
      j3.records["TX-A"][1]["phase"] == "RESERVED")
code, _ = run_ota_command(j3, m3, "TX-A", "HA")
check("T5-b retry after failed terminal: DUPLICATE (idempotent, no re-flash)",
      code == "DUPLICATE" and m3.jobs_started == 1)

print()
print("========================================================================")
print(f"RESULT: {PASS} passed, {FAIL} failed")
print("========================================================================")
if FAILURES:
    for f in FAILURES:
        print("  x " + f)
    sys.exit(1)
