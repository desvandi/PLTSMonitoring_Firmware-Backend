// =============================================================================
// Services/TelemetrySpool.cpp
// =============================================================================
// [GATE-7b / P7-S1-02 2026-09] PERSISTENT SEGMENTED TELEMETRY JOURNAL.
// See TelemetrySpool.h for the design contract. Implementation notes:
//
//   - Truth lives in the FILES, not in metadata: a segment's record count is
//     its file size; record validity is its CRC16; ordering is the segment
//     generation (header, immutable after creation). There is no mutable
//     on-disk index to desync from a crash — every recovery path re-derives
//     state by scanning, and every torn write can only land at the tail of
//     the active segment (append-only within segments, whole-segment
//     eviction only).
//   - Appends open "r+", seek to (HEADER + count*RECORD_SIZE), write, close.
//     A crash mid-write leaves a torn tail whose CRC16 fails at the next
//     scan — the record is dropped HONESTLY (dropCount++ + log line), never
//     silently truncated, and the next append overwrites the torn bytes.
//   - Consumption is an NVS watermark ("plts_spool"/"jseq") — persisted every
//     SPOOL_JOURNAL_WATERMARK_EVERY confirmed replays and at full drain.
//     Full drain deletes every segment file (the queue is the files).
//   - Single-writer contract: all journal file operations run from setup
//     (begin/scan) or the telemetry task (spool/replay) — never both.
// =============================================================================
#include "TelemetrySpool.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include "../Utils/Crypto.h"
#include "../Core/Config.h"
#include "../Core/Globals.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <cstring>
#include <cstdio>

namespace Services {

TelemetrySpool telemetrySpool;

// ---------------------------------------------------------------------------
// Journal segment file layout constants (mirror of the header struct).
// ---------------------------------------------------------------------------
static constexpr size_t JOURNAL_HEADER_SIZE = 14;          // sizeof(SegmentHeader) — checked by static_assert in the header
static constexpr size_t JOURNAL_RECORD_SIZE = sizeof(TelemetryRecord);
// Geometry consistency: the class table constants and the Core/Config.h law
// must be the SAME numbers — the segment table is sized by the class
// constants while every runtime bound uses the Core law.
static_assert((size_t)Core::SPOOL_JOURNAL_SEGMENT_RECORDS == (size_t)TelemetrySpool::SEGMENT_RECORDS,
              "segment record count mismatch: TelemetrySpool.h vs Core/Config.h");
static_assert((size_t)Core::SPOOL_JOURNAL_MAX_SEGMENTS == (size_t)TelemetrySpool::MAX_SEGMENTS,
              "segment max mismatch: TelemetrySpool.h vs Core/Config.h");

uint16_t TelemetrySpool::_computeCRC(const TelemetryRecord& r) const {
  // CRC over: sequence(4) + timestamp(4) + payloadLen(2) + recordType(1) + payload
  uint8_t buf[12 + MAX_PAYLOAD_LEN];
  size_t off = 0;
  memcpy(buf + off, &r.sequence, 4); off += 4;
  memcpy(buf + off, &r.timestamp, 4); off += 4;
  memcpy(buf + off, &r.payloadLen, 2); off += 2;
  memcpy(buf + off, &r.recordType, 1); off += 1;
  uint16_t pl = r.payloadLen;
  if (pl > MAX_PAYLOAD_LEN) pl = MAX_PAYLOAD_LEN;
  memcpy(buf + off, r.payload, pl); off += pl;
  return Utils::crc16Ccitt(buf, off);
}

void TelemetrySpool::_writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                                   const char* payload, uint16_t len, uint8_t type) {
  dst.sequence = seq;
  dst.timestamp = ts;
  dst.payloadLen = (len > MAX_PAYLOAD_LEN) ? MAX_PAYLOAD_LEN : len;
  dst.recordType = type;
  if (payload && len > 0) {
    memcpy(dst.payload, payload, dst.payloadLen);
  }
  dst.crc = _computeCRC(dst);
}

bool TelemetrySpool::verifyRecord(const TelemetryRecord& r) const {
  return _computeCRC(r) == r.crc;
}

