// =============================================================================
// Services/TelemetrySpool.cpp
// =============================================================================
#include "TelemetrySpool.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include "../Utils/Crypto.h"
#include "../Core/Config.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <cstring>
#include <cstdio>

namespace Services {

TelemetrySpool telemetrySpool;

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

void TelemetrySpool::begin() {
  _head = _count = 0;
  _criticalHead = _criticalCount = 0;
  _dropCount = _replayCount = 0;
  _lastReplayMs = 0;
  _replayIdx = 0;
  _replayedThisSec = 0;
  _lastSpooledSeq = 0;
  _loadCriticalFromNvs();
  // [p.437] Restore the regular ring from the LittleFS snapshot — telemetry
  // buffered before a reboot during an outage now replays after boot.
  _loadRegularFromFs();
}

SpoolResult TelemetrySpool::spool(uint32_t sequence, uint32_t timestamp,
                                   const char* payload, uint16_t len) {
  if (sequence == _lastSpooledSeq) return SpoolResult::Rejected;  // dedup
  _lastSpooledSeq = sequence;
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) {
    _dropCount++;
    return SpoolResult::Rejected;
  }
  SpoolResult result;
  if (_count < SPOOL_CAPACITY) {
    _writeRecord(_records[_head], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::Telemetry);
    _head = (_head + 1) % SPOOL_CAPACITY;
    _count++;
    result = SpoolResult::Stored;
  } else {
    // Overwrite oldest [p.438] — honest result: the caller is told a record
    // was evicted, not told "accepted" as the old bool-return implied.
    _writeRecord(_records[_head], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::Telemetry);
    _head = (_head + 1) % SPOOL_CAPACITY;
    _dropCount++;
    result = SpoolResult::EvictedOldest;
  }
  _fsDirty = true;   // [p.437] ring changed — snapshot is stale until flushed
  // [p.437] Wear-bounded LittleFS flush of the regular ring:
  //   first buffered record of an episode → flush immediately (bounds the
  //   loss window for short outages), then at most one flush per interval.
  if (_count == 1) {
    _firstPendingMs = millis();
    _flushRegularToFs();
  } else if ((millis() - _lastFlushMs) >= REGULAR_FLUSH_INTERVAL_MS) {
    _flushRegularToFs();
  }
  return result;
}

bool TelemetrySpool::spoolCritical(uint32_t sequence, uint32_t timestamp,
                                    const char* payload, uint16_t len) {
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) {
    _dropCount++; return false;
  }
  if (_criticalCount < CRITICAL_SPOOL_CAPACITY) {
    _writeRecord(_criticalRecords[_criticalHead], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::CriticalEvent);
    _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
    _criticalCount++;
  } else {
    _writeRecord(_criticalRecords[_criticalHead], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::CriticalEvent);
    _criticalHead = (_criticalHead + 1) % CRITICAL_SPOOL_CAPACITY;
  }
  _persistCriticalToNvs();
  return true;
}

uint8_t TelemetrySpool::replay() {
  unsigned long now = millis();
  if (now - _lastReplayMs >= 1000) {
    _lastReplayMs = now;
    _replayedThisSec = 0;
  }
  if (_replayedThisSec >= MAX_REPLAY_PER_SEC) return 0;
  if (!_publishCb) return 0;

  _replaying = true;

  // Critical records first (BOOT/ALARM/FAULT/SAFETY outrank telemetry).
  // [P1-005] Oldest-first ordering: index 0 of the logical queue is the
  // OLDEST record — replaying newest-first would worsen historical ordering.
  if (_criticalCount > 0) {
    for (uint8_t i = 0; i < _criticalCount; i++) {
      uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount + i) % CRITICAL_SPOOL_CAPACITY;
      const TelemetryRecord& r = _criticalRecords[idx];
      if (!verifyRecord(r)) {
        // Corrupt record — remove it from the queue (advance tail).
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
        _replaying = _criticalCount > 0 || _count > 0;
        return 1;
      }
      // Delivery not confirmed — stop; retry on the next tick (records stay).
      break;
    }
  }
  // Then regular records (oldest first).
  if (_count > 0) {
    uint8_t idx = (_head + SPOOL_CAPACITY - _count) % SPOOL_CAPACITY;
    const TelemetryRecord& r = _records[idx];
    if (verifyRecord(r)) {
      // [P1-005 / audit p.432] Record removed ONLY on confirmed socket write
      // (PubSubClient QoS-0 publish — NOT a broker PUBACK). Delivery is
      // at-least-once via replay + GAS sequence dedup.
      if (_publishCb(r.recordType, r.payload, r.payloadLen)) {
        _count--;
        _replayCount++;
        _replayedThisSec++;
        _replaying = _count > 0;
        // [p.437] Keep the LittleFS snapshot in step with the drain — a
        // stale snapshot would resurrect already-replayed records after a
        // later reboot (GAS dedup absorbs it, but the honest state is "gone").
        if (_count == 0) {
          _clearRegularFs();
          _fsDirty = false;
        } else if ((millis() - _lastFlushMs) >= REGULAR_FLUSH_INTERVAL_MS) {
          _flushRegularToFs();
        }
        return 1;
      }
    } else {
      _count--;  // drop corrupt (advance tail by decrementing count)
      _dropCount++;
    }
  }
  _replaying = _criticalCount > 0 || _count > 0;
  return 0;
}

