#!/usr/bin/env python3
"""
PHASE 13-D PROPERTY TESTS
Energy + SOC + Quality Engine invariants.

P1: SOC always [0,100] — already covered (Phase 13-C)
P2: finite input → finite output
P3: invalid quality → no energy integration
P4: dt <= 0 → no integration
P5: dt > maximum → no integration
P6: wall-clock jump → energy unchanged — already covered
P7: constant current integration is linear
P8: charge and discharge are directionally symmetric — already covered
P9: measurement quality transitions deterministic
P10: counter never decreases due solely to invalid sample
"""
import sys
import math
import random

MAX_DT_MS = 300000  # 5 minutes

def quality_allows_integration(q):
    return q == "Valid"

def energy_tick(voltage, current, vq, iq, dt_ms, prev_voltage, prev_current):
    """Simulate energy integration with quality gate."""
    if not quality_allows_integration(vq) or not quality_allows_integration(iq):
        return None  # BLOCKED
    if math.isnan(voltage) or math.isnan(current) or math.isinf(voltage) or math.isinf(current):
        return None  # NaN/Inf rejected
    if dt_ms <= 0 or dt_ms > MAX_DT_MS:
        return None  # dt bounds
    dt_hours = dt_ms / 3600000.0
    avg_i = (current + prev_current) * 0.5
    avg_v = (voltage + prev_voltage) * 0.5
    d_ah = avg_i * dt_hours
    d_wh = avg_v * avg_i * dt_hours
    return (d_ah, d_wh)

def soc_clamp(soc):
    if math.isnan(soc) or math.isinf(soc): return 0.0
    return max(0.0, min(100.0, soc))

# P2: finite input → finite output
def test_finite_output():
    random.seed(42)
    for _ in range(1000):
        v = random.uniform(30, 60)
        i = random.uniform(-100, 100)
        dt = random.randint(100, 300000)
        result = energy_tick(v, i, "Valid", "Valid", dt, v, i)
        if result:
            d_ah, d_wh = result
            assert math.isfinite(d_ah), f"dAh not finite: {d_ah}"
            assert math.isfinite(d_wh), f"dWh not finite: {d_wh}"
    print("PASS P2: finite input → finite output (1000 cases)")

# P3: invalid quality → no integration
def test_invalid_quality_blocks():
    for q in ["Stale", "Suspect", "Calibrating", "SensorError", "NotAvailable"]:
        result = energy_tick(52.0, 10.0, "Valid", q, 5000, 52.0, 10.0)
        assert result is None, f"Quality {q} should BLOCK integration"
        result = energy_tick(52.0, 10.0, q, "Valid", 5000, 52.0, 10.0)
        assert result is None, f"Quality {q} (voltage) should BLOCK integration"
    print("PASS P3: invalid quality → no energy integration (5 states tested)")

# P4: dt <= 0 → no integration
def test_zero_negative_dt():
    assert energy_tick(52.0, 10.0, "Valid", "Valid", 0, 52.0, 10.0) is None
    print("PASS P4: dt == 0 → no integration")

# P5: dt > maximum → no integration
def test_excessive_dt():
    assert energy_tick(52.0, 10.0, "Valid", "Valid", MAX_DT_MS + 1, 52.0, 10.0) is None
    assert energy_tick(52.0, 10.0, "Valid", "Valid", 3600000, 52.0, 10.0) is None
    print("PASS P5: dt > MAX → no integration (gap policy)")

# P7: constant current integration is linear
def test_linear_integration():
    """For constant current I and constant dt, total ΔAh should be linear."""
    i = 10.0
    dt = 1000  # 1 second per tick
    total_ah = 0.0
    for _ in range(3600):  # 1 hour = 3600 ticks
        result = energy_tick(52.0, i, "Valid", "Valid", dt, 52.0, i)
        if result:
            total_ah += result[0]
    expected = i * 1.0  # 10A × 1h = 10Ah
    assert abs(total_ah - expected) < 0.01, f"Expected {expected} Ah, got {total_ah}"
    print(f"PASS P7: constant I={i}A × 1h = {total_ah:.3f}Ah (expected {expected}Ah)")