// ============================================================================
// begin() — boot recovery
// ============================================================================
void TelemetrySpool::begin(uint32_t targetRetentionSec, uint16_t telemetryIntervalSec) {
  _criticalHead = _criticalCount = 0;
  _dropCount = _replayCount = 0;
  _lastReplayMs = 0;
  _replayedThisSec = 0;
  _lastSpooledSeq = 0;
  _replaying = false;
  _segCount = 0;
  _nextGeneration = 0;
  _journalPending = 0;
  _journalEvictions = 0;
  _watermark = 0;
  _replaySinceWatermark = 0;
  _replaySeg = 0;
  _replaySlot = 0;
  _oldestPendingSeq = _newestPendingSeq = 0;
  _fsLoaded = false;
  _fsRestoredCount = 0;

  _loadCriticalFromNvs();
  _loadWatermark();

  // Defaults: build-time target + the ACTUAL configured telemetry cadence.
  if (targetRetentionSec == 0) {
    targetRetentionSec = Core::SPOOL_JOURNAL_TARGET_RETENTION_SEC;
  }
  if (telemetryIntervalSec == 0) {
    telemetryIntervalSec = (Core::cfgTelemetryIntervalSec > 0)
                             ? (uint16_t)Core::cfgTelemetryIntervalSec
                             : (uint16_t)(Core::TELEMETRY_INTERVAL_MS / 1000);
  }

  // [P7-S1-02] PHYSICAL capacity first — derived from the actual LittleFS
  // partition, never from the target. The target only sets the aspiration
  // that diagnostics compare against (journalDegraded).
  _computeCapacity(targetRetentionSec, telemetryIntervalSec);

  // [P7-S1-02 regression] Crash recovery: scan every segment, drop torn
  // tails/corrupt records HONESTLY, rebuild the ordered segment table.
  _scanJournal();

  // v1.9.3 → GATE-7b migration: the old 8-slot snapshot becomes journal
  // records (sequences preserved — no telemetry lost across the OTA itself).
  _migrateLegacySnapshot();
  if (_journalPending > 0) {
    _fsLoaded = true;
    _fsRestoredCount = _journalPending;
  }
}

void TelemetrySpool::setTargetRetentionSec(uint32_t targetRetentionSec,
                                            uint16_t telemetryIntervalSec) {
  if (telemetryIntervalSec == 0) {
    telemetryIntervalSec = (Core::cfgTelemetryIntervalSec > 0)
                             ? (uint16_t)Core::cfgTelemetryIntervalSec
                             : (uint16_t)(Core::TELEMETRY_INTERVAL_MS / 1000);
  }
  // [P7-S1-02] Physical capacity is boot-derived and DOES NOT move here —
  // re-deriving it at runtime would consult LittleFS.usedBytes() WHILE the
  // journal itself is buffered (capacity would shrink under its own feet
  // from a non-telemetry task). A runtime target larger than the partition
  // can hold is reported as degraded, never silently honored.
  _applyTarget(targetRetentionSec, telemetryIntervalSec);
  Serial.printf("[SPOOL] journal target updated: %lu s (effective %lu s, capacity %u records%s)\n",
                (unsigned long)_journalTargetSec, (unsigned long)_journalEffectiveSec,
                _journalCapacityRecords, _journalDegraded ? " — DEGRADED (physical limit)" : "");
}

// ============================================================================
// [P7-S1-02] Capacity honesty — the audit's core requirement.
// ============================================================================
void TelemetrySpool::_computeCapacity(uint32_t targetSec, uint16_t intervalSec) {
  const size_t total = LittleFS.totalBytes();
  const size_t used  = LittleFS.usedBytes();
  const size_t avail = (total > used) ? (total - used) : 0;
  const size_t capPct = (total * Core::SPOOL_JOURNAL_BUDGET_PCT) / 100;

  // Budget: at most SPOOL_JOURNAL_BUDGET_PCT of the partition AND never more
  // than the bytes actually free right now. Logs/config/calibration and the
  // LittleFS copy-on-write working room keep the remainder.
  _journalBudgetBytes = (avail < capPct) ? avail : capPct;

  const size_t segmentBytes = JOURNAL_HEADER_SIZE +
                              (size_t)Core::SPOOL_JOURNAL_SEGMENT_RECORDS * JOURNAL_RECORD_SIZE;
  uint32_t maxSeg = _journalBudgetBytes / segmentBytes;
  if (maxSeg > Core::SPOOL_JOURNAL_MAX_SEGMENTS) maxSeg = Core::SPOOL_JOURNAL_MAX_SEGMENTS;
  _journalMaxSegments = (uint8_t)maxSeg;
  _journalCapacityRecords = (uint16_t)(_journalMaxSegments * Core::SPOOL_JOURNAL_SEGMENT_RECORDS);

  // [P7-S1-02] "never silently claim more retention than physically
  // available" — the honest boot report. One line, always; the WARNING makes
  // the degraded case impossible to miss from the serial console/diag.
  Serial.printf("[SPOOL] journal: budget=%u B free-of=%u B | capacity=%u records (%u segs x %u) | ",
                (unsigned)_journalBudgetBytes, (unsigned)total,
                _journalCapacityRecords, _journalMaxSegments,
                (unsigned)Core::SPOOL_JOURNAL_SEGMENT_RECORDS);
  _applyTarget(targetSec, intervalSec);
  if (_journalDegraded) {
    Services::Log.append(Core::LogType::StorageError,
        String("Telemetry journal DEGRADED: target ") + (unsigned long)_journalTargetSec +
        " s but physical capacity holds only " + (unsigned long)_journalEffectiveSec +
        " s (" + _journalCapacityRecords + " records). Retention is capacity-bound — see /api/diagnostics.", -1);
  }
}

