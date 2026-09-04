// =============================================================================
// Services/TransactionJournal.h — NVS-backed dedup for config/calib commands
// -----------------------------------------------------------------------------
// 64-entry ring. 2-phase commit (write valid=0, flip to valid=1). Magic +
// version + CRC32. Used by CommandCanonicalizer to detect DUPLICATE / CONFLICT.
//
// [P2-1 REMEDIATION 2026-09 — RETENTION CONTRACT (cross-layer invariant)]
// The journal guarantees AT-MOST-64-COMMAND dedup memory, NOT a time window.
// Once the ring wraps, an old requestId is forgotten and a byte-identical
// replay of that command would be re-executed as NEW. The cross-layer
// contract that closes this hole is COMMAND FRESHNESS, not journal size:
//
//   1. Every journaled command SHOULD carry `expiresAt` (unix-seconds);
//      senders (PWA/MQTT bridge) set it to issuedAt + bounded window.
//   2. Every ingress — REST (Config/Calibration/ExtraHandlers) and MQTT
//      (MqttConfigReceiver, MqttOtaHandler) — rejects an expired command
//      BEFORE the journal decides (CommandCanonicalizer::isCommandExpired).
//      An expired command can be neither applied NOR safely deduplicated.
//   3. Senders MUST choose a freshness window short enough that at most 64
//      commands are issued within it (typical operator cadence: minutes,
//      not days). A command older than its ring slot's lifetime is, by
//      this contract, dead on arrival.
//
// Consequence for industrial command audit: a transactionId's replay
// protection is guaranteed only up to min(expiresAt, ring eviction). The
// OTA paths layer Ed25519/HMAC verification on top, so even a re-executed
// OTA cannot flash an unsigned image.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_TRANSACTION_JOURNAL_H
#define PLTS_SERVICES_TRANSACTION_JOURNAL_H

#include <Arduino.h>
#include "../Core/Config.h"   // [audit-2 K-2] Core::JOURNAL_SIZE

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
  // [audit-2 K-2 FIX] Use Core::JOURNAL_SIZE (16, not 64). The previous
  // shadowing declaration (64) caused NVS budget mismatch: 64 × 1200 B =
  // ~75 KB does not fit in the 64 KB NVS partition (alongside plts, plts_health,
  // plts_emg, plts_alarm, plts_spool, plts_batt, plts_soc, plts_ota, plts_time
  // namespaces). Silent putBytes failure → dedup degradation → command replay.
  // Config.h:295 already documents "Reduced to 16 slots (~19 KB)".
  static const uint8_t JOURNAL_SIZE = Core::JOURNAL_SIZE;   // 16
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
