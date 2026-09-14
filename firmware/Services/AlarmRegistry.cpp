// =============================================================================
// Services/AlarmRegistry.cpp
// =============================================================================
#include "AlarmRegistry.h"
#include "LogService.h"
#include "../Core/Types.h"
#include "../Drivers/RtcDriver.h"
#include "../Utils/Crc.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Services {

AlarmRegistry alarms;

// ---------------------------------------------------------------------------
// [AUDIT 2026-09 ROUND 6 / p.453 + p.455] Persistence layout v2 — ONE record.
//
//   key "state" (namespace "plts_alarm"):
//     offset  0 : uint32 magic       "ALRM"
//     offset  4 : uint16 version     2 (this layout)
//     offset  6 : uint16 generation  save counter — proves which
//                                     transaction produced the snapshot
//     offset  8 : uint8  count
//     offset  9 : uint8  reserved[3]
//     offset 12 : uint32 crc32        over bytes [0,12) + the alarm array
//     offset 16 : Alarm  alarms[count]
//
// One putBytes = one journaled NVS transaction: a boot sees either the
// complete previous generation or the complete new one, never the
// mixed-generation pair the old two-record ("hdr" + "arr") layout could
// leave behind. The strict full-length write check plus read-back
// verification catch short writes and torn media — a failure is reported,
// never assumed persisted.
//
// The legacy v1 two-record layout is read ONCE for migration (adopt + one
// immediate re-save in the new format); a firmware rollback still finds the
// stale legacy pair, which remains self-consistent under its own CRC.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t ALARM_NVS_MAGIC = 0x414C524Du;   // "ALRM"
constexpr uint16_t ALARM_STATE_VERSION = 2u;         // single-record layout

struct AlarmStateBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;        // [p.455] transaction marker
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t crc32;             // over header-without-crc + alarm array
};
static_assert(sizeof(AlarmStateBlob) == 16, "alarm blob header must be packed 16 bytes");

// Legacy v1 (round-5 two-record layout) — read-only, for migration.
struct AlarmPersistHeaderV1 {
  uint32_t magic;
  uint32_t version;           // == 1
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t crc32;
};
} // namespace

void AlarmRegistry::begin() {
  _count = 0;
  _dirty = false;
  _generation = 0;
  for (uint8_t i = 0; i < MAX_ALARMS; i++) {
    _alarms[i] = {};
    _alarms[i].code[0] = '\0';
  }
  // [FW-23] Restore alarm state — alarm conditions surviving a reboot is
  // part of deterministic recovery (directive §21: alarm state must be
  // recovered after every reset).
  loadFromNVS();
}

uint8_t AlarmRegistry::_findIdx(const char* code) const {
  if (!code) return 0xFF;
  for (uint8_t i = 0; i < _count; i++) {
    if (strncmp(_alarms[i].code, code, Alarm::CODE_LEN) == 0) return i;
  }
  return 0xFF;
}

bool AlarmRegistry::raise(const char* code, Core::AlarmSeverity sev, const char* message) {
  // [p.454] Thin wrapper: TRUE = accepted in the RAM registry (NOT a
  // durability claim — see raiseTracked()).
  return raiseTracked(code, sev, message) != RaiseResult::Rejected;
}

