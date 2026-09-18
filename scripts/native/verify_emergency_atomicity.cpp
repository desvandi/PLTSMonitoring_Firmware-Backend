// verify_emergency_atomicity.cpp — [GATE-1 / PH8-01 + PH8-02] native harness.
//
// PROVES (treatment build): the driver-level safety latch + write-mutex
// barrier makes the emergency cascade ATOMIC against in-flight relay ON
// writes — the exact race documented in audit Phase 8 S0 (PH8-01):
//
//   relayTask:    generation check PASS  ->  [PREEMPTED]
//   emergencyTask: generation++, ALL OFF (cascade completes)
//   relayTask:    resumes applyCommand() -> _applyChannelState(ON)
//                 -> PCF8574 write ON      <- PHYSICALLY ON AFTER EMERGENCY
//
// MIRRORS the NEW RelayExpanderDriver structure 1:1 (std::atomic latch +
// std::mutex write serialization + register model). Two phases:
//
//   Phase A (deterministic interleave): the audit's exact scenario with
//   explicit checkpoints — generation check passes, emergency fires, the
//   executor resumes. Post-trip register MUST be 0xFF and the ON refused.
//
//   Phase B (stress): real threads hammering ON writes against concurrent
//   emergencies with randomized scheduling. After EVERY emergency completes,
//   the register must settle 0xFF (no ON bit resurrected by any stale write).
//
// NEGATIVE CONTROL (-DPH8_NO_LATCH): compiles the PRE-remediation shape
// (generation check only, no driver latch, no write mutex). Phase A MUST
// observe the post-emergency ON (sentinel exit), Phase B MUST count
// violations > 0. A clean negative control means the harness is blind.
//
// Build (treatment):
//   g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o vea \
//       verify_emergency_atomicity.cpp
// Build (negative control):
//   g++ -std=c++17 -g -fsanitize=thread -DPH8_NO_LATCH -pthread -o vea_nc \
//       verify_emergency_atomicity.cpp
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Mirror of the NEW RelayExpanderDriver (treatment) / OLD shape (NC).
// PCF8574 semantics: bit=1 -> relay OFF (active-LOW). 0xFF = all OFF.
// ---------------------------------------------------------------------------
static const uint8_t POWER_ON_STATE = 0xFF;

struct DriverMirror {
  std::atomic<bool>   safetyLatched{true};   // boot = LATCHED (isolated)
  // [CI lesson x2 — runs 35298266605 / 35299692237] PLAIN std::mutex with
  // BLOCKING acquisition. The first fix (single long-lived timed_mutex)
  // was not sufficient: under the CI runner's g++-13 + libtsan,
  // unique_lock::try_lock_for() timing out (pthread_mutex_timedlock) is
  // mis-tracked as "unlock of an unlocked mutex" — a runtime quirk we cannot
  // reproduce on g++-14 locally. The PROOF PROPERTY of this harness does not
  // depend on the bounded wait: PH8-01 is about {latch + mutual exclusion of
  // the ON-write vs the emergency 0xFF write}. The production driver's
  // bounded-wait + degraded fallback semantics remain covered by the source
  // contract and the logic mirrors (python suites); this native harness uses
  // the most conservative, universally-TSAN-safe synchronization.
  std::mutex          writeMutex;
  std::atomic<uint8_t> reg{POWER_ON_STATE};  // physical register model

#ifndef PH8_NO_LATCH
  bool setChannel(uint8_t ch, bool on) {
    // [PH8-01] ALL mutations serialized; ON re-checks the latch inside the
    // critical region. (Mirrors the fixed RelayExpanderDriver::setChannel.)
    std::lock_guard<std::mutex> lk(writeMutex);
    if (on && safetyLatched.load(std::memory_order_acquire)) return false;
    uint8_t base = reg.load(std::memory_order_relaxed);
    uint8_t next = on ? (uint8_t)(base & ~(1u << ch)) : (uint8_t)(base | (1u << ch));
    reg.store(next, std::memory_order_release);
    return true;
  }
  void forceSafetyAllOff() {
    // LATCH FIRST (atomic, mutex-independent), then ONE 0xFF write under
    // the write mutex — the emergency barrier critical region.
    safetyLatched.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(writeMutex);
    reg.store(POWER_ON_STATE, std::memory_order_release);
  }
  void clearSafetyLatch() { safetyLatched.store(false, std::memory_order_release); }
#else
  // ------------------- PRE-REMEDIATION SHAPE (negative control) ------------
  // No latch, no write mutex: the generation check in the CONTROLLER ran
  // before applyCommand; setChannel itself trusted it blindly.
  bool setChannel(uint8_t ch, bool on) {
    uint8_t base = reg.load(std::memory_order_relaxed);
    uint8_t next = on ? (uint8_t)(base & ~(1u << ch)) : (uint8_t)(base | (1u << ch));
    reg.store(next, std::memory_order_release);
    return true;
  }
  void forceSafetyAllOff() { reg.store(POWER_ON_STATE, std::memory_order_release); }
  void clearSafetyLatch() {}
#endif
};

