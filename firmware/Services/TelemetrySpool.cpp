// =============================================================================
// Services/TelemetrySpool.cpp
// =============================================================================
#include "TelemetrySpool.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include "../Utils/Crypto.h"
#include "../Core/Config.h"
#include <Preferences.h>
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
}

bool TelemetrySpool::spool(uint32_t sequence, uint32_t timestamp,
                            const char* payload, uint16_t len) {
  if (sequence == _lastSpooledSeq) return false;  // dedup
  _lastSpooledSeq = sequence;
  if (!payload || len == 0 || len > MAX_PAYLOAD_LEN) {
    _dropCount++;
    return false;
  }
  if (_count < SPOOL_CAPACITY) {
    _writeRecord(_records[_head], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::Telemetry);
    _head = (_head + 1) % SPOOL_CAPACITY;
    _count++;
    return true;
  } else {
    // Overwrite oldest
    _writeRecord(_records[_head], sequence, timestamp, payload, len,
                 (uint8_t)SpoolRecordType::Telemetry);
    _head = (_head + 1) % SPOOL_CAPACITY;
    _dropCount++;
    return true;  // accepted but evicted older
  }
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
      // [P1-005] Removal happens ONLY after the callback confirms delivery.
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
      // [P1-005] Record removed ONLY on confirmed delivery (QoS PUBACK).
      if (_publishCb(r.recordType, r.payload, r.payloadLen)) {
        _count--;
        _replayCount++;
        _replayedThisSec++;
        _replaying = _count > 0;
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