RaiseResult AlarmRegistry::raiseTracked(const char* code, Core::AlarmSeverity sev,
                                        const char* message) {
  if (!code) return RaiseResult::Rejected;
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) {
    // New alarm
    if (_count >= MAX_ALARMS) {
      // [AUDIT 2026-09 ROUND 5 / p.451] Eviction candidates are CLEARED
      // entries ONLY. ACTIVE / ACKNOWLEDGED alarms are safety state and are
      // NEVER sacrificed to make room — the old fallback ("drop the
      // lowest-severity active") silently deleted live safety state, and its
      // severity scan had no timestamp tie-break so "oldest" was not even
      // guaranteed. Rejection here is recoverable: evaluators re-raise every
      // tick, so the alarm is admitted the moment a slot frees.
      uint8_t oldest = 0xFF;
      uint32_t oldestClearedAt = 0xFFFFFFFF;
      uint32_t oldestRaisedAt = 0xFFFFFFFF;   // tie-break: stable order
      for (uint8_t i = 0; i < _count; i++) {
        if (_alarms[i].lifecycle == Core::AlarmLifecycle::Cleared &&
            (_alarms[i].clearedAt < oldestClearedAt ||
             (_alarms[i].clearedAt == oldestClearedAt &&
              _alarms[i].raisedAt < oldestRaisedAt))) {
          oldestClearedAt = _alarms[i].clearedAt;
          oldestRaisedAt = _alarms[i].raisedAt;
          oldest = i;
        }
      }
      if (oldest != 0xFF) {
        for (uint8_t i = oldest; i < _count - 1; i++) _alarms[i] = _alarms[i + 1];
        _count--;
        idx = _count;
      } else {
        // Registry saturated with non-cleared alarms — REJECT honestly.
        _overflowCount++;
        static uint32_t lastOverflowLogMs = 0;
        static char lastOverflowCode[Alarm::CODE_LEN] = {0};
        uint32_t now = millis();
        if (now - lastOverflowLogMs > 60000UL ||
            strncmp(lastOverflowCode, code, Alarm::CODE_LEN) != 0) {
          lastOverflowLogMs = now;
          strncpy(lastOverflowCode, code, Alarm::CODE_LEN - 1);
          lastOverflowCode[Alarm::CODE_LEN - 1] = '\0';
          Log.append(Core::LogType::StorageError,
                     String("[ALARM] registry saturated (") + _count +
                     " active/ack) — new alarm REJECTED (not stored): " + code +
                     ". Clear alarms to free slots (acknowledge keeps the slot "
                     "occupied — only CLEARED entries are evictable).", -1);
        }
        return RaiseResult::Rejected;   // honest: the caller knows the alarm was not stored
      }
    }
    // [FIX 2026-09-14 — found during post-merge re-verification of p.451]
    // Normal (not-full) path: the append slot was NEVER assigned — idx stayed
    // 0xFF (255) and `_alarms[idx]` wrote ~33 KB past the 24-entry array
    // (out-of-bounds write into unrelated globals), `_count` never
    // incremented, and find()/countActive() never saw the alarm — a silent
    // loss reported as success. The eviction branch above was the ONLY path
    // assigning idx. Assign the append slot explicitly; the 0xFF guard keeps
    // the post-eviction slot (idx == _count) untouched.
    if (idx == 0xFF) idx = _count;
    Alarm& a = _alarms[idx];
    strncpy(a.code, code, Alarm::CODE_LEN - 1);
    a.code[Alarm::CODE_LEN - 1] = '\0';
    a.severity = sev;
    a.lifecycle = Core::AlarmLifecycle::Active;
    a.raisedAt = Drivers::rtc.getUnixTime();
    a.acknowledgedAt = 0;
    a.clearedAt = 0;
    a.lastUpdatedAt = a.raisedAt;
    if (message) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
    } else {
      a.message[0] = '\0';
    }
    if (idx == _count) _count++;
    _dirty = true;
    // [p.450] A NEW alarm is a state transition — durable immediately (the
    // periodic checkpoint remains as backstop for multi-raise bursts).
    // [p.454] The persistence outcome is now part of the return contract.
    bool persisted = saveToNVS();
    if (!persisted) {
      Log.append(Core::LogType::StorageError,
                 String("[ALARM:") + code + "] accepted in RAM but the immediate " +
                 "NVS persistence FAILED (generation " + _generation +
                 ", retry via periodic checkpoint)", -1);
    }
    Log.append(Core::LogType::AlarmActive,
               String("[ALARM:") + code + "] " + (message ? message : ""), -1);
    return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
  } else {
    // Refresh existing alarm
    Alarm& a = _alarms[idx];
    bool meaningful = false;   // [p.450] durability applies to real mutations only
    // [p.460 prerequisite] REACTIVATION: raising a CLEARED entry starts a NEW
    // occurrence — Active again with a fresh raisedAt. The old refresh path
    // left lifecycle = Cleared forever, so any alarm cleared once could never
    // surface again (recurring overcurrent invisible after the first clear).
    // A new occurrence starts at the raised severity; the never-downgrade
    // rule below applies only to a still-live (Active/Acknowledged) alarm.
    if (a.lifecycle == Core::AlarmLifecycle::Cleared) {
      a.lifecycle = Core::AlarmLifecycle::Active;
      a.raisedAt = Drivers::rtc.getUnixTime();
      a.acknowledgedAt = 0;
      a.clearedAt = 0;
      a.lastUpdatedAt = a.raisedAt;
      a.severity = sev;
      meaningful = true;
      Log.append(Core::LogType::AlarmActive,
                 String("[ALARM:") + code + "] re-activated (recurrence after clear)", -1);
    } else {
      // Upgrade severity (never downgrade a live alarm)
      if ((uint8_t)sev > (uint8_t)a.severity) { a.severity = sev; meaningful = true; }
      a.lastUpdatedAt = Drivers::rtc.getUnixTime();
    }
    if (message && message[0] && strncmp(a.message, message, sizeof(a.message)) != 0) {
      strncpy(a.message, message, sizeof(a.message) - 1);
      a.message[sizeof(a.message) - 1] = '\0';
      meaningful = true;
    }
    // [p.450] Severity escalation / message change / reactivation =
    // safety-relevant update → persist NOW, same contract as clear()/
    // acknowledge(). A pure refresh (same severity, same message, live) only
    // touches lastUpdatedAt and rides the periodic checkpoint — writing NVS
    // on every evaluator tick (≈1 Hz × N alarms) would burn flash without
    // adding durability guarantees.
    if (meaningful) {
      _dirty = true;
      bool persisted = saveToNVS();
      if (!persisted) {
        Log.append(Core::LogType::StorageError,
                   String("[ALARM:") + code + "] meaningful update accepted in RAM but the immediate NVS persistence FAILED (retry via checkpoint)", -1);
      }
      return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
    }
    return RaiseResult::AcceptedRam;   // pure refresh — checkpoint owns durability
  }
}

