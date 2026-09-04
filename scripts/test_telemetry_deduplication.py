#!/usr/bin/env python3
"""
UNIT-017: Telemetry Deduplication — deviceId + sequence
Brief §42, §51: Same telemetry identity must not create duplicate records.
"""
import sys

class TelemetryDeduplicator:
    def __init__(self):
        self.seen = {}  # deviceId → set of sequences

    def is_duplicate(self, deviceId, sequence):
        if deviceId not in self.seen:
            self.seen[deviceId] = set()
        if sequence in self.seen[deviceId]:
            return True
        self.seen[deviceId].add(sequence)
        return False

def test_first_telemetry_not_duplicate():
    d = TelemetryDeduplicator()
    assert not d.is_duplicate("PLTS-AB12CD34", 1)
    print("PASS test_first_telemetry_not_duplicate")

def test_exact_duplicate_rejected():
    d = TelemetryDeduplicator()
    d.is_duplicate("PLTS-AB12CD34", 1)
    assert d.is_duplicate("PLTS-AB12CD34", 1), "Same deviceId+sequence should be duplicate"
    print("PASS test_exact_duplicate_rejected")

def test_different_sequence_not_duplicate():
    d = TelemetryDeduplicator()
    d.is_duplicate("PLTS-AB12CD34", 1)
    assert not d.is_duplicate("PLTS-AB12CD34", 2), "Different sequence should not be duplicate"
    print("PASS test_different_sequence_not_duplicate")

def test_different_device_not_duplicate():
    d = TelemetryDeduplicator()
    d.is_duplicate("PLTS-AB12CD34", 1)
    assert not d.is_duplicate("PLTS-EF56GH78", 1), "Different deviceId should not be duplicate"
    print("PASS test_different_device_not_duplicate")

def test_reboot_replay():
    """Brief §43: offline telemetry spooled and replayed on reconnect — should not duplicate."""
    d = TelemetryDeduplicator()
    # First "replay" of spooled telemetry
    for seq in range(100, 110):
        assert not d.is_duplicate("PLTS-AB12CD34", seq)
    # Re-replay after a reconnect — should all be duplicates
    for seq in range(100, 110):
        assert d.is_duplicate("PLTS-AB12CD34", seq), f"Replayed seq {seq} should be duplicate"
    print("PASS test_reboot_replay")

def test_high_sequence_numbers():
    d = TelemetryDeduplicator()
    d.is_duplicate("PLTS-AB12CD34", 4294967295)  # uint32 max
    assert d.is_duplicate("PLTS-AB12CD34", 4294967295)
    assert not d.is_duplicate("PLTS-AB12CD34", 4294967294)
    print("PASS test_high_sequence_numbers")

if __name__ == '__main__':
    tests = [test_first_telemetry_not_duplicate, test_exact_duplicate_rejected,
             test_different_sequence_not_duplicate, test_different_device_not_duplicate,
             test_reboot_replay, test_high_sequence_numbers]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
