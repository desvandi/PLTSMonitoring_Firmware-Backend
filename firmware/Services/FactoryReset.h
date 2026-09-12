// =============================================================================
// Services/FactoryReset.h — [audit p.427] single-source factory-reset wipe
// -----------------------------------------------------------------------------
// Three separate wipe paths had DIVERGED:
//   REST confirm    — 13 namespaces + LittleFS format + audit preservation
//   MQTT confirm    — only 9 namespaces (left plts_time, plts_emg, plts_auth,
//                     plts_relays ALIVE, no LittleFS format, no marker)
//   Boot completion — 13 namespaces (a third copy of the list)
//
// The MQTT gap was safety-relevant: a "factory reset" issued over MQTT kept
// the relay lockout states (plts_relays), the emergency trip counters
// (plts_emg), the auth refresh tokens (plts_auth) and the epoch estimate
// (plts_time) — the device was NOT actually factory-fresh.
//
// This module is now the ONLY place the namespace set is declared. Every
// path (REST confirm, MQTT confirm, boot-time completion) calls
// executeFactoryResetWipe().
//
// [STORAGE-GATE-06 note] The IN_PROGRESS marker ("ftr_p" in namespace
// "plts") must survive the sweep, so "plts" is wiped LAST in
// FACTORY_RESET_NAMESPACES. The previous list order wiped "plts" first,
// destroying the marker microseconds after it was written — the
// recoverable-reset claim only held for a microscopic power-loss window.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_FACTORY_RESET_H
#define PLTS_SERVICES_FACTORY_RESET_H

#include <stddef.h>

namespace Services {

// Canonical wipe set. ORDER MATTERS: "plts" is deliberately LAST so the
// IN_PROGRESS marker written into it survives the sweep of the other
// namespaces (see header comment above).
extern const char* const FACTORY_RESET_NAMESPACES[];
extern const size_t FACTORY_RESET_NAMESPACE_COUNT;

/// Full factory-reset wipe:
///   1. persist the IN_PROGRESS marker (ftr_p=true) in "plts"
///   2. clear every namespace in FACTORY_RESET_NAMESPACES ("plts" last)
///   3. preserve the audit log (LittleFS -> NVS "plts_audit" blob)
///   4. LittleFS.format()
///   5. restore the preserved audit log
///   6. clear the marker (ftr_p=false)
///
/// Does NOT reboot — the caller schedules the restart:
///   REST : ESP.restart() immediately after
///   MQTT : requestDeferredReboot() so the ACK is published first
///   BOOT : continue setup() (this is the interrupted-reset completion)
void executeFactoryResetWipe();

} // namespace Services

#endif // PLTS_SERVICES_FACTORY_RESET_H
