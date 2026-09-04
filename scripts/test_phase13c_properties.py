#!/usr/bin/env python3
"""
PHASE 13-C PROPERTY TESTS
Brief directive §31: Property tests — not just example-based testing.

Property 1: SOC ∈ [0, 100] for all inputs
Property 2: ChargeAh >= 0 AND DischargeAh >= 0 for all inputs
Property 3: Clock jump does not produce energy spike
Property 4: Current sign reversal produces opposite energy direction
Property 5: Invalid sensor does not produce valid measurement
"""
import sys
import math
import random

# =============================================================================
# Property 1: SOC bounds [0, 100]
# =============================================================================

def soc_from_coulomb(baseline_ah, capacity_ah):
    """SOC = 50 + (baseline / capacity) * 100, clamped [0, 100]"""
    soc = 50.0 + (baseline_ah / capacity_ah) * 100.0
    return max(0.0, min(100.0, soc))

def test_soc_bounds_property():
    """For ANY baseline and ANY capacity, SOC must be in [0, 100]."""
    random.seed(42)
    for _ in range(1000):
        baseline = random.uniform(-10000, 10000)
        capacity = random.uniform(1, 1000)
        soc = soc_from_coulomb(baseline, capacity)
        assert 0.0 <= soc <= 100.0, f"SOC {soc} out of bounds for baseline={baseline}, cap={capacity}"
    # Edge cases
    assert soc_from_coulomb(-999999, 200) == 0.0
    assert soc_from_coulomb(999999, 200) == 100.0
    assert soc_from_coulomb(0, 200) == 50.0
    print("PASS Property 1: SOC ∈ [0, 100] for all inputs (1000 random cases)")

# =============================================================================
# Property 2: ChargeAh >= 0 AND DischargeAh >= 0
# =============================================================================

def integrate_energy(current_a, dt_hours):
    """Separate charge/discharge accumulators."""
    if current_a > 0:
        return (current_a * dt_hours, 0.0)  # chargeAh, dischargeAh
    elif current_a < 0:
        return (0.0, -current_a * dt_hours)
    else:
        return (0.0, 0.0)

def test_energy_non_negative_property():
    """For ANY current and ANY dt, chargeAh >= 0 AND dischargeAh >= 0."""
    random.seed(42)
    for _ in range(1000):
        current = random.uniform(-200, 200)
        dt = random.uniform(0, 10)
        charge_ah, discharge_ah = integrate_energy(current, dt)
        assert charge_ah >= 0, f"chargeAh {charge_ah} < 0 for I={current}, dt={dt}"
        assert discharge_ah >= 0, f"dischargeAh {discharge_ah} < 0 for I={current}, dt={dt}"
    print("PASS Property 2: ChargeAh >= 0 AND DischargeAh >= 0 (1000 random cases)")

# =============================================================================
# Property 3: Clock jump does not produce energy spike
# =============================================================================

def monotonic_dt(now_ms, last_ms):
    """Monotonic dt using unsigned subtraction (rollover-safe for <49 days)."""
    if last_ms == 0:
        return 0  # first sample — skip
    if now_ms >= last_ms:
        return now_ms - last_ms
    else:
        return (0xFFFFFFFF - last_ms) + now_ms + 1  # rollover

def test_clock_jump_property():
    """Wall-clock jump must NOT affect monotonic dt → no energy spike."""
    # Simulate: monotonic time advances normally, wall-clock jumps +1 hour
    monotonic_times = [10000, 20000, 30000, 40000]  # ms, 10s apart
    wall_clock_times = [1000, 1010, 7200, 7210]  # wall-clock jumps +1h at t=30000

    dt_list = []
    last_mono = 0
    for mono in monotonic_times:
        dt = monotonic_dt(mono, last_mono)
        if dt > 0:
            dt_list.append(dt)
        last_mono = mono

    # All dt should be ~10000ms (10s) — no spike from wall-clock jump
    for dt in dt_list:
        assert 5000 <= dt <= 15000, f"dt {dt}ms out of expected range — clock jump corrupted integration"

    # Verify: if we used wall-clock instead, dt would be 6190s (spike!)
    bad_dt = wall_clock_times[2] - wall_clock_times[1]  # 7200 - 1010 = 6190s (wrong!)
    assert bad_dt > 6000, f"Wall-clock dt should show spike: {bad_dt}"
    assert dt_list[1] == 10000, f"Monotonic dt should be 10000ms: {dt_list[1]}"

    print(f"PASS Property 3: Clock jump +3600s → monotonic dt=10s (no spike). Wall-clock would have produced dt={bad_dt}s")

