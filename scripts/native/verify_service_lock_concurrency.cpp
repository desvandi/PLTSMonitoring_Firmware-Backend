// verify_service_lock_concurrency.cpp — native harness for the ROUND-10
// fail-open/fail-closed family remediation (p.472 + p.473 + p.474 + p.475).
//
// WHY THIS HARNESS EXISTS (auditor round-10 verdict): p.471 closed the
// AlarmRegistry fail-open hole, but the same family survived in four other
// services — LogService readers were unlocked AND its writers failed open
// (p.472), TransactionJournal::_lock() was a no-op when the handle was null
// (p.473), BatteryCommManager::crossCheckShunt() read _data BEFORE checking
// the mutex and never acquired it (p.474), and AuthManager::_lockAuth()
// failed open, reviving the refresh-token replay race (p.475). Static gates
// alone cannot prove these fixes; each claim needs a test that REALLY runs
// concurrent writers/readers and proves the services serialize them — and
// REALLY runs the lock-unavailable storm and proves every operation is
// refused fail-closed.
//
// STRUCTURAL MIRRORS of firmware/Services/LogService.{h,cpp},
// firmware/Services/TransactionJournal.{h,cpp},
// firmware/Comm/BatteryCommManager.{h,cpp} and
// firmware/Services/AuthManager.{h,cpp} @ round-10: the public operations
// are copied modulo the Arduino shims; the LOCK DISCIPLINE is the mirror's
// subject:
//   public API → if (!_lock()) return <fail-closed value> → body → _unlock()
// std::timed_mutex stands in for the FreeRTOS mutex (xSemaphoreCreateMutex +
// xSemaphoreTake with/without timeout): both are non-recursive; FreeRTOS
// adds priority inheritance, std::mutex does not — the harness threads are
// OS-scheduled, so this difference does not affect the serialization
// property under test.
//
// THREE BUILD MODES:
//   default            — the SHIPPED (fail-closed) services: must be
//                        TSAN-clean and hold every phase invariant, including
//                        the round-10 lock-unavailable storm (Phase X) and
//                        the deterministic recovery (Phase W).
//   -DSVC_NO_LOCK      — the fully UNLOCKED services as a NEGATIVE CONTROL:
//                        _lock()/_unlock() become no-ops. TSAN MUST report
//                        data races in the storm phases and the phase
//                        invariants MUST break (a clean run would mean the
//                        harness is blind — a test failure).
//   -DLOCK_FAIL_OPEN   — the PRE-ROUND-10 shapes as the p.472–p.475 NEGATIVE
//                        CONTROL: LogService proceeds without the lock when
//                        the handle is null, TransactionJournal::_lock() is
//                        a no-op, crossCheckShunt() reads _data before the
//                        mutex check and never acquires it, _lockAuth() only
//                        takes an existing handle. The Phase V boot-guard and
//                        Phase X fail-closed contracts MUST trip their
//                        sentinels in this mode (and TSAN reports races) —
//                        a clean exit would mean the harness cannot detect
//                        the fail-open class round-10 removes.
//
// Build (treatment):
//   g++ -std=c++17 -g -fsanitize=thread                  -pthread -o s10t verify_service_lock_concurrency.cpp
//   g++ -std=c++17 -g -fsanitize=address,undefined       -pthread -o s10a verify_service_lock_concurrency.cpp
// Build (negative controls):
//   g++ -std=c++17 -g -fsanitize=thread -DSVC_NO_LOCK    -pthread -o s10u verify_service_lock_concurrency.cpp
//   g++ -std=c++17 -g -fsanitize=thread -DLOCK_FAIL_OPEN -pthread -o s10f verify_service_lock_concurrency.cpp
//
// MIRROR-ONLY INJECTION HOOKS (no firmware counterpart):
//   g_injectMutexCreateFail — models xSemaphoreCreateMutex() returning
//     nullptr (heap exhaustion). The firmware's begin() FATAL-halts on this;
//     a test process cannot, so each begin() mirrors the halt as
//     `return false` ("boot refused") and the runtime path exercises the
//     fail-closed reject. Hooks are read/written single-threaded (phases
//     run between joins); the fail-closed path itself only touches ATOMICS
//     — exactly the firmware shape, and it keeps the treatment TSAN-clean.
//   tickWrite()'s inter-field sleep — widens the torn-snapshot window so a
//     NO_LOCK getData() copy can observe mixed generations deterministically
//     (the firmware writes the struct in one assignment; the property under
//     test — coherent snapshots — is unchanged).
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
// counterpart (the NVS driver has a global lock; the RTC read is a driver
// call). This keeps TSAN focused on the FOUR SERVICES: any race it reports
// below is a service race, not a shim artifact.
// ============================================================================
namespace Core {
enum class LogType : uint8_t { Info, StorageError, AuthFail, Logout, Custom };
} // namespace Core

static std::atomic<uint32_t> fake_rtc{1757000000u};
static uint32_t rtcNow() { return fake_rtc.fetch_add(1, std::memory_order_relaxed); }
// Constant fake clock (round-9 shape): time does not advance under the
// storm, so the freshness windows under test stay deterministic.
static std::atomic<uint32_t> fake_millis{1000};
static uint32_t millis() { return fake_millis.load(std::memory_order_relaxed); }

// [round-10] Mirror-only injection: models xSemaphoreCreateMutex() returning
// nullptr (see the file header for the modeling contract).
static std::atomic<bool> g_injectMutexCreateFail{false};

// LogService mirror of the LogService mirror (:-)) — the REAL LogService is
// itself one of the four services under test, so the other three mirrors
// log through this internally-locked spy, mirroring the firmware's
// "service mutex → LogService mutex (leaf)" lock hierarchy (no cycle).
struct LogSpy {
  std::mutex m;
  unsigned storageErrors = 0, authFails = 0, infos = 0;
  void append(Core::LogType t, const std::string&, int8_t = 0) {
    std::lock_guard<std::mutex> g(m);
    switch (t) {
      case Core::LogType::StorageError: storageErrors++; break;
      case Core::LogType::AuthFail:     authFails++; break;
      default:                          infos++; break;
    }
  }
};
static LogSpy Log;

// NVS driver mirror — internally locked (nvs_api serializes on its handle
// lock), one journaled key write per putBytes. The op counter proves the
// Phase X invariant "zero NVS writes during the lock-unavailable storm".
struct FakeNvsStore {
  std::mutex m;
  std::map<std::string, std::vector<uint8_t>> kv;
  std::atomic<uint64_t> ops{0};
};
static FakeNvsStore nvs;