uint32_t TelemetrySpool::oldestSequence() const {
  if (_count == 0 && _criticalCount == 0) return 0;
  if (_criticalCount > 0) {
    uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[idx])) return _criticalRecords[idx].sequence;
  }
  if (_count > 0) {
    uint8_t idx = (_head + SPOOL_CAPACITY - _count) % SPOOL_CAPACITY;
    if (verifyRecord(_records[idx])) return _records[idx].sequence;
  }
  return 0;
}

uint32_t TelemetrySpool::newestSequence() const {
  if (_count > 0) {
    uint8_t idx = (_head + SPOOL_CAPACITY - 1) % SPOOL_CAPACITY;
    if (verifyRecord(_records[idx])) return _records[idx].sequence;
  }
  if (_criticalCount > 0) {
    uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - 1) % CRITICAL_SPOOL_CAPACITY;
    if (verifyRecord(_criticalRecords[idx])) return _criticalRecords[idx].sequence;
  }
  return 0;
}

SpoolState TelemetrySpool::state() const {
  if (_nvsWriteFailures > 0) return SpoolState::ERROR;
  if (_dropCount > 0)       return SpoolState::DROP_OCCURRED;
  if (_replaying && (_count > 0 || _criticalCount > 0)) return SpoolState::REPLAYING;
  if (_count >= SPOOL_CAPACITY || _criticalCount >= CRITICAL_SPOOL_CAPACITY) return SpoolState::FULL;
  if (_count > 0 || _criticalCount > 0) return SpoolState::BUFFERING;
  return SpoolState::EMPTY;
}

void TelemetrySpool::clear() {
  _head = _count = 0;
  _criticalHead = _criticalCount = 0;
  _clearNvsSpool();
  _clearRegularFs();   // [p.437]
  _fsDirty = false;
}

// ============================================================================
// [p.437] Regular-ring LittleFS snapshot — binary-safe atomic write
// (.tmp → rename), CRC32-guarded. Format:
//   magic 'P','L','S','R' | version u8 | head u8 | count u8 | rsv u8 |
//   crc32 u32 (over header-minus-crc + the FULL records array) |
//   TelemetryRecord[SPOOL_CAPACITY]
// The whole array (not just _count entries) is written so the snapshot is
// a fixed size; on load, slots outside [head-count, head) are ignored.
// ============================================================================
void TelemetrySpool::_flushRegularToFs() {
  struct __attribute__((packed)) SnapHeader {
    char     magic[4];
    uint8_t  version;
    uint8_t  head;
    uint8_t  count;
    uint8_t  reserved;
    uint32_t crc32;
  };
  static_assert(sizeof(SnapHeader) == 12, "snapshot header layout");

  SnapHeader hdr = {};
  memcpy(hdr.magic, "PLSR", 4);
  hdr.version = SPOOL_SCHEMA_VERSION;
  hdr.head = _head;
  hdr.count = _count;
  uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
  if (_count > 0) {
    crc = Utils::crc32((const uint8_t*)_records, sizeof(_records), crc);
  }
  hdr.crc32 = crc;

  const char* tmp = "/spool_reg.tmp";
  File f = LittleFS.open(tmp, "w", true);
  if (!f) { _fsWriteFailures++; return; }
  size_t w1 = f.write((const uint8_t*)&hdr, sizeof(hdr));
  size_t w2 = f.write((const uint8_t*)_records, sizeof(_records));
  f.close();
  if (w1 != sizeof(hdr) || w2 != sizeof(_records)) {
    LittleFS.remove(tmp);
    _fsWriteFailures++;
    return;
  }
  // Atomic swap (rename semantics on LittleFS replace the destination).
  if (!LittleFS.rename(tmp, REGULAR_SNAPSHOT_PATH)) {
    LittleFS.remove(tmp);
    _fsWriteFailures++;
    return;
  }
  _fsDirty = false;
  _lastFlushMs = millis();
}

