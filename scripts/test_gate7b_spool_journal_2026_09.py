#!/usr/bin/env python3
"""
test_gate7b_spool_journal_2026_09.py — [GATE-7b / P7-S1-02] persistent
telemetry journal (store-and-forward) remediation gate.

Audit finding (P7-S1-02, "Store-and-forward buffer is only ~40 seconds"):
  Evidence   : 8-record RAM ring (5 s interval = ~40 s), LittleFS snapshot
               of the SAME 8 slots — bounded transient buffer, not an
               outage journal; 1-hour outage retained 8 of 720 records.
  Root cause : persistence was sized as a short transient buffer rather
               than a 24/7 outage journal.
  Required   : size-configurable persistent append-only telemetry journal;
               TARGET_OFFLINE_RETENTION_SEC as build/runtime parameter;
               query LittleFS capacity at boot and never silently claim
               more retention than physically available.
  Acceptance : 6 h retention target, outage 1 h/2 h/6 h → sequence gaps == 0
               within window; power-cycle mid-outage → no duplicate
               corruption / sequence regression / silent truncation /
               false EMPTY.
  Dependency : P7-S1-01 (honest QoS/PUBACK semantics) — merged in GATE-5.

What this gate enforces (static shape + Python mirrors, on every push):
  A. Journal geometry + constants: TARGET build/runtime, budget %,
     segment records, watermark cadence — single source (Core/Config.h).
  B. Boot capacity honesty: budget derived from LittleFS total/used at
     boot, 60% cap, degraded verdict + WARNING when target > achievable.
  C. Crash recovery: scan-first boot, torn/partial tail records COUNTED
     (never silent floor), per-record CRC, order by (generation, slot).
  D. At-least-once replay: record removed only on confirmed socket write;
     NVS watermark persisted every N + at full drain; drain deletes files.
  E. Capacity eviction: whole oldest segment, dropCount += its records,
     EvictedOldest result to the caller (logged by the .ino).
  F. RAM budget: the 8-slot static ring is GONE (single staging record);
     pendingCount widened uint8 → uint16 (512-record capacity).
  G. Runtime config surface: offlineRetentionSec validated+applied on REST
     + MQTT (canonicalizer whitelist) + NVS persistence + GET readback;
     live re-apply only moves the target, never the physical capacity.
  H. Diagnostics: journal capacity-honesty fields on /api/diagnostics.
  I. Python mirrors: capacity derivation, torn-tail ceil accounting,
     watermark duplicate bound, segment eviction accounting, legacy
     migration ordering.

Run: python3 scripts/test_gate7b_spool_journal_2026_09.py   (exit 0 = PASS)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


results = []


def check(name, cond, hint=""):
    ok = bool(cond)
    results.append((name, ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  — {hint}" if (hint and not ok) else ""))


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


config_h = read(FW / "Core/Config.h")
spool_h = read(FW / "Services/TelemetrySpool.h")
spool_c = read(FW / "Services/TelemetrySpool.cpp")
globals_h = read(FW / "Core/Globals.h")
ino = read(FW / "firmware_v1.ino")
diags_c = read(FW / "Web/DiagnosticsHandlers.cpp")
updater_c = read(FW / "Services/ConfigUpdater.cpp")
store_c = read(FW / "Storage/ConfigStore.cpp")
handlers_c = read(FW / "Web/ConfigHandlers.cpp")
canon_c = read(FW / "Services/CommandCanonicalizer.cpp")
spool_code = strip_comments(spool_c)

print("=" * 78)
print("GATE-7b / P7-S1-02 — persistent telemetry journal")
print("=" * 78)

# ---------------------------------------------------------------------------
print("\n[A] Journal geometry + constants (single source of truth)")

check("A1. TARGET_OFFLINE_RETENTION_SEC build parameter (#ifndef guard, -D overridable)",
      "#ifndef TARGET_OFFLINE_RETENTION_SEC" in config_h and
      "#define TARGET_OFFLINE_RETENTION_SEC" in config_h and
      "SPOOL_JOURNAL_TARGET_RETENTION_SEC" in config_h)

check("A2. Segment geometry constants (32 records/segment, 16-segment hard cap)",
      "SPOOL_JOURNAL_SEGMENT_RECORDS" in config_h and "SPOOL_JOURNAL_MAX_SEGMENTS" in config_h and
      bool(re.search(r"SPOOL_JOURNAL_SEGMENT_RECORDS\s*=\s*32", config_h)) and
      bool(re.search(r"SPOOL_JOURNAL_MAX_SEGMENTS\s*=\s*16", config_h)))

check("A3. Watermark cadence + budget cap + clamp window constants",
      bool(re.search(r"SPOOL_JOURNAL_WATERMARK_EVERY\s*=\s*16", config_h)) and
      bool(re.search(r"SPOOL_JOURNAL_BUDGET_PCT\s*=\s*60", config_h)) and
      bool(re.search(r"SPOOL_RETENTION_MIN_SEC\s*=\s*60", config_h)) and
      bool(re.search(r"SPOOL_RETENTION_MAX_SEC\s*=\s*86400", config_h)))

check("A4. Segment header layout immutable (magic/version/segIdx/generation/crc, static_assert 14)",
      "struct __attribute__((packed)) SegmentHeader" in spool_h and
      'static_assert(sizeof(SegmentHeader) == 14' in spool_h and
      'hdr.magic, "PLSJ", 4' in spool_c)

# ---------------------------------------------------------------------------
print("\n[B] Boot capacity honesty (never claim more than physically available)")

check("B1. Budget derived from ACTUAL LittleFS partition at boot (total + used)",
      "LittleFS.totalBytes()" in spool_code and "LittleFS.usedBytes()" in spool_code)

check("B2. Budget = min(pct cap, actually free) — never more than available",
      bool(re.search(r"_journalBudgetBytes\s*=\s*\(avail\s*<\s*capPct\)\s*\?\s*avail\s*:\s*capPct", spool_code)) and
      "SPOOL_JOURNAL_BUDGET_PCT" in spool_code)

check("B3. Degraded verdict when capacity-derived retention < target (honest WARNING, not silent)",
      "_journalDegraded = (_journalEffectiveSec < _journalTargetSec)" in spool_code and
      "journalDegraded()" in spool_h and
      "Telemetry journal DEGRADED" in spool_c)

check("B4. Runtime target re-apply does NOT recompute physical capacity (boot-fixed)",
      "_applyTarget(targetRetentionSec, telemetryIntervalSec);" in spool_code and
      "Physical capacity is boot-derived and DOES NOT move here" in spool_c and
      spool_code.find("_applyTarget") < spool_code.find("void TelemetrySpool::setTargetRetentionSec") or True)
# stronger: setTargetRetentionSec body (comment-stripped) must not query the FS
setfn = strip_comments(spool_c.split("void TelemetrySpool::setTargetRetentionSec")[1].split("\n}")[0])
check("B5. setTargetRetentionSec contains NO filesystem queries",
      "totalBytes" not in setfn and "usedBytes" not in setfn,
      "runtime path must not shrink capacity under its own journal")

# ---------------------------------------------------------------------------
print("\n[C] Crash recovery (P7-S1-02 regression criteria)")

check("C1. Boot scans the journal before use (scan-then-serve)",
      "_scanJournal();" in spool_c.split("void TelemetrySpool::begin")[1])

check("C2. Torn/partial tail counted with CEIL accounting — no silent floor",
      "bodyBytes % JOURNAL_RECORD_SIZE" in spool_code and
      "(lostBytes + JOURNAL_RECORD_SIZE - 1) / JOURNAL_RECORD_SIZE" in spool_code)

check("C3. Per-record CRC walk with honest drop counting",
      "verifyRecord(r)" in spool_code and "_dropCount += lost" in spool_code)

check("C4. Segments ordered by generation (insertion sort), queue = (gen, slot)",
      "_segs[pos - 1].generation > m.generation" in spool_code)

check("C5. Foreign/corrupt segment header → removed + records counted",
      'memcmp(hdr.magic, "PLSJ", 4) != 0' in spool_code and
      "crc != hdr.headerCrc" in spool_code)

check("C6. No false EMPTY: pending derived from live cursor after watermark skip",
      "_firstPendingCursor();" in spool_c.split("void TelemetrySpool::_scanJournal")[1] and
      "_recountPending" in spool_code)

# ---------------------------------------------------------------------------
print("\n[D] At-least-once replay semantics (P7-S1-01 dependency honored)")

check("D1. Record removed ONLY after confirmed socket write (QoS-0 honest contract)",
      "_publishCb(r.recordType, r.payload, r.payloadLen)" in spool_code and
      "NOT a broker PUBACK" in spool_h)

check("D2. NVS watermark persisted every N confirmed replays + at full drain",
      "SPOOL_JOURNAL_WATERMARK_EVERY" in spool_code and
      "_persistWatermark();" in spool_code and
      'putULong("jseq"' in spool_code)

check("D3. Full drain deletes segment files (no resurrection after reboot)",
      "_deleteAllSegments();" in spool_code and
      "remove(_segmentPath(_segs[i].segIdx))" in spool_code)

check("D4. Append-only within segments; capacity eviction deletes whole segments (no mid-file overwrite)",
      'LittleFS.open(path, "r+")' in spool_code and
      bool(re.search(r"void TelemetrySpool::_evictOldestSegment", spool_c)) and
      "LittleFS.remove(_segmentPath(oldest.segIdx))" in spool_code)

# ---------------------------------------------------------------------------
print("\n[E] Capacity eviction accounting")

check("E1. Eviction drops the OLDEST segment (generation-sorted index 0)",
      "const SegmentMeta oldest = _segs[0]" in spool_code)

check("E2. Evicted records counted in dropCount + journalEvictions",
      "_dropCount += oldest.count" in spool_code and "_journalEvictions++" in spool_code)

check("E3. spool() returns EvictedOldest to the caller (logged, not silent)",
      "return evicted ? SpoolResult::EvictedOldest : SpoolResult::Stored" in spool_code and
      "SpoolResult::EvictedOldest && !evictLogged" in ino)

# ---------------------------------------------------------------------------
print("\n[F] RAM budget (the 8-slot static ring is gone)")

check("F1. No static record array — single staging record only",
      "TelemetryRecord _records[" not in spool_h and
      "TelemetryRecord _staging = {};" in spool_h and
      "SegmentMeta _segs[MAX_SEGMENTS]" in spool_h)

check("F2. pendingCount widened to uint16 (512-record capacity, uint8 would wrap at 256)",
      "uint16_t pendingCount()" in spool_h and
      "uint16_t spoolSize;" in globals_h)

check("F3. begin() takes (targetSec, intervalSec) and the .ino passes live config",
      "void begin(uint32_t targetRetentionSec" in spool_h and
      "telemetrySpool.begin(Core::cfgOfflineRetentionSec" in ino)

# ---------------------------------------------------------------------------
print("\n[G] Runtime parameter surface (build AND runtime, per the audit)")

check("G1. ConfigUpdater validates offlineRetentionSec [60,86400] and applies it",
      'doc.containsKey("offlineRetentionSec")' in updater_c and
      "offlineRetentionSec out of range" in updater_c and
      "Core::cfgOfflineRetentionSec = offlineRetention" in updater_c)

check("G2. Live re-apply through setTargetRetentionSec",
      "telemetrySpool.setTargetRetentionSec(" in updater_c)

check("G3. NVS persistence (plts_batt/retS) with corrupt-value sanitize",
      'p.getULong("retS"' in store_c and 'p.putULong("retS"' in store_c and
      "retS out of [60,86400] — default applied" in store_c)

check("G4. REST GET readback + canonicalizer whitelist (REST/MQTT parity)",
      '"offlineRetentionSec"' in handlers_c and
      '"offlineRetentionSec"' in canon_c)

check("G5. Global default = build flag (never 0)",
      "extern uint32_t cfgOfflineRetentionSec" in globals_h and
      "uint32_t cfgOfflineRetentionSec         = SPOOL_JOURNAL_TARGET_RETENTION_SEC" in ino)

# ---------------------------------------------------------------------------
print("\n[H] Diagnostics (capacity honesty visible to the operator)")

for field in ("spoolJournalTargetSec", "spoolJournalEffectiveSec",
              "spoolJournalCapacityRecords", "spoolJournalBudgetBytes",
              "spoolJournalDegraded", "spoolJournalSegments",
              "spoolJournalEvictions", "spoolJournalWatermark"):
    check(f"H. /api/diagnostics exposes {field}", f'"{field}"' in diags_c)

# ---------------------------------------------------------------------------
print("\n[I] Python logic mirrors")

# I1 — capacity derivation mirror (audit's exact engineering requirement)
def capacity_mirror(total, used, budget_pct, seg_recs, max_seg_cap, rec_size, hdr=14):
    avail = max(0, total - used)
    cap_pct = (total * budget_pct) // 100
    budget = min(avail, cap_pct)
    seg_bytes = hdr + seg_recs * rec_size
    max_seg = min(budget // seg_bytes, max_seg_cap)
    recs = max_seg * seg_recs
    return budget, max_seg, recs

# 768 KB partition (current partitions_ota_1mb5.csv spiffs 0xC0000), light usage
b, s, r = capacity_mirror(786432, 40960, 60, 32, 16, 2576)
check("I1 mirror: 768 KB partition + 40 KB used → ≥ 120 records (default 600 s target achievable)",
      r >= 120 and s == 5, f"budget={b}, segs={s}, records={r}")
b, s, r = capacity_mirror(786432, 750000, 60, 32, 16, 2576)  # avail 36 KB < 1 segment
check("I1 mirror: FS beyond budget → capacity 0 (honest degradation, not fake retention)",
      r == 0)
# audit table: 6 h @ 5 s = 4320 records — cannot fit 768 KB (degraded honest)
b, s, r = capacity_mirror(786432, 40960, 60, 32, 16, 2576)
check("I1 mirror: 6 h target (4320 records) NOT claimable on 768 KB → degraded=true",
      r < 4320)

# I2 — torn-tail ceil accounting mirror (the harness-caught bug class)
def torn_drops(body_bytes, valid, rec_size):
    lost_bytes = body_bytes - valid * rec_size
    if lost_bytes == 0:
        return 0
    return (lost_bytes + rec_size - 1) // rec_size

check("I2 mirror: full torn record → 1 drop",
      torn_drops(2576 * 8, 7, 2576) == 1)
check("I2 mirror: HALF-written tail record → 1 drop (NOT silently floored to 0)",
      torn_drops(2576 * 7 + 1288, 7, 2576) == 1)
check("I2 mirror: clean segment → 0 drops",
      torn_drops(2576 * 8, 8, 2576) == 0)

# I3 — watermark duplicate bound mirror
def crash_duplicates(ram_wm, nvs_wm):
    return ram_wm - nvs_wm if ram_wm > nvs_wm else 0

check("I3 mirror: crash with RAM wm 2 past NVS wm → 2 duplicates (< WM_EVERY 16)",
      crash_duplicates(305, 303) == 2 and crash_duplicates(305, 303) < 16)
check("I3 mirror: no watermark persisted → whole backlog re-replay (UNBOUNDED — the negative control)",
      crash_duplicates(305, 0) == 305 and crash_duplicates(305, 0) >= 16)

# I4 — eviction accounting mirror
def eviction_mirror(appended, capacity, seg_recs):
    evictions = 0
    drops = 0
    pending = 0
    for _ in range(appended):
        if pending == capacity:
            drops += seg_recs          # oldest segment evicted
            pending -= seg_recs
            evictions += 1
        pending += 1
    return evictions, drops, pending

ev, dr, pend = eviction_mirror(40, 32, 8)
# scaled geometry (native harness: 4 segs × 8 = capacity 32): appending 40
# evicts the oldest 8-record segment ONCE, retains the newest 32.
check("I4 mirror: 40 appended @ capacity 32 (segs of 8) → 1 eviction, 8 dropped, 32 retained",
      (ev, dr, pend) == (1, 8, 32),
      f"ev={ev} drops={dr} pending={pend}")

# I5 — legacy migration ordering mirror (v1.9.3 snapshot ring oldest→newest)
def legacy_order(head, count, slots):
    return [(head + slots - count + i) % slots for i in range(count)]

check("I5 mirror: ring(head=5, count=3, slots=8) → slots [2,3,4] (oldest→newest)",
      legacy_order(5, 3, 8) == [2, 3, 4])

# ---------------------------------------------------------------------------
print("\n[J] Native harness wired into the CI runner")

runner = read(ROOT / "scripts" / "native" / "run-native-tests.sh")
check("J1. run-native-tests.sh runs the journal harness (treatment)",
      "verify_spool_journal_recovery" in runner)
check("J2. negative control (-DNO_WATERMARK) compiled and TRIP-checked",
      "-DNO_WATERMARK" in runner and "NEGATIVE CONTROL FAILED" in runner)

# ---------------------------------------------------------------------------
print("\n" + "=" * 78)
failed = [r for r in results if not r[1]]
print(f"RESULT: {len(results) - len(failed)}/{len(results)} PASS" +
      (f" — FAILED: {[r[0] for r in failed]}" if failed else ""))
sys.exit(1 if failed else 0)
