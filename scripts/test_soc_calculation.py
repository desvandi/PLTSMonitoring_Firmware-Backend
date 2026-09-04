#!/usr/bin/env python3
"""
UNIT-008: SOC Calculation — Coulomb Counting + Voltage Sync
Brief §17-21: ΔAh = I × Δt / 3600, full-charge detection, baseline correction
"""
import sys
import math

class SocCalculator:
    def __init__(self, capacity_ah=200.0):
        self.capacity_ah = capacity_ah
        self.soc = 50.0  # start at 50%
        self.charge_ah = 0.0
        self.discharge_ah = 0.0
        self.state = "NORMAL"
        self.last_sync = None

    def integrate(self, current_a, dt_sec):
        """Brief §17: ΔAh = I × Δt / 3600"""
        if current_a is None or math.isnan(current_a):
            return  # do not integrate invalid (brief §16)
        dt_h = dt_sec / 3600.0
        delta_ah = current_a * dt_h
        if delta_ah > 0:
            self.charge_ah += delta_ah
        else:
            self.discharge_ah += -delta_ah
        # Update SOC
        self.soc += (delta_ah / self.capacity_ah) * 100.0
        self.soc = max(0.0, min(100.0, self.soc))

    def check_full_charge(self, voltage, current, threshold_a=2.0, persistence_sec=600):
        """Brief §19: V >= 54 AND I < threshold AND persists → FULL_CONFIRMED"""
        if voltage >= 54.0 and current is not None and current < threshold_a and current >= 0:
            if self.state != "FULL_CANDIDATE":
                self.state = "FULL_CANDIDATE"
                self._candidate_start = 0
            self._candidate_start += 1  # simplified
            if self._candidate_start >= persistence_sec:
                self.state = "FULL_CONFIRMED"
                self.soc = 100.0
                self.last_sync = voltage
                return True
        else:
            if self.state == "FULL_CANDIDATE":
                self.state = "CHARGING"
        return False

def test_basic_coulomb_counting():
    calc = SocCalculator(200.0)
    calc.integrate(20.0, 3600)  # 20A for 1 hour = +20Ah = +10% SOC
    assert abs(calc.charge_ah - 20.0) < 0.01
    assert abs(calc.soc - 60.0) < 0.01, f"Expected 60%, got {calc.soc}"
    print(f"PASS test_basic_coulomb_counting: SOC={calc.soc}%")

def test_discharge():
    calc = SocCalculator(200.0)
    calc.soc = 80.0
    calc.integrate(-40.0, 3600)  # -40A for 1 hour = -40Ah = -20% SOC
    assert abs(calc.discharge_ah - 40.0) < 0.01
    assert abs(calc.soc - 60.0) < 0.01, f"Expected 60%, got {calc.soc}"
    print(f"PASS test_discharge: SOC={calc.soc}%")

def test_soc_bounds():
    calc = SocCalculator(200.0)
    calc.integrate(1000.0, 36000)  # huge charge
    assert calc.soc == 100.0, f"SOC should cap at 100%, got {calc.soc}"
    calc.soc = 10.0
    calc.integrate(-1000.0, 36000)  # huge discharge
    assert calc.soc == 0.0, f"SOC should floor at 0%, got {calc.soc}"
    print(f"PASS test_soc_bounds")

def test_invalid_current_not_integrated():
    calc = SocCalculator(200.0)
    initial_soc = calc.soc
    calc.integrate(float('nan'), 3600)
    assert calc.soc == initial_soc, "NaN current should not change SOC"
    calc.integrate(None, 3600)
    assert calc.soc == initial_soc, "None current should not change SOC"
    print(f"PASS test_invalid_current_not_integrated")

def test_full_charge_detection():
    calc = SocCalculator(200.0)
    calc.soc = 95.0
    # Simulate full charge condition for 600s
    for _ in range(600):
        result = calc.check_full_charge(54.5, 1.0, threshold_a=2.0, persistence_sec=600)
    assert calc.state == "FULL_CONFIRMED", f"Expected FULL_CONFIRMED, got {calc.state}"
    assert calc.soc == 100.0, f"Expected 100%, got {calc.soc}"
    print(f"PASS test_full_charge_detection: SOC={calc.soc}%")

def test_full_charge_lost():
    calc = SocCalculator(200.0)
    calc.state = "FULL_CANDIDATE"
    calc.check_full_charge(50.0, 10.0)  # condition lost
    assert calc.state == "CHARGING", f"Expected CHARGING, got {calc.state}"
    print(f"PASS test_full_charge_lost")

if __name__ == '__main__':
    tests = [test_basic_coulomb_counting, test_discharge, test_soc_bounds,
             test_invalid_current_not_integrated, test_full_charge_detection, test_full_charge_lost]
    passed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{passed}/{len(tests)} tests PASSED")
    sys.exit(0 if passed == len(tests) else 1)