struct Preferences {
  bool begin(const char*, bool) { return true; }
  void end() {}
  size_t putBytes(const char* key, const void* buf, size_t len) {
    // Simulated flash-write latency — widens the interleaving window the
    // p.473 store-vs-updateAck race needs to be observable; also keeps the
    // locked build honest about hold times.
    std::this_thread::sleep_for(std::chrono::microseconds(120));
    std::lock_guard<std::mutex> g(nvs.m);
    nvs.ops.fetch_add(1, std::memory_order_relaxed);
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

// ============================================================================
// MutexCell — the "SemaphoreHandle_t" mirror: nullable handle + placement-
// new'd std::timed_mutex. created==false mirrors _mutex == nullptr.
// [CI lesson, 2nd iteration] `created` is ATOMIC and the placement-new is
// gated by a process-wide std::mutex: two threads racing the lazy
// (re-)create could otherwise each construct their OWN timed_mutex over
// the storage — both would then "hold a lock" on different objects and
// race the protected state (exactly the CI TSAN signature on _head). The
// firmware's post-boot-guard retry-create has the same theoretical window;
// there it is unreachable-in-practice (the handle is never null after
// begin()) — the gate here is mirror-only defense.
// ============================================================================
struct MutexCell {
  mutable std::atomic<bool> created{false};
  union { unsigned char storage[sizeof(std::timed_mutex)]; long long align; };

  static std::mutex& createGate() {
    static std::mutex gate;
    return gate;
  }

  std::timed_mutex& ref() const {
    return *const_cast<std::timed_mutex*>(
        reinterpret_cast<const std::timed_mutex*>(storage));
  }
  // Mirrors xSemaphoreCreateMutex() — fails under injection. Creation is
  // serialized process-wide; double-construct is impossible by construction.
  bool create() const {
    if (created.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> g(createGate());
    if (created.load(std::memory_order_relaxed)) return true;
    if (g_injectMutexCreateFail.load(std::memory_order_relaxed)) return false;
    ::new (static_cast<void*>(const_cast<unsigned char*>(storage))) std::timed_mutex();
    created.store(true, std::memory_order_release);
    return true;
  }
};

// ============================================================================
// PHASE R MIRROR — LogService (p.472): ring + audit buffer, WRITERS and
// READERS both fail-closed. The old shapes: writers skipped the lock when
// the handle was null; readers never locked at all.
// ============================================================================
namespace Services {

class LogSvc {
public:
  static constexpr uint16_t MAX_ENTRIES = 64;

  // begin() mirrors the firmware BOOT GUARD (FATAL halt → test refusal).
  bool begin() {
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    (void)_cell.create();   // OLD shape: no boot guard — creation "cannot fail"
    resetState();
    return true;
#else
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;   // [p.472] BOOT REFUSED
    }
    resetState();
    return true;
#endif
  }

  void resetState() {
    _head = 0; _count = 0; _nextId = 1;
    _auditBuf.clear();
    _auditDirty = false;
  }

  // append(): writer, fail-closed (drop on unavailable — FW-24 semantics).
  void append(const char* msg) {
#if defined(SVC_NO_LOCK)
    appendBody(msg);                                   // OLD-OLD: no lock at all
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape (p.472): `if (_mutex && take != ok) return; ... if (_mutex) give;`
    // handle null → PROCEED WITHOUT THE LOCK.
    if (_cell.created && !tryLockBody(500)) return;
    appendBody(msg);
    if (_cell.created) _cell.ref().unlock();
#else
    if (!_lock()) return;   // [p.472] fail-closed drop, counted
    appendBody(msg);
    _unlock();
#endif
  }

  void audit(const char* line) {
#if defined(SVC_NO_LOCK)
    auditBody(line);
#elif defined(LOCK_FAIL_OPEN)
    if (_cell.created && !tryLockBody(500)) return;
    auditBody(line);
    if (_cell.created) _cell.ref().unlock();
#else
    if (!_lock()) return;   // [p.472] fail-closed, counted
    auditBody(line);
    _unlock();
#endif
  }

  // getActivity(): READER — the p.472 core. Old shape: NO lock ever.
  // Returns "id:msg;id:msg;..." (the JSON serialization is not the subject).
  std::string getActivity(uint16_t limit) const {
    (void)limit;   // the mirror emits the whole ring; JSON truncation is not the subject
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    return readBodyUnlocked();                         // OLD: unlocked reader
#else
    if (!_lock()) return "<LOCKUNAVAILABLE>";         // [p.472] honest empty
    std::string s = readBodyUnlocked();
    _unlock();
    return s;
#endif
  }

  std::string getAuditText() const {
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    return _auditBuf;                                  // OLD: unlocked copy
#else
    if (!_lock()) return "";                           // [p.472] honest empty
    std::string s = _auditBuf;
    _unlock();
    return s;
#endif
  }

  uint16_t activityCount() const {
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    return _count;                                     // OLD: unlocked read
#else
    if (!_lock()) return 0;                            // [p.472] conservative
    uint16_t c = _count;
    _unlock();
    return c;
#endif
  }

  // [p.472] LOCK-FREE by design — works when the mutex does not.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

  // MIRROR-ONLY test hook (file header): models catastrophic post-boot
  // corruption nulling the handle. MUST be called single-threaded.
  void dropMutexForTest() { _cell.created = false; }

private:
  // ---- treatment lock helpers (fail-closed; Serial-only fail path —
  // LogService is the log sink, it cannot log into itself) ----
  bool _lock() const {
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // [FW-24] bounded wait: a timeout is the designed drop-on-contention,
    // NOT a lockFailure (the mutex exists — not the p.472 class).
    return tryLockBody(500);
  }
  void _unlock() const { _cell.ref().unlock(); }
  bool tryLockBody(unsigned ms) const {
    return const_cast<MutexCell&>(_cell).ref().try_lock_for(std::chrono::milliseconds(ms));
  }

  void appendBody(const char* msg) {
    Entry& e = _entries[_head];
    e.id = _nextId++;
    snprintf(e.msg, sizeof(e.msg), "%s", msg);
    _head = (_head + 1) % MAX_ENTRIES;
    if (_count < MAX_ENTRIES) _count++;
  }
  void auditBody(const char* line) {
    _auditBuf += line;
    _auditBuf += "\n";
    _auditDirty = true;
  }
  std::string readBodyUnlocked() const {
    std::string out;
    uint16_t start = (_count < MAX_ENTRIES) ? 0 : _head;
    for (uint16_t i = 0; i < _count; i++) {
      uint16_t idx = (start + i) % MAX_ENTRIES;
      char line[64];
      snprintf(line, sizeof(line), "%u:%s;", _entries[idx].id, _entries[idx].msg);
      out += line;
    }
    return out;
  }

  struct Entry { uint32_t id = 0; char msg[48] = {0}; };
  Entry _entries[MAX_ENTRIES] = {};
  uint16_t _head = 0, _count = 0;
  uint32_t _nextId = 1;
  std::string _auditBuf;
  bool _auditDirty = false;

  mutable MutexCell _cell;
  mutable std::atomic<uint32_t> _lockFailures{0};
};

// ============================================================================
// PHASE S MIRROR — TransactionJournal (p.473): 16-slot ring, RAM-after-NVS
// with rollback, terminal acks, decide(). The old shape: _lock() was a
// silent no-op when the handle was null — storeTransaction (networkTask)
// and updateAck (relayTask) interleaved over the RAM mirror and the NVS
// slots exactly as the auditor described.
// ============================================================================
enum class Decision : uint8_t { New = 0, Duplicate = 1, Conflict = 2, Unavailable = 3 };

class TxnJournal {
public:
  static constexpr uint8_t JOURNAL_SIZE = 16;

  bool begin() {
    for (uint8_t i = 0; i < JOURNAL_SIZE; i++) { _ids[i].clear(); _hashes[i].clear(); _acks[i].clear(); _valid[i] = false; }
    _size = 0; _writeIdx = 0;
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    (void)_cell.create();   // OLD shape: no boot guard
    return true;
#else
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;   // [p.473] BOOT REFUSED
    }
    return true;
#endif
  }

  Decision decide(const std::string& id, const std::string& hash, std::string& outPrev) {
#if defined(SVC_NO_LOCK)
    return decideBody(id, hash, outPrev);
#elif defined(LOCK_FAIL_OPEN)
    lockOld();                                          // OLD: no-op when null
    Decision d = decideBody(id, hash, outPrev);
    unlockOld();
    return d;
#else
    if (!_lock()) return Decision::Unavailable;        // [p.473] honest reject
    Decision d = decideBody(id, hash, outPrev);
    _unlock();
    return d;
#endif
  }

  bool storeTransaction(const std::string& id, const std::string& hash, const std::string& ack) {
#if defined(SVC_NO_LOCK)
    return storeBody(id, hash, ack);
#elif defined(LOCK_FAIL_OPEN)
    lockOld();
    bool ok = storeBody(id, hash, ack);
    unlockOld();
    return ok;
#else
    if (!_lock()) return false;                        // [p.473] not durable
    bool ok = storeBody(id, hash, ack);
    _unlock();
    return ok;
#endif
  }

  bool updateAck(const std::string& id, const std::string& hash, const std::string& ack) {
#if defined(SVC_NO_LOCK)
    return updateBody(id, hash, ack);
#elif defined(LOCK_FAIL_OPEN)
    lockOld();
    bool ok = updateBody(id, hash, ack);
    unlockOld();
    return ok;
#else
    if (!_lock()) return false;                        // [p.473] not durable
    bool ok = updateBody(id, hash, ack);
    _unlock();
    return ok;
#endif
  }

  bool isProcessed(const std::string& id) {
#if defined(SVC_NO_LOCK)
    return findSlot(id) >= 0;
#elif defined(LOCK_FAIL_OPEN)
    lockOld();
    bool r = findSlot(id) >= 0;
    unlockOld();
    return r;
#else
    if (!_lock()) return true;   // [p.473] dedup oracle fails toward "processed"
    bool r = findSlot(id) >= 0;
    _unlock();
    return r;
#endif
  }

  std::string getAckJson(const std::string& id) {
#if defined(SVC_NO_LOCK)
    return ackOf(id);
#elif defined(LOCK_FAIL_OPEN)
    lockOld();
    std::string a = ackOf(id);
    unlockOld();
    return a;
#else
    if (!_lock()) return "";     // [p.473] honest unknown
    std::string a = ackOf(id);
    _unlock();
    return a;
#endif
  }

  // [p.473] LOCK-FREE by design.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

  // MIRROR-ONLY test hook (file header).
  void dropMutexForTest() { _cell.created = false; }

  // ---- storm verification helpers (test-side reads; NOT firmware API) ----
  uint8_t size() const { return _size; }
  const std::string& idAt(uint8_t i) const { return _ids[i]; }
  const std::string& ackAt(uint8_t i) const { return _acks[i]; }
  bool validAt(uint8_t i) const { return _valid[i]; }

private:
  bool _lock() {
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      // rate-limited CRIT mirror via LogSpy (different mutex — no cycle)
      return false;
    }
    _cell.ref().lock();   // portMAX_DELAY
    return true;
  }
  void _unlock() { _cell.ref().unlock(); }
  void lockOld() { if (_cell.created) _cell.ref().lock(); }   // OLD p.473 shape
  void unlockOld() { if (_cell.created) _cell.ref().unlock(); }

  int findSlot(const std::string& id) {
    for (uint8_t i = 0; i < _size; i++) {
      if (_valid[i] && _ids[i] == id) return i;
    }
    return -1;
  }
  std::string ackOf(const std::string& id) {
    int i = findSlot(id);
    return i >= 0 ? _acks[i] : std::string();
  }
  Decision decideBody(const std::string& id, const std::string& hash, std::string& outPrev) {
    int i = findSlot(id);
    if (i < 0) return Decision::New;
    if (_hashes[i] == hash) { outPrev = _acks[i]; return Decision::Duplicate; }
    return Decision::Conflict;
  }
  bool storeBody(const std::string& id, const std::string& hash, const std::string& ack) {
    int i = findSlot(id);
    if (i >= 0) return _hashes[i] == hash;   // idempotent; ack NOT rewritten
    return storeNewLocked(id, hash, ack);
  }
  bool updateBody(const std::string& id, const std::string& hash, const std::string& ack) {
    int i = findSlot(id);
    if (i >= 0) {
      if (_hashes[i] != hash) return false;  // identity mismatch — refuse
      std::string prev = _acks[i];
      _acks[i] = ack;
      if (!saveSlotToNvs(i)) { _acks[i] = prev; return false; }   // ROLLBACK
      return true;
    }
    return storeNewLocked(id, hash, ack);    // CREATE-ON-DEMAND
  }
  bool storeNewLocked(const std::string& id, const std::string& hash, const std::string& ack) {
    uint8_t n = _writeIdx;
    std::string pId = _ids[n], pHash = _hashes[n], pAck = _acks[n];
    bool pValid = _valid[n];
    _ids[n] = id; _hashes[n] = hash; _acks[n] = ack; _valid[n] = true;
    if (!saveSlotToNvs(n)) {
      _ids[n] = pId; _hashes[n] = pHash; _acks[n] = pAck; _valid[n] = pValid;
      return false;                          // ROLLBACK — no false durability
    }
    _writeIdx = (n + 1) % JOURNAL_SIZE;
    if (n >= _size) _size = n + 1;
    return true;
  }
  bool saveSlotToNvs(uint8_t idx) {
    // Blob mirror: id|hash|ack packed — the 2-phase flip is not this round's
    // subject (round-6 proved it); one locked putBytes per commit is.
    std::string blob = _ids[idx] + "|" + _hashes[idx] + "|" + _acks[idx];
    Preferences p;
    char key[8];
    snprintf(key, sizeof(key), "t_%u", idx);
    return p.putBytes(key, blob.data(), blob.size()) == blob.size();
  }

  std::string _ids[JOURNAL_SIZE];
  std::string _hashes[JOURNAL_SIZE];
  std::string _acks[JOURNAL_SIZE];
  bool _valid[JOURNAL_SIZE] = {false};
  uint8_t _size = 0, _writeIdx = 0;

  mutable MutexCell _cell;
  std::atomic<uint32_t> _lockFailures{0};
};

} // namespace Services

