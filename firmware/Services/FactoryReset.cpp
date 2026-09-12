// =============================================================================
// Services/FactoryReset.cpp — implementation of the shared wipe (audit p.427)
// =============================================================================
#include "FactoryReset.h"
#include "LogService.h"
#include <Preferences.h>
#include <LittleFS.h>

namespace Services {

// [audit p.427] Single source of truth — 13 operational namespaces.
// "plts" (the IN_PROGRESS marker's home) is wiped LAST on purpose: wiping it
// first would destroy the ftr_p marker microseconds after it was written and
// gut the STORAGE-GATE-06 recoverable-reset contract.
//
// [AUDIT ROUND 4] SECURITY LEADGER IS PRESERVED BY DESIGN: the anti-rollback
// ledger lives in the "plts_sec" namespace (Services/SecurityPosture) and is
// deliberately NOT in this sweep. The eFuse secure-version floor is one-way
// hardware; wiping its interpretation layer (ledger) together with user
// config would make every factory-reset device look like a rollback attack
// (ledger absent, floor > 0) and permanently refuse OTA. Security epoch
// state is device state, not user configuration — it must survive a reset.
// scripts/test_audit_round4_2026_09.py enforces this contract statically.
const char* const FACTORY_RESET_NAMESPACES[] = {
  "plts_health",   // health supervisor (crash-loop counters)
  "plts_energy",   // energy history
  "plts_ota",      // OTA state / rollback marker
  "plts_txn",      // transaction journal (dedup)
  "plts_spool",    // telemetry spool
  "plts_batt",     // battery snapshot
  "plts_alarm",    // alarm state with CRC
  "plts_time",     // epoch estimate
  "plts_soc",      // SOC integrator
  "plts_emg",      // emergency relay state + trip counter
  "plts_auth",     // refresh tokens
  "plts_relays",   // 8-channel relay config + lockout states
  "plts",          // main config — LAST (holds the ftr_p marker)
};
const size_t FACTORY_RESET_NAMESPACE_COUNT =
  sizeof(FACTORY_RESET_NAMESPACES) / sizeof(FACTORY_RESET_NAMESPACES[0]);

void executeFactoryResetWipe() {
  // 1. IN_PROGRESS marker — BEFORE any namespace is wiped (STORAGE-GATE-06).
  {
    Preferences m;
    if (m.begin("plts", false)) {
      m.putBool("ftr_p", true);
      m.end();
    }
  }
  // 2. Sweep every operational namespace ("plts" is last in the array).
  {
    Preferences p;
    for (size_t i = 0; i < FACTORY_RESET_NAMESPACE_COUNT; i++) {
      if (p.begin(FACTORY_RESET_NAMESPACES[i], false)) {
        p.clear();
        p.end();
      }
    }
  }
  // 2b. Re-write the IN_PROGRESS marker — wiping "plts" (last) cleared
  //     it; re-persisting keeps the recoverable-reset contract alive through
  //     the audit preserve + LittleFS format that follow. (Power loss in the
  //     few-ms window between the clear and this re-write boots into an
  //     already-fully-wiped NVS — the intended end state, minus the
  //     forensic audit blob.)
  {
    Preferences m;
    if (m.begin("plts", false)) {
      m.putBool("ftr_p", true);
      m.end();
    }
  }
  // 3. [audit-2 S-17] Preserve the forensic audit log across the format —
  //    evidence of WHO triggered the reset must survive the reset itself.
  bool auditPreserved = preserveAuditLogAcrossReset();
  // 4. Filesystem wipe (NVS is not touched by LittleFS.format()).
  LittleFS.format();
  // 5. Restore the preserved audit log (also clears the NVS backup blob).
  if (auditPreserved) restoreAuditLogAfterReset();
  // 6. Marker clear — the wipe is complete; a power loss from here on boots
  //    into the freshly-reset state (ftr_p is gone with the wiped/format
  //    pass above for the "plts" namespace, this write is belt-and-braces
  //    for the marker-slot default).
  {
    Preferences m;
    if (m.begin("plts", false)) {
      m.putBool("ftr_p", false);
      m.end();
    }
  }
}

} // namespace Services