void TelemetrySpool::_applyTarget(uint32_t targetSec, uint16_t intervalSec) {
  if (targetSec < Core::SPOOL_RETENTION_MIN_SEC) targetSec = Core::SPOOL_RETENTION_MIN_SEC;
  if (targetSec > Core::SPOOL_RETENTION_MAX_SEC) targetSec = Core::SPOOL_RETENTION_MAX_SEC;
  _journalTargetSec = targetSec;
  _journalEffectiveSec = (intervalSec > 0)
      ? (uint32_t)_journalCapacityRecords * intervalSec
      : 0;
  _journalDegraded = (_journalEffectiveSec < _journalTargetSec);
  Serial.printf("target=%lu s effective=%lu s%s\n",
                (unsigned long)_journalTargetSec, (unsigned long)_journalEffectiveSec,
                _journalDegraded ? " — DEGRADED: physical capacity below target (honest)" : "");
}

// ============================================================================
// Journal scan — crash recovery (P7-S1-02 regression criteria)
// ============================================================================
const char* TelemetrySpool::_segmentPath(uint8_t segIdx) const {
  // "/spoolj_<a..p>.bin" — static buffer is safe: single-writer context,
  // path is consumed before the next call (file ops complete within a method).
  static char path[16];
  snprintf(path, sizeof(path), "%s%c.bin", Core::SPOOL_JOURNAL_PATH_PREFIX,
           (char)('a' + segIdx));
  return path;
}

