// =============================================================================
// Services/CommandCanonicalizer.h — PD-001 canonical command + transaction id
// -----------------------------------------------------------------------------
// Shared canonical hash for REST + MQTT commands. Whitelist (fail-closed):
// unknown types/actions/fields REJECTED. Excludes requestId/transactionId/
// issuedAt/expiresAt from hash (envelope-only).
//
// [PRODUCTION-GRADE CORE-01..03 2026-09] — audit p.309-317 remediation:
//   CORE-01: field whitelist is scoped to the EXACT (type, action) tuple —
//            a field allowed for relay.pulse is NOT allowed for relay.on.
//   CORE-02: validateCommandEnvelope() — version, transactionId, issuedAt,
//            expiresAt are MANDATORY for every mutation (REST + MQTT).
//   CORE-03: every accepted semantic field participates in the canonical
//            hash (guaranteed by the exact-tuple whitelist — a field that is
//            allowed is always emitted, a field that is not emitted is
//            rejected).
//   Numeric hardening: NaN/Infinity REJECTED, -0.0 normalized to 0.0.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_COMMAND_CANONICALIZER_H
#define PLTS_SERVICES_COMMAND_CANONICALIZER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "TransactionJournal.h"
#include "../Utils/Crypto.h"

namespace Services {

static const uint8_t CANONICAL_COMMAND_VERSION = 1;
static const size_t MAX_TRANSACTION_ID_LEN = 64;
static const size_t MIN_TRANSACTION_ID_LEN = 1;

struct CanonicalResult {
  bool ok;
  String transactionId;
  String commandHash;
  String canonicalString;
  String errorMessage;
};

struct DecisionResult {
  TransactionDecision decision;
  String previousHash;
  String previousAckJson;
};

class CommandCanonicalizer {
public:
  static bool validateTransactionId(const String& tid, String& errOut);
  static bool validateProtocolVersion(int version, String& errOut);

  // [P2-1 REMEDIATION 2026-09 — retention/freshness contract]
  // Journaled commands are deduplicated ONLY while their requestId sits in
  // the journal ring (see TransactionJournal.h). A command whose expiresAt
  // is in the past can no longer be safely deduplicated NOR safely applied.
  // Every ingress (REST + MQTT) MUST call this BEFORE decideTransaction().
  // Returns TRUE when the command is expired (reject). A device without a
  // usable clock cannot evaluate freshness and fails open on THIS check only
  // (journal + HMAC auth remain in force).
  static bool isCommandExpired(JsonDocument& doc, String& errOut);

  // [CORE-02] Mandatory mutation envelope: version + transactionId/requestId
  // + issuedAt + expiresAt must ALL be present and sane. Envelope presence is
  // enforced even when the clock is unusable — the freshness *evaluation* may
  // be UNKNOWN, but the freshness *claim* is always required, so replay
  // protection never silently degrades to journal-retention-only.
  //   doc    — full command envelope
  //   errOut — human-readable rejection reason
  // Returns false + errOut when the envelope is incomplete/invalid.
  static bool validateCommandEnvelope(JsonDocument& doc, String& errOut);

  static CanonicalResult canonicalizeAndHash(JsonDocument& doc);
  static DecisionResult decideTransaction(const String& tid, const String& hash);

  // Whitelist check — used by ingress to REJECT unknown (type, action) pairs
  static bool isKnownCommandType(const String& type, const String& action);
  // [CORE-01] Field whitelist scoped to the EXACT (type, action) tuple.
  // Envelope fields (type/action/requestId/transactionId/version/issuedAt/
  // expiresAt) are always allowed — every other field must appear in the
  // registry entry for THIS action.
  static bool isFieldAllowed(const String& type, const String& action,
                              const String& field);

private:
  static String buildCanonicalString(JsonDocument& doc,
                                       const String& type, const String& action,
                                       String& errOut);
};

} // namespace Services

#endif // PLTS_SERVICES_COMMAND_CANONICALIZER_H
