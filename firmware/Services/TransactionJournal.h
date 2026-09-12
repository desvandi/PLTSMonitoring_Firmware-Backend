// =============================================================================
// Services/TransactionJournal.h — NVS-backed transaction identity + durable
// terminal outcomes
// -----------------------------------------------------------------------------
// 16-entry ring (Core::JOURNAL_SIZE, ~19 KB of the 64 KB NVS partition).
// 2-phase commit (write valid=0, flip to valid=1). Magic + version + CRC32.
//
// Two roles:
//
//   1. DEDUP — CommandCanonicalizer consults the journal to detect
//      DUPLICATE / CONFLICT on requestId + commandHash.
//
//   2. DURABLE TERMINAL OUTCOMES (audit p.413-415) — when the relay executor
//      reaches a final verdict, it UPDATES the stored ack of that
//      transaction with the terminal result (updateAck). The journal — not
//      the 8-entry RAM result ring — is the AUTHORITATIVE final-outcome
//      store:
//
//          GET /api/relays/transactions/{id}
//            1. RAM result ring   (fast cache, latest 8)
//            2. this journal      (durable — survives eviction + reboot)
//            3. honest UNKNOWN   (never an eternal PENDING)
//
//      Submission acks carry a BOOT MARKER (bootCount) so reconciliation can
//      distinguish "queued in the current boot" (still in flight) from
//      "accepted in a previous boot that restarted before the executor
//      reached terminal" (lost at reboot → honest UNKNOWN).
//
//      Mutations from both networkTask (storeTransaction) and relayTask
//      (updateAck) are serialized by an internal mutex — the RAM mirror
//      (ids/hashes/acks) and the slot-eviction pointer are shared state.
//
// [P2-1 REMEDIATION 2026-09 — RETENTION CONTRACT (cross-layer invariant)]
// The journal guarantees AT-MOST-16-COMMAND dedup memory, NOT a time window.
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

  // [audit p.414] Monotonic boot counter — NVS-persisted, incremented on every
  // begin(). Relay submission acks stamp it so reconciliation can distinguish
  // "in flight this boot" (QUEUED) from "lost at reboot" (UNKNOWN).
  uint32_t bootCount() const { return _bootCount; }

  // [audit p.413/414/415] Update the stored ack of a journaled transaction
  // with its TERMINAL outcome — the journal is the authoritative final-result
  // store; the executor's 8-entry RAM result ring is only a fast cache.
  // Identity (commandHash) is immutable — only the outcome evolves. If the
  // transaction is not yet journaled (terminal verdict raced ahead of the
  // ingress journal write, or that write failed while the command still
  // executed), the terminal record is CREATED on demand under the same
  // identity — a later ingress storeTransaction for this id then lands in the
  // idempotent duplicate branch and never overwrites the terminal ack with a
  // QUEUED one. RAM is mutated only AFTER the NVS commit succeeds. Returns
  // false only on identity mismatch or NVS commit failure — observable
  // degradation, never a wrong answer.
  bool updateAck(const String& requestId, const String& commandHash,
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

  // [audit p.413-415] Shared staging/commit path for NEW entries — used by
  // storeTransaction (networkTask ingress) AND updateAck's create-on-demand
  // (relayTask terminal verdict). Assumes _mutex is HELD by the caller.
  bool _storeNewEntryLocked(const String& requestId, const String& commandHash,
                            const String& ackJson);

  // [audit p.413-415] Cross-task serialization — storeTransaction runs in
  // networkTask (ingress), updateAck runs in relayTask (executor terminal
  // verdict). Both mutate the shared RAM mirror and the NVS slot contents.
  void* _mutex = nullptr;  // SemaphoreHandle_t (kept opaque in header)
  void _lock();
  void _unlock();
  uint32_t _bootCount = 0;
};

extern TransactionJournal journal;

} // namespace Services

#endif // PLTS_SERVICES_TRANSACTION_JOURNAL_H