// ============================================================================
// PHASE T MIRROR — BatteryCommManager (p.474): generation-tagged BmsData
// written by a bmsTask tick-writer, read by energy/web consumers. The old
// shapes: getData() copied only when the handle existed (no retry, no
// accounting); crossCheckShunt() read _data.current BEFORE the mutex check
// and NEVER acquired it — the reader synchronization gap.
// ============================================================================
namespace Comm {

struct BmsSnap {
  float current = NAN;
  float voltage = NAN;
  uint32_t lastUpdateMs = 0;
  uint32_t gen = 0;   // mirror-only coherence tag (see tickWrite)
  bool isFresh(uint32_t nowMs, uint32_t windowMs) const {
    return (lastUpdateMs != 0) && (nowMs - lastUpdateMs) < windowMs;
  }
  void reset() { current = NAN; voltage = NAN; lastUpdateMs = 0; gen = 0; }
};

class BmsMgr {
public:
  bool begin() {
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    (void)_cell.create();   // OLD shape: subsystem runs on regardless
    _state = State::Locked;
    return true;
#else
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;   // [p.474] BOOT REFUSED
    }
    _state = State::Locked;
    return true;
#endif
  }

  // bmsTask tick() mirror: replaces _data with a generation-tagged snapshot.
  // MIRROR-ONLY: the inter-field sleep widens the torn window so a NO_LOCK
  // getData() copy can observe mixed generations deterministically (the
  // property under test — coherent snapshots — is unchanged).
  void tickWrite(uint32_t gen) {
#if defined(SVC_NO_LOCK)
    writeBody(gen);
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape: `if (!_mutex) return; if (take != ok) return; ...`
    if (!_cell.created) return;
    if (!_cell.ref().try_lock_for(std::chrono::milliseconds(100))) return;
    writeBody(gen);
    _cell.ref().unlock();
#else
    if (!_lock(100)) return;   // [p.474] skip this cycle when unavailable
    writeBody(gen);
    _unlock();
#endif
  }

  BmsSnap getData() const {
#if defined(SVC_NO_LOCK)
    return _data;                                     // OLD-OLD: unlocked copy
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape: `if (_mutex && take==ok) { copy; give; } return copy;`
    BmsSnap copy;   // default: NAN fields, lastUpdateMs=0
    if (_cell.created && _cell.ref().try_lock_for(std::chrono::milliseconds(50))) {
      copy = _data;
      _cell.ref().unlock();
    }
    return copy;
#else
    BmsSnap copy;
    copy.reset();
    if (_lock(50)) { copy = _data; _unlock(); }       // [p.474] fail-closed snapshot
    return copy;
#endif
  }

  float crossCheckShunt(float shuntCurrentA) {
#if defined(SVC_NO_LOCK)
    return crossBody(shuntCurrentA);                  // OLD-OLD: unlocked
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape (the p.474 ordering bug): read _data.current BEFORE the
    // mutex check — and never acquire it at all.
    float bmsI = _data.current;
    if (!_cell.created) return NAN;
    return crossFrom(bmsI, shuntCurrentA);
#else
    // [p.474] THE MUTEX IS TAKEN BEFORE _data IS READ. Lock unavailable →
    // NAN with the streak state untouched (no arbitration on unreadable
    // state) + counted.
    if (!_lock(50)) return NAN;
    float bmsI = _data.current;
    float d = crossFrom(bmsI, shuntCurrentA);
    _unlock();
    return d;
#endif
  }

