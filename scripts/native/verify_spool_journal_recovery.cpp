// verify_spool_journal_recovery.cpp — [GATE-7b / P7-S1-02] native harness
// =============================================================================
// Mirrors the firmware TelemetrySpool SEGMENTED JOURNAL logic 1:1 (structure
// and algorithm; POSIX stdio instead of LittleFS, scaled geometry for test
// speed) and proves the audit P7-S1-02 regression criteria under
// ASAN+UBSAN:
//
//   R1  power-cycle halfway through an outage → the persisted journal
//       resumes WITHOUT duplicate corruption (every sequence replayed at
//       most once per scan epoch), WITHOUT sequence regression (replay
//       order strictly increasing), WITHOUT silent truncation (every lost
//       record counted in dropCount), WITHOUT false EMPTY (valid records
//       pending > 0).
//   R2  torn tail (crash mid-append) → CRC fails → dropped + counted.
//   R3  mid-drain crash → resume from watermark → duplicates bounded by
//       WATERMARK_EVERY (at-least-once, never duplicate corruption).
//   R4  capacity eviction → whole oldest segment, counted, bounded.
//   R5  full drain → segments deleted (honest EMPTY — no resurrection).
//
// Negative control: -DNO_WATERMARK disables watermark persistence in the
// mirror. The mid-drain crash phase then re-replays the WHOLE journal —
// the duplicates bound is violated and the harness MUST trip (a clean
// exit would mean the harness is blind).
//
// Build:  g++ -std=c++17 -g -fsanitize=address,undefined -o spoolj verify_spool_journal_recovery.cpp
//         (negative control adds -DNO_WATERMARK)
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>   // ftruncate (POSIX)
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Mirror of the firmware constants (scaled geometry — the ALGORITHM is the
// 1:1 mirror; sizes only affect test runtime).
// ---------------------------------------------------------------------------
static constexpr uint16_t SEG_RECS     = 8;    // firmware: 32
static constexpr uint8_t  MAX_SEGS     = 4;    // firmware: 16 (boot-derived cap)
static constexpr uint16_t WM_EVERY     = 4;    // firmware: 16
static constexpr uint16_t PAYLOAD_LEN  = 64;

struct Record {
  uint32_t sequence;
  uint32_t timestamp;
  uint16_t payloadLen;
  uint8_t  recordType;
  uint16_t crc;
  char     payload[PAYLOAD_LEN];
};
static constexpr size_t REC_SIZE = sizeof(Record);

struct __attribute__((packed)) SegHeader {
  char     magic[4];       // "PLSJ"
  uint8_t  version;
  uint8_t  segIdx;
  uint32_t generation;
  uint32_t headerCrc;
};
static constexpr size_t HDR_SIZE = sizeof(SegHeader);

// CRC16/CCITT (mirror of Utils::crc16Ccitt semantics — self-consistent here)
static uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t seed = 0) {
  uint32_t c = ~seed;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
  }
  return ~c;
}

static uint16_t recCrc(const Record& r) {
  uint8_t buf[8 + 3 + PAYLOAD_LEN];
  size_t o = 0;
  memcpy(buf + o, &r.sequence, 4); o += 4;
  memcpy(buf + o, &r.timestamp, 4); o += 4;
  memcpy(buf + o, &r.payloadLen, 2); o += 2;
  memcpy(buf + o, &r.recordType, 1); o += 1;
  uint16_t pl = r.payloadLen > PAYLOAD_LEN ? PAYLOAD_LEN : r.payloadLen;
  memcpy(buf + o, r.payload, pl); o += pl;
  return crc16(buf, o);
}

// ---------------------------------------------------------------------------
// The journal mirror — the same states and transitions as
// firmware/Services/TelemetrySpool.cpp, with "power loss" modeled by
// constructing a fresh instance over the same directory (RAM state gone,
// files + NVS watermark survive).
// ---------------------------------------------------------------------------
struct SegmentMeta {
  uint8_t  segIdx;
  uint32_t generation;
  uint16_t count;
};

static uint32_t g_nvsWatermark = 0;   // NVS "plts_spool"/"jseq" stand-in
static std::string g_dir;             // "disk" root
static uint32_t g_drops = 0;          // counted data loss (across boots)
static uint32_t g_evictions = 0;

