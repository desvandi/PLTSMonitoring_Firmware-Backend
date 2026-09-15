// =============================================================================
// Services/TransactionJournal.cpp — NVS dedup + 2-phase commit + durable
//                                          terminal outcomes (audit p.413-415)
// =============================================================================
#include "TransactionJournal.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include "LogService.h"   // [p.473] rate-limited CRIT log on the fail-closed path
#include <Preferences.h>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Services {

TransactionJournal journal;

// [audit p.413-415] Cross-task serialization. storeTransaction is called from
// networkTask (REST/MQTT ingress); updateAck is called from relayTask (the
// executor's terminal verdict). Without this lock, a concurrent slot
// eviction (storeTransaction wrapping _writeIdx) and a terminal update
// (updateAck re-committing the same slot) could interleave and leave the NVS
// blob and the RAM mirror disagreeing about which transaction owns the slot.
//
// [AUDIT 2026-09 ROUND 10 / p.473] FAIL-CLOSED acquisition — same shape as
// AlarmRegistry::_lock() (round-9 p.471): retry-create ONCE; still null →
// count (atomic) + rate-limited CRIT log via LogService (different mutex,
// no cycle) + return false. The caller REFUSES the operation — the journal
// never mutates RAM or NVS unsynchronized.
bool TransactionJournal::_lock() {
  if (_mutex == nullptr) _mutex = xSemaphoreCreateMutex();   // retry-create ONCE
  if (_mutex != nullptr) {
    xSemaphoreTake((SemaphoreHandle_t)_mutex, portMAX_DELAY);
    return true;
  }
  _lockFailures.fetch_add(1, std::memory_order_relaxed);
  uint32_t last = _lastLockFailLogMs.load(std::memory_order_relaxed);
  uint32_t now = millis();
  if (now - last > 60000UL &&
      _lastLockFailLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    Log.append(Core::LogType::StorageError,
               String("[TXN] journal mutex UNAVAILABLE — operation REJECTED "
                      "(fail-closed, p.473); lockFailures=") +
                   _lockFailures.load(std::memory_order_relaxed),
               -1);
  }
  return false;
}
void TransactionJournal::_unlock() {
  // Only ever called after _lock() returned TRUE — the handle is non-null
  // and HELD here by construction.
  xSemaphoreGive((SemaphoreHandle_t)_mutex);
}


static const uint8_t BLOB_MAGIC1 = 0x54;  // 'T'
static const uint8_t BLOB_MAGIC2 = 0x4A;  // 'J'
static const uint8_t BLOB_VERSION = 1;

int TransactionJournal::_findInJournal(const String& requestId) {
  for (uint8_t i = 0; i < _size; i++) {
    if (_valid[i] && _ids[i] == requestId) return i;
  }
  return -1;
}

uint32_t TransactionJournal::_computeCRC(const uint8_t* data, size_t len) {
  return Utils::crc32(data, len);
}

bool TransactionJournal::_saveEntryToNVSAtomic(uint8_t idx) {
  // Serialize blob
  uint8_t blob[BLOB_SIZE] = {0};
  size_t off = 0;
  blob[off++] = BLOB_MAGIC1;
  blob[off++] = BLOB_MAGIC2;
  blob[off++] = BLOB_VERSION;
  blob[off++] = 0;  // valid = 0 (phase 1)
  off += 4;          // CRC placeholder
  // requestId
  uint8_t idLen = (uint8_t)_ids[idx].length();
  if (idLen > 64) idLen = 64;
  blob[off++] = idLen;
  memcpy(blob + off, _ids[idx].c_str(), idLen);
  off += idLen;
  // commandHash
  uint8_t hashLen = (uint8_t)_hashes[idx].length();
  if (hashLen > 64) hashLen = 64;
  blob[off++] = hashLen;
  memcpy(blob + off, _hashes[idx].c_str(), hashLen);
  off += hashLen;
  // ackJson (2-byte LE length, max 1024)
  uint16_t ackLen = (uint16_t)_acks[idx].length();
  if (ackLen > 1024) ackLen = 1024;
  blob[off++] = (uint8_t)(ackLen & 0xFF);
  blob[off++] = (uint8_t)(ackLen >> 8);
  memcpy(blob + off, _acks[idx].c_str(), ackLen);
  off += ackLen;

  // Compute CRC over payload (bytes 8..off)
  uint32_t crc = _computeCRC(blob + 8, off - 8);
  blob[4] = (uint8_t)(crc & 0xFF);
  blob[5] = (uint8_t)((crc >> 8) & 0xFF);
  blob[6] = (uint8_t)((crc >> 16) & 0xFF);
  blob[7] = (uint8_t)((crc >> 24) & 0xFF);

  Preferences p;
  if (!p.begin("plts_txn", false)) return false;
  char key[8];
  snprintf(key, sizeof(key), "t_%u", idx);
  bool ok = p.putBytes(key, blob, BLOB_SIZE) == BLOB_SIZE;
  p.end();
  if (!ok) return false;

  // Phase 2: flip valid byte to 1
  blob[3] = 1;
  if (!p.begin("plts_txn", false)) return false;
  ok = p.putBytes(key, blob, BLOB_SIZE) == BLOB_SIZE;
  p.end();
  return ok;
}

