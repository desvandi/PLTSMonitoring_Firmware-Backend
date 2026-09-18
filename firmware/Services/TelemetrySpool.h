// =============================================================================
// Services/TelemetrySpool.h — Store-and-forward telemetry + critical events
// -----------------------------------------------------------------------------
// [GATE-7b / P7-S1-02 2026-09] PERSISTENT SEGMENTED TELEMETRY JOURNAL.
//
// The audit finding (P7-S1-02): the previous regular-telemetry ring held 8
// records (~40 s at the 5 s interval) in RAM with a bounded LittleFS snapshot
// of the SAME 8 slots — store-and-forward was a transient buffer, not an
// outage journal. A 1-hour broker outage retained 8 of 720 records.
//
// This revision replaces the 8-slot RAM ring with a size-configurable
// persistent append-only SEGMENTED JOURNAL on LittleFS:
//
//   - TARGET_OFFLINE_RETENTION_SEC is a build parameter (Config.h, -D
//     overridable) AND a runtime parameter (config "offlineRetentionSec",
//     REST/MQTT config.update, NVS "plts_batt"/"retS", clamp [60, 86400]).
//   - At boot the journal derives its PHYSICAL capacity from the actual
//     LittleFS partition (total/used bytes, 60% budget cap) and NEVER claims
//     more retention than physically available: if the capacity-derived
//     effective retention is below the configured target, the device logs a
//     Warning and diagnostics expose journalDegraded=true with both numbers
//     (audit P7-S1-02: "never silently claim more retention than physically
//     available").
//   - Layout: rotating segment files /spoolj_<a..p>.bin, each = a fixed
//     14-byte header (magic "PLSJ", version, segment id, monotonic append
//     generation, CRC32) followed by up to SPOOL_JOURNAL_SEGMENT_RECORDS
//     packed TelemetryRecord slots. Records are APPEND-ONLY within a segment;
//     capacity eviction deletes the OLDEST WHOLE SEGMENT (never a mid-file
//     overwrite — a torn write can only ever land at the tail of the active
//     segment). Journal retention = all live segments.
//   - Crash recovery (audit regression criteria): at boot every segment is
//     scanned record-by-record; per-record CRC16 failures and torn tails are
//     dropped HONESTLY (dropCount++ + logged) — never silently truncated,
//     never a false EMPTY state while valid records remain. Records are
//     ordered by (generation, slot); the global telemetry sequence is
//     monotonic across reboots (high-water mark, FW-17), so file order IS
//     sequence order — no sequence regression is possible after recovery.
//   - At-least-once replay: a record is removed ONLY after a confirmed socket
//     write (PubSubClient QoS-0 reality — NOT a broker PUBACK). Consumption
//     is tracked by a persistent NVS watermark ("plts_spool"/"jseq") written
//     every 16 confirmed replays and at full drain; a crash mid-drain re-
//     replays at most 16 records, which GAS-side sequence dedup absorbs
//     (duplicate delivery, never duplicate corruption).
//   - Flash-wear note: the journal is touched ONLY while the transport is
//     down (the healthy path never writes flash). Worst case (broker down
//     24/7) the journal cycles its ~60%-of-partition budget roughly every
//     effective-retention window — bounded, honest, and visible via
//     diagnostics counters.
//
// RAM BUDGET: the 8-slot static ring (~20.6 KB) is GONE. The journal keeps
// ONE static staging TelemetryRecord (~2.6 KB) + a segment table (≤16 × 12 B)
// + cursors. The critical-event path (NVS 4-slot ring, spoolCritical) is
// UNCHANGED — rare high-value events belong in NVS.
//
// [AUDIT 2026-09 ROUND 5 / p.438] spool() semantics remain EXPLICIT:
//   SpoolResult::Stored         — record is in the journal, nothing was lost
//   SpoolResult::EvictedOldest  — stored; the OLDEST segment was dropped to
//                                 make room (dropCount incremented)
//   SpoolResult::Rejected       — not stored (invalid/duplicate/disk-full)
//
// Threading contract: begin() runs at setup; spool() and replay() are driven
// exclusively by the telemetry task (single-writer — same contract as the
// previous snapshot design). NVS critical-ring operations remain NVS-internal
// (thread-safe). LittleFS access from this service is therefore
// single-context by construction.
//
// Flash wear: critical events ~10/day → ~27 years per NVS sector.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_TELEMETRY_SPOOL_H
#define PLTS_SERVICES_TELEMETRY_SPOOL_H

