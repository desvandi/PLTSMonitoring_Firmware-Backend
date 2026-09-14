// verify_alarm_concurrency.cpp — native harness for the ROUND-7
// AlarmRegistry serialization remediation (p.467 + p.468 + p.469).
//
// WHY THIS HARNESS EXISTS (auditor round-6 verdict): the round-6 native
// harnesses are single-threaded structural mirrors — they proved the
// persistence ALGORITHM atomic (p.453-p.455) but, by construction, say
// nothing about task interleavings. p.467/p.468/p.469 require a test that
// REALLY runs concurrent writers/readers/persistence and proves the
// registry serializes them.
//
// STRUCTURAL MIRROR of firmware/Services/AlarmRegistry.{h,cpp} @ round-7:
// raiseTracked()/raise()/clear()/clearIfActive()/acknowledge()/
// acknowledgeAll()/snapshotInto()/find()/highestActiveSeverity()/
// copyActiveAlarms()/saveToNVS()/loadFromNVS() are copied verbatim modulo
// the Arduino shims. The lock discipline is the mirror's subject:
//   public API → _lock() → *Unlocked() → _unlock()
// std::mutex stands in for the FreeRTOS mutex (xSemaphoreCreateMutex):
// both are non-recursive, priority-queueing (FreeRTOS adds priority
// inheritance; std::mutex does not — the harness's threads are
// OS-scheduled, so this difference does not affect the serialization
// property under test).
//
// TWO BUILD MODES:
//   default            — the SHIPPED (locked) registry: must be TSAN-clean
//                        and hold every snapshot invariant.
//   -DREGISTRY_NO_LOCK — the ROUND-6 (unlocked) registry as a NEGATIVE
//                        CONTROL: _lock()/_unlock() become no-ops and TSAN
//                        MUST report data races (a clean run here would
//                        mean the harness is blind — a test failure).
//
// Build (treatment):
//   g++ -std=c++17 -g -fsanitize=thread        -pthread -o t verify_alarm_concurrency.cpp
//   g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o a verify_alarm_concurrency.cpp
// Build (negative control):
//   g++ -std=c++17 -g -fsanitize=thread -DREGISTRY_NO_LOCK -pthread -o u verify_alarm_concurrency.cpp
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Arduino shims — each one is INTERNALLY SYNCHRONIZED like its firmware
// counterpart (LogService has a mutex; the NVS driver has a global lock;
// the RTC read is a driver call). This keeps TSAN focused on the REGISTRY:
// any race it reports below is a registry race, not a shim artifact.
// ============================================================================
namespace Core {
enum class AlarmSeverity : uint8_t { Info = 0, Warning = 1, Critical = 2 };
enum class AlarmLifecycle : uint8_t { Active, Acknowledged, Cleared };
enum class LogType : uint8_t { Info, AlarmActive, AlarmCleared, AlarmAcknowledged, StorageError };
} // namespace Core

static std::atomic<uint32_t> fake_rtc{1757000000u};
static uint32_t rtcNow() { return fake_rtc.fetch_add(1, std::memory_order_relaxed); }
static std::atomic<uint32_t> fake_millis{1000};
static uint32_t millis() { return fake_millis.load(std::memory_order_relaxed); }

// LogService mirror — internally locked (the real LogService owns a mutex).
struct LogSpy {
  std::mutex m;
  unsigned storageErrors = 0, alarmActive = 0, alarmCleared = 0, alarmAck = 0, info = 0;
  void append(Core::LogType t, const std::string&, int8_t = 0) {
    std::lock_guard<std::mutex> g(m);
    switch (t) {
      case Core::LogType::StorageError: storageErrors++; break;
      case Core::LogType::AlarmActive: alarmActive++; break;
      case Core::LogType::AlarmCleared: alarmCleared++; break;
      case Core::LogType::AlarmAcknowledged: alarmAck++; break;
      default: info++; break;
    }
  }
};
static LogSpy Log;

