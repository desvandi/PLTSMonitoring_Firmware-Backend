// =============================================================================
// Services/TransactionJournal.cpp — NVS dedup + 2-phase commit
// =============================================================================
#include "TransactionJournal.h"
#include "../Core/Common.h"
#include "../Utils/Crc.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

namespace Services {

TransactionJournal journal;

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
  _loadFromNVS();
  Serial.printf("[TXN] journal loaded: %u entries\n", _size);
}

bool TransactionJournal::isProcessed(const String& requestId) {
  return _findInJournal(requestId) >= 0;
}
String TransactionJournal::getCommandHash(const String& requestId) {
  int idx = _findInJournal(requestId);
  return idx >= 0 ? _hashes[idx] : String();
}
String TransactionJournal::getAckJson(const String& requestId) {
  int idx = _findInJournal(requestId);
  return idx >= 0 ? _acks[idx] : String();
}

bool TransactionJournal::storeTransaction(const String& requestId,
                                          const String& commandHash,
                                          const String& ackJson) {
  // Find existing slot or new
  int idx = _findInJournal(requestId);
  if (idx >= 0) {
    // Existing — only allow if hash matches (idempotent)
    if (_hashes[idx] != commandHash) return false;
    return true;
  }
  // New slot
  uint8_t newIdx = _writeIdx;
  _writeIdx = (_writeIdx + 1) % JOURNAL_SIZE;
  if (newIdx >= _size) _size = newIdx + 1;
  _ids[newIdx] = requestId;
  _hashes[newIdx] = commandHash;
  _acks[newIdx] = ackJson;
  _valid[newIdx] = true;
  _clearSlotNVS(newIdx);  // R10I-4
  return _saveEntryToNVSAtomic(newIdx);
}

TransactionDecision TransactionJournal::decide(const String& requestId,
                                                const String& commandHash,
                                                String& outPreviousAck) {
  int idx = _findInJournal(requestId);
  if (idx < 0) return TransactionDecision::New;
  if (_hashes[idx] == commandHash) {
    outPreviousAck = _acks[idx];
    return TransactionDecision::Duplicate;
  }
  return TransactionDecision::Conflict;
}

} // namespace Services