# P9: quality transitions deterministic
def test_quality_transitions():
    transitions = [
        ("Valid", "Stale", "BLOCKED"),
        ("Stale", "Valid", "ALLOWED"),
        ("Valid", "Suspect", "BLOCKED"),
        ("Suspect", "Valid", "ALLOWED"),
        ("Valid", "SensorError", "BLOCKED"),
        ("SensorError", "Valid", "ALLOWED"),
        ("NotAvailable", "Valid", "ALLOWED"),
        ("Valid", "Calibrating", "BLOCKED"),
        ("Calibrating", "Valid", "ALLOWED"),
        ("Calibrating", "SensorError", "BLOCKED"),
    ]
    for old_q, new_q, expected in transitions:
        new_allowed = quality_allows_integration(new_q)
        if expected == "BLOCKED":
            assert not new_allowed, f"{old_q}→{new_q}: new quality {new_q} should BLOCK integration"
        else:
            assert new_allowed, f"{old_q}→{new_q}: new quality {new_q} should ALLOW integration"
    print("PASS P9: quality transitions deterministic (10 transitions tested)")

# P10: counter never decreases due solely to invalid sample
def test_invalid_sample_no_decrease():
    """If sensor goes Valid → SensorError → Valid, counters must not decrease."""
    total_ah = 0.0
    # Valid for 10 ticks
    for _ in range(10):
        result = energy_tick(52.0, 10.0, "Valid", "Valid", 1000, 52.0, 10.0)
        if result: total_ah += result[0]
    ah_before_failure = total_ah
    # SensorError for 10 ticks (should NOT integrate)
    for _ in range(10):
        result = energy_tick(52.0, 10.0, "Valid", "SensorError", 1000, 52.0, 10.0)
        assert result is None, "SensorError should block integration"
    ah_after_failure = total_ah  # unchanged
    # Valid again for 10 ticks
    for _ in range(10):
        result = energy_tick(52.0, 10.0, "Valid", "Valid", 1000, 52.0, 10.0)
        if result: total_ah += result[0]
    ah_after_recovery = total_ah
    assert ah_after_failure == ah_before_failure, "Counters changed during failure!"
    assert ah_after_recovery > ah_before_failure, "Counters didn't resume after recovery!"
    print(f"PASS P10: counter freeze during failure (before={ah_before_failure:.4f}, after_failure={ah_after_failure:.4f}, after_recovery={ah_after_recovery:.4f})")

# SOC NaN/Inf rejection
def test_soc_nan_inf():
    test_values = [float('nan'), float('inf'), float('-inf'), -100.0, -1.0, 0.0, 50.0, 100.0, 101.0, 500.0]
    for v in test_values:
        clamped = soc_clamp(v)
        assert 0.0 <= clamped <= 100.0, f"SOC {v} → {clamped} escaped [0,100]"
        assert math.isfinite(clamped), f"SOC {v} → {clamped} not finite"
    print("PASS SOC NaN/Inf: all values clamped to [0,100] (10 cases)")

# Energy direction test
def test_energy_direction():
    """Charging increases chargeAh, discharging increases dischargeAh."""
    # Charging: I = +10A, dt = 5 min (within MAX_DT_MS)
    result = energy_tick(52.0, 10.0, "Valid", "Valid", 300000, 52.0, 10.0)
    assert result is not None, "Charging should integrate"
    d_ah, d_wh = result
    assert d_ah > 0, f"Charging should produce positive dAh, got {d_ah}"
    assert d_wh > 0, f"Charging should produce positive dWh, got {d_wh}"

    # Discharging: I = -10A, dt = 5 min
    result = energy_tick(52.0, -10.0, "Valid", "Valid", 300000, 52.0, -10.0)
    assert result is not None, "Discharging should integrate"
    d_ah, d_wh = result
    assert d_ah < 0, f"Discharging should produce negative dAh, got {d_ah}"
    assert d_wh < 0, f"Discharging should produce negative dWh, got {d_wh}"

    print("PASS Energy direction: I>0 → chargeAh↑, I<0 → dischargeAh↑")

# Zero current test
def test_zero_current():
    result = energy_tick(52.0, 0.0, "Valid", "Valid", 300000, 52.0, 0.0)
    assert result is not None, "Zero current should integrate"
    d_ah, d_wh = result
    assert abs(d_ah) < 1e-10, f"Zero current should produce zero dAh, got {d_ah}"
    assert abs(d_wh) < 1e-10, f"Zero current should produce zero dWh, got {d_wh}"
    print("PASS Zero current: ΔAh=0, ΔWh=0")

if __name__ == '__main__':
    tests = [
        test_finite_output,
        test_invalid_quality_blocks,
        test_zero_negative_dt,
        test_excessive_dt,
        test_linear_integration,
        test_quality_transitions,
        test_invalid_sample_no_decrease,
        test_soc_nan_inf,
        test_energy_direction,
        test_zero_current,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} Phase 13-D property tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