// NVS driver mirror — internally locked (nvs_api serializes on its own
// handle lock), one journaled key write per putBytes. failNextWrite models
// a TRANSIENT NVS hiccup: exactly one short write, so the harness exercises
// the firmware's real retry loop — a failed immediate save leaves _dirty
// set, and the persistence-task checkpoint retries it later.
struct FakeNvsStore {
  std::mutex m;
  std::map<std::string, std::vector<uint8_t>> kv;
  uint64_t ops = 0;
  bool failNextWrite = false;
};
static FakeNvsStore nvs;

struct Preferences {
  bool begin(const char*, bool) { return true; }
  void end() {}
  size_t putBytes(const char* key, const void* buf, size_t len) {
    // Simulated flash-write latency — widens the interleaving window the
    // persistence race (p.468) needs to be observable in the negative
    // control; also keeps the locked build honest about hold times.
    std::this_thread::sleep_for(std::chrono::microseconds(120));
    std::lock_guard<std::mutex> g(nvs.m);
    nvs.ops++;
    if (nvs.failNextWrite) {
      nvs.failNextWrite = false;   // transient — exactly one failed write
      nvs.kv[key] = std::vector<uint8_t>((const uint8_t*)buf, (const uint8_t*)buf + (len ? len - 1 : 0));
      return len ? len - 1 : 0;   // short write
    }
    const uint8_t* p = (const uint8_t*)buf;
    nvs.kv[key] = std::vector<uint8_t>(p, p + len);
    return len;
  }
  size_t getBytes(const char* key, void* buf, size_t len) {
    std::lock_guard<std::mutex> g(nvs.m);
    auto it = nvs.kv.find(key);
    if (it == nvs.kv.end()) return 0;
    size_t n = it->second.size() < len ? it->second.size() : len;
    memcpy(buf, it->second.data(), n);
    return n;
  }
};

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

// ============================================================================
// STRUCTURAL MIRROR of the round-7 AlarmRegistry (locked discipline).
// Everything below mirrors firmware/Services/AlarmRegistry.{h,cpp}.
// ============================================================================
namespace Services {

struct Alarm {
  static constexpr uint8_t CODE_LEN = 32;
  char code[CODE_LEN];
  Core::AlarmSeverity severity;
  Core::AlarmLifecycle lifecycle;
  uint32_t raisedAt;
  uint32_t acknowledgedAt;
  uint32_t clearedAt;
  uint32_t lastUpdatedAt;
  char message[80];
};

enum class RaiseResult : uint8_t {
  Rejected = 0,
  AcceptedRam = 1,
  AcceptedPersisted = 2,
  AcceptedPersistFailed = 3,
};

class AlarmRegistry {
public:
  static constexpr uint8_t MAX_ALARMS = 24;

  struct Snapshot {
    Alarm alarms[MAX_ALARMS];
    uint8_t count;
    uint32_t overflowCount;
    uint32_t persistFailures;
    uint16_t generation;
    bool dirty;
  };

  void begin() {
    if (!_mutexCreated) { new (&_mutex_storage) std::mutex(); _mutexCreated = true; }
    _count = 0;
    _dirty = false;
    _generation = 0;
    for (uint8_t i = 0; i < MAX_ALARMS; i++) { _alarms[i] = Alarm{}; _alarms[i].code[0] = '\0'; }
    loadFromNVS();
  }

  RaiseResult raiseTracked(const char* code, Core::AlarmSeverity sev, const char* message = "") {
    _lock();
    RaiseResult r = _raiseTrackedUnlocked(code, sev, message);
    _unlock();
    return r;
  }
  bool raise(const char* code, Core::AlarmSeverity sev, const char* message = "") {
    return raiseTracked(code, sev, message) != RaiseResult::Rejected;
  }
  void clear(const char* code) { _lock(); _clearUnlocked(code); _unlock(); }
  bool clearIfActive(const char* code) {
    _lock();
    bool c = _clearIfActiveUnlocked(code);
    _unlock();
    return c;
  }
  void acknowledge(const char* code) { _lock(); _acknowledgeUnlocked(code); _unlock(); }
  void acknowledgeAll() { _lock(); _acknowledgeAllUnlocked(); _unlock(); }

