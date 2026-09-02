// =============================================================================
// Services/CommandCanonicalizer.h — PD-001 canonical command + transaction id
// -----------------------------------------------------------------------------
// Shared canonical hash for REST + MQTT commands. Whitelist (fail-closed):
// unknown types/actions/fields REJECTED. Excludes requestId/transactionId/
// issuedAt/expiresAt from hash (envelope-only).
//
// Whitelisted commands (PLTS-specific — no relays, no scheduler):
//   config.update    — update battery config (capacityAh, fullV, lowV, ...)
//   calibration.update / calibration.point / calibration.acs712_zero
//   alarm.acknowledge / alarm.acknowledgeAll
//   ota.start / ota.check
//   system.reboot / system.factory_reset_prepare / system.factory_reset_confirm
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

  static CanonicalResult canonicalizeAndHash(JsonDocument& doc);
  static DecisionResult decideTransaction(const String& tid, const String& hash);

  // Whitelist check — used by ingress to REJECT unknown (type, action) pairs
  static bool isKnownCommandType(const String& type, const String& action);
  // Field whitelist per type — REJECT unknown fields BEFORE hashing
  static bool isFieldAllowed(const String& type, const String& field);

private:
  static String buildCanonicalString(JsonDocument& doc,
                                       const String& type, const String& action,
                                       String& errOut);
};

} // namespace Services

#endif // PLTS_SERVICES_COMMAND_CANONICALIZER_H
