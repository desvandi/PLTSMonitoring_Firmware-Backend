// verify_status_snapshot_detach.cpp — native harness for the ROUND-8
// status-snapshot remediation (p.470): torn alarm-list reads through
// latestStatus.activeAlarms in GET /api/status and the GAS advisor post.
//
// STRUCTURAL MIRROR of the firmware shape:
//   telemetryTask (publishTelemetry):
//     take telemetryMutex → rewrite static s_activeAlarmsBuf[k] (struct
//     assignment) + set count + latestStatus.activeAlarms = buf → give.
//   web / gas tasks (the OLD code, pre-fix):
//     take telemetryMutex → snap = latestStatus (struct copy — the POINTER
//     still aliases the static buffer) → give → read snap.activeAlarms[i]
//     OUTSIDE the mutex (serialize) → torn entry possible.
//   web / gas tasks (the FIX, serializeLatestStatusLocked):
//     take telemetryMutex → snap = latestStatus → DEEP-COPY the alarm list
//     to caller-owned heap → give → read the copy outside — always a state
//     from ONE instant.
//
// TWO BUILD MODES:
//   default        — TREATMENT (deep-copy under the mutex): TSAN-clean, and
//                   every observed entry internally consistent (tagA==tagB).
//   -DNO_DETACH    — NEGATIVE CONTROL (old behavior: serialize the aliased
//                   pointer after the mutex is released): TSAN MUST report
//                   the data race; a clean run means the harness is blind.
//
// Build (treatment):
//   g++ -std=c++17 -g -fsanitize=thread          -pthread -o t verify_status_snapshot_detach.cpp
//   g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o a verify_status_snapshot_detach.cpp
// Build (negative control):
//   g++ -std=c++17 -g -fsanitize=thread -DNO_DETACH -pthread -o u verify_status_snapshot_detach.cpp
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

// ============================================================================
// Mirror of the firmware data structures (shape, not full size — the race
// property is independent of the 3.3 KB vs 96 B payload width).
// ============================================================================
namespace Mirror {

constexpr uint8_t MAX_ALARMS = 24;

// Each entry carries TWO generation tags the writer always sets together.
// A reader observing tagA != tagB within one entry has seen a TORN write —
// the exact observable of the p.470 race (mixed old/new fields in one
// alarm entry of GET /api/status).
struct Alarm {
  uint32_t tagA;
  char     code[16];
  uint32_t tagB;
};

struct SystemStatus {
  const Alarm* activeAlarms;   // pointer into the writer's static buffer
  uint8_t      activeAlarmCount;
};

std::mutex telemetryMutex;                 // telemetryMutex stand-in
Alarm s_activeAlarmsBuf[MAX_ALARMS];        // publishTelemetry()'s static buffer
SystemStatus latestStatus;                  // the shared global

std::atomic<bool> g_stop{false};
std::atomic<uint32_t> g_publishes{0}, g_reads{0}, g_tornEntries{0};

// telemetryTask — publishTelemetry(): rewrite the static buffer under the
// mutex. Entries k get generation-consistent fields: tagA = tagB = gen,
// code = "A<gen>-<k>". Mirrors copyActiveFrom() writing up to count entries.
void publishTelemetryCycle(uint32_t gen, uint8_t count) {
  std::lock_guard<std::mutex> g(telemetryMutex);
  for (uint8_t i = 0; i < count; i++) {
    // Struct assignment, same as the firmware's `dst[n++] = list[i]` —
    // tagA, code, tagB are written as one assignment but the compiler may
    // copy them in any order; that ordering freedom IS the race window.
    Alarm a;
    a.tagA = gen;
    snprintf(a.code, sizeof(a.code), "A%u-%u", gen, (unsigned)i);
    a.tagB = gen;
    s_activeAlarmsBuf[i] = a;
  }
  latestStatus.activeAlarmCount = count;
  latestStatus.activeAlarms = s_activeAlarmsBuf;
  g_publishes.fetch_add(1, std::memory_order_relaxed);
}

// Validate one entry as a serializer would read it: tagA and tagB must come
// from the same generation (both were set together under the mutex).
bool entryConsistent(const Alarm& a) { return a.tagA == a.tagB; }

} // namespace Mirror

static std::atomic<int> g_checks{0}, g_failures{0};
#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    g_checks.fetch_add(1, std::memory_order_relaxed);                        \
    if (!(cond)) {                                                           \
      g_failures.fetch_add(1, std::memory_order_relaxed);                    \
      printf("  FAIL: %s (line %d)\n", msg, __LINE__);                       \
    }                                                                        \
  } while (0)