  void snapshotInto(Snapshot& out) const {
    _lock();
    memcpy(out.alarms, _alarms, sizeof(_alarms));
    out.count = _count;
    out.overflowCount = _overflowCount;
    out.persistFailures = _persistFailures;
    out.generation = _generation;
    out.dirty = _dirty;
    _unlock();
  }
  uint8_t countAll() const { _lock(); uint8_t c = _count; _unlock(); return c; }
  bool find(const char* code, Alarm& out) const {
    _lock();
    uint8_t idx = _findIdx(code);
    bool found = (idx != 0xFF);
    if (found) out = _alarms[idx];
    _unlock();
    return found;
  }
  static Core::AlarmSeverity highestSeverityIn(const Alarm* list, uint8_t count) {
    Core::AlarmSeverity h = Core::AlarmSeverity::Info;
    for (uint8_t i = 0; i < count; i++) {
      if (list[i].lifecycle != Core::AlarmLifecycle::Cleared &&
          (uint8_t)list[i].severity > (uint8_t)h) h = list[i].severity;
    }
    return h;
  }
  Core::AlarmSeverity highestActiveSeverity() const {
    _lock();
    Core::AlarmSeverity h = highestSeverityIn(_alarms, _count);
    _unlock();
    return h;
  }
  static uint8_t copyActiveFrom(const Alarm* list, uint8_t count, Alarm* dst, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < count && n < max; i++) {
      if (list[i].lifecycle != Core::AlarmLifecycle::Cleared) dst[n++] = list[i];
    }
    return n;
  }
  uint8_t copyActiveAlarms(Alarm* dst, uint8_t max) const {
    _lock();
    uint8_t n = copyActiveFrom(_alarms, _count, dst, max);
    _unlock();
    return n;
  }
  uint32_t overflowCount() const { _lock(); uint32_t v = _overflowCount; _unlock(); return v; }
  bool isDirty() const { _lock(); bool d = _dirty; _unlock(); return d; }
  uint32_t persistFailures() const { _lock(); uint32_t v = _persistFailures; _unlock(); return v; }
  uint16_t generation() const { _lock(); uint16_t g = _generation; _unlock(); return g; }

  bool saveToNVS() { _lock(); bool ok = _saveToNVSUnlocked(); _unlock(); return ok; }
  void loadFromNVS() { _lock(); _loadFromNVSUnlocked(); _unlock(); }

private:
  // [p.467] The mutex. REGISTRY_NO_LOCK compiles the ROUND-6 registry
  // (no-ops) as the negative control.
  bool _mutexCreated = false;
  union { unsigned char _mutex_storage[sizeof(std::mutex)]; long long _align; };
#ifdef REGISTRY_NO_LOCK
  void _lock() const {}
  void _unlock() const {}
#else
  std::mutex& _mutex() const {
    return *const_cast<std::mutex*>(reinterpret_cast<const std::mutex*>(_mutex_storage));
  }
  void _lock() const { _mutex().lock(); }
  void _unlock() const { _mutex().unlock(); }
#endif

  Alarm _alarms[MAX_ALARMS] = {};
  uint8_t _count = 0;
  bool _dirty = false;
  uint32_t _overflowCount = 0;
  uint32_t _persistFailures = 0;
  uint16_t _generation = 0;

  uint8_t _findIdx(const char* code) const {
    if (!code) return 0xFF;
    for (uint8_t i = 0; i < _count; i++) {
      if (strncmp(_alarms[i].code, code, Alarm::CODE_LEN) == 0) return i;
    }
    return 0xFF;
  }