void TelemetrySpool::_scanJournal() {
  _segCount = 0;
  _nextGeneration = 0;
  uint16_t pending = 0;

  for (uint8_t i = 0; i < Core::SPOOL_JOURNAL_MAX_SEGMENTS; i++) {
    const char* path = _segmentPath(i);
    if (!LittleFS.exists(path)) continue;

    File f = LittleFS.open(path, "r");
    if (!f) { _fsWriteFailures++; continue; }

    SegmentHeader hdr = {};
    size_t rh = f.read((uint8_t*)&hdr, sizeof(hdr));
    size_t fsize = f.size();
    if (rh != sizeof(hdr) || memcmp(hdr.magic, "PLSJ", 4) != 0 ||
        hdr.version != SPOOL_SCHEMA_VERSION || hdr.segIdx != i) {
      // Foreign/corrupt header — the file cannot be identified or ordered.
      // Honest removal (its records are unverifiable).
      f.close();
      LittleFS.remove(path);
      _dropCount += (uint16_t)((fsize > JOURNAL_HEADER_SIZE)
                    ? (fsize - JOURNAL_HEADER_SIZE) / JOURNAL_RECORD_SIZE : 0);
      Serial.printf("[SPOOL] journal segment %u: invalid header — removed (%u records dropped)\n",
                    i, (unsigned)((fsize > JOURNAL_HEADER_SIZE)
                    ? (fsize - JOURNAL_HEADER_SIZE) / JOURNAL_RECORD_SIZE : 0));
      continue;
    }
    // Header CRC gate.
    uint32_t hcrc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
    if (hcrc != hdr.headerCrc) {
      f.close();
      LittleFS.remove(path);
      _dropCount += (uint16_t)((fsize > JOURNAL_HEADER_SIZE)
                    ? (fsize - JOURNAL_HEADER_SIZE) / JOURNAL_RECORD_SIZE : 0);
      Serial.printf("[SPOOL] journal segment %u: header CRC fail — removed\n", i);
      continue;
    }

    // Records: file size is the count. Walk the VALID PREFIX (a torn write
    // can only land at the tail; a mid-file CRC failure is flash corruption
    // — everything from there on is unverifiable-ordered and dropped
    // honestly, keeping the packed invariant for appends).
    // [HARNESS-CAUGHT BUG 2026-09] A partial trailing record (crash mid-write,
    // body length NOT a multiple of RECORD_SIZE) used to be silently floored
    // by the integer division — the half-written record vanished WITHOUT
    // being counted = silent truncation, exactly what the audit forbids.
    // The leftover bytes are now counted as ONE additional lost record.
    size_t bodyBytes = (fsize > JOURNAL_HEADER_SIZE)
                       ? (size_t)(fsize - JOURNAL_HEADER_SIZE) : 0;
    size_t maxRecs = bodyBytes / JOURNAL_RECORD_SIZE;
    size_t leftover = bodyBytes % JOURNAL_RECORD_SIZE;
    uint16_t valid = 0;
    bool torn = false;
    TelemetryRecord r = {};
    for (size_t k = 0; k < maxRecs; k++) {
      f.seek(JOURNAL_HEADER_SIZE + (uint32_t)(k * JOURNAL_RECORD_SIZE), SeekSet);
      size_t rr = f.read((uint8_t*)&r, sizeof(r));
      if (rr != sizeof(r)) { torn = true; break; }
      if (!verifyRecord(r)) { torn = true; break; }
      valid++;
    }
    f.close();

    size_t logicalBytes = JOURNAL_HEADER_SIZE + (size_t)valid * JOURNAL_RECORD_SIZE;
    if (valid == 0 && fsize <= JOURNAL_HEADER_SIZE) {
      // Empty segment (created, crash before first record) — remove it so
      // the slot is reusable.
      LittleFS.remove(path);
      continue;
    }
    if (torn || leftover != 0 || fsize != logicalBytes) {
      // Torn tail / trailing garbage / partial record: every dropped record
      // is COUNTED (audit: "no silent truncation") — including the
      // half-written tail record (ceil accounting). The physical bytes
      // remain and will be overwritten by the next append at the logical
      // position.
      size_t lostBytes = bodyBytes - (size_t)valid * JOURNAL_RECORD_SIZE;
      uint16_t lost = (uint16_t)((lostBytes + JOURNAL_RECORD_SIZE - 1) / JOURNAL_RECORD_SIZE);
      if (lost > 0) {
        _dropCount += lost;
        Serial.printf("[SPOOL] journal segment %u: %u torn/corrupt record(s) dropped (counted)\n",
                      i, lost);
      }
    }

    SegmentMeta m = {};
    m.segIdx = i;
    m.generation = hdr.generation;
    m.count = valid;
    m.active = true;

    // Insertion sort by generation (≤ MAX_SEGMENTS entries — trivial).
    uint8_t pos = _segCount;
    while (pos > 0 && _segs[pos - 1].generation > m.generation) {
      _segs[pos] = _segs[pos - 1];
      pos--;
    }
    _segs[pos] = m;
    _segCount++;
    if (m.generation >= _nextGeneration) _nextGeneration = m.generation + 1;
    pending += valid;
  }

  _journalPending = pending;
  _replaySeg = 0;
  _replaySlot = 0;

  if (pending > 0) {
    // Skip records already confirmed-delivered before the crash (watermark)
    // and recompute pending from the live cursor — pendingCount() must never
    // include records that were already delivered before the crash.
    _firstPendingCursor();
    Serial.printf("[SPOOL] journal restored: %u valid record(s) across %u segment(s)"
                  ", %u pending after watermark %lu (pre-reboot outage buffered data)\n",
                  pending, _segCount, _journalPending, (unsigned long)_watermark);
  }
}

// Advance the replay cursor to the first record that is (a) after the
// watermark and (b) valid. Returns 1 when a pending record exists at the
// cursor, 0 when the journal is fully consumed. Also recomputes
// _journalPending (records from the cursor to the tail) — the cursor is the
// single source of truth for consumption.
uint8_t TelemetrySpool::_firstPendingCursor() {
  while (_replaySeg < _segCount) {
    const SegmentMeta& seg = _segs[_replaySeg];
    if (_replaySlot < seg.count) {
      TelemetryRecord r = {};
      if (_readRecord(_replaySeg, _replaySlot, r) && r.sequence > _watermark) {
        _oldestPendingSeq = r.sequence;
        _recountPending();
        return 1;
      }
      _replaySlot++;   // consumed pre-crash (≤ watermark) or corrupt — skip
      continue;
    }
    _replaySeg++;
    _replaySlot = 0;
  }
  _oldestPendingSeq = 0;
  _recountPending();
  return 0;
}

