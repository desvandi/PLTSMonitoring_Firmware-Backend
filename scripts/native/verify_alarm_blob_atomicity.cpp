// verify_alarm_blob_atomicity.cpp — native ASAN harness for the ROUND-6
// AlarmRegistry remediation (p.453 + p.454 + p.455 + p.460-prerequisite).
//
// STRUCTURAL MIRROR of firmware/Services/AlarmRegistry.{h,cpp} @ round-6:
// raiseTracked()/raise()/clear()/saveToNVS()/loadFromNVS() are copied
// verbatim modulo the Arduino shims (Preferences/millis/RTC/Log). The
// round-5 lesson (worklog): the mirror test implemented the INTENT of
// registry.append, not the STRUCTURE, and missed the 0xFF out-of-bounds
// P0. This harness keeps structure 1:1.
//
// Build: g++ -std=c++17 -g -fsanitize=address,undefined -o verify_alarm_blob_atomicity verify_alarm_blob_atomicity.cpp
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// Arduino shims
// ============================================================================
namespace Core {
enum class AlarmSeverity : uint8_t { Info = 0, Warning = 1, Critical = 2 };
enum class AlarmLifecycle : uint8_t { Active, Acknowledged, Cleared };
enum class LogType : uint8_t { Info, AlarmActive, AlarmCleared, AlarmAcknowledged, StorageError };
} // namespace Core

static uint32_t fake_millis = 1000;
static uint32_t millis() { return fake_millis; }
static uint32_t fake_rtc = 1757000000;
static inline uint32_t rtcNow() { return fake_rtc++; }
struct LogSpy {
  int storageErrors = 0;
  std::vector<std::string> lines;
  void append(Core::LogType t, const std::string& s, int8_t = 0) {
    if (t == Core::LogType::StorageError) storageErrors++;
    lines.push_back(s);
  }
};
static LogSpy Log;