  bool socAuthoritative() const {
#if defined(SVC_NO_LOCK)
    return authorityBody();                           // OLD-OLD: unlocked read
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape: documented-benign unlocked multi-field read (the round-10
    // snapshot contract closes this too).
    return authorityBody();
#else
    if (!_lock(50)) return false;   // [p.474] conservative: BMS loses authority
    bool a = authorityBody();
    _unlock();
    return a;
#endif
  }

  bool isMismatchActive() const {
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    return _mismatchActive;                           // OLD: unlocked inline read
#else
    if (!_lock(50)) return false;
    bool a = _mismatchActive;
    _unlock();
    return a;
#endif
  }

  // [p.474] LOCK-FREE by design.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }
  uint32_t mismatchActivations() const { return _mismatchActivations; }

  // MIRROR-ONLY test hook (file header).
  void dropMutexForTest() const { _cell.created = false; }

private:
  enum class State : uint8_t { Disabled, Locked, Lost };

  bool _lock(uint32_t timeoutMs) const {
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Timeout → false WITHOUT counting (bounded-wait skip-cycle semantics
    // are pre-existing design, not the unavailable-mutex class).
    return const_cast<MutexCell&>(_cell).ref().try_lock_for(
        std::chrono::milliseconds(timeoutMs));
  }
  void _unlock() const { _cell.ref().unlock(); }

  void writeBody(uint32_t gen) {
    _data.current = (float)gen;
    // mirror-only interleaving widener (file header)
    std::this_thread::sleep_for(std::chrono::microseconds(40));
    _data.voltage = (float)(gen * 10);
    _data.gen = gen;
    _data.lastUpdateMs = millis();
  }
  float crossFrom(float bmsI, float shuntCurrentA) {
    if (!_data.isFresh(millis(), 60000)) {
      _mismatchStreak = 0; _lastMismatchA = NAN; _mismatchActive = false;
      return NAN;
    }
    float delta = fabsf(bmsI - shuntCurrentA);
    if (delta > 10.0f) {
      if (_mismatchStreak < 3) _mismatchStreak++;
      if (_mismatchStreak >= 3 && !_mismatchActive) {
        _mismatchActive = true;
        _mismatchActivations++;
      }
    } else {
      _mismatchStreak = 0; _mismatchActive = false;
    }
    _lastMismatchA = delta;
    return delta;
  }
  float crossBody(float shuntCurrentA) {
    return crossFrom(_data.current, shuntCurrentA);
  }
  bool authorityBody() const {
    return _state == State::Locked && _data.isFresh(millis(), 60000) &&
           _data.gen != 0 && !_mismatchActive;
  }

  mutable MutexCell _cell;
  State _state = State::Disabled;
  BmsSnap _data;
  uint32_t _mismatchStreak = 0;
  bool _mismatchActive = false;
  float _lastMismatchA = NAN;
  uint32_t _mismatchActivations = 0;
  mutable std::atomic<uint32_t> _lockFailures{0};
};

} // namespace Comm

// ============================================================================
// PHASE U MIRROR — AuthManager (p.475): 4-slot refresh-token LRU with the
// ONE-ATOMIC consume (find→validate→mark-used→rotate→persist) and the now-
// locked issue path. The old shape: _lockAuth() only took an EXISTING handle
// — creation failure let the whole rotation critical section run
// unsynchronized (the A→B AND A→C replay race).
// ============================================================================
namespace Services {

class AuthMgr {
public:
  static constexpr int MAX_REFRESH_TOKENS = 4;

  bool begin() {
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) _slots[i] = Slot{};
#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
    (void)_cell.create();   // OLD shape: no boot guard
    return true;
#else
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;   // [p.475] BOOT REFUSED
    }
    return true;
#endif
  }

  std::string issueRefreshToken(const char* ip) {
#if defined(SVC_NO_LOCK)
    return issueBody(ip);
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape: issueRefreshToken took NO lock at all (round-10 closes this
    // gap too — see AuthManager.h).
    return issueBody(ip);
#else
    if (!_lockAuth()) return "";   // [p.475] no token without serialization
    std::string t = issueBody(ip);
    // [p.475 — harness-caught] Persist INSIDE the lock (TSAN flagged the
    // after-unlock persist as a race on the slot array in the first draft
    // of this remediation — the firmware was fixed the same way).
    persist();
    _unlockAuth();
    return t;
#endif
  }

  bool consumeRefreshToken(const std::string& oldToken, std::string& outNew) {
#if defined(SVC_NO_LOCK)
    return consumeBody(oldToken, outNew);
#elif defined(LOCK_FAIL_OPEN)
    // OLD shape (p.475): `if (null) create; if (handle) take;` — creation
    // failure → the WHOLE critical section runs unsynchronized.
    if (!_cell.created) (void)_cell.create();
    if (_cell.created) _cell.ref().lock();
    bool ok = consumeBody(oldToken, outNew);
    if (_cell.created) _cell.ref().unlock();
    return ok;
#else
    if (!_lockAuth()) return false;   // [p.475] no rotation without serialization
    bool ok = consumeBody(oldToken, outNew);
    _unlockAuth();
    return ok;
#endif
  }

  void revokeAllRefreshTokens() {
#if defined(SVC_NO_LOCK)
    revokeBody();
#elif defined(LOCK_FAIL_OPEN)
    if (!_cell.created) (void)_cell.create();
    if (_cell.created) _cell.ref().lock();
    revokeBody();
    if (_cell.created) _cell.ref().unlock();
#else
    if (!_lockAuth()) return;   // [p.475] refused — never a false "revoked"
    revokeBody();
    _unlockAuth();
#endif
  }

  // [p.475] LOCK-FREE by design.
  uint32_t lockFailures() const {
    return _lockFailures.load(std::memory_order_relaxed);
  }

  // MIRROR-ONLY test hook (file header).
  void dropMutexForTest() const { _cell.created = false; }

  // ---- storm verification helpers (test-side reads) ----
  int liveTokenCount() const {
    int n = 0;
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) {
      if (_slots[i].token[0] != '\0' && !_slots[i].used) n++;
    }
    return n;
  }
  bool slotOccupied(int i) const { return _slots[i].token[0] != '\0'; }
  bool slotTokenWellFormed(int i) const {
    // Token format: 'T' + 12 digits = 13 chars + NUL (Slot.token[16]).
    const char* t = _slots[i].token;
    if (t[0] != 'T') return false;
    for (int k = 1; k <= 12; k++) {
      if (t[k] < '0' || t[k] > '9') return false;
    }
    return t[13] == '\0';
  }

private:
  struct Slot {
    char token[16] = {0};
    uint32_t issuedAt = 0, expiresAt = 0;
    bool used = false;
  };