void TelemetrySpool::_recountPending() {
  uint32_t n = 0;
  for (uint8_t i = _replaySeg; i < _segCount; i++) {
    n += _segs[i].count;
    if (i == _replaySeg) n -= _replaySlot;
  }
  _journalPending = (n > 0xFFFF) ? 0xFFFF : (uint16_t)n;
}

// ============================================================================
// Legacy v1.9.3 snapshot migration (bounded: ≤ 8 records)
// ============================================================================
void TelemetrySpool::_migrateLegacySnapshot() {
  if (!LittleFS.exists("/spool_reg.bin")) {
    LittleFS.remove("/spool_reg.tmp");   // stale temp from an interrupted flush
    return;
  }
  struct __attribute__((packed)) LegacySnapHeader {
    char     magic[4];       // "PLSR"
    uint8_t  version;
    uint8_t  head;
    uint8_t  count;
    uint8_t  reserved;
    uint32_t crc32;
  };
  static constexpr uint8_t LEGACY_SLOTS = 8;    // v1.9.3 SPOOL_CAPACITY (frozen format)

  File f = LittleFS.open("/spool_reg.bin", "r");
  if (!f) { LittleFS.remove("/spool_reg.bin"); return; }
  LegacySnapHeader hdr = {};
  size_t rh = f.read((uint8_t*)&hdr, sizeof(hdr));
  TelemetryRecord legacy[LEGACY_SLOTS] = {};
  size_t rr = f.read((uint8_t*)legacy, sizeof(legacy));
  f.close();
  if (rh == sizeof(hdr) && rr == sizeof(legacy) &&
      memcmp(hdr.magic, "PLSR", 4) == 0 && hdr.version == SPOOL_SCHEMA_VERSION &&
      hdr.count <= LEGACY_SLOTS && hdr.head < LEGACY_SLOTS) {
    uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
    crc = Utils::crc32((const uint8_t*)legacy, sizeof(legacy), crc);
    if (crc == hdr.crc32) {
      // Ring order oldest→newest, then journal-append each VALID record.
      uint16_t migrated = 0;
      for (uint8_t i = 0; i < hdr.count; i++) {
        uint8_t src = (hdr.head + LEGACY_SLOTS - hdr.count + i) % LEGACY_SLOTS;
        if (!verifyRecord(legacy[src])) { _dropCount++; continue; }
        if (legacy[src].sequence <= _watermark) continue;  // already delivered pre-upgrade
        _writeRecord(_staging, legacy[src].sequence, legacy[src].timestamp,
                     legacy[src].payload, legacy[src].payloadLen,
                     (uint8_t)SpoolRecordType::Telemetry);
        // Ensure there is an appendable segment (fresh journal after OTA).
        if (_segCount == 0 ||
            _segs[_segCount - 1].count >= Core::SPOOL_JOURNAL_SEGMENT_RECORDS) {
          if (_segCount >= _journalMaxSegments) _evictOldestSegment();
          if (!_rollSegment()) { _dropCount++; continue; }
        }
        if (_appendStaged()) {
          _journalPending++;
          _newestPendingSeq = legacy[src].sequence;
          if (_journalPending == 1) _oldestPendingSeq = legacy[src].sequence;
          migrated++;
        } else {
          _dropCount++;
        }
      }
      if (migrated > 0) {
        Serial.printf("[SPOOL] legacy snapshot migrated: %u record(s) -> journal\n", migrated);
      }
    } else {
      _dropCount += hdr.count;   // corrupt snapshot — honest
    }
  }
  LittleFS.remove("/spool_reg.bin");
  LittleFS.remove("/spool_reg.tmp");
}