// Utils::crc32 — verbatim algorithm from firmware/Utils/Crc.cpp
namespace Utils {
static uint32_t T[256];
static bool Tinit = false;
static void initTable() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (uint8_t k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    T[i] = c;
  }
  Tinit = true;
}
uint32_t crc32(const uint8_t* d, size_t n, uint32_t init = 0) {
  if (!Tinit) initTable();
  uint32_t crc = init;
  for (size_t i = 0; i < n; i++) crc = T[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
} // namespace Utils

// Fake NVS with failure injection
struct FakeNvsStore {
  std::map<std::string, std::vector<uint8_t>> kv;
  bool failBegin = false;          // p.begin() fails
  size_t truncateWriteTo = ~0u;    // putBytes commits only first N bytes, returns N (short write)
  int flipByteOnRead = -1;         // getBytes flips this index (torn/worn media)
  size_t ops = 0;
};
static FakeNvsStore nvs;

struct Preferences {
  bool _ok = false;
  bool begin(const char*, bool) { return !(nvs.failBegin); }
  void end() {}
  size_t putBytes(const char* key, const void* buf, size_t len) {
    nvs.ops++;
    size_t w = (nvs.truncateWriteTo < len) ? nvs.truncateWriteTo : len;
    const uint8_t* p = (const uint8_t*)buf;
    nvs.kv[key] = std::vector<uint8_t>(p, p + w);
    return w;
  }
  size_t getBytes(const char* key, void* buf, size_t len) {
    auto it = nvs.kv.find(key);
    if (it == nvs.kv.end()) return 0;
    const std::vector<uint8_t>& v = it->second;
    size_t n = (v.size() < len) ? v.size() : len;
    uint8_t* p = (uint8_t*)buf;
    for (size_t i = 0; i < n; i++) p[i] = v[i];
    if (nvs.flipByteOnRead >= 0 && (size_t)nvs.flipByteOnRead < n) p[nvs.flipByteOnRead] ^= 0xFF;
    return n;
  }
};

// ============================================================================
// STRUCTURAL MIRROR of AlarmRegistry (round-6 source)
// ============================================================================
namespace Services {

struct Alarm {
  static constexpr uint8_t CODE_LEN = 32;
  char code[CODE_LEN];
  Core::AlarmSeverity severity;
  Core::AlarmLifecycle lifecycle;
  uint32_t raisedAt, acknowledgedAt, clearedAt, lastUpdatedAt;
  char message[80];
};

enum class RaiseResult : uint8_t {
  Rejected = 0,
  AcceptedRam = 1,
  AcceptedPersisted = 2,
  AcceptedPersistFailed = 3,
};

namespace {
constexpr uint32_t ALARM_NVS_MAGIC = 0x414C524Du;   // "ALRM"
constexpr uint16_t ALARM_STATE_VERSION = 2u;
struct AlarmStateBlob {
  uint32_t magic;
  uint16_t version;
  uint16_t generation;
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t crc32;
};
static_assert(sizeof(AlarmStateBlob) == 16, "alarm blob header must be packed 16 bytes");
struct AlarmPersistHeaderV1 {
  uint32_t magic;
  uint32_t version;
  uint8_t  count;
  uint8_t  reserved[3];
  uint32_t crc32;
};
} // namespace

class AlarmRegistry {
public:
  static constexpr uint8_t MAX_ALARMS = 24;
  Alarm _alarms[MAX_ALARMS] = {};
  uint8_t _count = 0;
  bool _dirty = false;
  uint32_t _overflowCount = 0;
  uint32_t _persistFailures = 0;
  uint16_t _generation = 0;

  uint8_t _findIdx(const char* code) const {
    if (!code) return 0xFF;
    for (uint8_t i = 0; i < _count; i++)
      if (strncmp(_alarms[i].code, code, Alarm::CODE_LEN) == 0) return i;
    return 0xFF;
  }

  bool raise(const char* code, Core::AlarmSeverity sev, const char* message = "") {
    return raiseTracked(code, sev, message) != RaiseResult::Rejected;
  }

  RaiseResult raiseTracked(const char* code, Core::AlarmSeverity sev, const char* message = "") {
    if (!code) return RaiseResult::Rejected;
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) {
      if (_count >= MAX_ALARMS) {
        uint8_t oldest = 0xFF;
        uint32_t oldestClearedAt = 0xFFFFFFFF;
        uint32_t oldestRaisedAt = 0xFFFFFFFF;
        for (uint8_t i = 0; i < _count; i++) {
          if (_alarms[i].lifecycle == Core::AlarmLifecycle::Cleared &&
              (_alarms[i].clearedAt < oldestClearedAt ||
               (_alarms[i].clearedAt == oldestClearedAt && _alarms[i].raisedAt < oldestRaisedAt))) {
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
          _overflowCount++;
          return RaiseResult::Rejected;
        }
      }
      if (idx == 0xFF) idx = _count;
      Alarm& a = _alarms[idx];
      strncpy(a.code, code, Alarm::CODE_LEN - 1);
      a.code[Alarm::CODE_LEN - 1] = '\0';
      a.severity = sev;
      a.lifecycle = Core::AlarmLifecycle::Active;
      a.raisedAt = rtcNow();
      a.acknowledgedAt = 0;
      a.clearedAt = 0;
      a.lastUpdatedAt = a.raisedAt;
      if (message) {
        strncpy(a.message, message, sizeof(a.message) - 1);
        a.message[sizeof(a.message) - 1] = '\0';
      } else a.message[0] = '\0';
      if (idx == _count) _count++;
      _dirty = true;
      bool persisted = saveToNVS();
      return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
    } else {
      Alarm& a = _alarms[idx];
      bool meaningful = false;
      if (a.lifecycle == Core::AlarmLifecycle::Cleared) {
        a.lifecycle = Core::AlarmLifecycle::Active;
        a.raisedAt = rtcNow();
        a.acknowledgedAt = 0;
        a.clearedAt = 0;
        a.lastUpdatedAt = a.raisedAt;
        a.severity = sev;
        meaningful = true;
      } else {
        if ((uint8_t)sev > (uint8_t)a.severity) { a.severity = sev; meaningful = true; }
        a.lastUpdatedAt = rtcNow();
      }
      if (message && message[0] && strncmp(a.message, message, sizeof(a.message)) != 0) {
        strncpy(a.message, message, sizeof(a.message) - 1);
        a.message[sizeof(a.message) - 1] = '\0';
        meaningful = true;
      }
      if (meaningful) {
        _dirty = true;
        bool persisted = saveToNVS();
        return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
      }
      return RaiseResult::AcceptedRam;
    }
  }

  void clear(const char* code) {
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) return;
    Alarm& a = _alarms[idx];
    a.lifecycle = Core::AlarmLifecycle::Cleared;
    a.clearedAt = rtcNow();
    a.lastUpdatedAt = a.clearedAt;
    _dirty = true;
    saveToNVS();
  }

  uint8_t countActive() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < _count; i++)
      if (_alarms[i].lifecycle != Core::AlarmLifecycle::Cleared) n++;
    return n;
  }
  const Alarm* find(const char* code) const {
    uint8_t idx = _findIdx(code);
    return (idx == 0xFF) ? nullptr : &_alarms[idx];
  }

  bool saveToNVS() {
    const size_t blobLen = sizeof(AlarmStateBlob) + (size_t)_count * sizeof(Alarm);
    uint8_t* buf = (uint8_t*)malloc(blobLen * 2);
    if (!buf) { _persistFailures++; return false; }
    uint8_t* blob = buf;
    uint8_t* rb = buf + blobLen;
    AlarmStateBlob h = {};
    h.magic = ALARM_NVS_MAGIC;
    h.version = ALARM_STATE_VERSION;
    h.generation = (uint16_t)(_generation + 1);
    h.count = _count;
    memcpy(blob, &h, sizeof(h));
    if (_count > 0) memcpy(blob + sizeof(AlarmStateBlob), _alarms, _count * sizeof(Alarm));
    uint32_t crc = Utils::crc32(blob, sizeof(AlarmStateBlob) - sizeof(uint32_t));
    if (_count > 0) crc = Utils::crc32(blob + sizeof(AlarmStateBlob), _count * sizeof(Alarm), crc);
    memcpy(blob + 12, &crc, sizeof(uint32_t));
    Preferences p;
    if (!p.begin("plts_alarm", false)) { free(buf); _persistFailures++; return false; }
    size_t w = p.putBytes("state", blob, blobLen);
    if (w != blobLen) { p.end(); free(buf); _persistFailures++; return false; }
    size_t got = p.getBytes("state", rb, blobLen);
    p.end();
    bool verified = (got == blobLen) && (memcmp(rb, blob, blobLen) == 0);
    free(buf);
    if (!verified) { _persistFailures++; return false; }
    _generation = (uint16_t)(_generation + 1);
    _dirty = false;
    return true;
  }

  void loadFromNVS() {
    Preferences p;
    if (!p.begin("plts_alarm", true)) return;
    {
      AlarmStateBlob h = {};
      size_t gotHdr = p.getBytes("state", &h, sizeof(h));
      if (gotHdr == sizeof(h) && h.magic == ALARM_NVS_MAGIC && h.version == ALARM_STATE_VERSION) {
        if (h.count > MAX_ALARMS) { p.end(); return; }
        const size_t blobLen = sizeof(AlarmStateBlob) + (size_t)h.count * sizeof(Alarm);
        uint8_t* blob = (uint8_t*)malloc(blobLen);
        if (!blob) { p.end(); return; }
        size_t got = p.getBytes("state", blob, blobLen);
        p.end();
        bool crcOk = false;
        if (got == blobLen) {
          uint32_t crc = Utils::crc32(blob, sizeof(AlarmStateBlob) - sizeof(uint32_t));
          if (h.count > 0) crc = Utils::crc32(blob + sizeof(AlarmStateBlob), h.count * sizeof(Alarm), crc);
          uint32_t stored = 0;
          memcpy(&stored, blob + 12, sizeof(uint32_t));
          crcOk = (crc == stored);
        }
        if (!crcOk) { free(blob); return; }
        const Alarm* src = (const Alarm*)(blob + sizeof(AlarmStateBlob));
        for (uint8_t i = 0; i < h.count; i++) _alarms[i] = src[i];
        _count = h.count;
        _dirty = false;
        _generation = h.generation;
        free(blob);
        return;
      }
    }
    {
      AlarmPersistHeaderV1 hdr = {};
      size_t gotHdr = p.getBytes("hdr", &hdr, sizeof(hdr));
      if (gotHdr != sizeof(hdr) || hdr.magic != ALARM_NVS_MAGIC ||
          hdr.version != 1u || hdr.count > MAX_ALARMS) {
        p.end();
        return;
      }
      Alarm buf[MAX_ALARMS] = {};
      size_t gotArr = (hdr.count > 0) ? p.getBytes("arr", buf, hdr.count * sizeof(Alarm)) : 0;
      p.end();
      if (hdr.count > 0 && gotArr != hdr.count * sizeof(Alarm)) return;
      uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - sizeof(uint32_t));
      if (hdr.count > 0) crc = Utils::crc32((const uint8_t*)buf, hdr.count * sizeof(Alarm), crc);
      if (crc != hdr.crc32) return;
      for (uint8_t i = 0; i < hdr.count; i++) _alarms[i] = buf[i];
      _count = hdr.count;
      _dirty = false;
      saveToNVS();   // one-time migration write
    }
  }
};

} // namespace Services