  RaiseResult _raiseTrackedUnlocked(const char* code, Core::AlarmSeverity sev, const char* message) {
    if (!code) return RaiseResult::Rejected;
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) {
      // New alarm
      if (_count >= MAX_ALARMS) {
        uint8_t oldest = 0xFF;
        uint32_t oldestClearedAt = 0xFFFFFFFF;
        uint32_t oldestRaisedAt = 0xFFFFFFFF;
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
      } else {
        a.message[0] = '\0';
      }
      if (idx == _count) _count++;
      _dirty = true;
      bool persisted = _saveToNVSUnlocked();
      if (!persisted) {
        Log.append(Core::LogType::StorageError,
                   std::string("[ALARM:") + code + "] accepted in RAM but the immediate " +
                   "NVS persistence FAILED", -1);
      }
      Log.append(Core::LogType::AlarmActive, std::string("[ALARM:") + code + "] " +
                   (message ? message : ""), -1);
      return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
    } else {
      // Refresh existing alarm
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
        Log.append(Core::LogType::AlarmActive,
                   std::string("[ALARM:") + code + "] re-activated", -1);
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
        bool persisted = _saveToNVSUnlocked();
        if (!persisted) {
          Log.append(Core::LogType::StorageError,
                     std::string("[ALARM:") + code + "] meaningful update accepted in RAM "
                     "but the immediate NVS persistence FAILED", -1);
        }
        return persisted ? RaiseResult::AcceptedPersisted : RaiseResult::AcceptedPersistFailed;
      }
      return RaiseResult::AcceptedRam;
    }
  }

  void _clearUnlocked(const char* code) {
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) return;
    Alarm& a = _alarms[idx];
    a.lifecycle = Core::AlarmLifecycle::Cleared;
    a.clearedAt = rtcNow();
    a.lastUpdatedAt = a.clearedAt;
    _dirty = true;
    _saveToNVSUnlocked();
    Log.append(Core::LogType::AlarmCleared, std::string("Alarm cleared: ") + code, -1);
  }

  bool _clearIfActiveUnlocked(const char* code) {
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) return false;
    Alarm& a = _alarms[idx];
    if (a.lifecycle == Core::AlarmLifecycle::Cleared) return false;
    a.lifecycle = Core::AlarmLifecycle::Cleared;
    a.clearedAt = rtcNow();
    a.lastUpdatedAt = a.clearedAt;
    _dirty = true;
    _saveToNVSUnlocked();
    Log.append(Core::LogType::AlarmCleared, std::string("Alarm cleared: ") + code, -1);
    return true;
  }

  void _acknowledgeUnlocked(const char* code) {
    uint8_t idx = _findIdx(code);
    if (idx == 0xFF) return;
    Alarm& a = _alarms[idx];
    a.lifecycle = Core::AlarmLifecycle::Acknowledged;
    a.acknowledgedAt = rtcNow();
    a.lastUpdatedAt = a.acknowledgedAt;
    _dirty = true;
    _saveToNVSUnlocked();
    Log.append(Core::LogType::AlarmAcknowledged, std::string("Alarm acknowledged: ") + code, -1);
  }

  void _acknowledgeAllUnlocked() {
    for (uint8_t i = 0; i < _count; i++) {
      if (_alarms[i].lifecycle == Core::AlarmLifecycle::Active) {
        _alarms[i].lifecycle = Core::AlarmLifecycle::Acknowledged;
        _alarms[i].acknowledgedAt = rtcNow();
      }
    }
    _dirty = true;
    _saveToNVSUnlocked();
  }

  bool _saveToNVSUnlocked() {
    const size_t blobLen = sizeof(AlarmStateBlob) + (size_t)_count * sizeof(Alarm);
    uint8_t* buf = (uint8_t*)malloc(blobLen * 2);
    if (!buf) {
      _persistFailures++;
      Log.append(Core::LogType::StorageError, "out of memory building state blob", -1);
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

  void _loadFromNVSUnlocked() {
    // Harness begins empty ("namespace absent — fresh, honest" path). The
    // persistence-format algorithm itself is covered 1:1 by the round-6
    // harness verify_alarm_blob_atomicity.cpp; this harness is about
    // CONCURRENCY, not blob layout.
  }

  struct AlarmStateBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t generation;
    uint8_t count;
    uint8_t reserved[3];
    uint32_t crc32;
  };
  static constexpr uint32_t ALARM_NVS_MAGIC = 0x414C524Du;
  static constexpr uint16_t ALARM_STATE_VERSION = 2u;
};

static AlarmRegistry alarms;

} // namespace Services

// ============================================================================
// Test driver
// ============================================================================
static std::atomic<int> g_checks{0}, g_failures{0};
#define CHECK(cond, msg)                                                        \
  do {                                                                          \
    g_checks.fetch_add(1, std::memory_order_relaxed);                           \
    if (!(cond)) {                                                               \
      g_failures.fetch_add(1, std::memory_order_relaxed);                       \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                           \
    }                                                                           \
  } while (0)