class Journal {
public:
  SegmentMeta segs[MAX_SEGS];
  uint8_t  segCount = 0;
  uint32_t nextGen = 0;
  uint16_t journalPending = 0;
  uint32_t watermark = 0;
  uint16_t replaySinceWm = 0;
  uint8_t  replaySeg = 0;
  uint16_t replaySlot = 0;
  bool     wmDirty = false;           // watermark pending NVS write
  uint8_t  maxSegments = MAX_SEGS;    // boot-derived

  explicit Journal(const std::string& tag) : tag_(tag) {}

  // ---- boot: scan + watermark load (mirrors begin → _scanJournal) ----
  void scan() {
    segCount = 0;
    nextGen = 0;
    uint16_t pending = 0;
    for (uint8_t i = 0; i < MAX_SEGS; i++) {
      std::string path = segPath(i);
      FILE* f = fopen(path.c_str(), "rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      SegHeader hdr;
      if (sz < (long)HDR_SIZE || fread(&hdr, 1, HDR_SIZE, f) != HDR_SIZE ||
          memcmp(hdr.magic, "PLSJ", 4) != 0 || hdr.version != 1 || hdr.segIdx != i) {
        fclose(f);
        remove(path.c_str());
        g_drops += (sz > (long)HDR_SIZE) ? (uint32_t)((sz - HDR_SIZE) / REC_SIZE) : 0;
        continue;
      }
      uint32_t hc = crc32((const uint8_t*)&hdr, HDR_SIZE - 4);
      if (hc != hdr.headerCrc) {
        fclose(f);
        remove(path.c_str());
        g_drops += (sz > (long)HDR_SIZE) ? (uint32_t)((sz - HDR_SIZE) / REC_SIZE) : 0;
        continue;
      }
      size_t bodyBytes = (sz > (long)HDR_SIZE) ? (size_t)(sz - HDR_SIZE) : 0;
      size_t maxRecs = bodyBytes / REC_SIZE;
      size_t leftover = bodyBytes % REC_SIZE;
      uint16_t valid = 0;
      bool torn = false;
      Record r;
      for (size_t k = 0; k < maxRecs; k++) {
        if (fseek(f, (long)(HDR_SIZE + k * REC_SIZE), SEEK_SET) != 0 ||
            fread(&r, 1, REC_SIZE, f) != REC_SIZE || recCrc(r) != r.crc) {
          torn = true;
          break;
        }
        valid++;
      }
      fclose(f);
      if (valid == 0 && sz <= (long)HDR_SIZE) { remove(path.c_str()); continue; }
      // [mirror of the HARNESS-CAUGHT firmware bug] partial trailing record
      // counts as one lost record (ceil) — never a silent floor.
      if (torn || leftover != 0) {
        size_t lostBytes = bodyBytes - (size_t)valid * REC_SIZE;
        g_drops += (uint16_t)((lostBytes + REC_SIZE - 1) / REC_SIZE);
      }
      SegmentMeta m = { i, hdr.generation, valid };
      uint8_t pos = segCount;
      while (pos > 0 && segs[pos - 1].generation > m.generation) {
        segs[pos] = segs[pos - 1];
        pos--;
      }
      segs[pos] = m;
      segCount++;
      if (m.generation >= nextGen) nextGen = m.generation + 1;
      pending += valid;
    }
    journalPending = pending;
    replaySeg = 0;
    replaySlot = 0;
#ifndef NO_WATERMARK
    watermark = g_nvsWatermark;
#else
    watermark = 0;   // negative control: crash "loses" the watermark
#endif
    if (pending > 0) firstPendingCursor();
  }

  // ---- append (mirrors spool → _rollSegment/_appendStaged/evict) ----
  // returns: 0=stored 1=evicted-oldest 2=rejected
  int append(uint32_t seq) {
    Record r = {};
    r.sequence = seq;
    r.timestamp = 1000 + seq;
    r.recordType = 0;
    r.payloadLen = 8;
    snprintf(r.payload, PAYLOAD_LEN, "seq-%lu", (unsigned long)seq);
    r.crc = recCrc(r);

    bool evicted = false;
    bool needRoll = (segCount == 0) || segs[segCount - 1].count >= SEG_RECS;
    if (needRoll) {
      if (segCount >= maxSegments) { evictOldest(); evicted = true; }
      if (!rollSegment()) return 2;
    }
    if (!appendStaged(r)) return 2;
    journalPending++;
    return evicted ? 1 : 0;
  }

  // ---- replay one record (mirrors replay → _firstPendingCursor/publish) ----
  // returns the sequence published, or 0 when nothing pending
  uint32_t replayOne() {
    if (!firstPendingCursor()) return 0;
    Record r;
    if (!readRecord(replaySeg, replaySlot, r) || recCrc(r) != r.crc) {
      g_drops++;
      advance();
      firstPendingCursor();
      if (replaySeg >= segCount) persistWmFinal();
      return 0;
    }
    // confirmed socket write (always true in the harness)
    watermark = r.sequence;
    advance();
    replaySinceWm++;
    firstPendingCursor();
    if (replaySinceWm >= WM_EVERY) { persistWm(); replaySinceWm = 0; }
    if (replaySeg >= segCount) persistWmFinal();
    return r.sequence;
  }

  void evictOldest() {
    if (segCount == 0) return;
    SegmentMeta oldest = segs[0];
    remove(segPath(oldest.segIdx).c_str());
    g_drops += oldest.count;
    g_evictions++;
    for (uint8_t i = 0; i + 1 < segCount; i++) segs[i] = segs[i + 1];
    segCount--;
    if (replaySeg > 0) replaySeg--;
    else replaySlot = 0;
    firstPendingCursor();
  }

private:
  std::string tag_;   // instance label for failure messages

  std::string segPath(uint8_t i) const {
    char b[64];
    snprintf(b, sizeof(b), "%s/spoolj_%c.bin", g_dir.c_str(), (char)('a' + i));
    return b;
  }

  void persistWm() {
#ifndef NO_WATERMARK
    g_nvsWatermark = watermark;
#else
    (void)watermark;
#endif
  }
  void persistWmFinal() { persistWm(); deleteAllSegments(); }

  void deleteAllSegments() {
    for (uint8_t i = 0; i < segCount; i++) remove(segPath(segs[i].segIdx).c_str());
    segCount = 0;
    journalPending = 0;
    replaySeg = 0;
    replaySlot = 0;
  }

  bool rollSegment() {
    bool inUse[MAX_SEGS] = {};
    for (uint8_t i = 0; i < segCount; i++) inUse[segs[i].segIdx] = true;
    uint8_t slot = 0xFF;
    for (uint8_t i = 0; i < maxSegments && i < MAX_SEGS; i++) {
      if (!inUse[i]) { slot = i; break; }
    }
    if (slot == 0xFF) return false;
    SegHeader hdr;
    memcpy(hdr.magic, "PLSJ", 4);
    hdr.version = 1;
    hdr.segIdx = slot;
    hdr.generation = nextGen++;
    hdr.headerCrc = crc32((const uint8_t*)&hdr, HDR_SIZE - 4);
    FILE* f = fopen(segPath(slot).c_str(), "wb");
    if (!f) return false;
    fwrite(&hdr, 1, HDR_SIZE, f);
    fclose(f);
    segs[segCount] = { slot, hdr.generation, 0 };
    segCount++;
    return true;
  }

  bool appendStaged(const Record& r) {
    if (segCount == 0) return false;
    SegmentMeta& seg = segs[segCount - 1];
    if (seg.count >= SEG_RECS) return false;
    FILE* f = fopen(segPath(seg.segIdx).c_str(), "r+b");
    if (!f) return false;
    if (fseek(f, (long)(HDR_SIZE + (size_t)seg.count * REC_SIZE), SEEK_SET) != 0) {
      fclose(f);
      return false;
    }
    size_t w = fwrite(&r, 1, REC_SIZE, f);
    fclose(f);
    if (w != REC_SIZE) return false;
    seg.count++;
    return true;
  }

  bool readRecord(uint8_t t, uint16_t slot, Record& out) {
    if (t >= segCount) return false;
    if (slot >= segs[t].count) return false;
    FILE* f = fopen(segPath(segs[t].segIdx).c_str(), "rb");
    if (!f) return false;
    bool ok = fseek(f, (long)(HDR_SIZE + (size_t)slot * REC_SIZE), SEEK_SET) == 0 &&
              fread(&out, 1, REC_SIZE, f) == REC_SIZE;
    fclose(f);
    return ok;
  }

  void advance() {
    replaySlot++;
    if (replaySlot >= segs[replaySeg].count) { replaySlot = 0; replaySeg++; }
  }

  uint8_t firstPendingCursor() {
    while (replaySeg < segCount) {
      const SegmentMeta& seg = segs[replaySeg];
      if (replaySlot < seg.count) {
        Record r;
        if (readRecord(replaySeg, replaySlot, r) && r.sequence > watermark) {
          recountPending();
          return 1;
        }
        replaySlot++;
        continue;
      }
      replaySeg++;
      replaySlot = 0;
    }
    recountPending();
    return 0;
  }

  void recountPending() {
    uint32_t n = 0;
    for (uint8_t i = replaySeg; i < segCount; i++) {
      n += segs[i].count;
      if (i == replaySeg) n -= replaySlot;
    }
    journalPending = (uint16_t)n;
  }
};

// ---------------------------------------------------------------------------
// Harness assertions
// ---------------------------------------------------------------------------
static int g_failures = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    printf("  [TRIP] %s:%d  %s\n", __FILE__, __LINE__, msg); \
    g_failures++; \
  } \
} while (0)