void TelemetrySpool::_loadRegularFromFs() {
  struct __attribute__((packed)) SnapHeader {
    char     magic[4];
    uint8_t  version;
    uint8_t  head;
    uint8_t  count;
    uint8_t  reserved;
    uint32_t crc32;
  };
  if (!LittleFS.exists(REGULAR_SNAPSHOT_PATH)) return;
  File f = LittleFS.open(REGULAR_SNAPSHOT_PATH, "r");
  if (!f) { _fsWriteFailures++; return; }
  SnapHeader hdr = {};
  size_t rh = f.read((uint8_t*)&hdr, sizeof(hdr));
  if (rh != sizeof(hdr) || memcmp(hdr.magic, "PLSR", 4) != 0 ||
      hdr.version != SPOOL_SCHEMA_VERSION ||
      hdr.count > SPOOL_CAPACITY || hdr.head >= SPOOL_CAPACITY) {
    f.close();
    _clearRegularFs();   // corrupt/foreign snapshot — remove, honest
    return;
  }
  // [p.437][RAM BUDGET] The snapshot IS the records array — read it straight
  // into _records (zero extra buffer; a second static array would overflow
  // DRAM, and a heap copy is unnecessary at boot).
  size_t rr = f.read((uint8_t*)_records, sizeof(_records));
  f.close();
  if (rr != sizeof(_records)) { _clearRegularFs(); return; }
  uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
  crc = Utils::crc32((const uint8_t*)_records, sizeof(_records), crc);
  if (crc != hdr.crc32) {
    // Snapshot corrupt — discard rather than replay unverifiable data.
    _clearRegularFs();
    return;
  }
  // Restore the ring: walk the logical queue oldest→newest, compacting VALID
  // records to the front of the array in place (target idx <= source idx, so
  // the forward struct copy is overlap-safe). Per-record CRC failures drop
  // that record honestly (dropCount++) instead of discarding the snapshot.
  uint8_t valid = 0;
  for (uint8_t i = 0; i < hdr.count; i++) {
    uint8_t src = (hdr.head + SPOOL_CAPACITY - hdr.count + i) % SPOOL_CAPACITY;
    if (verifyRecord(_records[src])) {
      _records[valid] = _records[src];
      valid++;
    } else {
      _dropCount++;
    }
  }
  _count = valid;
  _head = _count % SPOOL_CAPACITY;   // compaction: head follows count
  _fsLoaded = (_count > 0);
  _fsRestoredCount = _count;
  if (_count > 0) {
    _fsDirty = true;
    _firstPendingMs = millis();
    Serial.printf("[SPOOL] restored %u regular telemetry record(s) from "
                  "LittleFS snapshot (pre-reboot outage buffered data)\n", _count);
  }
}

void TelemetrySpool::_clearRegularFs() {
  LittleFS.remove(REGULAR_SNAPSHOT_PATH);
  LittleFS.remove("/spool_reg.tmp");
}

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
    // Compact: drop corrupted slots
    uint8_t valid = 0;
    for (uint8_t i = 0; i < _criticalCount; i++) {
      uint8_t idx = (_criticalHead + CRITICAL_SPOOL_CAPACITY - _criticalCount + i) % CRITICAL_SPOOL_CAPACITY;
      if (!verifyRecord(_criticalRecords[idx])) {
        // Mark slot invalid by zeroing CRC
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