// ============================================================================
// Append path
// ============================================================================
bool TelemetrySpool::_rollSegment() {
  // Find a free filename slot (after eviction at least one is free; when
  // _segCount < journalMaxSegments an unused letter exists by pigeonhole).
  bool inUse[Core::SPOOL_JOURNAL_MAX_SEGMENTS] = {};
  for (uint8_t i = 0; i < _segCount; i++) inUse[_segs[i].segIdx] = true;
  uint8_t slot = 0xFF;
  for (uint8_t i = 0; i < _journalMaxSegments && i < Core::SPOOL_JOURNAL_MAX_SEGMENTS; i++) {
    if (!inUse[i]) { slot = i; break; }
  }
  if (slot == 0xFF) return false;   // no capacity slot — capacity check failed upstream

  SegmentHeader hdr = {};
  memcpy(hdr.magic, "PLSJ", 4);
  hdr.version = SPOOL_SCHEMA_VERSION;
  hdr.segIdx = slot;
  hdr.generation = _nextGeneration++;
  hdr.headerCrc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));

  const char* path = _segmentPath(slot);
  File f = LittleFS.open(path, "w", true);
  if (!f) { _fsWriteFailures++; return false; }
  size_t w = f.write((const uint8_t*)&hdr, sizeof(hdr));
  f.close();
  if (w != sizeof(hdr)) { _fsWriteFailures++; LittleFS.remove(path); return false; }

  SegmentMeta m = {};
  m.segIdx = slot;
  m.generation = hdr.generation;
  m.count = 0;
  m.active = true;
  // New segment is the newest generation → append at the table tail.
  _segs[_segCount] = m;
  _segCount++;
  return true;
}

bool TelemetrySpool::_appendStaged() {
  if (_segCount == 0) return false;
  SegmentMeta& seg = _segs[_segCount - 1];   // active = highest generation
  if (seg.count >= Core::SPOOL_JOURNAL_SEGMENT_RECORDS) return false;

  const char* path = _segmentPath(seg.segIdx);
  File f = LittleFS.open(path, "r+");
  if (!f) { _fsWriteFailures++; return false; }
  // Write at the END of the valid prefix: appends land at (header + count).
  // A torn tail from a previous crash is overwritten here — its CRC already
  // failed at scan and was dropped/counted.
  if (!f.seek(JOURNAL_HEADER_SIZE + (uint32_t)(seg.count * JOURNAL_RECORD_SIZE), SeekSet)) {
    f.close(); _fsWriteFailures++; return false;
  }
  size_t w = f.write((const uint8_t*)&_staging, JOURNAL_RECORD_SIZE);
  f.close();
  if (w != JOURNAL_RECORD_SIZE) { _fsWriteFailures++; return false; }
  seg.count++;
  return true;
}

void TelemetrySpool::_evictOldestSegment() {
  if (_segCount == 0) return;
  const SegmentMeta oldest = _segs[0];   // sorted by generation — index 0 is oldest
  LittleFS.remove(_segmentPath(oldest.segIdx));
  _dropCount += oldest.count;
  _journalEvictions++;
  // Compact the table (order-preserving).
  for (uint8_t i = 0; i + 1 < _segCount; i++) _segs[i] = _segs[i + 1];
  _segCount--;
  // Replay cursor fix-up: the table shifted left by one.
  if (_replaySeg > 0) {
    _replaySeg--;                       // cursor pointed past the evicted segment
  } else {
    _replaySlot = 0;                    // cursor pointed INTO the evicted segment —
                                        // its un-replayed records are gone (counted)
  }
  _firstPendingCursor();   // recompute cursor, pending count + oldest bound
}

SpoolResult TelemetrySpool::spool(uint32_t sequence, uint32_t timestamp,
                                   const char* payload, uint16_t len) {
  if (sequence == _lastSpooledSeq) return SpoolResult::Rejected;  // dedup
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) {
    _dropCount++;
    return SpoolResult::Rejected;
  }
  _lastSpooledSeq = sequence;

  // [P7-S1-02 honest degradation] No physical capacity (budget < 1 segment,
  // FS exhausted at boot): the record is NOT stored and the loss is counted
  // — never a silent "accepted".
  if (_journalMaxSegments == 0) {
    _dropCount++;
    _fsWriteFailures++;
    return SpoolResult::Rejected;
  }

  _writeRecord(_staging, sequence, timestamp, payload, len,
               (uint8_t)SpoolRecordType::Telemetry);

  bool evicted = false;
  bool needRoll = (_segCount == 0) ||
                  (_segs[_segCount - 1].count >= Core::SPOOL_JOURNAL_SEGMENT_RECORDS);
  if (needRoll) {
    if (_segCount >= _journalMaxSegments) {
      // Journal at capacity: evict the OLDEST segment — honest data loss at
      // the capacity boundary (p.438 EvictedOldest + dropCount + episode log
      // in the caller). NOT a mid-file overwrite: whole-segment deletion.
      _evictOldestSegment();
      evicted = true;
    }
    if (!_rollSegment()) {
      _dropCount++;
      _fsWriteFailures++;
      return SpoolResult::Rejected;
    }
  }
  if (!_appendStaged()) {
    _dropCount++;
    _fsWriteFailures++;
    return SpoolResult::Rejected;
  }
  _journalPending++;
  if (_journalPending == 1) _oldestPendingSeq = sequence;
  _newestPendingSeq = sequence;

  return evicted ? SpoolResult::EvictedOldest : SpoolResult::Stored;
}

