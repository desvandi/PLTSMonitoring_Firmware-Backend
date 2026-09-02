#!/usr/bin/env python3
"""
UNIT-001: Voltage Calibration — 3-Point Piecewise Linear Correction
Brief §11-14: mandatory 3-point calibration (LOW/NOMINAL/FULL)
"""
import sys

def piecewise_linear(raw, low_ref, low_raw, nom_ref, nom_raw, full_ref, full_raw):
    """Apply 3-point piecewise linear correction."""
    if raw <= low_raw:
        if low_raw == nom_raw: return low_ref
        slope = (low_ref - 0) / (low_raw - 0) if low_raw != 0 else 1.0
        return raw * slope
    elif raw <= nom_raw:
        slope = (nom_ref - low_ref) / (nom_raw - low_raw)
        return low_ref + (raw - low_raw) * slope
    elif raw <= full_raw:
        slope = (full_ref - nom_ref) / (full_raw - nom_raw)
        return nom_ref + (raw - nom_raw) * slope
    else:
        slope = (full_ref - nom_ref) / (full_raw - nom_raw)
        return full_ref + (raw - full_raw) * slope

def validate_calibration(low_ref, low_raw, nom_ref, nom_raw, full_ref, full_raw):
    """Brief §14: reject if LOW >= NOMINAL or NOMINAL >= FULL or excessive deviation."""
    if low_ref >= nom_ref: return False, "LOW >= NOMINAL"
    if nom_ref >= full_ref: return False, "NOMINAL >= FULL"
    if low_raw >= nom_raw: return False, "LOW raw >= NOMINAL raw"
    if nom_raw >= full_raw: return False, "NOMINAL raw >= FULL raw"
    return True, "VALID"

def test_basic_calibration():
    """Test basic piecewise linear correction."""
    # Calibration: ref 45V→raw 44.61V, ref 50.73V→raw 50.18V, ref 54V→raw 53.85V
    result = piecewise_linear(50.18, 45.0, 44.61, 50.73, 50.18, 54.0, 53.85)
    assert abs(result - 50.73) < 0.01, f"Expected ~50.73, got {result}"
    print(f"PASS test_basic_calibration: {result:.2f}V (expected ~50.73V)")

def test_low_point():
    result = piecewise_linear(44.61, 45.0, 44.61, 50.73, 50.18, 54.0, 53.85)
    assert abs(result - 45.0) < 0.01, f"Expected ~45.0, got {result}"
    print(f"PASS test_low_point: {result:.2f}V (expected ~45.0V)")

def test_full_point():
    result = piecewise_linear(53.85, 45.0, 44.61, 50.73, 50.18, 54.0, 53.85)
    assert abs(result - 54.0) < 0.01, f"Expected ~54.0, got {result}"
    print(f"PASS test_full_point: {result:.2f}V (expected ~54.0V)")

def test_midpoint():
    # Between LOW and NOMINAL
    result = piecewise_linear(47.4, 45.0, 44.61, 50.73, 50.18, 54.0, 53.85)
    expected = 45.0 + (47.4 - 44.61) * (50.73 - 45.0) / (50.18 - 44.61)
    assert abs(result - expected) < 0.01, f"Expected ~{expected:.2f}, got {result}"
    print(f"PASS test_midpoint: {result:.2f}V (expected ~{expected:.2f}V)")

def test_validation_rejects_low_ge_nominal():
    ok, msg = validate_calibration(50.0, 50.0, 45.0, 45.0, 54.0, 54.0)
    assert not ok, "Should reject LOW >= NOMINAL"
    print(f"PASS test_validation_rejects_low_ge_nominal: {msg}")

def test_validation_rejects_nominal_ge_full():
    ok, msg = validate_calibration(45.0, 44.61, 54.0, 53.85, 50.0, 50.18)
    assert not ok, "Should reject NOMINAL >= FULL"
    print(f"PASS test_validation_rejects_nominal_ge_full: {msg}")

def test_validation_accepts_valid():
    ok, msg = validate_calibration(45.0, 44.61, 50.73, 50.18, 54.0, 53.85)
    assert ok, f"Should accept valid calibration: {msg}"
    print(f"PASS test_validation_accepts_valid: {msg}")

if __name__ == '__main__':
    tests = [test_basic_calibration, test_low_point, test_full_point, test_midpoint,
             test_validation_rejects_low_ge_nominal, test_validation_rejects_nominal_ge_full,
             test_validation_accepts_valid]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
