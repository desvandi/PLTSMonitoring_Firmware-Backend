#!/usr/bin/env bash
# Native ASAN+UBSAN harnesses for audit round-6 remediation (p.453-p.465).
# These mirror the firmware logic STRUCTURE 1:1 (the round-5 lesson: a mirror
# of the INTENT alone missed the 0xFF out-of-bounds P0).
# Usage: bash scripts/native/run-native-tests.sh   (needs g++; exit 0 = PASS)
set -u
cd "$(dirname "$0")"
fails=0
for h in verify_alarm_blob_atomicity verify_emg_safety_gate verify_anomaly_quality_gates; do
  echo "=== $h ==="
  if ! g++ -std=c++17 -g -fsanitize=address,undefined -o "$h" "$h.cpp"; then
    echo "COMPILE FAIL: $h"; fails=$((fails+1)); continue
  fi
  "./$h" || fails=$((fails+1))
done
rm -f verify_alarm_blob_atomicity verify_emg_safety_gate verify_anomaly_quality_gates
echo
if [ "$fails" -eq 0 ]; then echo "ALL NATIVE HARNESS GREEN"; else echo "$fails harness(es) FAILED"; exit 1; fi