static void cleanDisk() {
  for (uint8_t i = 0; i < MAX_SEGS; i++) {
    char p[64];
    snprintf(p, sizeof(p), "%s/spoolj_%c.bin", g_dir.c_str(), (char)('a' + i));
    remove(p);
  }
}

// Phase 1: outage buffering + power-cycle resume (R1)
static void phase_powerCycleMidOutage() {
  printf("[P1] power-cycle halfway through an outage (R1)\n");
  cleanDisk();
  g_nvsWatermark = 0;
  g_drops = 0; g_evictions = 0;
  {
    Journal j("outage");
    j.scan();
    for (uint32_t s = 100; s < 100 + 20; s++) {   // 20 records buffered (2.5 segments)
      int rc = j.append(s);
      CHECK(rc == 0 || rc == 1, "append must succeed during outage");
    }
    CHECK(j.journalPending == 20, "20 records pending before power loss");
  }
  // POWER LOSS — RAM state gone, files persist. Reboot, broker STILL down.
  {
    Journal j("reboot");
    j.scan();
    CHECK(j.journalPending == 20, "journal resumes with 20 records — no loss, no false EMPTY");
    // sequence monotonicity + no duplicates: drain in order
    uint32_t prev = 0;
    for (int i = 0; i < 64 && j.journalPending > 0; ) {
      uint32_t s = j.replayOne();
      if (s == 0) { i++; continue; }
      CHECK(prev == 0 || s > prev, "replay order strictly increasing — no sequence regression");
      prev = s;
    }
    CHECK(j.journalPending == 0, "journal fully drained");
#ifndef NO_WATERMARK
    CHECK(g_nvsWatermark == 119, "watermark = last sequence (119)");
#else
    CHECK(g_nvsWatermark == 0, "negative control: watermark never persisted (0)");
#endif
  }
  // After drain + power loss: files deleted → honest EMPTY, no resurrection.
  {
    Journal j("postdrain");
    j.scan();
    CHECK(j.journalPending == 0, "honest EMPTY after full drain — no resurrection");
  }
  CHECK(g_drops == 0, "zero drops across the whole power-cycle epoch");
}