// ============================================================================
// Replay path
// ============================================================================
bool TelemetrySpool::_readRecord(uint8_t segTableIdx, uint16_t slot, TelemetryRecord& out) {
  if (segTableIdx >= _segCount) return false;
  const SegmentMeta& seg = _segs[segTableIdx];
  if (slot >= seg.count) return false;
  File f = LittleFS.open(_segmentPath(seg.segIdx), "r");
  if (!f) { _fsWriteFailures++; return false; }
  bool ok = false;
  if (f.seek(JOURNAL_HEADER_SIZE + (uint32_t)(slot * JOURNAL_RECORD_SIZE), SeekSet)) {
    size_t rr = f.read((uint8_t*)&out, sizeof(out));
    ok = (rr == sizeof(out));
  }
  f.close();
  if (!ok) { _fsWriteFailures++; return false; }
  return true;
}

bool TelemetrySpool::_persistWatermark() {
  Preferences p;
  if (!p.begin("plts_spool", false)) { _nvsWriteFailures++; return false; }
  bool ok = p.putULong("jseq", _watermark) == sizeof(uint32_t);
  p.end();
  if (!ok) _nvsWriteFailures++;
  return ok;
}

void TelemetrySpool::_loadWatermark() {
  Preferences p;
  if (!p.begin("plts_spool", true)) return;   // absent namespace = fresh start (0)
  _watermark = p.getULong("jseq", 0);
  p.end();
}

void TelemetrySpool::_deleteAllSegments() {
  for (uint8_t i = 0; i < _segCount; i++) {
    LittleFS.remove(_segmentPath(_segs[i].segIdx));
  }
  _segCount = 0;
  _journalPending = 0;
  _replaySeg = 0;
  _replaySlot = 0;
  _oldestPendingSeq = _newestPendingSeq = 0;
}

uint16_t TelemetrySpool::replay() {
  unsigned long now = millis();
  if (now - _lastReplayMs >= 1000) {
    _lastReplayMs = now;
    _replayedThisSec = 0;
  }
  if (_replayedThisSec >= MAX_REPLAY_PER_SEC) return 0;
  if (!_publishCb) return 0;

  _replaying = true;

  // Critical records first (BOOT/ALARM/FAULT/SAFETY outrank telemetry).
  // [P1-005] Oldest-first ordering. NVS path is UNCHANGED (GATE-7b scope:
  // the regular telemetry journal only).
  if (_criticalCount > 0) {
    for (uint8_t i = 0; i < _criticalCount; i++) {
      uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount + i) % CRITICAL_SPOOL_CAPACITY;
      const TelemetryRecord& r = _criticalRecords[idx];
      if (!verifyRecord(r)) {
        _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
        _criticalCount--;
        _dropCount++;
        continue;
      }
      // [P1-005 / audit p.432] Removal happens ONLY after the callback
      // confirms a socket write (PubSubClient QoS-0 — no broker PUBACK).
      if (_publishCb(r.recordType, r.payload, r.payloadLen)) {
        _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
        _criticalCount--;
        _replayCount++;
        _replayedThisSec++;
        _persistCriticalToNvs();
        _replaying = _criticalCount > 0 || _journalPending > 0;
        return 1;
      }
      break;   // delivery not confirmed — retry on the next tick
    }
  }

  // Journal records (oldest first: segments ordered by generation, slots in
  // append order = sequence order).
  if (_firstPendingCursor()) {
    TelemetryRecord& r = _staging;   // safe: single-writer context, spool() not active
    if (_readRecord(_replaySeg, _replaySlot, r) && verifyRecord(r)) {
      // [P1-005 / audit p.432] Record removed ONLY on confirmed socket write
      // (PubSubClient QoS-0 publish — NOT a broker PUBACK). Delivery is
      // at-least-once via the journal + watermark + GAS sequence dedup.
      if (_publishCb(r.recordType, r.payload, r.payloadLen)) {
        _watermark = r.sequence;
        _replaySlot++;
        if (_replaySlot >= _segs[_replaySeg].count) {
          _replaySlot = 0;
          _replaySeg++;
        }
        _replayCount++;
        _replayedThisSec++;
        _replaySinceWatermark++;
        _firstPendingCursor();   // refresh pending count + oldest bound

        if (_replaySinceWatermark >= Core::SPOOL_JOURNAL_WATERMARK_EVERY) {
          _persistWatermark();
          _replaySinceWatermark = 0;
        }
        if (_replaySeg >= _segCount) {
          // Fully drained: persist the final watermark, then the queue (the
          // files themselves) is deleted — no resurrection of replayed
          // records after a later reboot.
          _persistWatermark();
          _deleteAllSegments();
        }
        _replaying = _journalPending > 0 || _criticalCount > 0;
        return 1;
      }
    } else {
      // Mid-session corruption (bit-rot): advance honestly — counted drop.
      _dropCount++;
      _replaySlot++;
      if (_replaySlot >= _segs[_replaySeg].count) {
        _replaySlot = 0;
        _replaySeg++;
      }
      _firstPendingCursor();
      if (_replaySeg >= _segCount) {
        _persistWatermark();
        _deleteAllSegments();
      }
    }
  } else if (_replaySeg >= _segCount && _segCount > 0) {
    // Cursor at end but segments still exist (all records above watermark
    // were consumed): drain-complete cleanup.
    _persistWatermark();
    _deleteAllSegments();
  }
  _replaying = _journalPending > 0 || _criticalCount > 0;
  return 0;
}

