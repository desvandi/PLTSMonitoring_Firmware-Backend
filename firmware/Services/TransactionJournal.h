// =============================================================================
// Services/TransactionJournal.h — NVS-backed dedup for config/calib commands
// -----------------------------------------------------------------------------
// 64-entry ring. 2-phase commit (write valid=0, flip to valid=1). Magic +
// version + CRC32. Used by CommandCanonicalizer to detect DUPLICATE / CONFLICT.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_TRANSACTION_JOURNAL_H
#define PLTS_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>

namespace Services {

enum class TransactionDecision : uint8_t {
  New       = 0,
  Duplicate = 1,
  Conflict   = 2,
};

class TransactionJournal {
public:
  void begin();
  bool isProcessed(const String& requestId);
  String getCommandHash(const String& requestId);
  String getAckJson(const String& requestId);
  bool storeTransaction(const String& requestId, const String& commandHash,
                        const String& ackJson);

  // Look up requestId + compare hash → returns decision + previousAck if DUPLICATE
  TransactionDecision decide(const String& requestId, const String& commandHash,
                              String& outPreviousAck);

  uint8_t getJournalSize() const { return _size; }

private:
  static const uint8_t JOURNAL_SIZE = 64;
  static const uint16_t BLOB_SIZE = 1200;
  // Blob layout: magic(2) + ver(1) + valid(1) + CRC(4) +
  //              idLen(1) + id + hashLen(1) + hash + ackLen(2) + ack

  String _ids[JOURNAL_SIZE];
  String _hashes[JOURNAL_SIZE];
  String _acks[JOURNAL_SIZE];
  bool   _valid[JOURNAL_SIZE];
  uint8_t _size = 0;
  uint8_t _writeIdx = 0;

  int  _findInJournal(const String& requestId);
  bool _saveEntryToNVSAtomic(uint8_t idx);
  void _loadFromNVS();
  void _clearSlotNVS(uint8_t idx);
  uint32_t _computeCRC(const uint8_t* data, size_t len);
};

extern TransactionJournal journal;

} // namespace Services

#endif // PLTS_SERVICES_TRANSACTION_JOURNAL_H