  bool _lockAuth() {
    if (!_cell.create()) {
      _lockFailures.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    _cell.ref().lock();   // portMAX_DELAY
    return true;
  }
  void _unlockAuth() { _cell.ref().unlock(); }

  std::string nextToken() {
    uint32_t n = _tokenCounter.fetch_add(1, std::memory_order_relaxed);
    char buf[16];
    snprintf(buf, sizeof(buf), "T%012u", n);
    return std::string(buf);
  }
  int findRefreshSlot() {
    int oldest = -1;
    uint32_t oldestTs = 0xFFFFFFFF;
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) {
      if (_slots[i].token[0] == '\0') return i;
      if (_slots[i].issuedAt < oldestTs) { oldestTs = _slots[i].issuedAt; oldest = i; }
    }
    return (oldest >= 0) ? oldest : 0;
  }
  std::string issueBody(const char*) {
    int slot = findRefreshSlot();
    std::string t = nextToken();
    snprintf(_slots[slot].token, sizeof(_slots[slot].token), "%s", t.c_str());
    _slots[slot].issuedAt = rtcNow();
    _slots[slot].expiresAt = _slots[slot].issuedAt + 604800;
    _slots[slot].used = false;
    return t;
  }
  bool consumeBody(const std::string& oldToken, std::string& outNew) {
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) {
      if (_slots[i].token[0] == '\0') continue;
      if (oldToken != _slots[i].token) continue;
      if (_slots[i].used) return false;             // replay — reject
      if (rtcNow() > _slots[i].expiresAt) return false;
      _slots[i].used = true;                        // mark old used
      std::string t = issueBody("consume");         // successor in fresh slot
      outNew = t;
      persist();
      return true;
    }
    return false;
  }
  void revokeBody() {
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) {
      memset(_slots[i].token, 0, sizeof(_slots[i].token));
      _slots[i].used = true;
      _slots[i].issuedAt = 0;
      _slots[i].expiresAt = 0;
    }
    persist();
  }
  void persist() {
    // Blob mirror: one locked putBytes per persist (the CRC layout is
    // round-6/8 territory; the lock discipline is this round's subject).
    std::string blob;
    for (int i = 0; i < MAX_REFRESH_TOKENS; i++) {
      blob += _slots[i].token;
      blob += '|';
      blob += (_slots[i].used ? '1' : '0');
      blob += ';';
    }
    Preferences p;
    p.putBytes("rtokens", blob.data(), blob.size());
  }

  Slot _slots[MAX_REFRESH_TOKENS];
  std::atomic<uint32_t> _tokenCounter{1};
  mutable MutexCell _cell;
  std::atomic<uint32_t> _lockFailures{0};
};

} // namespace Services

// ============================================================================
// PHASES. REQUIRE runs in every build (invariant check). SENTINEL_TRIP runs
// ONLY in the negative-control builds — it proves the harness DETECTS the
// old shape (a clean negative build would mean a blind harness).
// ============================================================================
static int g_fails = 0;
static int g_sentinels = 0;
static const char* g_phase = "?";

#define REQUIRE(cond, msg)                                                   \
  do {                                                                       \
    if (!(cond)) {                                                           \
      printf("FAIL [%s] %s:%d: %s\n", g_phase, __FILE__, __LINE__, msg);     \
      g_fails++;                                                             \
    }                                                                        \
  } while (0)

#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
#define SENTINEL_TRIP(which, msg)                                            \
  do {                                                                       \
    printf("P%s NEGATIVE CONTROL TRIPPED: %s\n", which, msg);                \
    g_sentinels++;                                                           \
    return false;                                                            \
  } while (0)
#else
#define SENTINEL_TRIP(which, msg) (void)0
#endif

static void microSleep(std::mt19937& rng, unsigned maxUs) {
  std::this_thread::sleep_for(
      std::chrono::microseconds(1 + (rng() % maxUs)));
}

// ---------------------------------------------------------------------------
// Phase V — BOOT GUARD (p.472-p.475): mutex-creation failure at begin() is
// FATAL in the firmware (mirrored as refusal). The OLD shapes had no guard.
// ---------------------------------------------------------------------------
static bool phaseV() {
  g_phase = "V-boot-guard";
  g_injectMutexCreateFail.store(true);

  Services::LogSvc logA;
  Services::TxnJournal journalA;
  Comm::BmsMgr bmsA;
  Services::AuthMgr authA;
  // Collect ALL four verdicts BEFORE any sentinel return — the injection
  // flag MUST be reset even on the early-out path, or every later phase
  // inherits "creation impossible" and the old-shape mirrors never even
  // create their handles (which silently kills the Phase T race window).
  const bool bL = logA.begin(), bJ = journalA.begin(),
             bB = bmsA.begin(), bA = authA.begin();
  g_injectMutexCreateFail.store(false);

#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
  if (bL) SENTINEL_TRIP("472", "LogService begin() proceeded without its mutex (old shape)");
  if (bJ) SENTINEL_TRIP("473", "TransactionJournal begin() proceeded without its mutex (old shape)");
  if (bB) SENTINEL_TRIP("474", "BatteryCommManager begin() proceeded without its mutex (old shape)");
  if (bA) SENTINEL_TRIP("475", "AuthManager begin() proceeded without its mutex (old shape)");
#else
  REQUIRE(!bL, "p.472 LogService boot guard must refuse on creation failure");
  REQUIRE(!bJ, "p.473 TransactionJournal boot guard must refuse");
  REQUIRE(!bB, "p.474 BatteryCommManager boot guard must refuse");
  REQUIRE(!bA, "p.475 AuthManager boot guard must refuse");
  REQUIRE(logA.lockFailures() > 0, "p.472 boot refusal must be counted");
  REQUIRE(journalA.lockFailures() > 0, "p.473 boot refusal must be counted");
  REQUIRE(bmsA.lockFailures() > 0, "p.474 boot refusal must be counted");
  REQUIRE(authA.lockFailures() > 0, "p.475 boot refusal must be counted");
#endif

  printf("Phase V OK — boot guards %s\n",
         (g_sentinels || g_fails) ? "TRIPPED" : "fail-closed for all four services");
  return true;
}

// ---------------------------------------------------------------------------
// Phase R — LogService storm (p.472): 4 writer tasks (append + audit) vs
// 4 reader tasks (getActivity / activityCount / getAuditText). Invariants:
// complete (never torn) messages, strictly increasing ids in ring order,
// bounded count, well-formed audit lines.
// ---------------------------------------------------------------------------
static bool wellFormedEntry(const std::string& s) {
  // ^[1-9][0-9]*:W[0-9]{2}-[0-9]{6};$
  size_t colon = s.find(':');
  if (colon == std::string::npos) return false;
  if (colon == 0 || colon > 10) return false;
  for (size_t i = 0; i < colon; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  const std::string msg = s.substr(colon + 1);
  if (msg.size() != 10) return false;
  if (msg[0] != 'W') return false;
  if (msg[1] < '0' || msg[1] > '9' || msg[2] < '0' || msg[2] > '9') return false;
  if (msg[3] != '-') return false;
  for (size_t i = 4; i < 10; i++) {
    if (msg[i] < '0' || msg[i] > '9') return false;
  }
  return true;
}

static bool phaseR() {
  g_phase = "R-logsvc-storm";
  Services::LogSvc svc;
  REQUIRE(svc.begin(), "p.472 LogService begin must succeed (no injection)");

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  constexpr int WRITERS = 4, READERS = 4, PER_WRITER = 400;

  for (int w = 0; w < WRITERS; w++) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(0xA10 + w);
      for (int n = 0; n < PER_WRITER; n++) {
        char msg[32], line[32];
        snprintf(msg, sizeof(msg), "W%02d-%06d", w, n);
        snprintf(line, sizeof(line), "A%02d-%06d", w, n);
        svc.append(msg);
        svc.audit(line);
        microSleep(rng, 60);
      }
    });
  }
  for (int r = 0; r < READERS; r++) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(0xB20 + r);
      (void)r;
      while (!stop.load()) {
        std::string snap = svc.getActivity(64);
        if (snap == "<LOCKUNAVAILABLE>") {
          REQUIRE(false, "p.472 reader must not see LOCKUNAVAILABLE with a healthy mutex");
          continue;
        }
        // Parse "id:msg;id:msg;..." — every entry complete + ids increasing.
        uint32_t lastId = 0;
        size_t pos = 0;
        while (pos < snap.size()) {
          size_t semi = snap.find(';', pos);
          if (semi == std::string::npos) {
            REQUIRE(false, "p.472 getActivity snapshot is not a complete entry list (torn tail)");
            break;
          }
          std::string entry = snap.substr(pos, semi - pos);
          REQUIRE(wellFormedEntry(entry), "p.472 torn/partial entry in reader snapshot (torn write or unlocked reader)");
          uint32_t id = (uint32_t)strtoul(entry.c_str(), nullptr, 10);
          REQUIRE(id > lastId, "p.472 ring ids must be strictly increasing in one snapshot (incoherent read)");
          lastId = id;
          pos = semi + 1;
        }
        uint16_t c = svc.activityCount();
        REQUIRE(c <= Services::LogSvc::MAX_ENTRIES, "p.472 count bounded by ring size");
        (void)svc.getAuditText();
        microSleep(rng, 80);
      }
    });
  }

  // Writers finish; readers stop after writers join.
  for (int i = 0; i < WRITERS; i++) threads[i].join();
  stop.store(true);
  for (int i = WRITERS; i < WRITERS + READERS; i++) threads[i].join();

  // Post-storm invariants (single-threaded).
  uint16_t finalCount = svc.activityCount();
  REQUIRE(finalCount == Services::LogSvc::MAX_ENTRIES,
          "p.472 ring must be exactly full after 1600 appends into 64 slots");
  REQUIRE(svc.lockFailures() == 0, "p.472 no lock failures with a healthy mutex");
  std::string audit = svc.getAuditText();
  size_t lines = 0;
  for (char c : audit) if (c == '\n') lines++;
  REQUIRE(lines == (size_t)(WRITERS * PER_WRITER),
          "p.472 every audit line must survive the storm");
  printf("Phase R OK — %u entries in ring, %zu audit lines, lockFailures=%u\n",
         finalCount, lines, svc.lockFailures());
  return true;
}