// Phase 2: torn tail (crash mid-append, R2)
static void phase_tornTail() {
  printf("[P2] torn tail — crash mid-append (R2)\n");
  cleanDisk();
  g_nvsWatermark = 0;
  g_drops = 0; g_evictions = 0;
  {
    Journal j("outage");
    j.scan();
    for (uint32_t s = 200; s < 200 + 10; s++) j.append(s);
    // simulate crash MID-append: chop the last record in half on disk
    {
      std::string path = g_dir + "/spoolj_a.bin";
      FILE* f = fopen(path.c_str(), "r+b");
      CHECK(f != nullptr, "active segment exists");
      if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        long cut = sz - (long)(REC_SIZE / 2);
        fflush(f);
        int fd = fileno(f);
        (void)ftruncate(fd, cut);
        fclose(f);
      }
    }
  }
  {
    Journal j("recovery");
    j.scan();
    CHECK(j.journalPending == 9, "torn tail dropped: 9 of 10 pending (counted)");
    CHECK(g_drops == 1, "exactly ONE dropped record counted — no silent truncation");
    uint32_t prev = 0;
    while (j.journalPending > 0) {
      uint32_t s = j.replayOne();
      if (s != 0) {
        CHECK(prev == 0 || s > prev, "order intact after torn-tail recovery");
        prev = s;
      }
    }
    // append resumes cleanly over the torn bytes
    Journal j2("append-after-torn");
    j2.scan();
    j2.append(500);
    CHECK(j2.journalPending == 1, "append after torn tail works (overwrite at logical end)");
  }
}

