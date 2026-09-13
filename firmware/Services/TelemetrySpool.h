// =============================================================================
// Services/TelemetrySpool.h — Bounded store-and-forward (brief §22, §23)
// -----------------------------------------------------------------------------
// RAM ring (8 slots, regular telemetry) + NVS ring (4 slots, critical events:
// BOOT/ALARM/FAULT/SAFETY). CRC-16/CCITT per record. Schema-versioned.
// Replay rate-limited (2/sec). Dedup by sequence.
//
// [AUDIT 2026-09 ROUND 5 / p.437] REGULAR TELEMETRY NOW SURVIVES REBOOT.
// Previously only critical events were persisted to NVS — an outage that
// crossed a reboot (power loss, brownout, watchdog) silently destroyed the
// whole regular RAM ring (~40 s of telemetry at the 5 s interval), breaking
// the store-and-forward claim exactly when connectivity AND power were both
// unreliable. The regular ring is now flushed to a CRC-verified LittleFS
// snapshot (/spool_reg.bin, atomic .tmp → rename) with a WEAR-BOUNDED policy:
//   - flushed when the ring transitions drained → non-empty (first record),
//   - then at most once per REGULAR_FLUSH_INTERVAL_MS while records are
//     pending (a reboot mid-outage loses at most one flush interval),
//   - cleared when the ring fully drains.
// Worst-case added flash churn: ~21 KB per flush interval DURING OUTAGES
// ONLY — the healthy path never touches the file.
//
// [AUDIT 2026-09 ROUND 5 / p.438] spool() semantics are now EXPLICIT:
//   SpoolResult::Stored         — record is in the ring, nothing was lost
//   SpoolResult::EvictedOldest  — record is in the ring, the OLDEST pending
//                                 record was dropped to make room (dropCount
//                                 incremented — the caller must not read this
//                                 as "this telemetry is safe")
//   SpoolResult::Rejected       — not stored (invalid/duplicate payload)
// The old bool true/false could not distinguish "stored cleanly" from
// "stored but silently evicted an older record".
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
  Rejected      = 0,   // invalid payload / duplicate sequence — not stored
  Stored        = 1,   // stored; no record was evicted
  EvictedOldest = 2,    // stored; the oldest pending record was dropped
};

// [P1-005] Explicit spool lifecycle state — exposed via diagnostics.
enum class SpoolState : uint8_t {
  EMPTY         = 0,
  BUFFERING     = 1,   // records held, transport unavailable
  REPLAYING     = 2,   // draining now
  FULL          = 3,   // ring full — next insert evicts oldest
  DROP_OCCURRED = 4,   // at least one record dropped since boot
  ERROR         = 5,   // NVS failure on critical persistence
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
  // Raised to 2560; SPOOL_CAPACITY reduced 16→8 to bound RAM (~21 KB).
  char     payload[2560];
};

static constexpr uint8_t SPOOL_SCHEMA_VERSION = 1;

class TelemetrySpool {
public:
  static constexpr uint8_t  SPOOL_CAPACITY = 8;        // RAM ring (was 16 — resized for 2.5 KB payloads)
  static constexpr uint8_t  CRITICAL_SPOOL_CAPACITY = 4; // was 8 — [FW-27] DRAM/NVS budget: critical events are rare (~10/day); 4 slots ≈ 10 KB NVS blob
  static constexpr uint16_t MAX_REPLAY_PER_SEC = 2;
  static constexpr uint16_t MAX_PAYLOAD_LEN = 2560;

  // [p.437] Wear-bounded flush cadence for the LittleFS regular-ring
  // snapshot (outage conditions only — see header comment).
  static constexpr uint32_t REGULAR_FLUSH_INTERVAL_MS = 30000;  // 30 s
  // [p.437] LittleFS snapshot path (atomic .tmp → rename on write).
  static constexpr const char* REGULAR_SNAPSHOT_PATH  = "/spool_reg.bin";

  void begin();
  // [p.438] Explicit outcome — see SpoolResult. bool callers must migrate.
  SpoolResult spool(uint32_t sequence, uint32_t timestamp, const char* payload, uint16_t len);
  bool spoolCritical(uint32_t sequence, uint32_t timestamp,
                     const char* payload, uint16_t len);
  uint8_t replay();  // returns # records replayed
  uint8_t pendingCount() const { return _count; }
  uint8_t criticalPendingCount() const { return _criticalCount; }
  uint32_t dropCount() const { return _dropCount; }
  uint32_t replayCount() const { return _replayCount; }
  bool isEmpty() const { return _count == 0 && _criticalCount == 0; }
  void clear();
  bool verifyRecord(const TelemetryRecord& r) const;
  bool isNvsLoaded() const { return _nvsLoaded; }
  uint32_t nvsLoadFailures() const { return _nvsLoadFailures; }
  uint32_t nvsWriteFailures() const { return _nvsWriteFailures; }

  // [p.437] Regular-ring reboot persistence observability.
  bool     fsLoaded() const { return _fsLoaded; }          // snapshot restored at boot
  uint8_t  fsRestoredCount() const { return _fsRestoredCount; }  // records replayed across reboot
  uint32_t fsWriteFailures() const { return _fsWriteFailures; }

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
  // at-least-once spool replay + GAS-side sequence dedup, not by transport
  // acknowledgement (P1-005 spool semantics).
  typedef bool (*PublishCb)(uint8_t recordType, const char* payload, size_t len);
  void setPublishCallback(PublishCb cb) { _publishCb = cb; }

  SpoolState state() const;   // [P1-005] EMPTY/BUFFERING/REPLAYING/FULL/DROP_OCCURRED

private:
  TelemetryRecord _records[SPOOL_CAPACITY] = {};
  TelemetryRecord _criticalRecords[CRITICAL_SPOOL_CAPACITY] = {};
  uint8_t  _head = 0, _count = 0;
  uint8_t  _criticalHead = 0, _criticalCount = 0;
  uint32_t _dropCount = 0, _replayCount = 0;
  uint32_t _lastSpooledSeq = 0;
  unsigned long _lastReplayMs = 0;
  uint8_t  _replayIdx = 0;
  uint8_t  _replayedThisSec = 0;

  bool _nvsLoaded = false;
  uint32_t _nvsLoadFailures = 0;
  uint32_t _nvsWriteFailures = 0;
  PublishCb _publishCb = nullptr;
  bool _replaying = false;      // [P1-005] REPLAYING state flag

  // [p.437] Regular-ring LittleFS snapshot state
  bool     _fsLoaded = false;          // snapshot existed and was restored at boot
  uint8_t  _fsRestoredCount = 0;       // valid records recovered from the snapshot
  uint32_t _fsWriteFailures = 0;
  uint32_t _lastFlushMs = 0;
  bool     _fsDirty = false;           // ring changed since last flush
  uint32_t _firstPendingMs = 0;        // when the current buffering episode began

  uint16_t _computeCRC(const TelemetryRecord& r) const;
  void _writeRecord(TelemetryRecord& dst, uint32_t seq, uint32_t ts,
                     const char* payload, uint16_t len, uint8_t type);
  void _persistCriticalToNvs();
  void _loadCriticalFromNvs();
  void _clearNvsSpool();

  // [p.437] LittleFS snapshot of the regular ring (atomic .tmp → rename).
  void _flushRegularToFs();
  void _loadRegularFromFs();
  void _clearRegularFs();
};

extern TelemetrySpool telemetrySpool;

} // namespace Services

#endif // PLTS_SERVICES_TELEMETRY_SPOOL_H