// ---------------------------------------------------------------------------
// Phase S — TransactionJournal storm (p.473): networkTask storeTransaction
// vs relayTask updateAck over a shared id space (24 ids over a 16-slot ring
// → one eviction wrap). Invariants: no duplicate ids in RAM, terminal acks
// never regress to QUEUED, decide() consistent, RAM == NVS.
// ---------------------------------------------------------------------------
static bool phaseS() {
  g_phase = "S-journal-storm";
  Services::TxnJournal j;
  REQUIRE(j.begin(), "p.473 journal begin must succeed");

  constexpr int IDS = 24;   // > JOURNAL_SIZE → one eviction wrap
  std::atomic<int> storedOk{0}, ackOk{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(0xC30 + t);
      for (int i = t; i < IDS; i += 4) {
        char id[16], hash[16], ack[24];
        snprintf(id, sizeof(id), "tx-%03d", i);
        snprintf(hash, sizeof(hash), "h-%03d", i);
        snprintf(ack, sizeof(ack), "QUEUED:%03d", i);
        if (j.storeTransaction(id, hash, ack)) storedOk.fetch_add(1);
        microSleep(rng, 120);
      }
    });
  }
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(0xD40 + t);
      for (int round = 0; round < 3; round++) {
        for (int i = t; i < IDS; i += 4) {
          char id[16], hash[16], ack[24];
          snprintf(id, sizeof(id), "tx-%03d", i);
          snprintf(hash, sizeof(hash), "h-%03d", i);
          snprintf(ack, sizeof(ack), "TERM:%03d", i);
          if (j.updateAck(id, hash, ack)) ackOk.fetch_add(1);
          microSleep(rng, 150);
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  // [CI lesson] DETERMINISTIC TERMINAL PASS. The storm races store vs
  // updateAck freely (that is the p.473 subject), but "final ack == TERM"
  // is only guaranteed per-id when the LAST writer is updateAck — a store
  // delayed past the relay's final round would legitimately leave a QUEUED
  // ack as the outcome. One sequential updateAck pass AFTER all stores
  // have landed pins the invariant without weakening the storm above.
  for (int i = 0; i < IDS; i++) {
    char id[16], hash[16], ack[24];
    snprintf(id, sizeof(id), "tx-%03d", i);
    snprintf(hash, sizeof(hash), "h-%03d", i);
    snprintf(ack, sizeof(ack), "TERM:%03d", i);
    (void)j.updateAck(id, hash, ack);
  }

  // Post-storm invariants (single-threaded).
  REQUIRE(storedOk.load() > 0, "p.473 some storeTransaction must succeed");
  REQUIRE(ackOk.load() > 0, "p.473 some updateAck must succeed");
  REQUIRE(j.lockFailures() == 0, "p.473 no lock failures with a healthy mutex");

  // No duplicate ids in the RAM mirror.
  for (uint8_t a = 0; a < j.JOURNAL_SIZE; a++) {
    if (!j.validAt(a)) continue;
    for (uint8_t b = a + 1; b < j.JOURNAL_SIZE; b++) {
      if (!j.validAt(b)) continue;
      REQUIRE(j.idAt(a) != j.idAt(b), "p.473 an id may occupy only ONE slot (eviction/terminal interleave)");
    }
  }
  // Terminal acks never regress to QUEUED + RAM == NVS.
  for (uint8_t a = 0; a < j.JOURNAL_SIZE; a++) {
    if (!j.validAt(a)) continue;
    const std::string& id = j.idAt(a);
    REQUIRE(j.ackAt(a).compare(0, 5, "TERM:") == 0,
            "p.473 final ack of a journaled transaction must be its TERMINAL outcome");
    int i = atoi(id.c_str() + 3);
    char want[24];
    snprintf(want, sizeof(want), "TERM:%03d", i);
    REQUIRE(j.ackAt(a) == want, "p.473 terminal ack must belong to the slot's own transaction");
    char hash[16];
    snprintf(hash, sizeof(hash), "h-%03d", i);
    std::string prev;
    REQUIRE(j.decide(id, hash, prev) == Services::Decision::Duplicate,
            "p.473 decide() must answer Duplicate for a stored id+hash");
    REQUIRE(j.decide(id, "WRONG", prev) == Services::Decision::Conflict,
            "p.473 decide() must answer Conflict for a stored id with a different hash");
    // RAM == NVS (the p.473 failure mode: the blob and the mirror disagreeing
    // about which transaction owns the slot).
    char key[8];
    snprintf(key, sizeof(key), "t_%u", a);
    std::lock_guard<std::mutex> g(nvs.m);
    auto it = nvs.kv.find(key);
    REQUIRE(it != nvs.kv.end(), "p.473 every valid RAM slot must have an NVS blob");
    if (it != nvs.kv.end()) {
      std::string blob((const char*)it->second.data(), it->second.size());
      REQUIRE(blob == id + "|" + hash + "|" + j.ackAt(a),
              "p.473 NVS blob must agree with the RAM mirror (owner + outcome)");
    }
  }
  std::string prev;
  REQUIRE(j.decide("tx-unknown", "h-x", prev) == Services::Decision::New,
          "p.473 decide() must answer New for an unknown id");
  printf("Phase S OK — stored=%d terminalAcks=%d, slots=%u, lockFailures=%u\n",
         storedOk.load(), ackOk.load(), j.size(), j.lockFailures());
  return true;
}

// ---------------------------------------------------------------------------
// Phase T — BatteryCommManager storm (p.474): one bmsTask tick-writer vs
// energy/web readers. Invariants: getData() snapshots are generation-coherent
// (current == gen, voltage == 10*gen), the mismatch interlock still works
// end-to-end (activate on disagreement, clear on invalid shunt), authority
// gate tracks the interlock.
// ---------------------------------------------------------------------------
static bool phaseT() {
  g_phase = "T-bms-storm";
  Comm::BmsMgr bms;
  REQUIRE(bms.begin(), "p.474 BMS begin must succeed");

  std::atomic<bool> stop{false};
  std::atomic<int> tornReads{0};
  std::vector<std::thread> threads;

  // bmsTask — the single writer (gen 1..3000).
  threads.emplace_back([&] {
    for (uint32_t gen = 1; gen <= 3000; gen++) bms.tickWrite(gen);
  });
  // 2 web/energy readers — coherent snapshot copies.
  for (int r = 0; r < 2; r++) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(0xE50 + r);
      while (!stop.load()) {
        Comm::BmsSnap s = bms.getData();
        if (s.gen != 0) {
          bool coherent = (s.current == (float)s.gen) && (s.voltage == (float)(s.gen * 10));
          if (!coherent) tornReads.fetch_add(1);
          REQUIRE(coherent, "p.474 getData() snapshot must be generation-coherent (torn struct read)");
        }
        (void)bms.socAuthoritative();
        microSleep(rng, 50);
      }
    });
  }
  // energyTask — crossCheckShunt, EVENT-DRIVEN (CI lesson: an iteration
  // BUDGET is scheduling-sensitive — on an oversubscribed 2-core runner the
  // budget can run out before the tick writer reaches gen>10, so the
  // interlock never activates and the assertion fails spuriously): phase 1
  // disagrees until the interlock has REALLY activated. Phase 2 feeds a
  // FIXED block of NAN — every successful call CLEARS the streak/interlock
  // under the lock, so a block of attempts overwhelms any bounded-wait
  // timeout streak. isMismatchActive() must NOT gate this loop: its
  // conservative timeout→false answer would lie. Caps guard a broken build.
  threads.emplace_back([&] {
    std::mt19937 rng(0x1F00);
    for (int i = 0; i < 200000 && bms.mismatchActivations() < 1; i++) {
      (void)bms.crossCheckShunt(0.0f);   // disagree with the BMS
      microSleep(rng, 40);
    }
    for (int i = 0; i < 3000; i++) {
      (void)bms.crossCheckShunt(NAN);    // invalid shunt → arbitration resets
      microSleep(rng, 40);
    }
  });

  // threads[] = [0] tick-writer, [1..2] readers, [3] energy. The tick
  // writer's 3000 writes finish on their own; readers + energy spin on
  // `stop`, so it must be set BETWEEN the joins (joining a stop-spinning
  // thread before stop=true would hang forever).
  threads[0].join();
  stop.store(true);
  threads[1].join();
  threads[2].join();
  threads[3].join();

  REQUIRE(bms.mismatchActivations() >= 1,
          "p.474 the mismatch interlock must have activated on sustained disagreement");
  // Settled-state retry: a single isMismatchActive()/socAuthoritative()
  // call can time out its 50 ms bounded wait on a saturated runner and
  // answer the conservative false — retry until both agree (or 1 s, which
  // after 3000 clearing NAN calls can only mean a real defect).
  bool cleared = false, authoritative = false;
  for (int r = 0; r < 100 && !(cleared && authoritative); r++) {
    cleared = !bms.isMismatchActive();
    authoritative = bms.socAuthoritative();
    if (!(cleared && authoritative)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  REQUIRE(cleared,
          "p.474 invalid-shunt half must have cleared the interlock");
  REQUIRE(authoritative,
          "p.474 authority must return once the interlock clears and data is fresh");
  REQUIRE(bms.lockFailures() == 0, "p.474 no lock failures with a healthy mutex");
  REQUIRE(tornReads.load() == 0, "p.474 zero torn snapshots across the storm");
  printf("Phase T OK — interlock activations=%u, torn=%d, lockFailures=%u\n",
         bms.mismatchActivations(), tornReads.load(), bms.lockFailures());
  return true;
}

// ---------------------------------------------------------------------------
// Phase U — AuthManager (p.475): the A→B rotation may succeed EXACTLY ONCE
// no matter how many concurrent consumers present the same token; the issue
// path must not corrupt slots under a concurrent consume storm; revoke
// leaves zero live sessions.
// ---------------------------------------------------------------------------
static bool phaseU() {
  g_phase = "U-auth-storm";
  Services::AuthMgr auth;
  REQUIRE(auth.begin(), "p.475 auth begin must succeed");

  std::string token = auth.issueRefreshToken("login");
  REQUIRE(!token.empty(), "p.475 seed token must issue");

  std::string retiredToken;   // the token CONSUMED by the final rotation round
  for (int round = 0; round < 3; round++) {
    std::mutex resultMx;
    int successes = 0;
    std::string winner;
    // START BARRIER: all 8 racers hit consumeRefreshToken() as
    // SIMULTANEOUSLY as the scheduler allows — a staggered start would
    // let the first racer mark the token used before the others arrive,
    // hiding the A->B AND A->C race in the negative controls.
    std::atomic<bool> go{false};
    std::vector<std::thread> racers;
    for (int t = 0; t < 8; t++) {
      racers.emplace_back([&, t] {
        (void)t;
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        std::string out;
        if (auth.consumeRefreshToken(token, out)) {
          std::lock_guard<std::mutex> g(resultMx);
          successes++;
          if (winner.empty()) winner = out;
        }
      });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));   // let all 8 park on the barrier
    go.store(true, std::memory_order_release);
    for (auto& th : racers) th.join();
    REQUIRE(successes == 1,
            "p.475 exactly ONE concurrent consumer may rotate a refresh token (A->B and A->C is the replay race)");
    REQUIRE(!winner.empty(), "p.475 the winner must receive the successor token");
    retiredToken = token;    // this round's input is now USED
    token = winner;
  }

  // issue-vs-consume storm: logins allocate slots while a replay storm
  // pounds the RETIRED (already-used) token — every attempt must be rejected.
  std::atomic<bool> stop{false};
  std::atomic<int> replaySuccesses{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(0x070 + t);
      while (!stop.load()) {
        std::string out;
        if (auth.consumeRefreshToken(retiredToken, out)) replaySuccesses.fetch_add(1);
        microSleep(rng, 40);
      }
    });
  }
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(0x080 + t);
      for (int i = 0; i < 60; i++) {
        (void)auth.issueRefreshToken("login-storm");
        microSleep(rng, 60);
      }
    });
  }
  for (int i = 4; i < 8; i++) threads[i].join();
  stop.store(true);
  for (int i = 0; i < 4; i++) threads[i].join();

  REQUIRE(replaySuccesses.load() == 0, "p.475 a retired token must never re-rotate (replay)");
  REQUIRE(auth.liveTokenCount() >= 1 && auth.liveTokenCount() <= Services::AuthMgr::MAX_REFRESH_TOKENS,
          "p.475 live tokens bounded by the slot count");
  for (int i = 0; i < Services::AuthMgr::MAX_REFRESH_TOKENS; i++) {
    if (auth.slotOccupied(i)) {
      REQUIRE(auth.slotTokenWellFormed(i), "p.475 every occupied slot must hold a well-formed token (no torn writes)");
    }
  }
  // A freshly issued token must rotate cleanly after the storm (the storm's
  // 240 logins legitimately LRU-evicted older slots — that is the documented
  // eviction contract, not corruption; a torn slot would fail the well-formed
  // check above or this rotation).
  std::string fresh = auth.issueRefreshToken("post-storm");
  REQUIRE(!fresh.empty(), "p.475 issue must succeed after the storm");
  std::string next;
  REQUIRE(auth.consumeRefreshToken(fresh, next), "p.475 a fresh token must rotate after the storm (no slot corruption)");
  token = next;

  auth.revokeAllRefreshTokens();
  REQUIRE(auth.liveTokenCount() == 0, "p.475 revokeAll must leave zero live sessions");
  REQUIRE(auth.lockFailures() == 0, "p.475 no lock failures with a healthy mutex");
  printf("Phase U OK — 3 single-winner rotations, replay rejections clean, revoke clean\n");
  return true;
}