void TransactionJournal::_clearSlotNVS(uint8_t idx) {
  Preferences p;
  if (!p.begin("plts_txn", false)) return;
  char key[8];
  snprintf(key, sizeof(key), "t_%u", idx);
  p.remove(key);
  p.end();
}

void TransactionJournal::_loadFromNVS() {
  Preferences p;
  if (!p.begin("plts_txn", true)) return;
  _size = 0;
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    char key[8];
    snprintf(key, sizeof(key), "t_%u", i);
    uint8_t blob[BLOB_SIZE] = {0};
    size_t got = p.getBytes(key, blob, BLOB_SIZE);
    if (got != BLOB_SIZE) continue;
    if (blob[0] != BLOB_MAGIC1 || blob[1] != BLOB_MAGIC2) continue;
    if (blob[2] != BLOB_VERSION) continue;
    if (blob[3] != 1) continue;  // not committed
    // Verify CRC
    uint32_t storedCrc = (uint32_t)blob[4] | ((uint32_t)blob[5] << 8) |
                          ((uint32_t)blob[6] << 16) | ((uint32_t)blob[7] << 24);
    uint32_t calcCrc = _computeCRC(blob + 8, got - 8);
    if (storedCrc != calcCrc) continue;
    // Parse
    size_t off = 8;
    uint8_t idLen = blob[off++];
    if (idLen > 64) continue;
    _ids[i] = String((const char*)(blob + off)).substring(0, idLen);
    off += idLen;
    uint8_t hashLen = blob[off++];
    if (hashLen > 64) continue;
    _hashes[i] = String((const char*)(blob + off)).substring(0, hashLen);
    off += hashLen;
    uint16_t ackLen = (uint16_t)blob[off] | ((uint16_t)blob[off + 1] << 8);
    off += 2;
    if (ackLen > 1024) continue;
    _acks[i] = String((const char*)(blob + off)).substring(0, ackLen);
    _valid[i] = true;
    if (i >= _size) _size = i + 1;
  }
  _writeIdx = _size % JOURNAL_SIZE;
  p.end();
}

