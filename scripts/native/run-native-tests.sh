#!/usr/bin/env bash
# Native sanitizer harnesses for audit remediation.
#
# Round-6 harnesses (verify_alarm_blob_atomicity / verify_emg_safety_gate /
# verify_anomaly_quality_gates) mirror the firmware logic STRUCTURE 1:1 with
# ASAN+UBSAN — they prove the persistence/safety ALGORITHMS.
#
# Round-7 (p.467/p.468/p.469) adds verify_alarm_concurrency: a REAL
# multi-threaded harness (std::thread) mirroring the locked AlarmRegistry,
# run under BOTH ThreadSanitizer (absence of data races) and ASAN+UBSAN
# (memory safety under the interleaving), PLUS a negative control — the same
# harness compiled with -DREGISTRY_NO_LOCK (the round-6 unlocked registry)
# which ThreadSanitizer MUST flag. A clean negative control would mean the
# harness is blind and the treatment result is worthless.
#
# Round-9 (p.471) adds a SECOND negative control to the same harness:
# -DLOCK_FAIL_OPEN compiles the ROUND-7 fail-open _lock() (mutex unavailable
# → proceed WITHOUT synchronization and claim success). The p.471 phases
# (boot guard, fail-closed storm, recovery) MUST trip their assertions in
# that mode — a clean exit would mean the harness cannot detect the
# fail-open class p.471 removes.
#
# Round-10 (p.472-p.475) adds verify_service_lock_concurrency: the fail-open
# FAMILY in LogService (unlocked readers + fail-open writers),
# TransactionJournal (no-op _lock), BatteryCommManager (crossCheckShunt
# reading _data before the mutex check, never acquiring it) and AuthManager
# (fail-open _lockAuth reviving the refresh replay race). Same discipline:
# treatment under TSAN + ASAN/UBSAN, plus TWO negative controls
# (-DSVC_NO_LOCK = fully unlocked, -DLOCK_FAIL_OPEN = pre-round-10 shapes)
# which MUST trip (sentinels and/or TSAN reports) — a clean negative control
# would mean the harness is blind.
#
# Round-11 (p.476) extends the SAME harness with Phase F + the Phase X/W
# factory-reset invariants: prepare/confirm one-time token under the auth
# mutex (the pre-round-11 shape had NO lock at all on those two functions —
# both negative controls run them unlocked and MUST trip the P476
# sentinels / TSAN races).
#
# Usage: bash scripts/native/run-native-tests.sh   (needs g++; exit 0 = PASS)
set -u
cd "$(dirname "$0")"
fails=0
for h in verify_alarm_blob_atomicity verify_emg_safety_gate verify_anomaly_quality_gates; do
  echo "=== $h (ASAN+UBSAN) ==="
  if ! g++ -std=c++17 -g -fsanitize=address,undefined -o "$h" "$h.cpp"; then
    echo "COMPILE FAIL: $h"; fails=$((fails+1)); continue
  fi
  "./$h" || fails=$((fails+1))
done

echo "=== verify_alarm_concurrency (TREATMENT, ThreadSanitizer) ==="
if ! g++ -std=c++17 -g -fsanitize=thread -pthread -o vac_tsan verify_alarm_concurrency.cpp; then
  echo "COMPILE FAIL: verify_alarm_concurrency (tsan)"; fails=$((fails+1))
else
  ./vac_tsan || fails=$((fails+1))
fi

echo "=== verify_alarm_concurrency (TREATMENT, ASAN+UBSAN) ==="
if ! g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o vac_asan verify_alarm_concurrency.cpp; then
  echo "COMPILE FAIL: verify_alarm_concurrency (asan)"; fails=$((fails+1))
else
  ./vac_asan || fails=$((fails+1))
fi

echo "=== verify_alarm_concurrency (NEGATIVE CONTROL, round-6 UNLOCKED registry under TSAN) ==="
# The unlocked registry MUST produce races — this proves the harness detects
# the p.467/p.468/p.469 race class. Exit 0 here would be a FAILURE (blind
# test), as would a report that does not involve the registry state.
if ! g++ -std=c++17 -g -fsanitize=thread -DREGISTRY_NO_LOCK -pthread -o vac_unlocked verify_alarm_concurrency.cpp; then
  echo "COMPILE FAIL: verify_alarm_concurrency (negative control)"; fails=$((fails+1))