// ============================================================================
// Observability
// ============================================================================
uint32_t TelemetrySpool::oldestSequence() const {
  if (_journalPending == 0 && _criticalCount == 0) return 0;
  if (_criticalCount > 0) {
    uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[idx])) return _criticalRecords[idx].sequence;
  }
  return _oldestPendingSeq;
}

uint32_t TelemetrySpool::newestSequence() const {
  if (_newestPendingSeq != 0) return _newestPendingSeq;
  if (_criticalCount > 0) {
    uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - 1) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[idx])) return _criticalRecords[idx].sequence;
  }
  return 0;
}

SpoolState TelemetrySpool::state() const {
  if (_nvsWriteFailures > 0) return SpoolState::ERROR;
  if (_dropCount > 0)       return SpoolState::DROP_OCCURRED;
  if (_replaying && (_journalPending > 0 || _criticalCount > 0)) return SpoolState::REPLAYING;
  if (_journalPending >= _journalCapacityRecords || _criticalCount >= CRITICAL_SPOOL_CAPACITY)
    return SpoolState::FULL;
  if (_journalPending > 0 || _criticalCount > 0) return SpoolState::BUFFERING;
  return SpoolState::EMPTY;
}

void TelemetrySpool::clear() {
  _criticalHead = _criticalCount = 0;
  _deleteAllSegments();
  _clearNvsSpool();
  _watermark = 0;
  _oldestPendingSeq = _newestPendingSeq = 0;
}

// ============================================================================
// Critical-event NVS ring — UNCHANGED from round-5 remediation (p.437).
// ============================================================================
void TelemetrySpool::_persistCriticalToNvs() {
  Preferences p;
  if (!p.begin("plts_spool", false)) { _nvsWriteFailures++; return; }
  size_t sz = sizeof(_criticalRecords);
  if (!p.putBytes("crit", _criticalRecords, sz)) _nvsWriteFailures++;
  p.putUChar("chead", _criticalHead);
  p.putUChar("ccount", _criticalCount);
  p.end();
}

void TelemetrySpool::_loadCriticalFromNvs() {
  Preferences p;
  if (!p.begin("plts_spool", true)) {
    _nvsLoadFailures++;
    return;
  }
  size_t got = p.getBytes("crit", _criticalRecords, sizeof(_criticalRecords));
  if (got == sizeof(_criticalRecords)) {
    _criticalHead = p.getUChar("chead", 0);
    _criticalCount = p.getUChar("ccount", 0);
    uint8_t valid = 0;
    for (uint8_t i = 0; i < _criticalCount; i++) {
      uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount + i) % CRITICAL_SPOOL_CAPACITY;
      if (!verifyRecord(_criticalRecords[idx])) {
        _criticalRecords[idx].crc = 0;
      } else {
        valid++;
      }
    }
    _criticalCount = valid;
    _nvsLoaded = true;
  } else {
    _nvsLoadFailures++;
  }
  p.end();
}

void TelemetrySpool::_clearNvsSpool() {
  Preferences p;
  if (p.begin("plts_spool", false)) {
    p.clear();
    p.end();
  }
}

} // namespace Services