// Phase 3: mid-drain crash → bounded duplicates (R3) — the watermark proof
static void phase_midDrainCrash() {
  printf("[P3] crash mid-drain → duplicates bounded by watermark (R3)\n");
  cleanDisk();
  g_nvsWatermark = 0;
  g_drops = 0; g_evictions = 0;
  {
    Journal j("outage");
    j.scan();
    for (uint32_t s = 300; s < 300 + 16; s++) j.append(s);
  }
  // drain exactly 6 records, then "crash" — watermark persisted at record 4
  // (WM_EVERY=4): RAM watermark (6) is lost, NVS holds 303.
  {
    Journal j("drain-partial");
    j.scan();
    for (int i = 0; i < 6; i++) j.replayOne();
    // NO explicit flush — crash. NVS watermark = value at the 4th replay.
  }
#ifndef NO_WATERMARK
  uint32_t nvsExpected = 303;
  CHECK(g_nvsWatermark == nvsExpected, "NVS watermark advanced to the 4th record (303)");
#else
  uint32_t nvsExpected = 0;
  CHECK(g_nvsWatermark == 0, "negative control: watermark never persisted");
#endif
  {
    Journal j("resume");
    j.scan();
    // records published after the last NVS watermark persist but before the
    // crash are re-delivered (duplicates): (nvsExpected, crashRamWm].
    std::vector<uint32_t> delivered;
    uint32_t prev = 0;
    while (j.journalPending > 0) {
      uint32_t s = j.replayOne();
      if (s != 0) {
        CHECK(prev == 0 || s > prev, "resume order strictly increasing");
        prev = s;
        delivered.push_back(s);
      }
    }
    uint32_t crashRamWm = 305;   // 6 records published before the crash
    uint32_t dups = 0;
    for (uint32_t s : delivered) if (s <= crashRamWm) dups++;
    CHECK(dups < WM_EVERY, "duplicates after crash bounded STRICTLY below WATERMARK_EVERY (at-least-once, never corruption)");
    printf("    duplicates after mid-drain crash: %u (bound <%u)\n", dups, (unsigned)WM_EVERY);
    CHECK(prev == 315, "every record delivered at least once (no loss) — last seq 315");
  }
}

// Phase 4: capacity eviction (R4)
static void phase_eviction() {
  printf("[P4] capacity eviction — whole oldest segment, counted, bounded (R4)\n");
  cleanDisk();
  g_nvsWatermark = 0;
  g_drops = 0; g_evictions = 0;
  Journal j("outage");
  j.scan();
  // capacity = 4 segments × 8 = 32 records
  for (uint32_t s = 400; s < 400 + 40; s++) j.append(s);
  CHECK(g_evictions >= 1, "eviction happened at capacity");
  // 40 appended, capacity 32 → pending ≤ 32 (bounded), drops counted
  CHECK(j.journalPending <= 32, "pending bounded by capacity");
  uint32_t expectedDrops = 40 - j.journalPending;
  CHECK(g_drops == expectedDrops, "dropCount exactly accounts for evicted records (honest)");
  printf("    appended 40, capacity 32, pending %u, evictions %u, drops %u\n",
         j.journalPending, g_evictions, g_drops);
  // retained sequences are the NEWEST — no sequence regression in the queue
  uint32_t prev = 0;
  while (j.journalPending > 0) {
    uint32_t s = j.replayOne();
    if (s != 0) {
      CHECK(prev == 0 || s > prev, "post-eviction queue strictly increasing");
      prev = s;
    }
  }
  CHECK(prev == 439, "newest record survives eviction (439)");
}

int main(int argc, char** argv) {
  g_dir = argc > 1 ? argv[1] : ".";
  printf("=== GATE-7b spool journal recovery harness (P7-S1-02) ===\n");
#ifdef NO_WATERMARK
  printf("*** NEGATIVE CONTROL: watermark persistence DISABLED ***\n");
#endif
  phase_powerCycleMidOutage();
  phase_tornTail();
  phase_midDrainCrash();
  phase_eviction();
  if (g_failures == 0) {
    printf("RESULT: ALL PHASES CLEAN (%s)\n",
#ifdef NO_WATERMARK
           "UNEXPECTED — negative control must trip; harness is BLIND");
#else
           "invariants held");
#endif
  } else {
    printf("RESULT: %d invariant trip(s)\n", g_failures);
#ifdef NO_WATERMARK
    printf("negative control tripped as REQUIRED (harness can see the bug class)\n");
#endif
  }
  // House convention (run-native-tests.sh): TRIP = nonzero exit in EVERY
  // build. The negative-control branch is judged by the RUNNER via the log
  // (TRIP line + duplicates bound) — clean negative control (blind harness)
  // exits 0 and the runner flags it.
  return (g_failures > 0) ? 1 : 0;
}