void AlarmRegistry::clear(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  Alarm& a = _alarms[idx];
  a.lifecycle = Core::AlarmLifecycle::Cleared;
  a.clearedAt = Drivers::rtc.getUnixTime();
  a.lastUpdatedAt = a.clearedAt;
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
  Log.append(Core::LogType::AlarmCleared, String("Alarm cleared: ") + code, -1);
}

void AlarmRegistry::acknowledge(const char* code) {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return;
  Alarm& a = _alarms[idx];
  a.lifecycle = Core::AlarmLifecycle::Acknowledged;
  a.acknowledgedAt = Drivers::rtc.getUnixTime();
  a.lastUpdatedAt = a.acknowledgedAt;
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
  Log.append(Core::LogType::AlarmAcknowledged, String("Alarm acknowledged: ") + code, -1);
}

void AlarmRegistry::acknowledgeAll() {
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle == Core::AlarmLifecycle::Active) {
      _alarms[i].lifecycle = Core::AlarmLifecycle::Acknowledged;
      _alarms[i].acknowledgedAt = Drivers::rtc.getUnixTime();
    }
  }
  _dirty = true;
  saveToNVS();   // [FW-23] operator action — persist immediately
}

uint8_t AlarmRegistry::countActive() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared) n++;
  }
  return n;
}

uint8_t AlarmRegistry::countAll() const { return _count; }

uint8_t AlarmRegistry::copyActiveAlarms(Alarm* dst, uint8_t max) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < _count && n < max; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared) {
      dst[n++] = _alarms[i];
    }
  }
  return n;
}

