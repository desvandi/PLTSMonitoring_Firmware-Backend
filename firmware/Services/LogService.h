// =============================================================================
// Services/LogService.h — Activity log (JSON-lines, rotates @ 200) +
//                          Audit log (plain text, rotates @ 8KB)
// -----------------------------------------------------------------------------
// Brief §61 — events:
//   DEVICE_BOOT, WIFI_CONNECTED, WIFI_DISCONNECTED, TIME_SYNCED,
//   SENSOR_FAILURE, SENSOR_RECOVERED, ALARM_ACTIVE, ALARM_ACKNOWLEDGED,
//   ALARM_CLEARED, SOC_BASELINE_CORRECTED, CALIBRATION_CHANGED,
//   CONFIGURATION_CHANGED, OTA_STARTED, OTA_SUCCESS, OTA_FAILED,
//   STORAGE_ERROR.
//
// NEVER log passwords, tokens, or secrets.
//
// [AUDIT 2026-09 ROUND 10 / p.472] LOCK CONTRACT — fail-closed BOTH ways:
//   Writer side (FW-24, 2026-08): append()/audit()/flushToDisk() take _mutex
//     across the RAM mutation AND the LittleFS mirror (LittleFS has no
//     internal locking).
//   Reader side (p.472): getActivityJson()/getAuditText()/getActivityCount()/
//     getAuditBytes() previously read _head/_count/_entries[]/_auditBuf with
//     NO synchronization while writers mutated them from up to seven tasks —
//     torn/incoherent snapshots (a half-written entry, a _head that moved
//     between the index computation and the read). ALL getters now take the
//     same mutex.
//   Acquisition is FAIL-CLOSED (same pattern as AlarmRegistry round-9
//   p.471): begin() treats mutex-creation failure as FATAL (boot guard —
//   halt WITHOUT feeding the task watchdog → TWDT panic reset, honest
//   crash-chain); at runtime _lock() retries creation ONCE and returns false
//   when unavailable — the caller then refuses the operation. The failure
//   path logs via Serial ONLY: this service is the log sink, it cannot log
//   its own lock failure into itself (that would be a self-call on the very
//   mutex that is missing).
//
//   Degraded-mode (mutex unavailable) per-entry contract:
//     append()/audit()/flushToDisk() → no-op (entry dropped — the FW-24
//                           "drop rather than block" semantics, extended to
//                           the no-lock case; lockFailures++ makes it visible)
//     getActivityJson()  → {"logs":[],"lockUnavailable":true} (honest empty)
//     getAuditText()    → "" (honest empty)
//     getActivityCount()/getAuditBytes() → 0 (conservative)
//     lockFailures()    → lock-free atomic read — the ONE accessor that works
//                         in the degraded mode (diagnostics path).
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_LOG_SERVICE_H
#define PLTS_SERVICES_LOG_SERVICE_H

#include <Arduino.h>
#include <atomic>
#include "../Core/Types.h"
#include "../Core/Config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Services {

class LogService {
public:
  // [p.472] Boot guard: returns normally only when the mutex exists. On
  // creation failure the firmware shape FATAL-halts (see .cpp) — the device
  // never enters multi-task state with an unlocked log service.
  void begin();
  // Append to activity log (JSON-lines, ring buffer in RAM + mirror to FS)
  // [FW-24 REMEDIATION 2026-08] append()/audit()/flushToDisk() are called from
  // MULTIPLE FreeRTOS tasks (sensor, measurement, energy, telemetry, network,
  // health, persistence) with no synchronization — LittleFS is not thread-safe
  // and the RAM ring raced. All public mutators now take _mutex.
  // [p.472] Mutators AND getters are fail-closed when the mutex is
  // unavailable (see the header contract above).
  void append(Core::LogType type, const String& message, int8_t channel = -1);
  // Append to audit log (plain text, used for config/calibration changes)
  void audit(const char* action, const char* parameter,
             const char* oldValue, const char* newValue,
             const char* source, uint32_t revision);
  // Activity log retrieval — [p.472] all getters take _mutex (reader-side
  // serialization; they run in the web-server task against seven writer
  // tasks). Fail-closed to the documented empty values.
  String getActivityJson(uint16_t limit, int8_t filterType) const;
  String getAuditText(uint16_t maxBytes) const;
  uint16_t getActivityCount() const;
  uint16_t getAuditBytes() const;
  // Flush in-RAM log to FS (called by PersistenceTask)
  void flushToDisk();

  // [p.472] Lock-free atomic read — deliberately the ONE accessor working
  // when the mutex is unavailable, so /api/diagnostics can still SEE the
  // degraded mode. Non-zero = at least one operation was refused fail-closed
  // since boot.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

private:
  static constexpr uint16_t MAX_ENTRIES = Core::MAX_ACTIVITY_LOG_ENTRIES;
  struct Entry {
    uint32_t id;
    uint32_t timestamp;
    Core::LogType type;
    int8_t channel;
    char message[96];
  };
  Entry _entries[MAX_ENTRIES] = {};
  uint16_t _head = 0;
  uint16_t _count = 0;
  uint32_t _nextId = 1;
  String _auditBuf;
  bool _auditDirty = false;

  // [p.472] mutable: the const getters acquire the same mutex as the
  // mutators (reader-side serialization).
  mutable SemaphoreHandle_t _mutex = nullptr;   // [FW-24] created in begin()

  // [p.472] Fail-closed accounting — ATOMIC by necessity: written on the
  // mutex-UNAVAILABLE path where no synchronization exists. Relaxed ordering
  // (counter + rate-limit bookkeeping, no state publication).
  mutable std::atomic<uint32_t> _lockFailures{0};
  mutable std::atomic<uint32_t> _lastLockFailLogMs{0};

  // [p.472] Fail-closed acquisition: retry-create ONCE, then false. The
  // failure log is Serial-only (self-logging is impossible — see header).
  bool _lock() const;
  void _unlock() const;

  void _writeActivityToFs(const Entry& e);
  void _rotateAuditIfNeeded();
};

extern LogService Log;

// [CI fix] Free functions for audit log preservation across factory reset.
// Defined in LogService.cpp inside namespace Services.
bool preserveAuditLogAcrossReset();
void restoreAuditLogAfterReset();

} // namespace Services

#endif // PLTS_SERVICES_LOG_SERVICE_H