else
  if ./vac_unlocked > vac_unlocked.log 2>&1; then
    echo "NEGATIVE CONTROL FAILED: unlocked registry ran clean — the harness cannot detect the race class"
    fails=$((fails+1))
  else
    if grep -q "WARNING: ThreadSanitizer" vac_unlocked.log; then
      echo "NEGATIVE CONTROL OK: $(grep -c 'WARNING: ThreadSanitizer' vac_unlocked.log) race report(s) on the unlocked registry — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE: nonzero exit but no TSAN report — investigate vac_unlocked.log"
      fails=$((fails+1))
    fi
  fi
fi

echo "=== verify_alarm_concurrency (NEGATIVE CONTROL p.471, ROUND-7 FAIL-OPEN _lock under TSAN) ==="
# [AUDIT 2026-09 ROUND 9 / p.471] The fail-open shape (mutex unavailable →
# proceed WITHOUT the lock, claim success) is the bug class round-9 removes.
# The p.471 phases (boot guard / fail-closed storm / recovery) MUST trip in
# this mode — assertions, TSAN races, or both. A clean exit is a FAILURE
# (blind harness); a nonzero exit with neither a TSAN report nor the p.471
# sentinel is INCONCLUSIVE.
if ! g++ -std=c++17 -g -fsanitize=thread -DLOCK_FAIL_OPEN -pthread -o vac_failopen verify_alarm_concurrency.cpp; then
  echo "COMPILE FAIL: verify_alarm_concurrency (p.471 negative control)"; fails=$((fails+1))
else
  if ./vac_failopen > vac_failopen.log 2>&1; then
    echo "NEGATIVE CONTROL FAILED (p.471): fail-open registry ran clean — the harness cannot detect the fail-open class"
    fails=$((fails+1))
  else
    if grep -q "WARNING: ThreadSanitizer" vac_failopen.log || grep -q "P471 NEGATIVE CONTROL TRIPPED" vac_failopen.log; then
      echo "NEGATIVE CONTROL OK (p.471): fail-open shape detected ($(grep -c 'WARNING: ThreadSanitizer' vac_failopen.log) TSAN report(s) + sentinel) — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE (p.471): nonzero exit but no TSAN report / no sentinel — investigate vac_failopen.log"
      fails=$((fails+1))
    fi
  fi
fi

echo "=== verify_status_snapshot_detach (TREATMENT, ThreadSanitizer) ==="
if ! g++ -std=c++17 -g -fsanitize=thread -pthread -o vsd_tsan verify_status_snapshot_detach.cpp; then
  echo "COMPILE FAIL: verify_status_snapshot_detach (tsan)"; fails=$((fails+1))
else
  ./vsd_tsan || fails=$((fails+1))
fi

echo "=== verify_status_snapshot_detach (TREATMENT, ASAN+UBSAN) ==="
if ! g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o vsd_asan verify_status_snapshot_detach.cpp; then
  echo "COMPILE FAIL: verify_status_snapshot_detach (asan)"; fails=$((fails+1))
else
  ./vsd_asan || fails=$((fails+1))
fi

echo "=== verify_status_snapshot_detach (NEGATIVE CONTROL, round-7 aliased-pointer read under TSAN) ==="
# The OLD status-serialization shape (struct copy, then consume the ALIASED
# pointer after the mutex is released) MUST produce races — this proves the
# round-8 harness detects the p.470 race class. Exit 0 without a TSAN report
# would be a FAILURE (blind test).
if ! g++ -std=c++17 -g -fsanitize=thread -DNO_DETACH -pthread -o vsd_unlocked verify_status_snapshot_detach.cpp; then
  echo "COMPILE FAIL: verify_status_snapshot_detach (negative control)"; fails=$((fails+1))
else
  if ./vsd_unlocked > vsd_unlocked.log 2>&1; then
    if grep -q "WARNING: ThreadSanitizer" vsd_unlocked.log; then
      echo "NEGATIVE CONTROL OK: $(grep -c 'WARNING: ThreadSanitizer' vsd_unlocked.log) race report(s) on the aliased-pointer read — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE: clean exit with no TSAN report — investigate vsd_unlocked.log"
      fails=$((fails+1))
    fi
  else
    if grep -q "WARNING: ThreadSanitizer" vsd_unlocked.log; then
      echo "NEGATIVE CONTROL OK: $(grep -c 'WARNING: ThreadSanitizer' vsd_unlocked.log) race report(s) on the aliased-pointer read — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE: nonzero exit but no TSAN report — investigate vsd_unlocked.log"
      fails=$((fails+1))
    fi
  fi
fi

rm -f verify_alarm_blob_atomicity verify_emg_safety_gate verify_anomaly_quality_gates \
      vac_tsan vac_asan vac_unlocked vac_unlocked.log vac_failopen vac_failopen.log \
      vsd_tsan vsd_asan vsd_unlocked vsd_unlocked.log