// ----------------------------------------------------------------------------
// Reader task — the web / GAS consumer. TREATMENT mirrors
// Web::serializeLatestStatusLocked(): deep-copy the list under the mutex,
// then consume the copy outside. NEGATIVE CONTROL (-DNO_DETACH) mirrors the
// OLD code: struct-copy under the mutex, consume through the ALIASED pointer
// after release.
// ----------------------------------------------------------------------------
static void readerTask(int id) {
  std::mt19937 rng(0x57A7u + (uint32_t)id);
  uint32_t torn = 0, reads = 0;
  while (!Mirror::g_stop.load(std::memory_order_relaxed)) {
    Mirror::SystemStatus snap;
    Mirror::Alarm* listCopy = nullptr;
    {
      std::lock_guard<std::mutex> g(Mirror::telemetryMutex);
      snap = Mirror::latestStatus;   // struct copy — pointer still aliases
#ifndef NO_DETACH
      if (snap.activeAlarmCount > 0 && snap.activeAlarms != nullptr) {
        listCopy = (Mirror::Alarm*)malloc(snap.activeAlarmCount * sizeof(Mirror::Alarm));
        if (listCopy != nullptr) {
          memcpy(listCopy, snap.activeAlarms,
                 (size_t)snap.activeAlarmCount * sizeof(Mirror::Alarm));
          snap.activeAlarms = listCopy;   // detach — read caller-owned storage
        } else {
          snap.activeAlarmCount = 0;
          snap.activeAlarms = nullptr;
        }
      }
#endif
    }   // mutex RELEASED — serialization happens outside (the firmware shape)

    reads++;
    for (uint8_t i = 0; i < snap.activeAlarmCount; i++) {
      if (!Mirror::entryConsistent(snap.activeAlarms[i])) torn++;
    }
    free(listCopy);
    std::this_thread::sleep_for(std::chrono::microseconds(20 + (rng() % 60)));
  }
  Mirror::g_reads.fetch_add(reads, std::memory_order_relaxed);
  Mirror::g_tornEntries.fetch_add(torn, std::memory_order_relaxed);
}

// Writer task — telemetryTask cadence (5 s in firmware; compressed here).
static void writerTask() {
  std::mt19937 rng(0x7E1u);
  uint32_t gen = 1;
  while (!Mirror::g_stop.load(std::memory_order_relaxed)) {
    Mirror::publishTelemetryCycle(gen++, (uint8_t)(1 + rng() % Mirror::MAX_ALARMS));
    std::this_thread::sleep_for(std::chrono::microseconds(40 + (rng() % 120)));
  }
}

// Watchdog — a deadlock (nested telemetryMutex acquisition) must fail loudly.
static void watchdog() {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!Mirror::g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (std::chrono::steady_clock::now() > deadline) {
      printf("  FAIL: watchdog timeout — deadlock (line %d)\n", __LINE__);
      g_failures++;
      std::abort();
    }
  }
}

int main() {
  printf("=== status-snapshot detach harness (round-8 p.470) ===\n");
#ifdef NO_DETACH
  printf("mode: NEGATIVE CONTROL (aliased pointer after release) — races EXPECTED\n");
#else
  printf("mode: TREATMENT (deep-copy under the mutex) — must be clean\n");
#endif

  Mirror::publishTelemetryCycle(0, 4);   // prime the shared state

  std::thread wd(watchdog);
  std::thread w(writerTask);
  std::thread rA(readerTask, 0);
  std::thread rB(readerTask, 1);
  std::thread rC(readerTask, 2);

  std::this_thread::sleep_for(std::chrono::seconds(3));
  Mirror::g_stop.store(true, std::memory_order_relaxed);

  w.join(); rA.join(); rB.join(); rC.join(); wd.join();

  printf("stats: publishes=%u reads=%u tornEntries=%u\n",
         Mirror::g_publishes.load(), Mirror::g_reads.load(),
         Mirror::g_tornEntries.load());
  CHECK(Mirror::g_publishes.load() > 1000, "substantial publish churn executed");
  CHECK(Mirror::g_reads.load() > 1000, "substantial reader churn executed");

#ifndef NO_DETACH
  // TREATMENT: every observed entry must be internally consistent — the
  // deep copy ran under the mutex, so the whole list is ONE instant.
  CHECK(Mirror::g_tornEntries.load() == 0, "no torn entries through the detached copy");
  printf("checks=%d failures=%d\n", g_checks.load(), g_failures.load());
  if (g_failures.load() == 0) {
    printf("STATUS SNAPSHOT DETACH HARNESS: ALL CHECKS PASS\n");
    return 0;
  }
  printf("STATUS SNAPSHOT DETACH HARNESS: FAILED\n");
  return 1;
#else
  // NEGATIVE CONTROL: TSAN is the primary detector (the unsynchronized
  // read/write IS the p.470 race class). The logical torn-entry counter is
  // defense in depth — it may or may not catch one in a 3 s window.
  printf("checks=%d failures=%d tornEntries=%u (logical detector; TSAN is primary)\n",
         g_checks.load(), g_failures.load(), Mirror::g_tornEntries.load());
  printf("STATUS SNAPSHOT DETACH HARNESS: NEGATIVE CONTROL COMPLETED\n");
  return 0;   // runner decides via TSAN report presence, like round-7
#endif
}