#include <Arduino.h>
#include <cstdint>

namespace Services {

enum class SpoolRecordType : uint8_t {
  Telemetry      = 0,
  CriticalEvent = 1,  // BOOT / ALARM / FAULT / SAFETY
};

// [p.438] Explicit spool() outcome — see header comment.
enum class SpoolResult : uint8_t {
  Rejected      = 0,   // invalid payload / duplicate sequence / FS failure — not stored
  Stored        = 1,   // stored; no record was evicted
  EvictedOldest = 2,    // stored; the oldest journal segment was dropped
};

// [P1-005] Explicit spool lifecycle state — exposed via diagnostics.
enum class SpoolState : uint8_t {
  EMPTY         = 0,
  BUFFERING     = 1,   // records held, transport unavailable
  REPLAYING     = 2,   // draining now
  FULL          = 3,   // journal at capacity — next insert evicts the oldest segment
  DROP_OCCURRED = 4,   // at least one record dropped since boot
  ERROR         = 5,   // NVS/FS failure on persistence
};

struct TelemetryRecord {
  uint32_t sequence;
  uint32_t timestamp;
  uint16_t payloadLen;
  uint8_t  recordType;       // SpoolRecordType
  uint16_t crc;
  // [FW-08 REMEDIATION 2026-08] 512 bytes was smaller than the serialized
  // telemetry envelope (~1.3–2.2 KB) — every record hit the size guard and
  // was silently DROPPED (dropCount++), making store-and-forward a no-op.
  // Raised to 2560. (The old RAM ring shrank 16→8 to compensate — the
  // GATE-7b journal removes that RAM budget entirely, so the full payload
  // bound now costs ONE staging record instead of eight.)
  char     payload[2560];
};

static constexpr uint8_t SPOOL_SCHEMA_VERSION = 1;

class TelemetrySpool {
public:
  // Critical events: NVS ring (UNCHANGED — see header comment).
  static constexpr uint8_t  CRITICAL_SPOOL_CAPACITY = 4; // was 8 — [FW-27] DRAM/NVS budget: critical events are rare (~10/day); 4 slots ≈ 10 KB NVS blob
  static constexpr uint16_t MAX_REPLAY_PER_SEC = 2;
  static constexpr uint16_t MAX_PAYLOAD_LEN = 2560;

  // [GATE-7b / P7-S1-02] Journal geometry. Constants live in Core/Config.h
  // (SPOOL_JOURNAL_*); the segment record count is repeated here for the
  // static segment table sizing in this header's implementation.
  static constexpr uint16_t SEGMENT_RECORDS = 32;   // Core::SPOOL_JOURNAL_SEGMENT_RECORDS
  static constexpr uint8_t  MAX_SEGMENTS    = 16;   // Core::SPOOL_JOURNAL_MAX_SEGMENTS (hard cap)

  // [GATE-7b] begin(): targetRetentionSec = configured target (runtime
  // config value), telemetryIntervalSec = current publish cadence — used to
  // derive records required and to REPORT (never to overclaim) achievable
  // retention against the physical LittleFS budget.
  void begin(uint32_t targetRetentionSec = 0, uint16_t telemetryIntervalSec = 0);
  // [GATE-7b] Runtime re-apply of the retention target (ConfigUpdater path —
  // "offlineRetentionSec"). Physical capacity stays boot-derived (honest);
  // only the target (and therefore the degraded comparison) moves.
  void setTargetRetentionSec(uint32_t targetRetentionSec, uint16_t telemetryIntervalSec);