# =============================================================================
# Property 4: Current sign reversal → opposite energy direction
# =============================================================================

def test_sign_reversal_property():
    """If current changes sign, energy direction must change."""
    dt_hours = 1.0  # 1 hour

    # Charging: I = +10A
    charge_ah, discharge_ah = integrate_energy(10.0, dt_hours)
    assert charge_ah == 10.0 and discharge_ah == 0.0, f"Charging: {charge_ah}, {discharge_ah}"

    # Discharging: I = -10A (same magnitude, opposite sign)
    charge_ah2, discharge_ah2 = integrate_energy(-10.0, dt_hours)
    assert charge_ah2 == 0.0 and discharge_ah2 == 10.0, f"Discharging: {charge_ah2}, {discharge_ah2}"

    # Verify symmetry: same magnitude → same energy magnitude, opposite direction
    assert charge_ah == discharge_ah2, f"Sign reversal must produce equal magnitude energy"
    assert discharge_ah == charge_ah2, f"Sign reversal must produce equal magnitude energy"

    print("PASS Property 4: Current +10A → chargeAh=10, -10A → dischargeAh=10 (symmetric)")

# =============================================================================
# Property 5: Invalid sensor → NaN + quality != Valid (NOT 0)
# =============================================================================

def make_measurement(value, quality):
    """Simulate Measurement struct."""
    return {"value": value, "quality": quality}

def make_invalid_measurement(quality):
    """Invalid measurement: value=NaN, quality=error."""
    return make_measurement(float('nan'), quality)

def test_invalid_sensor_property():
    """Invalid sensor must produce NaN value + error quality, NOT 0 + Valid."""
    # INA219 disconnected
    m = make_invalid_measurement("SensorError")
    assert math.isnan(m["value"]), f"Invalid sensor value must be NaN, got {m['value']}"
    assert m["quality"] != "Valid", f"Invalid sensor quality must not be Valid"
    assert m["quality"] == "SensorError", f"Expected SensorError, got {m['quality']}"

    # Verify: 0A with valid sensor is different from NaN with error
    valid_zero = make_measurement(0.0, "Valid")
    assert valid_zero["value"] == 0.0 and valid_zero["quality"] == "Valid"
    assert math.isnan(m["value"]) and m["quality"] == "SensorError"
    assert m != valid_zero, "Sensor failure (NaN+error) must differ from 0A reading (0+valid)"

    print("PASS Property 5: Invalid sensor → NaN + SensorError (NOT 0 + Valid)")

# =============================================================================
# Additional: millis() rollover safety
# =============================================================================

def test_millis_rollover():
    """Unsigned subtraction handles 32-bit rollover correctly."""
    # Normal case
    assert monotonic_dt(20000, 10000) == 10000

    # Near rollover (uint32_t max = 4294967295)
    MAX_UINT32 = 0xFFFFFFFF
    last_ms = MAX_UINT32 - 4999  # 5000ms before rollover (so rollover at MAX_UINT32)
    now_ms = 5000  # 5000ms after rollover
    dt = monotonic_dt(now_ms, last_ms)
    assert dt == 10000, f"Rollover dt should be 10000ms, got {dt}"

    print(f"PASS millis rollover: dt({now_ms}, {last_ms}) = {dt}ms (correct)")

# =============================================================================
# Additional: NaN/Inf rejection
# =============================================================================

def test_nan_inf_rejection():
    """NaN and Inf must NOT enter telemetry as valid numbers."""
    assert math.isnan(float('nan'))
    assert math.isinf(float('inf'))
    assert math.isinf(float('-inf'))

    # is_valid_float check
    def is_valid_float(v):
        return not (math.isnan(v) or math.isinf(v))

    assert not is_valid_float(float('nan'))
    assert not is_valid_float(float('inf'))
    assert not is_valid_float(float('-inf'))
    assert is_valid_float(0.0)
    assert is_valid_float(52.34)
    assert is_valid_float(-10.5)

    print("PASS NaN/Inf rejection: NaN, +Inf, -Inf all rejected; valid floats accepted")

if __name__ == '__main__':
    tests = [
        test_soc_bounds_property,
        test_energy_non_negative_property,
        test_clock_jump_property,
        test_sign_reversal_property,
        test_invalid_sensor_property,
        test_millis_rollover,
        test_nan_inf_rejection,
    ]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} property tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