// ---------------------------------------------------------------------------
// Phase X — LOCK-UNAVAILABLE STORM (the p.472-p.475 core): post-boot handle
// loss + creation failure → EVERY operation must be refused fail-closed with
// ZERO state mutation and (journal) ZERO NVS writes. The OLD shapes proceed
// unsynchronized — the sentinels must trip there.
// ---------------------------------------------------------------------------
static bool phaseX() {
  g_phase = "X-lock-unavailable";
  // Set B: healthy boot, THEN the handle is lost (mirror of catastrophic
  // post-boot corruption — unreachable after the boot guard; the runtime
  // retry + reject is defense-in-depth, exactly like round-9 p.471).
  Services::LogSvc logB;
  Services::TxnJournal journalB;
  Comm::BmsMgr bmsB;
  Services::AuthMgr authB;
  REQUIRE(logB.begin() && journalB.begin() && bmsB.begin() && authB.begin(),
          "set-B services must boot healthy before the handle loss");

  std::string liveTok = authB.issueRefreshToken("seed");
  REQUIRE(!liveTok.empty(), "seed token must exist before the outage");
  logB.append("W00-000001");
  uint64_t nvsBefore = nvs.ops.load(std::memory_order_relaxed);
  uint16_t countBefore = logB.activityCount();

  // Lose the handles + make creation impossible.
  g_injectMutexCreateFail.store(true);
  logB.dropMutexForTest();
  journalB.dropMutexForTest();
  bmsB.dropMutexForTest();
  authB.dropMutexForTest();

  // Storm the unavailable services from 8 threads.
  std::atomic<int> journalNewDecisions{0}, authRotations{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(0x190 + t);
      for (int i = 0; i < 150; i++) {
        logB.append("W99-999999");
        std::string prev;
        if (journalB.decide("tx-x", "h-x", prev) == Services::Decision::New) journalNewDecisions.fetch_add(1);
        (void)journalB.storeTransaction("tx-x", "h-x", "QUEUED:x");
        (void)journalB.updateAck("tx-x", "h-x", "TERM:x");
        (void)bmsB.crossCheckShunt(0.0f);
        (void)bmsB.getData();
        (void)bmsB.tickWrite(99);
        std::string out;
        if (authB.consumeRefreshToken(liveTok, out)) authRotations.fetch_add(1);
        (void)authB.issueRefreshToken("outage");
        microSleep(rng, 30);
      }
    });
  }
  for (auto& th : threads) th.join();
  uint64_t nvsAfter = nvs.ops.load(std::memory_order_relaxed);
  uint16_t countAfter = logB.activityCount();