static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_snapshotValidations{0};
static std::atomic<uint64_t> g_raises{0}, g_rejections{0}, g_saves{0};

// Snapshot invariants — these are the p.469 coherence properties: whatever
// a reader observes must be a state the registry could legally be in at
// ONE instant (no torn counts, no duplicate codes, no unterminated/garbage
// slots within count, counters monotonic).
static void validateSnapshot(const Services::AlarmRegistry::Snapshot& s,
                              uint32_t& prevOverflow, uint32_t& prevPersist,
                              uint16_t& prevGen, const char* who) {
  CHECK(s.count <= Services::AlarmRegistry::MAX_ALARMS,
        "snapshot count within MAX_ALARMS");
  for (uint8_t i = 0; i < s.count; i++) {
    const Services::Alarm& a = s.alarms[i];
    bool terminated = memchr(a.code, '\0', Services::Alarm::CODE_LEN) != nullptr;
    CHECK(terminated && a.code[0] != '\0', "code is terminated and non-empty");
    CHECK((uint8_t)a.severity <= 2, "severity is a valid enum");
    CHECK((uint8_t)a.lifecycle <= 2, "lifecycle is a valid enum");
    bool msgTerminated = memchr(a.message, '\0', sizeof(a.message)) != nullptr;
    CHECK(msgTerminated, "message is terminated");
  }
  // No duplicate codes among [0, count) — the double-append race of p.467
  // (two writers both seeing _findIdx()==0xFF) would manifest right here.
  for (uint8_t i = 0; i < s.count; i++) {
    for (uint8_t j = (uint8_t)(i + 1); j < s.count; j++) {
      if (strncmp(s.alarms[i].code, s.alarms[j].code, Services::Alarm::CODE_LEN) == 0) {
        g_checks.fetch_add(1, std::memory_order_relaxed);
        g_failures.fetch_add(1, std::memory_order_relaxed);
        printf("  FAIL: duplicate code '%s' at slots %u/%u — torn registry state (%s)\n",
               s.alarms[i].code, i, j, who);
        return;
      }
    }
  }
  // Monotonic counters — a reader must never see time (or accounting) go
  // backwards between successive snapshots.
  CHECK(s.overflowCount >= prevOverflow, "overflowCount monotonic");
  CHECK(s.persistFailures >= prevPersist, "persistFailures monotonic");
  CHECK(s.generation >= prevGen, "generation monotonic per reader");
  prevOverflow = s.overflowCount;
  prevPersist = s.persistFailures;
  prevGen = s.generation;
  g_snapshotValidations++;
}

// Writer A/B — evaluator-style churn: raise/clear/clearIfActive over MORE
// codes than there are slots (saturates the registry, exercising eviction
// of CLEARED entries and honest rejection), plus SHARED_* codes both
// writers touch (the _findIdx contention window of p.467).
static void writerThread(int id, int iterations) {
  std::mt19937 rng(0xC0DE0000u + (uint32_t)id);
  const int OWN = 30;   // > MAX_ALARMS(24): saturation + eviction + reject
  for (int it = 0; it < iterations && !g_stop; it++) {
    char code[Services::Alarm::CODE_LEN];
    int pick = (int)(rng() % 100);
    if (pick < 10) {
      snprintf(code, sizeof(code), "SHARED_%02d", (int)(rng() % 4));
    } else {
      snprintf(code, sizeof(code), "W%d_%02d", id, (int)(rng() % OWN));
    }
    Core::AlarmSeverity sev =
        (Core::AlarmSeverity)(rng() % 3);
    char msg[32];
    snprintf(msg, sizeof(msg), "m%d-%d", id, it % 7);
    Services::RaiseResult r = Services::alarms.raiseTracked(code, sev, msg);
    g_raises++;
    if (r == Services::RaiseResult::Rejected) g_rejections++;
    int act = (int)(rng() % 10);
    if (act == 0) {
      Services::alarms.clear(code);
    } else if (act == 1) {
      Services::alarms.clearIfActive(code);
    } else if (act == 2) {
      Services::alarms.acknowledge(code);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(30 + (rng() % 120)));
  }
}