bool AlarmRegistry::saveToNVS() {
  // [p.453] STRICT full-length check: the old code accepted the array write
  // with `w2 != 0`, so a partial write of the alarm array still reported
  // success while the CRC would only reject it at the NEXT boot — the save
  // status at the moment of the fault was a lie. One record, one length.
  // [p.455] generation marks the transaction.
  const size_t blobLen = sizeof(AlarmStateBlob) + (size_t)_count * sizeof(Alarm);

  // Single scratch buffer: [blob | read-back] — heap (not stack): the full
  // 24-alarm record is ~3.3 KB and this runs on web/task stacks too.
  uint8_t* buf = (uint8_t*)malloc(blobLen * 2);
  if (!buf) {
    _persistFailures++;
    Log.append(Core::LogType::StorageError,
               "Alarm NVS save: out of memory building state blob", -1);
    return false;
  }
  uint8_t* blob = buf;
  uint8_t* rb = buf + blobLen;

  AlarmStateBlob h = {};
  h.magic = ALARM_NVS_MAGIC;
  h.version = ALARM_STATE_VERSION;
  h.generation = (uint16_t)(_generation + 1);
  h.count = _count;
  memcpy(blob, &h, sizeof(h));
  if (_count > 0) memcpy(blob + sizeof(AlarmStateBlob), _alarms, _count * sizeof(Alarm));

  // CRC over the header-without-crc + the alarm array
  uint32_t crc = Utils::crc32(blob, sizeof(AlarmStateBlob) - sizeof(uint32_t));
  if (_count > 0) {
    crc = Utils::crc32(blob + sizeof(AlarmStateBlob), _count * sizeof(Alarm), crc);
  }
  memcpy(blob + 12, &crc, sizeof(uint32_t));

  Preferences p;
  if (!p.begin("plts_alarm", false)) {
    free(buf);
    _persistFailures++;   // [p.439-family] fail-closed accounting, no fake "saved"
    return false;
  }
  size_t w = p.putBytes("state", blob, blobLen);
  if (w != blobLen) {
    p.end();
    free(buf);
    _persistFailures++;
    Log.append(Core::LogType::StorageError,
               String("Alarm NVS short write (") + w + "/" + blobLen +
               " bytes) — state NOT persisted", -1);
    return false;
  }
  // [p.439-family / p.453] Read-back verification — what NVS actually
  // stored must equal what we intended to store.
  size_t got = p.getBytes("state", rb, blobLen);
  p.end();
  bool verified = (got == blobLen) && (memcmp(rb, blob, blobLen) == 0);
  free(buf);
  if (!verified) {
    _persistFailures++;
    Log.append(Core::LogType::StorageError,
               "Alarm NVS read-back mismatch — state NOT verifiably persisted", -1);
    return false;
  }
  _generation = (uint16_t)(_generation + 1);   // commit: this generation is durable
  _dirty = false;
  return true;
}