void TransactionJournal::begin() {
  for (uint8_t i = 0; i < JOURNAL_SIZE; i++) {
    _ids[i] = ""; _hashes[i] = ""; _acks[i] = "";
    _valid[i] = false;
  }
  _size = 0; _writeIdx = 0;

  // [audit p.414] Boot marker — incremented on EVERY boot and persisted, so
  // relay reconciliation can distinguish "queued in the current boot" (still
  // in flight) from "accepted in a previous boot that restarted before the
  // executor reached terminal" (lost at reboot → honest UNKNOWN).
  {
    Preferences p;
    if (p.begin("plts_txn", false)) {
      _bootCount = p.getUInt("boot", 0) + 1;
      p.putUInt("boot", _bootCount);
      p.end();
    }
  }

  // [audit p.413-415] Mutations now arrive from two execution contexts —
  // create the cross-task lock BEFORE any concurrent access can happen
  // (begin() runs in single-tasked setup context).
  //
  // [p.473] BOOT GUARD — fail-closed. The journal is the idempotency/
  // durability backbone of the relay executor: without serialization,
  // storeTransaction (networkTask) and updateAck (relayTask) interleave over
  // the RAM mirror and the NVS slots — dedup and terminal-state consistency
  // break exactly as the auditor described. Refuse to bring the journal (and
  // the command paths behind it) up: log FATAL and halt WITHOUT feeding the
  // task watchdog → deterministic TWDT panic reset, honest crash-chain
  // (BOOT/CRASHLOOP), never an unsynchronized zombie.
  if (_mutex == nullptr) {
    _mutex = xSemaphoreCreateMutex();
  }
  if (_mutex == nullptr) {
    Serial.println(F("[FATAL] TransactionJournal mutex creation failed (heap exhausted at boot) "
                    "— refusing to enter multi-task state (p.473 fail-closed)"));
    Serial.flush();
    Log.append(Core::LogType::StorageError,
               String("[FATAL] TransactionJournal mutex creation failed — boot REFUSED "
                      "(p.473): journal serialization guards dedup + terminal-state "
                      "consistency; halting for TWDT panic reset"),
               -1);
    while (true) {
      delay(10000);   // no esp_task_wdt_reset() on purpose → panic reset
    }
  }

  _loadFromNVS();
  Serial.printf("[TXN] journal loaded: %u entries (boot %lu)\n",
                _size, (unsigned long)_bootCount);
}

bool TransactionJournal::isProcessed(const String& requestId) {
  // [p.473] Fail-closed: TRUE = "treat as processed" = refuse to re-execute.
  // For a dedup oracle, "cannot check" must err on the safe side (a command
  // re-run without dedup could double-apply a relay mutation).
  if (!_lock()) return true;
  bool r = _findInJournal(requestId) >= 0;
  _unlock();
  return r;
}
String TransactionJournal::getCommandHash(const String& requestId) {
  // [p.473] Fail-closed: empty String = honest unknown (RelayHandlers'
  // reconciliation already falls through to UNKNOWN on empty).
  if (!_lock()) return String();
  int idx = _findInJournal(requestId);
  String r = idx >= 0 ? _hashes[idx] : String();
  _unlock();
  return r;
}
String TransactionJournal::getAckJson(const String& requestId) {
  // [p.473] Fail-closed: empty String = honest unknown (GET transactions
  // falls through the journal to the honest UNKNOWN verdict).
  if (!_lock()) return String();
  int idx = _findInJournal(requestId);
  String r = idx >= 0 ? _acks[idx] : String();
  _unlock();
  return r;
}

bool TransactionJournal::storeTransaction(const String& requestId,
                                          const String& commandHash,
                                          const String& ackJson) {
  // [audit p.413-415] networkTask ingress path — serialized against the
  // relayTask terminal-update path (updateAck).
  // [p.473] Fail-closed: false = "not durable" (the REST/MQTT ingresses
  // already surface this as HTTP 503 / honest degradation — never a silent
  // DUPLICATE claim for a transaction that was never stored).
  if (!_lock()) return false;
  // Find existing slot or new
  int idx = _findInJournal(requestId);
  if (idx >= 0) {
    // Existing — only allow if hash matches (idempotent). The ack is NOT
    // rewritten: if the executor already persisted a TERMINAL ack here
    // (updateAck raced ahead — see create-on-demand), it must survive.
    bool ok = _hashes[idx] == commandHash;
    _unlock();
    return ok;
  }
  // New slot — [CORE-04 audit p.319-321] transactional from the caller's
  // perspective: RAM is mutated only AFTER the NVS commit succeeds. The old
  // code wrote RAM first, so a failed NVS commit still produced DUPLICATE
  // decisions for a transaction that was never durable (false durability —
  // after reboot the retry would be treated as NEW and re-executed).
  //
  // [audit p.323] Do NOT _clearSlotNVS() before the new write — NVS
  // putBytes() replaces the record under the same key atomically at the
  // storage layer; deleting first only widens the power-loss window in
  // which the previous record is already gone and the new one not yet
  // committed.
  bool ok = _storeNewEntryLocked(requestId, commandHash, ackJson);
  _unlock();
  return ok;
}