// Operator thread — acknowledgeAll over whatever is present (the MQTT
// alarm.acknowledgeAll path). Once, mid-run, it also injects a TRANSIENT
// NVS write failure: the immediate save inside the next mutation fails,
// _dirty stays set, and the persistence-task checkpoint retries — the
// exact firmware recovery contract (RaiseResult::AcceptedPersistFailed
// → periodic checkpoint owns the retry).
static void operatorThread(int iterations) {
  std::mt19937 rng(0xABCD1234u);
  bool injected = false;
  for (int it = 0; it < iterations / 4 && !g_stop; it++) {
    if (!injected && it == 10) {
      std::lock_guard<std::mutex> g(nvs.m);
      nvs.failNextWrite = true;
      injected = true;
    }
    Services::alarms.acknowledgeAll();
    std::this_thread::sleep_for(std::chrono::microseconds(200 + (rng() % 400)));
  }
}

// Persistence task — the firmware_v1.ino checkpoint loop, verbatim shape:
// "if (isDirty()) saveToNVS()". This is the p.468 contestant: its save
// races the immediate saves inside raise/clear/ack unless serialized.
static void persistenceThread() {
  std::mt19937 rng(0xFEED0000u);
  while (!g_stop) {
    if (Services::alarms.isDirty()) {
      bool ok = Services::alarms.saveToNVS();
      (void)ok;
      g_saves++;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(50 + (rng() % 150)));
  }
}

// Reader threads — API/telemetry style consumption: one snapshotInto per
// loop, validated against the coherence invariants (p.469). The old
// countAll()+getAlarm(i) multi-call pattern is deliberately NOT mirrored
// — the snapshot is the remediation under test.
static void readerThread(int id) {
  uint32_t prevOverflow = 0, prevPersist = 0;
  uint16_t prevGen = 0;
  std::mt19937 rng(0xBEEF0000u + (uint32_t)id);
  while (!g_stop) {
    Services::AlarmRegistry::Snapshot* s =
        (Services::AlarmRegistry::Snapshot*)malloc(sizeof(Services::AlarmRegistry::Snapshot));
    if (!s) continue;
    Services::alarms.snapshotInto(*s);
    // Envelope-style derivation from the SAME snapshot (telemetry path).
    Services::Alarm buf[Services::AlarmRegistry::MAX_ALARMS];
    uint8_t n = Services::AlarmRegistry::copyActiveFrom(s->alarms, s->count, buf,
                                                        Services::AlarmRegistry::MAX_ALARMS);
    (void)Services::AlarmRegistry::highestSeverityIn(s->alarms, s->count);
    (void)n;
    validateSnapshot(*s, prevOverflow, prevPersist, prevGen, id == 0 ? "readerA" : "readerB");
    free(s);
    std::this_thread::sleep_for(std::chrono::microseconds(40 + (rng() % 120)));
  }
}

// Watchdog — a deadlock in the locked build (nested acquisition bug) would
// hang the harness; fail loudly instead of blocking CI.
static void watchdog() {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (std::chrono::steady_clock::now() > deadline) {
      printf("  FAIL: watchdog timeout — registry deadlock (line %d)\n", __LINE__);
      g_failures++;
      std::abort();
    }
  }
}