void AlarmRegistry::loadFromNVS() {
  Preferences p;
  if (!p.begin("plts_alarm", true)) return;   // namespace absent — fresh, honest

  // ---- Preferred: single-record blob (v2) ----
  {
    AlarmStateBlob h = {};
    size_t gotHdr = p.getBytes("state", &h, sizeof(h));
    if (gotHdr == sizeof(h) && h.magic == ALARM_NVS_MAGIC &&
        h.version == ALARM_STATE_VERSION) {
      if (h.count > MAX_ALARMS) {
        p.end();
        Log.append(Core::LogType::StorageError,
                   "Persisted alarm state corrupt (count out of range) — starting empty", -1);
        return;
      }
      const size_t blobLen = sizeof(AlarmStateBlob) + (size_t)h.count * sizeof(Alarm);
      uint8_t* blob = (uint8_t*)malloc(blobLen);
      if (!blob) { p.end(); return; }
      size_t got = p.getBytes("state", blob, blobLen);
      p.end();
      bool crcOk = false;
      if (got == blobLen) {
        uint32_t crc = Utils::crc32(blob, sizeof(AlarmStateBlob) - sizeof(uint32_t));
        if (h.count > 0) {
          crc = Utils::crc32(blob + sizeof(AlarmStateBlob), h.count * sizeof(Alarm), crc);
        }
        uint32_t stored = 0;
        memcpy(&stored, blob + 12, sizeof(uint32_t));
        crcOk = (crc == stored);
      }
      if (!crcOk) {
        free(blob);
        Log.append(Core::LogType::StorageError,
                   "Persisted alarm state corrupt (CRC/length) — starting empty", -1);
        return;   // corrupt → empty, honest
      }
      const Alarm* src = (const Alarm*)(blob + sizeof(AlarmStateBlob));
      for (uint8_t i = 0; i < h.count; i++) _alarms[i] = src[i];
      _count = h.count;
      _dirty = false;
      _generation = h.generation;   // [p.455] adopt the transaction marker
      free(blob);
      Log.append(Core::LogType::Info,
                 String("[ALARM] restored ") + _count + " alarm(s) from NVS (generation " +
                 _generation + ")", -1);
      return;
    }
    // No v2 record — fall through to the legacy reader below.
  }

  // ---- Legacy (round-5 two-record layout) → migrate to v2 ----
  {
    AlarmPersistHeaderV1 hdr = {};
    size_t gotHdr = p.getBytes("hdr", &hdr, sizeof(hdr));
    if (gotHdr != sizeof(hdr) || hdr.magic != ALARM_NVS_MAGIC ||
        hdr.version != 1u || hdr.count > MAX_ALARMS) {
      p.end();
      return;   // absent (first boot) or unrecognized — fresh, honest
    }
    Alarm buf[MAX_ALARMS] = {};
    size_t gotArr = (hdr.count > 0) ? p.getBytes("arr", buf, hdr.count * sizeof(Alarm)) : 0;
    p.end();
    if (hdr.count > 0 && gotArr != hdr.count * sizeof(Alarm)) {
      Log.append(Core::LogType::StorageError,
                 "Legacy alarm state incomplete — starting empty", -1);
      return;
    }
    uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
    if (hdr.count > 0) {
      crc = Utils::crc32((const uint8_t*)buf, hdr.count * sizeof(Alarm), crc);
    }
    if (crc != hdr.crc32) {
      Log.append(Core::LogType::StorageError,
                 "Legacy alarm state corrupt (CRC) — starting empty", -1);
      return;   // corrupt → empty, honest
    }
    for (uint8_t i = 0; i < hdr.count; i++) _alarms[i] = buf[i];
    _count = hdr.count;
    _dirty = false;
    Log.append(Core::LogType::Info,
               String("[ALARM] migrated ") + _count +
               " alarm(s) from the legacy two-record layout — rewriting as a single " +
               "atomic record", -1);
    saveToNVS();   // one-time migration write (v2 becomes authoritative)
    // The legacy pair is deliberately left in place (NOT kept in sync): a
    // firmware rollback still finds a self-consistent, if stale, snapshot.
  }
}

const Alarm* AlarmRegistry::getAlarm(uint8_t idx) const {
  if (idx >= _count) return nullptr;
  return &_alarms[idx];
}

const Alarm* AlarmRegistry::find(const char* code) const {
  uint8_t idx = _findIdx(code);
  if (idx == 0xFF) return nullptr;
  return &_alarms[idx];
}

Core::AlarmSeverity AlarmRegistry::highestActiveSeverity() const {
  Core::AlarmSeverity h = Core::AlarmSeverity::Info;
  for (uint8_t i = 0; i < _count; i++) {
    if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared &&
        (uint8_t)_alarms[i].severity > (uint8_t)h) {
      h = _alarms[i].severity;
    }
  }
  return h;
}

} // namespace Services