// [audit p.413-415] Shared NEW-entry staging + 2-phase NVS commit. The
// caller MUST hold _mutex. On NVS commit failure the previous occupant of
// the wrapped slot is restored exactly (a ring slot may still hold a live
// entry) — RAM must never claim durability it does not have.
bool TransactionJournal::_storeNewEntryLocked(const String& requestId,
                                              const String& commandHash,
                                              const String& ackJson) {
  uint8_t newIdx = _writeIdx;

  String prevId = _ids[newIdx];
  String prevHash = _hashes[newIdx];
  String prevAck = _acks[newIdx];
  bool prevValid = _valid[newIdx];

  _ids[newIdx] = requestId;
  _hashes[newIdx] = commandHash;
  _acks[newIdx] = ackJson;
  _valid[newIdx] = true;

  if (!_saveEntryToNVSAtomic(newIdx)) {
    // ROLLBACK — restore the previous occupant of this slot.
    _ids[newIdx] = prevId;
    _hashes[newIdx] = prevHash;
    _acks[newIdx] = prevAck;
    _valid[newIdx] = prevValid;
    return false;
  }
  // NVS commit succeeded — advance the write pointer and size now.
  _writeIdx = (newIdx + 1) % JOURNAL_SIZE;
  if (newIdx >= _size) _size = newIdx + 1;
  return true;
}

// [audit p.413/414/415] Terminal-outcome persistence. The relay executor
// calls this when a journaled transaction reaches its final state; the
// updated ack becomes the durable, reboot-safe record that
// GET /api/relays/transactions/{id} falls back to once the 8-entry RAM
// result ring has evicted the transaction (or after a reboot wiped the
// ring). Identity is deliberately NOT touched — commandHash stays immutable;
// only the outcome evolves.
bool TransactionJournal::updateAck(const String& requestId,
                                   const String& commandHash,
                                   const String& ackJson) {
  // [p.473] Fail-closed: false = terminal verdict NOT durably recorded —
  // observable degradation (the verdict stays in the RAM ring), never a
  // wrong durable answer.
  if (!_lock()) return false;
  int idx = _findInJournal(requestId);
  if (idx >= 0) {
    if (_hashes[idx] != commandHash) {
      // Identity mismatch — refuse to touch the entry. The terminal verdict
      // stays in the RAM ring only (observable degradation, never a wrong
      // durable answer).
      _unlock();
      return false;
    }
    String prevAck = _acks[idx];
    _acks[idx] = ackJson;
    if (!_saveEntryToNVSAtomic(idx)) {
      _acks[idx] = prevAck;  // ROLLBACK — no false durability in RAM either
      _unlock();
      return false;
    }
    _unlock();
    return true;
  }
  // CREATE-ON-DEMAND (audit p.413) — the executor's terminal verdict raced
  // AHEAD of the ingress journal write (queueCommand precedes
  // storeTransaction in the REST handler), or that ingress write failed
  // outright (the 503 path) while the command still executed. Store the
  // terminal record now under the SAME identity: a later storeTransaction
  // for this id/hash lands in the idempotent duplicate branch above and can
  // never overwrite this terminal ack with a stale QUEUED one. Without this,
  // such a transaction would keep a QUEUED journal entry forever while its
  // real outcome lived only in the evictable RAM ring — the exact
  // "terminal looks non-terminal" defect of audit p.413.
  bool ok = _storeNewEntryLocked(requestId, commandHash, ackJson);
  _unlock();
  return ok;
}

TransactionDecision TransactionJournal::decide(const String& requestId,
                                                const String& commandHash,
                                                String& outPreviousAck) {
  // [p.473] Fail-closed: Unavailable — the ingress REJECTS (REST 503 /
  // MQTT JOURNAL_UNAVAILABLE). Deliberately NOT Conflict/Duplicate (those
  // claim knowledge) and NOT New (that would execute without dedup).
  if (!_lock()) {
    outPreviousAck = String();
    return TransactionDecision::Unavailable;
  }
  int idx = _findInJournal(requestId);
  if (idx < 0) {
    _unlock();
    return TransactionDecision::New;
  }
  if (_hashes[idx] == commandHash) {
    outPreviousAck = _acks[idx];
    _unlock();
    return TransactionDecision::Duplicate;
  }
  _unlock();
  return TransactionDecision::Conflict;
}

} // namespace Services