// ============================================================================
// Tests
// ============================================================================
static int fails = 0;
static void expect(bool cond, const char* name) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
  if (!cond) fails++;
}

// Seed a VALID legacy v1 pair (what round-5 firmware wrote)
static void seedLegacy(Services::AlarmRegistry& reg) {
  Services::Alarm a[2] = {};
  strncpy(a[0].code, "BATTERY_VOLTAGE_LOW", 31);
  a[0].severity = Core::AlarmSeverity::Warning;
  a[0].lifecycle = Core::AlarmLifecycle::Active;
  a[0].raisedAt = 1757000100; a[0].lastUpdatedAt = a[0].raisedAt;
  strncpy(a[1].code, "SHT31_FAILURE", 31);
  a[1].severity = Core::AlarmSeverity::Warning;
  a[1].lifecycle = Core::AlarmLifecycle::Cleared;
  a[1].raisedAt = 1757000050; a[1].clearedAt = 1757000200;
  struct { uint32_t magic, version; uint8_t count; uint8_t r[3]; uint32_t crc32; } hdr = {};
  hdr.magic = 0x414C524Du; hdr.version = 1; hdr.count = 2;
  uint32_t crc = Utils::crc32((const uint8_t*)&hdr, sizeof(hdr) - 4);
  crc = Utils::crc32((const uint8_t*)a, 2 * sizeof(Services::Alarm), crc);
  hdr.crc32 = crc;
  std::vector<uint8_t> hv((uint8_t*)&hdr, (uint8_t*)&hdr + sizeof(hdr));
  std::vector<uint8_t> av((uint8_t*)a, (uint8_t*)a + 2 * sizeof(Services::Alarm));
  nvs.kv["hdr"] = hv;
  nvs.kv["arr"] = av;
  (void)reg;
}