  // [p.438] Explicit outcome — see SpoolResult. bool callers must migrate.
  SpoolResult spool(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  bool spoolCritical(uint32_t sequence, uint32_t timestamp,
                     const char* payload, uint16_t len);
  uint16_t replay();  // returns # records replayed this call (0 or 1)
  uint16_t pendingCount() const { return _journalPending + _criticalCount; }  // [GATE-7b] journal + critical
  uint8_t  criticalPendingCount() const { return _criticalCount; }
  uint32_t dropCount() const { return _dropCount; }
  uint32_t replayCount() const { return _replayCount; }
  bool isEmpty() const { return _journalPending == 0 && _criticalCount == 0; }
  void clear();
  bool verifyRecord(const TelemetryRecord& r) const;
  bool isNvsLoaded() const { return _nvsLoaded; }
  uint32_t nvsLoadFailures() const { return _nvsLoadFailures; }
  uint32_t nvsWriteFailures() const { return _nvsWriteFailures; }

  // [p.437 → GATE-7b] Journal reboot-persistence observability.
  bool     fsLoaded() const { return _fsLoaded; }          // journal records restored at boot
  uint16_t fsRestoredCount() const { return _fsRestoredCount; }  // records pending across the reboot
  uint32_t fsWriteFailures() const { return _fsWriteFailures; }

  // [GATE-7b / P7-S1-02] Capacity honesty — the audit's "never silently
  // claim more retention than physically available" surface.
  uint16_t journalCapacityRecords() const { return _journalCapacityRecords; }
  uint32_t journalTargetSec() const { return _journalTargetSec; }
  uint32_t journalEffectiveSec() const { return _journalEffectiveSec; }
  uint32_t journalBudgetBytes() const { return _journalBudgetBytes; }
  bool     journalDegraded() const { return _journalDegraded; }
  uint8_t  journalSegments() const { return _segCount; }
  uint32_t journalEvictions() const { return _journalEvictions; }
  uint32_t journalWatermark() const { return _watermark; }

  // [P1-005] Real spool diagnostics — oldest/newest sequence of pending
  // records (0 when empty). Exposed on /api/diagnostics and telemetry health.
  uint32_t oldestSequence() const;
  uint32_t newestSequence() const;

  // [FW-29 REMEDIATION 2026-08] Callback now receives the RECORD TYPE so the
  // publisher routes to the correct topic (plts/<id>/status vs .../log).
  // The old signature passed a literal topic string ("critical"/"status")
  // that no broker would route.
  // [audit p.432 — HONEST CONTRACT] Returns true ONLY on a confirmed socket
  // write — PubSubClient 2.8 publish() is ALWAYS QoS 0 (fire-and-forget);
  // this is NOT a broker PUBACK. End-to-end delivery is achieved by
  // at-least-once journal replay + GAS-side sequence dedup, not by transport
  // acknowledgement (P1-005 spool semantics).
  typedef bool (*PublishCb)(uint8_t recordType, const char* payload, size_t len);
  void setPublishCallback(PublishCb cb) { _publishCb = cb; }

  SpoolState state() const;   // [P1-005] EMPTY/BUFFERING/REPLAYING/FULL/DROP_OCCURRED

private:
  // [GATE-7b] Journal segment file: header + packed records, append-only.
  // The whole file is self-describing: file size tells the record count
  // (size = HEADER + k*sizeof(TelemetryRecord)); per-record CRC16 tells
  // record validity. The header is written ONCE at segment creation — a
  // crash between a record write and any metadata update cannot desync
  // truth from bookkeeping because there IS no mutable metadata in the file.
  struct __attribute__((packed)) SegmentHeader {
    char     magic[4];       // "PLSJ"
    uint8_t  version;        // SPOOL_SCHEMA_VERSION
    uint8_t  segIdx;         // 0..MAX_SEGMENTS-1 (filename slot self-check)
    uint32_t generation;     // monotonic append generation (ordering)
    uint32_t headerCrc;      // CRC32 over this header minus this field
  };
  static_assert(sizeof(SegmentHeader) == 14, "journal segment header layout");

  struct SegmentMeta {
    uint8_t  segIdx;         // filename slot
    uint32_t generation;
    uint16_t count;          // records currently in the file (packed)
    bool     active;         // current append target
  };

  // ONE staging record (replaces the 8-slot static RAM ring).
  TelemetryRecord _staging = {};
  TelemetryRecord _criticalRecords[CRITICAL_SPOOL_CAPACITY] = {};
  uint8_t  _criticalHead = 0, _criticalCount = 0;
  uint32_t _dropCount = 0, _replayCount = 0;
  uint32_t _lastSpooledSeq = 0;
  unsigned long _lastReplayMs = 0;
  uint8_t  _replayedThisSec = 0;
  bool     _replaying = false;      // [P1-005] REPLAYING state flag

  // [GATE-7b] Journal state (single-writer: telemetry task only).
  SegmentMeta _segs[MAX_SEGMENTS] = {};
  uint8_t  _segCount = 0;
  uint32_t _nextGeneration = 0;
  uint16_t _journalPending = 0;      // valid, not-yet-confirmed records (journal + critical path adds separately)
  uint32_t _journalBudgetBytes = 0;
  uint16_t _journalCapacityRecords = 0;
  uint8_t  _journalMaxSegments = 0;  // boot-derived (≤ MAX_SEGMENTS)
  uint32_t _journalTargetSec = 0;
  uint32_t _journalEffectiveSec = 0;
  bool     _journalDegraded = false;
  uint32_t _journalEvictions = 0;
  uint32_t _watermark = 0;           // last confirmed-replayed sequence (NVS)
  uint16_t _replaySinceWatermark = 0;
  uint8_t  _replaySeg = 0;           // replay cursor: index into _segs
  uint16_t _replaySlot = 0;          // replay cursor: record slot in that segment
  uint32_t _oldestPendingSeq = 0, _newestPendingSeq = 0;

  bool _nvsLoaded = false;
  uint32_t _nvsLoadFailures = 0;
  uint32_t _nvsWriteFailures = 0;
  PublishCb _publishCb = nullptr;

  // [p.437 → GATE-7b] Journal persistence + recovery.
  bool     _fsLoaded = false;
  uint16_t _fsRestoredCount = 0;
  uint32_t _fsWriteFailures = 0;

  uint16_t _computeCRC(const TelemetryRecord& r) const;
  void _writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                    const char* payload, uint16_t len, uint8_t type);
  void _persistCriticalToNvs();
  void _loadCriticalFromNvs();
  void _clearNvsSpool();

  // [GATE-7b] Journal internals (all LittleFS access from the telemetry/
  // setup context only — single-writer by construction).
  static constexpr size_t RECORD_SIZE = sizeof(TelemetryRecord);
  void _computeCapacity(uint32_t targetSec, uint16_t intervalSec);
  void _applyTarget(uint32_t targetSec, uint16_t intervalSec);
  void _scanJournal();
  void _migrateLegacySnapshot();
  bool _rollSegment();
  bool _appendStaged();
  bool _readRecord(uint8_t segIdx, uint16_t slot, TelemetryRecord& out);
  void _evictOldestSegment();
  void _deleteAllSegments();
  bool _persistWatermark();
  void _loadWatermark();
  const char* _segmentPath(uint8_t segIdx) const;
  uint8_t _firstPendingCursor();     // advance replay cursor past watermark/corrupt slots
  void _recountPending();            // recompute _journalPending from the cursor (single source of truth)
};

extern TelemetrySpool telemetrySpool;

} // namespace Services

#endif // PLTS_SERVICES_TELEMETRY_SPOOL_H
