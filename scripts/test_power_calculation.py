#!/usr/bin/env python3
"""
UNIT-005: Power Calculation — Signed P = V × I
Brief §15: P > 0 = charging, P < 0 = discharging
"""
import sys
import math

def compute_power(voltage, current):
    """Brief §15: Pbattery = Vbattery × Ibattery. Sign follows current."""
    if voltage is None or current is None or math.isnan(voltage) or math.isnan(current):
        return None
    return voltage * current

def test_positive_power_charging():
    p = compute_power(52.34, 12.5)
    assert p > 0, "Positive current should produce positive power (charging)"
    assert abs(p - 654.25) < 0.01
    print(f"PASS test_positive_power_charging: {p:.2f}W")

def test_negative_power_discharging():
    p = compute_power(52.34, -37.2)
    assert p < 0, "Negative current should produce negative power (discharging)"
    assert abs(p - (-1947.048)) < 0.01
    print(f"PASS test_negative_power_discharging: {p:.2f}W")

def test_zero_current():
    p = compute_power(52.34, 0.0)
    assert p == 0.0, "Zero current should produce zero power"
    print(f"PASS test_zero_current: {p}W")

def test_null_voltage():
    p = compute_power(None, 12.5)
    assert p is None, "Null voltage should produce null power"
    print(f"PASS test_null_voltage: {p}")

def test_null_current():
    p = compute_power(52.34, None)
    assert p is None, "Null current should produce null power"
    print(f"PASS test_null_current: {p}")

def test_nan_voltage():
    p = compute_power(float('nan'), 12.5)
    assert p is None, "NaN voltage should produce null power"
    print(f"PASS test_nan_voltage: {p}")

def test_nan_current():
    p = compute_power(52.34, float('nan'))
    assert p is None, "NaN current should produce null power"
    print(f"PASS test_nan_current: {p}")

if __name__ == '__main__':
    tests = [test_positive_power_charging, test_negative_power_discharging, test_zero_current,
             test_null_voltage, test_null_current, test_nan_voltage, test_nan_current]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
