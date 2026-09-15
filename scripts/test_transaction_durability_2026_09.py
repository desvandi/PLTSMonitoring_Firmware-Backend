#!/usr/bin/env python3
"""
test_transaction_durability_2026_09.py — Durable relay transaction
reconciliation tests (audit p.413-415)
================================================================================
Level-2 evidence for the 2026-09 follow-up audit round 2. Static source
contracts plus Python mirrors of the reconciliation logic:

  p.413  EVICTION-SAFE FINAL RESULTS — the 8-entry RAM result ring is a FAST
         CACHE only; every terminal outcome is mirrored into the durable NVS
         TransactionJournal, and GET /api/relays/transactions/{id} falls back
         ring → journal → honest UNKNOWN. A terminal transaction can never
         regress to a non-terminal state (no eternal PENDING).
  p.414  REBOOT DURABILITY — begin() no longer destroys final outcomes: the
         journal is reloaded from NVS at boot, and submission acks carry a
         BOOT MARKER so reconciliation distinguishes "in flight this boot"
         (QUEUED) from "accepted in a previous boot that restarted before the
         executor reached terminal" (UNKNOWN, terminal — not eternal PENDING).
  p.415  JOURNAL = AUTHORITATIVE SOURCE — the journal stores the transaction
         lifecycle (submission ack → terminal ack) with immutable identity
         (commandHash never rewritten); cross-task mutations (networkTask
         storeTransaction vs relayTask updateAck) are serialized by a mutex;
         updateAck is CREATE-ON-DEMAND so an executor verdict that races
         ahead of the ingress journal write is still durable, and a late
         ingress write can never overwrite a terminal ack with a stale
         QUEUED one.

Usage: python3 scripts/test_transaction_durability_2026_09.py   (exit 0 = PASS)
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
RC_CPP = read(os.path.join(FW, "Services", "RelayController.cpp"))
RC_H = read(os.path.join(FW, "Services", "RelayController.h"))
RH_CPP = read(os.path.join(FW, "Web", "RelayHandlers.cpp"))
TJ_CPP = read(os.path.join(FW, "Services", "TransactionJournal.cpp"))
TJ_H = read(os.path.join(FW, "Services", "TransactionJournal.h"))

RC_CPP_NC = strip_comments(RC_CPP)
RH_CPP_NC = strip_comments(RH_CPP)
TJ_CPP_NC = strip_comments(TJ_CPP)
TJ_H_NC = strip_comments(TJ_H)


# ===========================================================================
# p.413 — eviction-safe final results
# ===========================================================================
def test_413_eviction():
    print("\n--- p.413: eviction-safe final results (ring is a cache) ---")

    # GET endpoint must consult the journal after a ring miss
    check("p.413.1 GET falls back to journal (getAckJson in handler)",
          "journal.getAckJson(" in RH_CPP_NC,
          "RelayHandlers.cpp handleGetTransaction must read the journal")

    # The eternal-PENDING defect must be gone
    check("p.413.2 no PENDING state in the transaction endpoint",
          '"state":"PENDING"' not in RH_CPP_NC and
          '\\"state\\":\\"PENDING\\"' not in RH_CPP_NC,
          "terminal/non-terminal must resolve to QUEUED/TERMINAL/UNKNOWN")

    # Executor must mirror every terminal result into the journal
    m = re.search(r"void RelayController::_recordTransactionResult", RC_CPP_NC)
    body = RC_CPP_NC[m.start():m.start() + 3500] if m else ""
    check("p.413.3 _recordTransactionResult persists via journal.updateAck",
          "journal.updateAck(" in body,
          "terminal write must be mirrored into the durable journal")

    # Ring documented as fast cache, journal as authoritative
    check("p.413.4 ring documented as FAST CACHE (not authoritative)",
          "FAST CACHE" in RC_H and "authoritative" in RC_H.lower())

    # All four honest response branches exist
    check("p.413.5 ring hit branch (source=ring)",
          '"source"] = "ring"' in RH_CPP_NC)
    check("p.413.6 journal terminal branch (source=journal)",
          '"source"] = "journal"' in RH_CPP_NC)
    check("p.413.7 in-flight branch (QUEUED + result QUEUED)",
          re.search(r'state.*QUEUED.*result.*QUEUED|result.*QUEUED.*state.*QUEUED',
                    RH_CPP_NC, re.S) is not None)
    check("p.413.8 unknown-id branch (honest UNKNOWN, not PENDING)",
          "no durable record" in RH_CPP_NC)

    # 2N+1 > ring capacity transactions must still answer for the oldest
    # ones that are within the journal window (Python mirror below proves
    # behavior; here assert the capacity constants stay as documented)
    check("p.413.9 RESULT_RING_SIZE == 8 (documented cache depth)",
          "RESULT_RING_SIZE = 8" in RC_H)
    check("p.413.10 JOURNAL_SIZE == 16 (durable window)",
          "JOURNAL_SIZE = Core::JOURNAL_SIZE" in TJ_H and
          "JOURNAL_SIZE = 16" in read(os.path.join(FW, "Core", "Config.h")))


# ===========================================================================
# p.414 — reboot durability + boot marker
# ===========================================================================
def test_414_reboot():
    print("\n--- p.414: reboot durability + boot marker ---")

    # begin() reloads the journal from NVS (survives reboot)
    check("p.414.1 journal reloads from NVS in begin()",
          "_loadFromNVS();" in TJ_CPP_NC.split("TransactionJournal::begin")[-1]
          and "getUInt" in TJ_CPP_NC.split("TransactionJournal::begin")[-1])

    # boot counter is persisted and monotonic
    check("p.414.2 boot counter persisted (putUInt)",
          'putUInt("boot"' in TJ_CPP_NC and 'getUInt("boot"' in TJ_CPP_NC)

    # Both relay submission ack builders stamp the boot marker
    check("p.414.3 submission acks stamped with boot (both handlers)",
          RH_CPP_NC.count('ackDoc["boot"] = Services::journal.bootCount()') == 2,
          f"found {RH_CPP_NC.count(chr(97)+chr(99)+chr(107)+'')} — expect 2")

    # Lost-at-reboot is detected via the boot marker and reported honestly
    check("p.414.4 lost-at-reboot branch (previous boot → UNKNOWN)",
          "previous boot" in RH_CPP_NC and
          "bootCount()" in RH_CPP_NC)

    # begin()'s ring wipe must be commented as cache-only
    check("p.414.5 begin() ring wipe documented as cache clear",
          "result-ring CACHE" in RC_CPP or "ring wipe below only drops" in RC_CPP)


# ===========================================================================
# p.415 — journal as authoritative lifecycle store
# ===========================================================================
def test_415_authoritative():
    print("\n--- p.415: journal = authoritative lifecycle store ---")

    # Identity is immutable: updateAck verifies the hash
    check("p.415.1 updateAck verifies identity (hash mismatch → refuse)",
          "_hashes[idx] != commandHash" in TJ_CPP_NC)

    # Create-on-demand: terminal verdict racing ahead of ingress is durable
    check("p.415.2 updateAck create-on-demand (_storeNewEntryLocked)",
          TJ_CPP_NC.split("TransactionJournal::updateAck")[-1].count(
              "_storeNewEntryLocked(") >= 1)

    # Ingress duplicate branch must NOT overwrite an existing (terminal) ack
    st = TJ_CPP_NC.split("TransactionJournal::storeTransaction")[-1]
    dup_branch = st.split("int idx = _findInJournal")[0]
    check("p.415.3 storeTransaction duplicate branch leaves ack untouched",
          "_acks" not in st.split("_storeNewEntryLocked")[0].split(
              "if (idx >= 0)")[1].split("return ok;")[0],
          "duplicate path must return without ack mutation")

    # Cross-task serialization
    # [ROUND-10 UPDATE 2026-09-15, p.473] The assertion previously required
    # the void-call `_lock();` statement in both mutators. Round-10 made the
    # acquisition FAIL-CLOSED (bool _lock(); refusal + atomic accounting on
    # unavailability — see test_audit_round10_2026_09.py B1/B5), so the
    # statement became `if (!_lock()) return false;`. Serialization is now
    # STRICTLY STRONGER: the mutator not only locks, it REFUSES to run when
    # the lock cannot be acquired (the old shape silently proceeded).
    check("p.415.4 mutations serialized (mutex in storeTransaction + updateAck)",
          "if (!_lock()) return false;" in st and
          TJ_CPP_NC.split("TransactionJournal::updateAck")[-1].count("if (!_lock()) return false;") >= 1 and
          "xSemaphoreCreateMutex" in TJ_CPP_NC)

    # Journal header documents the authoritative resolution chain
    check("p.415.5 journal documented as AUTHORITATIVE final-outcome store",
          "AUTHORITATIVE" in TJ_H and "ring" in TJ_H.lower())

    # Rollback pattern: RAM mutated only after NVS commit
    check("p.415.6 updateAck rolls back on NVS failure",
          "_acks[idx] = prevAck;" in
          TJ_CPP_NC.split("TransactionJournal::updateAck")[-1])


# ===========================================================================
# Python mirror — reconciliation state machine (behavioral proof)
# ===========================================================================
class JournalMirror:
    """Mirror of the C++ design: 16-slot durable journal + 8-slot RAM ring."""

    JOURNAL_SIZE = 16
    RING_SIZE = 8

    def __init__(self):
        self.slots = {}          # txnId -> ack dict (ordered by insertion)
        self.boot = 0
        self.ring = {}            # txnId -> terminal ack (fast cache)
        self.ring_order = []

    def begin(self):
        """Reboot: ring wiped, journal reloaded from NVS, boot++."""
        self.boot += 1
        self.ring = {}
        self.ring_order = []

    def ingress(self, tid, hash_):
        """REST submission: queueCommand then storeTransaction."""
        if tid in self.slots:
            return False  # duplicate — ack untouched
        if len(self.slots) >= self.JOURNAL_SIZE:
            self.slots.pop(next(iter(self.slots)))  # eviction
        self.slots[tid] = {
            "transactionId": tid, "state": "QUEUED", "result": "QUEUED",
            "boot": self.boot,
        }
        return True

    def terminal(self, tid, hash_, result):
        """Executor verdict: ring write + journal mirror (create-on-demand)."""
        ack = {"transactionId": tid, "state": "TERMINAL", "result": result,
               "boot": self.boot}
        # ring (fast cache, bounded 8)
        if tid in self.ring:
            self.ring_order.remove(tid)
        self.ring[tid] = ack
        self.ring_order.append(tid)
        if len(self.ring_order) > self.RING_SIZE:
            self.ring.pop(self.ring_order.pop(0))
        # journal (authoritative)
        if tid in self.slots:
            self.slots[tid] = ack
        else:
            if len(self.slots) >= self.JOURNAL_SIZE:
                self.slots.pop(next(iter(self.slots)))
            self.slots[tid] = ack

    def get(self, tid):
        """GET /api/relays/transactions/{tid} — the resolution chain."""
        if tid in self.ring:
            return dict(self.ring[tid], source="ring")
        ack = self.slots.get(tid)
        if ack is not None:
            if ack["state"] == "TERMINAL":
                return dict(ack, source="journal")
            if ack.get("boot") == self.boot:
                return {"transactionId": tid, "state": "QUEUED"}
            return {"transactionId": tid, "state": "TERMINAL",
                    "result": "UNKNOWN",
                    "message": "accepted in a previous boot ..."}
        return {"transactionId": tid, "state": "TERMINAL", "result": "UNKNOWN",
                "message": "no durable record ... (never accepted, or "
                           "evicted beyond the 16-entry journal retention)"}


def test_mirror():
    print("\n--- Python mirror: behavioral proof of the resolution chain ---")
    j = JournalMirror()
    j.begin()  # boot 1

    # 20 transactions: TX-00 .. TX-19 all EXECUTED
    for i in range(20):
        tid = f"TX-{i:02d}"
        j.ingress(tid, f"hash{i}")
        j.terminal(tid, f"hash{i}", "EXECUTED")

    check("mirror.1 TX-19 answers from ring (latest)",
          j.get("TX-19")["source"] == "ring" and
          j.get("TX-19")["result"] == "EXECUTED")

    check("mirror.2 TX-11 answers from journal after ring eviction "
          "(>8 newer transactions)", j.get("TX-11")["source"] == "journal" and
          j.get("TX-11")["result"] == "EXECUTED")

    check("mirror.3 TX-03 (beyond 16-entry journal retention) → honest UNKNOWN",
          j.get("TX-03")["result"] == "UNKNOWN" and
          j.get("TX-03")["state"] == "TERMINAL")

    # Reboot: ring wiped, journal reloaded, boot 2
    j.begin()
    check("mirror.4 after reboot, TX-10 still TERMINAL from journal (p.414)",
          j.get("TX-10")["source"] == "journal" and
          j.get("TX-10")["result"] == "EXECUTED")

    # In-flight command in the CURRENT boot → QUEUED
    j.ingress("TX-LIVE", "hashlive")
    check("mirror.5 in-flight this boot → QUEUED (honest)",
          j.get("TX-LIVE")["state"] == "QUEUED")

    # Lost at reboot: accepted in boot 1, never terminal, now boot 2
    j.begin()  # boot 3
    check("mirror.6 accepted in previous boot without terminal → UNKNOWN",
          j.get("TX-LIVE")["result"] == "UNKNOWN" and
          j.get("TX-LIVE")["state"] == "TERMINAL")

    # Race: terminal races ahead of ingress journal write
    j2 = JournalMirror()
    j2.begin()
    j2.terminal("TX-RACE", "hr", "EXECUTED")   # executor first
    j2.ingress("TX-RACE", "hr")                # ingress late
    r = j2.get("TX-RACE")
    check("mirror.7 terminal-before-ingress race → durable TERMINAL "
          "(create-on-demand)", r["result"] == "EXECUTED" and
          r["source"] in ("ring", "journal"))

    # After eviction from ring + reboot, the raced terminal survives
    for i in range(10):
        j2.ingress(f"TX-N{i}", f"hn{i}")
        j2.terminal(f"TX-N{i}", f"hn{i}", "EXECUTED")
    j2.begin()
    r = j2.get("TX-RACE")
    check("mirror.8 raced terminal survives reboot + ring eviction",
          r["result"] == "EXECUTED" and r["source"] == "journal")

    # UNKNOWN is reachable for a nonsense id (never an eternal PENDING)
    check("mirror.9 never-accepted id → TERMINAL/UNKNOWN",
          j2.get("TX-NOPE")["result"] == "UNKNOWN" and
          j2.get("TX-NOPE")["state"] == "TERMINAL")


def main():
    print("=" * 72)
    print("test_transaction_durability_2026_09.py — audit p.413/414/415")
    print("=" * 72)
    test_413_eviction()
    test_414_reboot()
    test_415_authoritative()
    test_mirror()
    print("\n" + "=" * 72)
    print(f"RESULT: {passed} passed, {failed} FAILED")
    if failures:
        for f in failures:
            print(f"  ✗ {f}")
    print("=" * 72)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