int main() {
  printf("=== AlarmRegistry concurrency harness (round-7 p.467/p.468/p.469) ===\n");
#ifdef REGISTRY_NO_LOCK
  printf("mode: NEGATIVE CONTROL (round-6 unlocked registry) — races EXPECTED\n");
#else
  printf("mode: TREATMENT (round-7 locked registry) — must be clean\n");
#endif
  Services::alarms.begin();

  const int ITER = 1500;

  std::thread wd(watchdog);
  std::thread wA(writerThread, 0, ITER);
  std::thread wB(writerThread, 1, ITER);
  std::thread op(operatorThread, ITER);
  std::thread per(persistenceThread);
  std::thread rA(readerThread, 0);
  std::thread rB(readerThread, 1);

  wA.join();
  wB.join();
  op.join();

  // ---- Quiescent final state ----
  Services::AlarmRegistry::Snapshot fin;
  Services::alarms.snapshotInto(fin);
  uint32_t pO = 0, pP = 0; uint16_t pG = 0;
  validateSnapshot(fin, pO, pP, pG, "final");
  CHECK(fin.count <= Services::AlarmRegistry::MAX_ALARMS, "final count <= 24");

  // Free the slots the stress storm left occupied (acknowledged entries are
  // NOT evictable — the honest p.451 policy — so the smoke test clears them
  // explicitly, the operator's documented remedy for a saturated registry).
  for (uint8_t i = 0; i < fin.count; i++) {
    Services::alarms.clear(fin.alarms[i].code);
  }

  // ---- Quiescent retry phase (deterministic checkpoint exercise) ----
  // The storm's immediate saves make _dirty unobservable to the
  // persistence thread (each successful save clears it while still under
  // the lock). This phase reproduces the firmware's actual recovery
  // contract deterministically: inject EXACTLY one failing save on a
  // quiescent registry — the immediate save inside raiseTracked() fails
  // (AcceptedPersistFailed, _dirty stays set) — and with no further
  // mutations, ONLY the persistence-task checkpoint can retry it.
  {
    {
      std::lock_guard<std::mutex> g(nvs.m);
      nvs.failNextWrite = true;
    }
    Services::RaiseResult r = Services::alarms.raiseTracked(
        "QUIET_RETRY", Core::AlarmSeverity::Warning, "quiescent retry probe");
    CHECK(r == Services::RaiseResult::AcceptedPersistFailed,
          "injected NVS failure surfaces honestly in the raise contract");
    // Idle — the persistence thread's checkpoint now owns the retry.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!Services::alarms.isDirty(), "checkpoint retried the failed save while idle");
  }

  g_stop = true;
  per.join();
  rA.join();
  rB.join();
  wd.join();

  // ---- Functional smoke after the storm (locked semantics intact) ----
  CHECK(Services::alarms.raise("FINAL_SMOKE", Core::AlarmSeverity::Critical, "smoke"), "raise accepted");
  Services::Alarm found;
  CHECK(Services::alarms.find("FINAL_SMOKE", found), "find after raise");
  CHECK(found.lifecycle == Core::AlarmLifecycle::Active, "active after raise");
  CHECK(found.severity == Core::AlarmSeverity::Critical, "severity preserved");
  Services::alarms.clear("FINAL_SMOKE");
  CHECK(Services::alarms.find("FINAL_SMOKE", found) &&
        found.lifecycle == Core::AlarmLifecycle::Cleared, "cleared after clear");
  CHECK(!Services::alarms.clearIfActive("FINAL_SMOKE"), "clearIfActive on cleared = false");
  CHECK(Services::alarms.raise("FINAL_SMOKE", Core::AlarmSeverity::Warning, "react") &&
        Services::alarms.find("FINAL_SMOKE", found) &&
        found.lifecycle == Core::AlarmLifecycle::Active, "reactivation works");

  // ---- Saturation actually exercised (the p.451/p.467 stress paths) ----
  printf("stats: raises=%llu rejections=%llu checkpointSaves=%llu snapshotValidations=%llu "
         "finalCount=%u\n",
         (unsigned long long)g_raises.load(), (unsigned long long)g_rejections.load(),
         (unsigned long long)g_saves.load(),
         (unsigned long long)g_snapshotValidations.load(), fin.count);
  CHECK(g_raises.load() > 1000, "substantial raise churn executed");
  CHECK(g_rejections.load() > 0, "saturation/rejection path exercised");
  CHECK(g_saves.load() > 0, "persistence checkpoint exercised (retry after transient NVS failure)");
  CHECK(g_snapshotValidations.load() > 1000, "substantial reader churn executed");
  CHECK(Services::alarms.persistFailures() >= 2,
        "both injected transient NVS failures were counted honestly");
  CHECK(Services::alarms.persistFailures() <= 4,
        "persist failures bounded (exactly two injected)");

  printf("checks=%d failures=%d\n", g_checks.load(), g_failures.load());
  if (g_failures.load() == 0) {
    printf("ALARM REGISTRY CONCURRENCY HARNESS: ALL CHECKS PASS\n");
    return 0;
  }
  printf("ALARM REGISTRY CONCURRENCY HARNESS: FAILED\n");
  return 1;
}
