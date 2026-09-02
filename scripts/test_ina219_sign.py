#!/usr/bin/env python3
"""
UNIT-003: INA219 Sign Convention
Brief §5: Positive current = CHARGING (entering battery), Negative = DISCHARGING.
Immutable at protocol level. Do NOT invert in PWA.
"""
import sys

def classify_direction(current_a, idle_threshold=0.5):
    if current_a is None or current_a != current_a:  # NaN check
        return "IDLE"
    if current_a > idle_threshold:
        return "CHARGING"
    if current_a < -idle_threshold:
        return "DISCHARGING"
    return "IDLE"

def test_positive_charging():
    assert classify_direction(42.5) == "CHARGING"
    print("PASS test_positive_charging")

def test_negative_discharging():
    assert classify_direction(-37.2) == "DISCHARGING"
    print("PASS test_negative_discharging")

def test_near_zero_idle():
    assert classify_direction(0.2) == "IDLE"
    assert classify_direction(-0.3) == "IDLE"
    print("PASS test_near_zero_idle")

def test_configurable_deadband():
    assert classify_direction(0.6, idle_threshold=1.0) == "IDLE"
    assert classify_direction(1.5, idle_threshold=1.0) == "CHARGING"
    print("PASS test_configurable_deadband")

def test_nan_returns_idle():
    assert classify_direction(float('nan')) == "IDLE"
    print("PASS test_nan_returns_idle")

def test_none_returns_idle():
    assert classify_direction(None) == "IDLE"
    print("PASS test_none_returns_idle")

if __name__ == '__main__':
    tests = [test_positive_charging, test_negative_discharging, test_near_zero_idle,
             test_configurable_deadband, test_nan_returns_idle, test_none_returns_idle]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