int main() {
  using namespace Services;
  printf("== T1: fresh raise → persisted, single record, generation 1 ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    RaiseResult r = reg.raiseTracked("BATTERY_OVERCURRENT_DISCHARGE", Core::AlarmSeverity::Critical, "test");
    expect(r == RaiseResult::AcceptedPersisted, "raiseTracked → AcceptedPersisted");
    expect(nvs.kv.count("state") == 1, "exactly ONE NVS record written");
    expect(nvs.kv.count("hdr") == 0 && nvs.kv.count("arr") == 0, "legacy two-record layout NOT written");
    expect(reg._generation == 1, "generation committed to 1");
    AlarmStateBlob h = {};
    memcpy(&h, nvs.kv["state"].data(), sizeof(h));
    expect(h.magic == 0x414C524Du && h.version == 2 && h.count == 1, "blob header valid (magic/version/count)");
    expect(h.generation == 1, "blob carries generation marker [p.455]");
  }

  printf("== T2: round-trip restore (lifecycle + generation preserved) ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("ALPHA", Core::AlarmSeverity::Warning, "m1");
    reg.raise("BETA", Core::AlarmSeverity::Critical, "m2");
    reg.clear("BETA");
    AlarmRegistry reg2;
    reg2.loadFromNVS();
    expect(reg2._count == 2, "2 alarms restored");
    expect(reg2.find("ALPHA") && reg2.find("ALPHA")->lifecycle == Core::AlarmLifecycle::Active, "ALPHA Active");
    expect(reg2.find("BETA") && reg2.find("BETA")->lifecycle == Core::AlarmLifecycle::Cleared, "BETA Cleared");
    expect(reg2._generation == reg._generation, "generation adopted from blob");
  }

  printf("== T3: short write is REJECTED (p.453 — was `w2 != 0`) ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("KEEP", Core::AlarmSeverity::Warning);        // gen 1 OK
    uint16_t genBefore = reg._generation;
    nvs.truncateWriteTo = 40;                                // partial commit
    RaiseResult r = reg.raiseTracked("NEW", Core::AlarmSeverity::Critical, "x");
    expect(r == RaiseResult::AcceptedPersistFailed, "raiseTracked → AcceptedPersistFailed (honest) [p.454]");
    expect(reg._persistFailures == 1, "persist failure counted");
    expect(reg._generation == genBefore, "generation NOT committed on short write");
    expect(reg.find("NEW") != nullptr, "alarm still alive in RAM (runtime unaffected)");
  }

  printf("== T4: read-back mismatch is REJECTED (torn/worn media) ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    nvs.flipByteOnRead = 5;                                  // stored bytes read back different
    expect(reg.saveToNVS() == false, "saveToNVS false on read-back mismatch");
    expect(reg._persistFailures == 1, "failure counted");
    nvs.flipByteOnRead = -1;
    expect(reg.saveToNVS() == true, "save succeeds once media reads honest");
  }

  printf("== T5: corrupted stored blob → honest empty restore ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("VOLT", Core::AlarmSeverity::Critical, "v");
    nvs.kv["state"][40] ^= 0xA5;                             // bit-rot inside the alarm array
    AlarmRegistry reg2;
    reg2.loadFromNVS();                                       // must not crash (ASAN)
    expect(reg2._count == 0, "corrupt blob → empty registry (no silent adoption)");
  }

  printf("== T6: truncated stored blob → honest empty restore ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("VOLT", Core::AlarmSeverity::Critical, "v");
    reg.raise("CURR", Core::AlarmSeverity::Warning, "i");
    nvs.kv["state"].resize(nvs.kv["state"].size() - 10);      // torn tail
    AlarmRegistry reg2;
    reg2.loadFromNVS();
    expect(reg2._count == 0, "short stored record → empty registry");
  }

  printf("== T7: legacy v1 pair migrates to the single record ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    seedLegacy(reg);
    reg.loadFromNVS();
    expect(reg._count == 2, "legacy pair adopted (2 alarms)");
    expect(reg.find("SHT31_FAILURE") && reg.find("SHT31_FAILURE")->lifecycle == Core::AlarmLifecycle::Cleared,
           "legacy lifecycle preserved");
    expect(nvs.kv.count("state") == 1, "migration wrote the v2 single record");
    AlarmStateBlob h = {};
    memcpy(&h, nvs.kv["state"].data(), sizeof(h));
    expect(h.version == 2 && h.count == 2, "migrated record is v2 layout");
    // Second load prefers v2
    AlarmRegistry reg2;
    reg2.loadFromNVS();
    expect(reg2._count == 2 && reg2.find("BATTERY_VOLTAGE_LOW") != nullptr, "second boot loads v2 path");
  }

  printf("== T8: mixed-generation legacy pair (p.453 power-loss scenario) → honest empty ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    seedLegacy(reg);
    // Simulate the v1 failure mode: header updated, array NOT (old/stale arr)
    nvs.kv["arr"][3] ^= 0xFF;   // content no longer matches the header CRC
    AlarmRegistry reg2;
    reg2.loadFromNVS();
    expect(reg2._count == 0, "CRC rejects the mixed pair — honest empty, no half-state");
  }

  printf("== T9: raise-after-clear REACTIVATES (p.460 prerequisite) ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("OVERCURRENT", Core::AlarmSeverity::Warning, "first");
    uint32_t raised1 = reg.find("OVERCURRENT")->raisedAt;
    reg.clear("OVERCURRENT");
    expect(reg.find("OVERCURRENT")->lifecycle == Core::AlarmLifecycle::Cleared, "cleared");
    fake_rtc += 100;
    RaiseResult r = reg.raiseTracked("OVERCURRENT", Core::AlarmSeverity::Warning, "recurrence");
    const Alarm* a = reg.find("OVERCURRENT");
    expect(a->lifecycle == Core::AlarmLifecycle::Active, "re-raise → ACTIVE again (was: zombie Cleared)");
    expect(a->raisedAt > raised1, "raisedAt reflects the NEW occurrence");
    expect(a->clearedAt == 0, "clearedAt reset");
    expect(r == RaiseResult::AcceptedPersisted, "reactivation persisted (meaningful)");
    expect(reg.countActive() == 1, "countActive sees the recurrence");
  }

  printf("== T10: saturation reject / eviction / wrapper contract ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    for (int i = 0; i < 24; i++) {
      char c[32]; snprintf(c, 32, "ALARM_%02d", i);
      reg.raise(c, Core::AlarmSeverity::Warning);
    }
    expect(!reg.raise("ALARM_25", Core::AlarmSeverity::Critical), "bool raise() false on reject [p.451]");
    expect(reg.raiseTracked("ALARM_25", Core::AlarmSeverity::Critical) == RaiseResult::Rejected,
           "raiseTracked → Rejected");
    reg.clear("ALARM_00");
    expect(reg.raise("ALARM_25", Core::AlarmSeverity::Critical), "slot freed → accepted via CLEARED eviction");
    expect(reg.find("ALARM_00") == nullptr, "oldest CLEARED evicted");
  }

  printf("== T11: begin() failure counted, never faked ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    nvs.failBegin = true;
    expect(reg.saveToNVS() == false, "NVS begin() failure → false");
    expect(reg._persistFailures == 1, "counted");
    nvs.failBegin = false;
  }

  printf("== T12: generation is monotonic across saves ==\n");
  {
    nvs = FakeNvsStore();
    AlarmRegistry reg;
    reg.raise("A", Core::AlarmSeverity::Warning);
    reg.raise("B", Core::AlarmSeverity::Warning);
    reg.clear("A");
    uint16_t g = reg._generation;
    expect(g >= 3, "generation advanced with each durable transaction");
    AlarmStateBlob h = {};
    memcpy(&h, nvs.kv["state"].data(), sizeof(h));
    expect(h.generation == g, "stored blob carries the committed generation");
  }

  printf("\n== HASIL: %s (%d kegagalan) ==\n", fails == 0 ? "SEMUA LULUS" : "ADA KEGAGALAN", fails);
  return fails;
}
