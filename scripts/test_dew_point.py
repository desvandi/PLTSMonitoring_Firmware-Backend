#!/usr/bin/env python3
"""
UNIT-012: Dew Point — Magnus Formula
Brief §30: Dew point must be deterministic.
"""
import sys
import math

def compute_dew_point(temp_c, humidity):
    """Magnus formula: Td = (b * α) / (a - α), where α = ln(RH/100) + (a*T)/(b+T)"""
    if temp_c is None or humidity is None or math.isnan(temp_c) or math.isnan(humidity):
        return None
    if humidity < 0 or humidity > 100:
        return None
    a = 17.625
    b = 243.04
    alpha = math.log(humidity / 100.0) + (a * temp_c) / (b + temp_c)
    return (b * alpha) / (a - alpha)

def test_standard_conditions():
    # At 20°C, 50% RH, dew point should be ~9.3°C
    td = compute_dew_point(20.0, 50.0)
    assert abs(td - 9.3) < 0.5, f"Expected ~9.3°C, got {td}"
    print(f"PASS test_standard_conditions: Td={td:.2f}°C")

def test_high_humidity():
    td = compute_dew_point(25.0, 90.0)
    assert abs(td - 23.3) < 0.5, f"Expected ~23.3°C, got {td}"
    print(f"PASS test_high_humidity: Td={td:.2f}°C")

def test_low_humidity():
    td = compute_dew_point(30.0, 20.0)
    assert td < 10.0, f"Expected <10°C, got {td}"
    print(f"PASS test_low_humidity: Td={td:.2f}°C")

def test_100_percent_humidity():
    td = compute_dew_point(25.0, 100.0)
    assert abs(td - 25.0) < 0.1, f"At 100% RH, Td should equal T, got {td}"
    print(f"PASS test_100_percent_humidity: Td={td:.2f}°C")

def test_nan_returns_none():
    assert compute_dew_point(float('nan'), 50.0) is None
    assert compute_dew_point(25.0, float('nan')) is None
    print("PASS test_nan_returns_none")

def test_none_returns_none():
    assert compute_dew_point(None, 50.0) is None
    assert compute_dew_point(25.0, None) is None
    print("PASS test_none_returns_none")

def test_invalid_humidity_returns_none():
    assert compute_dew_point(25.0, -10.0) is None
    assert compute_dew_point(25.0, 150.0) is None
    print("PASS test_invalid_humidity_returns_none")

if __name__ == '__main__':
    tests = [test_standard_conditions, test_high_humidity, test_low_humidity,
             test_100_percent_humidity, test_nan_returns_none, test_none_returns_none,
             test_invalid_humidity_returns_none]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