#if defined(SVC_NO_LOCK) || defined(LOCK_FAIL_OPEN)
  if (countAfter != countBefore)
    SENTINEL_TRIP("472", "append() proceeded WITHOUT the mutex during the outage (fail-open)");
  if (journalNewDecisions.load() > 0 || nvsAfter != nvsBefore)
    SENTINEL_TRIP("473", "journal decided/executed or wrote NVS WITHOUT the mutex during the outage (fail-open)");
  if (authRotations.load() > 0)
    SENTINEL_TRIP("475", "refresh rotation proceeded WITHOUT the mutex during the outage (fail-open)");
  // Old crossCheckShunt with a null handle returns NAN by accident — the
  // BMS old-shape detection is the Phase V boot-guard sentinel + the Phase T
  // TSAN races (unlocked crossCheckShunt vs locked tick writer).
#else
  // [p.472] During the outage the counter answers the CONSERVATIVE ZERO
  // (documented contract) — the ring itself is unreadable (that is the
  // point). The proof that the 1200 refused appends left NO residue is in
  // Phase W: after recovery the count is EXACTLY countBefore + 1.
  REQUIRE(countAfter == 0, "p.472 activityCount must answer the conservative 0 during the outage");
  REQUIRE(logB.getActivity(64) == "<LOCKUNAVAILABLE>", "p.472 reader must answer the honest empty marker");
  REQUIRE(logB.getAuditText().empty(), "p.472 audit reader must answer empty");
  REQUIRE(logB.lockFailures() > 0, "p.472 refusals must be counted");
  REQUIRE(journalNewDecisions.load() == 0, "p.473 decide must never answer New during the outage");
  std::string prev;
  REQUIRE(journalB.decide("tx-x", "h-x", prev) == Services::Decision::Unavailable,
          "p.473 decide must answer Unavailable during the outage");
  REQUIRE(nvsAfter == nvsBefore, "p.473 ZERO NVS writes during the outage");
  REQUIRE(journalB.lockFailures() > 0, "p.473 refusals must be counted");
  REQUIRE(std::isnan(bmsB.crossCheckShunt(0.0f)),
          "p.474 crossCheckShunt must answer NAN during the outage (no arbitration on unreadable state)");
  Comm::BmsSnap s = bmsB.getData();
  REQUIRE(s.gen == 0 && !(s.lastUpdateMs != 0), "p.474 getData must answer the reset snapshot during the outage");
  REQUIRE(!bmsB.socAuthoritative(), "p.474 authority must be suspended during the outage");
  REQUIRE(bmsB.lockFailures() > 0, "p.474 refusals must be counted");
  REQUIRE(authRotations.load() == 0, "p.475 NO rotation during the outage");
  std::string out;
  REQUIRE(!authB.consumeRefreshToken(liveTok, out), "p.475 consume must be refused during the outage");
  REQUIRE(authB.issueRefreshToken("outage").empty(), "p.475 issue must be refused during the outage");
  REQUIRE(authB.liveTokenCount() == 1, "p.475 no token state change during the outage");
  REQUIRE(authB.lockFailures() > 0, "p.475 refusals must be counted");
#endif

  printf("Phase X OK — outage storm: nvsWrites=%llu, rotations=%d, countAnswer=%u\n",
         (unsigned long long)(nvsAfter - nvsBefore), authRotations.load(), countAfter);

  // Phase W (recovery) runs in the SAME phase function: it needs the set-B
  // instances with injection OFF again.
  g_injectMutexCreateFail.store(false);
  g_phase = "W-recovery";
  // Single-threaded deterministic recovery — the retry-create now succeeds.
  logB.append("W00-000002");
  journalB.storeTransaction("tx-r", "h-r", "QUEUED:r");
  bmsB.tickWrite(7);
  std::string nr;
  REQUIRE(logB.activityCount() == countBefore + 1,
          "p.472 recovery must show EXACTLY one new entry — the 1200 refused outage appends left no residue");
  REQUIRE(journalB.decide("tx-r", "h-r", nr) == Services::Decision::Duplicate,
          "p.473 journal must resume after recovery");
  Comm::BmsSnap s2 = bmsB.getData();
  REQUIRE(s2.gen == 7 && s2.current == 7.0f && s2.voltage == 70.0f,
          "p.474 BMS snapshots must resume after recovery");
  REQUIRE(authB.consumeRefreshToken(liveTok, nr), "p.475 rotation must resume after recovery");
  // Brief post-recovery storm (4 threads) — full service again.
  std::vector<std::thread> mini;
  for (int t = 0; t < 4; t++) {
    mini.emplace_back([&, t] {
      std::mt19937 rng(0x1A0 + t);
      for (int i = 0; i < 100; i++) {
        logB.append("W01-000001");
        (void)journalB.storeTransaction("tx-m", "h-m", "QUEUED:m");
        (void)bmsB.crossCheckShunt(NAN);
        microSleep(rng, 25);
      }
    });
  }
  for (auto& th : mini) th.join();
  uint32_t lfLog = logB.lockFailures(), lfJ = journalB.lockFailures();
  REQUIRE(lfLog > 0 && lfJ > 0, "recovery must not erase the outage accounting");
  printf("Phase W OK — deterministic recovery, outage accounting intact (log=%u journal=%u)\n",
         lfLog, lfJ);
  return true;
}

// ---------------------------------------------------------------------------
int main() {
  printf("=== verify_service_lock_concurrency (round-10, p.472-p.475) ===\n");
#if defined(SVC_NO_LOCK)
  printf("build mode: SVC_NO_LOCK (negative control — fully unlocked)\n");
#elif defined(LOCK_FAIL_OPEN)
  printf("build mode: LOCK_FAIL_OPEN (negative control — pre-round-10 shapes)\n");
#else
  printf("build mode: TREATMENT (fail-closed, as shipped)\n");
#endif

  bool anyTrip = false;
  anyTrip |= !phaseV();
  anyTrip |= !phaseR();
  anyTrip |= !phaseS();
  anyTrip |= !phaseT();
  anyTrip |= !phaseU();
  anyTrip |= !phaseX();   // includes Phase W (recovery)
  (void)anyTrip;   // sentinel phases print + count; verdicts below are authoritative

  if (g_sentinels > 0) {
    printf("NEGATIVE CONTROL TRIPPED: %d sentinel(s), %d invariant failure(s) — "
           "harness CAN detect the pre-round-10 shapes\n", g_sentinels, g_fails);
    return 1;
  }
  if (g_fails > 0) {
    printf("RESULT: FAIL — %d invariant failure(s)\n", g_fails);
    return 1;
  }
  printf("RESULT: PASS — all phases green (treatment fail-closed contract holds)\n");
  return 0;
}