# ============================================================================
# [AUDIT 2026-09 ROUND 10 / p.472-p.475] verify_service_lock_concurrency —
# the fail-open family in LogService / TransactionJournal /
# BatteryCommManager / AuthManager. Treatment (TSAN + ASAN/UBSAN) must be
# clean; BOTH negative controls must trip (sentinels and/or TSAN reports).
# ============================================================================
echo "=== verify_service_lock_concurrency (TREATMENT, ThreadSanitizer) ==="
if ! g++ -std=c++17 -g -fsanitize=thread -pthread -o vsc_tsan verify_service_lock_concurrency.cpp; then
  echo "COMPILE FAIL: verify_service_lock_concurrency (tsan)"; fails=$((fails+1))
else
  # [CI lesson] On failure the log is EVIDENCE — print it (the redirect
  # would otherwise hide the failing phase from the CI console).
  if ! ./vsc_tsan > vsc_tsan.log 2>&1; then
    fails=$((fails+1)); echo "TREATMENT FAILED — tail of vsc_tsan.log:"; tail -60 vsc_tsan.log
  fi
fi

echo "=== verify_service_lock_concurrency (TREATMENT, ASAN+UBSAN) ==="
if ! g++ -std=c++17 -g -fsanitize=address,undefined -pthread -o vsc_asan verify_service_lock_concurrency.cpp; then
  echo "COMPILE FAIL: verify_service_lock_concurrency (asan)"; fails=$((fails+1))
else
  if ! ./vsc_asan > vsc_asan.log 2>&1; then
    fails=$((fails+1)); echo "TREATMENT FAILED — tail of vsc_asan.log:"; tail -60 vsc_asan.log
  fi
fi

echo "=== verify_service_lock_concurrency (NEGATIVE CONTROL, UNLOCKED services under TSAN) ==="
if ! g++ -std=c++17 -g -fsanitize=thread -DSVC_NO_LOCK -pthread -o vsc_unlocked verify_service_lock_concurrency.cpp; then
  echo "COMPILE FAIL: verify_service_lock_concurrency (unlocked nc)"; fails=$((fails+1))
else
  if ./vsc_unlocked > vsc_unlocked.log 2>&1; then
    echo "NEGATIVE CONTROL FAILED: unlocked services ran clean — the harness cannot detect the race class"
    fails=$((fails+1))
  else
    if grep -q "WARNING: ThreadSanitizer" vsc_unlocked.log || grep -q "NEGATIVE CONTROL TRIPPED" vsc_unlocked.log; then
      echo "NEGATIVE CONTROL OK: $(grep -c 'WARNING: ThreadSanitizer' vsc_unlocked.log) TSAN report(s) + sentinels on the unlocked services — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE: nonzero exit but no TSAN report / no sentinel — investigate vsc_unlocked.log"
      fails=$((fails+1))
    fi
  fi
fi

echo "=== verify_service_lock_concurrency (NEGATIVE CONTROL p.472-p.475, PRE-ROUND-10 FAIL-OPEN shapes under TSAN) ==="
if ! g++ -std=c++17 -g -fsanitize=thread -DLOCK_FAIL_OPEN -pthread -o vsc_failopen verify_service_lock_concurrency.cpp; then
  echo "COMPILE FAIL: verify_service_lock_concurrency (fail-open nc)"; fails=$((fails+1))
else
  if ./vsc_failopen > vsc_failopen.log 2>&1; then
    echo "NEGATIVE CONTROL FAILED (p.472-p.475): fail-open services ran clean — the harness cannot detect the fail-open family"
    fails=$((fails+1))
  else
    if grep -q "WARNING: ThreadSanitizer" vsc_failopen.log || grep -q "NEGATIVE CONTROL TRIPPED" vsc_failopen.log; then
      echo "NEGATIVE CONTROL OK (p.472-p.475): $(grep -c 'WARNING: ThreadSanitizer' vsc_failopen.log) TSAN report(s) + sentinels on the pre-round-10 shapes — harness sensitivity proven"
    else
      echo "NEGATIVE CONTROL INCONCLUSIVE (p.472-p.475): investigate vsc_failopen.log"
      fails=$((fails+1))
    fi
  fi
fi

rm -f vsc_tsan vsc_asan vsc_unlocked vsc_unlocked.log vsc_failopen vsc_failopen.log vsc_tsan.log vsc_asan.log
echo
if [ "$fails" -eq 0 ]; then echo "ALL NATIVE HARNESS GREEN"; else echo "$fails harness(es) FAILED"; exit 1; fi
