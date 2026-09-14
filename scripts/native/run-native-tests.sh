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

rm -f verify_alarm_blob_atomicity verify_emg_safety_gate verify_anomaly_quality_gates \
      vac_tsan vac_asan vac_unlocked vac_unlocked.log
echo
if [ "$fails" -eq 0 ]; then echo "ALL NATIVE HARNESS GREEN"; else echo "$fails harness(es) FAILED"; exit 1; fi