// ---------------------------------------------------------------------------
// Controller mirror: generation snapshot + executor + emergency authority.
// ---------------------------------------------------------------------------
struct ControllerMirror {
  DriverMirror    drv;
  std::atomic<uint32_t> safetyGeneration{0};

  // Executor path (relayTask): generation check THEN physical write.
  // `pauseAfterGenCheck` models the audit's preempt window deterministically.
  bool executeOn(uint8_t ch, uint32_t genSnapshot, bool pauseAfterGenCheck) {
    if (genSnapshot != safetyGeneration.load(std::memory_order_acquire)) {
      return false;   // BLOCKED_STALE_SAFETY
    }
    if (pauseAfterGenCheck) {
      // [PREEMPT WINDOW] emergencyTask fires HERE in Phase A.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return drv.setChannel(ch, true);
  }

  // Emergency authority (emergencyTask): cascade + generation bump.
  void emergencyAllOff() {
    drv.forceSafetyAllOff();                       // [PH8-01] barrier FIRST
    safetyGeneration.fetch_add(1, std::memory_order_acq_rel);
  }
};

// ---------------------------------------------------------------------------
static int failures = 0;
static void check(bool cond, const char* name, const char* detail) {
  if (!cond) {
    printf("  FAIL: %s — %s\n", name, detail);
    failures++;
  } else {
    printf("  PASS: %s\n", name);
  }
}

// ---------------------------------------------------------------------------
// Phase A — the audit's deterministic interleaving.
// ---------------------------------------------------------------------------
static void phaseA() {
  printf("Phase A — deterministic in-flight ON vs emergencyAllOff (audit PH8-01 scenario)\n");
  ControllerMirror c;
  c.drv.clearSafetyLatch();                    // system ARMED (RUN)
  const uint32_t gen = c.safetyGeneration.load();

  // Bring CH3 ON (normal operation).
  bool on1 = c.executeOn(3, gen, false);
  check(on1 && !(c.drv.reg.load() & (1u << 3)), "A0 baseline ON applied",
        "pre-emergency ON must work when latched-clear");

  // The race: generation check PASSes, then the emergency preempts BEFORE
  // the physical write.
  std::thread relay([&]() { c.executeOn(5, gen, /*pause=*/true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(1));  // preempt lands here
  c.emergencyAllOff();                                          // full cascade
  relay.join();

  const uint8_t finalReg = c.drv.reg.load();
  check(finalReg == POWER_ON_STATE, "A1 post-trip register == 0xFF (all OFF)",
        "an in-flight ON must never survive the emergency cascade");
  check(c.drv.safetyLatched.load(), "A2 safety latch set after trip",
        "ON authority must be latched off");
  // Post-trip NEW command (fresh generation) must still be refused (latch).
  const uint32_t gen2 = c.safetyGeneration.load();
  bool on2 = c.executeOn(0, gen2, false);
  check(!on2 && c.drv.reg.load() == POWER_ON_STATE,
        "A3 post-trip ON (fresh generation) refused by latch",
        "operator ARM is the only path back");
  // ARM restores authority.
  c.drv.clearSafetyLatch();
  bool on3 = c.executeOn(2, c.safetyGeneration.load(), false);
  check(on3, "A4 ARM restores ON authority", "clearSafetyLatch must re-enable ON");
}

// ---------------------------------------------------------------------------
// Phase B — stress: hammering executor + concurrent emergencies.
// ---------------------------------------------------------------------------
static void phaseB() {
  printf("Phase B — randomized stress (200 emergency epochs x concurrent ON writers)\n");
  // Harness bookkeeping — atomic so TSAN only flags REAL state races.
  std::atomic<long> violations{0};   // ON bits present after an emergency
  std::atomic<long> blockedByGen{0}, blockedByLatch{0}, applied{0};

  // [CI lesson — run 35298266605] ONE ControllerMirror for the whole phase:
  // its writeMutex is constructed ONCE and outlives every epoch, exactly like
  // the production driver (RelayExpanderDriver's _safetyWriteMutex is created
  // in begin() and never destroyed). Re-constructing the mirror inside the
  // loop reused the same stack slot for 200 consecutive std::timed_mutex
  // lifetimes; TSAN on g++-13 mis-tracks stack-reused mutex lifetimes and
  // reported "unlock of an unlocked mutex" false positives (2 warnings; all
  // functional assertions passed, violations=0). State is now RESET per
  // epoch instead of reconstructed.
  ControllerMirror c;

  for (int round = 0; round < 200; round++) {
    c.drv.clearSafetyLatch();                  // RUN
    c.drv.reg.store(POWER_ON_STATE);

    std::atomic<bool> stopWriters{false};
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; w++) {
      writers.emplace_back([&, w]() {
        std::mt19937 rng(1234 + w * 7 + round);
        while (!stopWriters.load()) {
          uint32_t gen = c.safetyGeneration.load(std::memory_order_acquire);
          if (gen != c.safetyGeneration.load(std::memory_order_acquire)) continue;
          // Executor pipeline: generation check then physical write, with a
          // random yield in between to widen the preempt window.
          std::this_thread::yield();
          if (gen != c.safetyGeneration.load(std::memory_order_acquire)) {
            blockedByGen++; continue;
          }
          bool r = c.executeOn((uint8_t)(rng() % 8), gen, /*pause=*/false);
          if (r) applied++; else blockedByLatch++;
        }
      });
    }
    // Fire the emergency at a random moment.
    std::this_thread::sleep_for(std::chrono::microseconds(200 + (round * 37) % 800));
    c.emergencyAllOff();
    stopWriters.store(true);
    for (auto& t : writers) t.join();

    // [PH8-01 acceptance] After the cascade completes, the register must be
    // ALL OFF — no ON bit may survive via any stale/in-flight write.
    uint8_t finalReg = c.drv.reg.load();
    if (finalReg != POWER_ON_STATE) violations++;
  }

  printf("  stats: violations=%ld applied=%ld genBlocked=%ld latchBlocked=%ld\n",
         violations.load(), applied.load(), blockedByGen.load(), blockedByLatch.load());
#ifdef PH8_NO_LATCH
  // Negative control: the OLD shape MUST be caught (violations > 0).
  check(violations.load() > 0, "B1 NEGATIVE CONTROL TRIPPED (pre-fix shape races detected)",
        "harness is blind if the old shape shows zero violations");
  printf("PH8 NEGATIVE CONTROL TRIPPED: %ld violating epochs\n", violations.load());
#else
  char det[128];
  snprintf(det, sizeof det, "violations=%ld applied=%ld genBlocked=%ld latchBlocked=%ld",
           violations.load(), applied.load(), blockedByGen.load(), blockedByLatch.load());
  check(violations.load() == 0, "B1 zero post-emergency ON resurrections (200 epochs)", det);
#endif
}

// ---------------------------------------------------------------------------
int main() {
  printf("=== verify_emergency_atomicity (GATE-1 / PH8-01 + PH8-02) ===\n");
#ifdef PH8_NO_LATCH
  printf("mode: NEGATIVE CONTROL (pre-remediation shape: no latch, no write mutex)\n");
#endif
  phaseA();
  phaseB();
  if (failures == 0) {
    printf("RESULT: PASS\n");
    return 0;
  }
  printf("RESULT: FAIL (%d)\n", failures);
#ifdef PH8_NO_LATCH
  printf("PH8 NEGATIVE CONTROL TRIPPED: %d assertion(s)\n", failures);
#endif
  return 1;
}
