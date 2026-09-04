#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_bms_comm.py — v1.6.0 multi-protocol BMS/inverter comm layer logic tests.

Mirrors (in Python) the exact decode/state semantics of:
  firmware/Comm/PylontechCanClient.cpp  (static decoders)
  firmware/Comm/ModbusRtuClient.cpp     (CRC16, frame builder, register decode)
  firmware/Comm/BatteryCommManager.cpp  (probe/lock/lost state machine + cross-check)

These are LOGIC MIRRORS, like the repo's existing test_soc_calculation.py /
test_voltage_calibration.py pattern: the C++ sources remain the shipping truth;
this suite pins their semantics with concrete test vectors so any divergence
fails loudly. Hardware-dependent paths (TWAI/UART/TCP) are covered by the
bench checklist (docs/remediation-2026-08/12_BENCH_EXECUTION_CHECKLIST.md §G).

Run:  python3 scripts/test_bms_comm.py
"""

import math
import os
import re
import sys

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"  FAIL: {name} {detail}")


# ============================================================================
# 1. Modbus CRC-16 (poly 0xA001, init 0xFFFF, low byte first on the wire)
#    Canonical vector from the Modbus specification example.
# ============================================================================

def crc16(buf):
    crc = 0xFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_read_holding(slave, start, count):
    pdu = [slave, 0x03, (start >> 8) & 0xFF, start & 0xFF,
           (count >> 8) & 0xFF, count & 0xFF]
    crc = crc16(pdu)
    return pdu + [crc & 0xFF, crc >> 8]        # LOW byte first


def test_crc16():
    # Authoritative check value: CRC-16/MODBUS("123456789") = 0x4B37
    # (catalogued check value for poly 0xA001, init 0xFFFF, reflected).
    got = crc16(b"123456789")
    check("crc16 catalog check value", got == 0x4B37, f"got 0x{got:04X}")
    frame = build_read_holding(0x01, 100, 13)
    check("build_read_holding length", len(frame) == 8)
    check("build_read_holding slave+fc", frame[0] == 0x01 and frame[1] == 0x03)
    check("build_read_holding start reg BE", frame[2] == 0x00 and frame[3] == 100)
    check("build_read_holding count BE", frame[4] == 0x00 and frame[5] == 13)
    # Wire order: CRC low byte first — verify by recomputing over the PDU part.
    lo, hi = frame[6], frame[7]
    check("build_read_holding crc low-first", crc16(frame[:6]) == (lo | (hi << 8)))
    # Self-consistency: a response frame carrying its own CRC round-trips.
    resp = [0x01, 0x03, 26] + [0x12] * 26
    rcrc = crc16(resp)
    framed = resp + [rcrc & 0xFF, rcrc >> 8]
    check("response crc round-trips",
          crc16(framed[:-2]) == (framed[-2] | (framed[-1] << 8)))
    # A single flipped bit must break the CRC (noise false-positive guard).
    corrupted = list(framed)
    corrupted[3] ^= 0x01
    check("flipped bit breaks crc",
          crc16(corrupted[:-2]) != (corrupted[-2] | (corrupted[-1] << 8)))


# ============================================================================
# 2. Modbus register decode (plausibility-gated — NaN honesty)
# ============================================================================

REG_BASE = 100
REG_COUNT = 13
OFF_PACK_VOLTAGE, OFF_PACK_CURRENT, OFF_PACK_SOC, OFF_PACK_SOH = 0, 1, 2, 3
OFF_CELL_MIN_MV, OFF_CELL_MAX_MV, OFF_TEMP_X01C, _R7 = 4, 5, 6, 7
OFF_CHG_LIMIT_A, OFF_DIS_LIMIT_A, OFF_CYCLE_COUNT, OFF_FAULT_FLAGS, OFF_CELL_COUNT = 8, 9, 10, 11, 12

SCALE_VOLTAGE, SCALE_CURRENT, SCALE_TEMPERATURE, SCALE_LIMIT, SCALE_CELL_MV = 0.01, 0.1, 0.1, 0.1, 0.001
MODBUS_RACK_CURRENT_SIGN = -1.0


def plausible_soc(v):   return not math.isnan(v) and 0 <= v <= 100
def plausible_soh(v):   return not math.isnan(v) and 0 <= v <= 100
def plausible_volt(v):  return not math.isnan(v) and 10 <= v <= 70
def plausible_cur(v):   return not math.isnan(v) and -1000 <= v <= 1000
def plausible_cell(v):  return not math.isnan(v) and 1.5 <= v <= 4.5
# [2026-09 #4] bmsTempPlausible mirror — physically-possible envelope (a real
# 90 C overheating pack IS reported; only impossible garbage is nulled).
def plausible_temp(v):  return not math.isnan(v) and -40 < v < 100


def decode_registers(regs):
    """Mirror of ModbusRtuClient::decodeRegisters — implausible → None."""
    def u16(off): return regs[off] if off < len(regs) else 0
    def i16(off): return u16(off) - 65536 if u16(off) > 32767 else u16(off)

    out = {}
    v = u16(OFF_PACK_VOLTAGE) * SCALE_VOLTAGE
    i = i16(OFF_PACK_CURRENT) * SCALE_CURRENT * MODBUS_RACK_CURRENT_SIGN
    soc = float(u16(OFF_PACK_SOC))
    soh = float(u16(OFF_PACK_SOH))
    cmin = u16(OFF_CELL_MIN_MV) * SCALE_CELL_MV
    cmax = u16(OFF_CELL_MAX_MV) * SCALE_CELL_MV
    t = i16(OFF_TEMP_X01C) * SCALE_TEMPERATURE
    ccl = u16(OFF_CHG_LIMIT_A) * SCALE_LIMIT
    dcl = u16(OFF_DIS_LIMIT_A) * SCALE_LIMIT

    out["voltage"] = v if plausible_volt(v) else None
    out["current"] = i if plausible_cur(i) else None
    out["soc"] = soc if plausible_soc(soc) else None
    out["soh"] = soh if plausible_soh(soh) else None
    out["cellVoltageMin"] = cmin if plausible_cell(cmin) else None
    out["cellVoltageMax"] = cmax if plausible_cell(cmax) else None
    out["temperature"] = t if plausible_temp(t) else None
    out["chargeCurrentLimit"] = ccl if plausible_cur(ccl) else None
    out["dischargeCurrentLimit"] = dcl if plausible_cur(dcl) else None
    out["cycleCount"] = u16(OFF_CYCLE_COUNT)
    out["faultFlags"] = u16(OFF_FAULT_FLAGS)
    cells = u16(OFF_CELL_COUNT)
    out["cellCount"] = cells if 0 < cells < 256 else 0
    return out


def test_register_decode():
    regs = [0] * REG_COUNT
    regs[OFF_PACK_VOLTAGE] = 4800     # 48.00 V
    regs[OFF_PACK_CURRENT] = 65536 - 150   # i16 -150 → vendor discharge-negative = charge
    regs[OFF_PACK_SOC] = 62
    regs[OFF_PACK_SOH] = 98
    regs[OFF_CELL_MIN_MV] = 3305
    regs[OFF_CELL_MAX_MV] = 3320
    regs[OFF_TEMP_X01C] = 285         # 28.5 °C
    regs[OFF_CHG_LIMIT_A] = 1000      # 100.0 A
    regs[OFF_DIS_LIMIT_A] = 800       # 80.0 A
    regs[OFF_CYCLE_COUNT] = 42
    regs[OFF_FAULT_FLAGS] = 0
    regs[OFF_CELL_COUNT] = 16

    d = decode_registers(regs)
    check("reg decode voltage", d["voltage"] == 48.00)
    check("reg decode current sign-negated to +charge", d["current"] == 15.0,
          f"got {d['current']}")
    check("reg decode soc", d["soc"] == 62.0)
    check("reg decode soh", d["soh"] == 98.0)
    check("reg decode cell min", abs(d["cellVoltageMin"] - 3.305) < 1e-9)
    check("reg decode cell max", abs(d["cellVoltageMax"] - 3.320) < 1e-9)
    check("reg decode temperature", d["temperature"] == 28.5)
    check("reg decode ccl", d["chargeCurrentLimit"] == 100.0)
    check("reg decode dcl", d["dischargeCurrentLimit"] == 80.0)
    check("reg decode cycle count", d["cycleCount"] == 42)
    check("reg decode cell count", d["cellCount"] == 16)

    # Implausible values must stay None (never fabricated).
    bad = [0] * REG_COUNT
    bad[OFF_PACK_SOC] = 150           # impossible SOC
    bad[OFF_PACK_VOLTAGE] = 8000      # 80 V — outside 10..70 plausibility
    bad[OFF_CELL_MIN_MV] = 9999       # 9.999 V — impossible cell
    bad[OFF_CELL_COUNT] = 500         # impossible count → 0 (not reported)
    bad[OFF_TEMP_X01C] = 32767        # +3276.7 C — sensor garbage
    d2 = decode_registers(bad)
    check("implausible soc → None", d2["soc"] is None)
    check("implausible voltage → None", d2["voltage"] is None)
    check("implausible cell → None", d2["cellVoltageMin"] is None)
    check("implausible cellCount → 0 (not reported)", d2["cellCount"] == 0)
    check("implausible temperature (sensor garbage) → None", d2["temperature"] is None)
    # [2026-09 #4] A REAL overheat must survive the gate (possible ≠ safe).
    hot = [0] * REG_COUNT
    hot[OFF_TEMP_X01C] = 900          # +90.0 C — dangerous but REAL
    d3 = decode_registers(hot)
    check("real overheat (+90 C) is REPORTED, not nulled", d3["temperature"] == 90.0)
    cold = [0] * REG_COUNT
    cold[OFF_TEMP_X01C] = -600        # -60.0 C — impossible for this system
    d4 = decode_registers(cold)
    check("impossible cold (-60 C) → None", d4["temperature"] is None)


# ============================================================================
# 3. Pylontech CAN decoders (public protocol document frame map)
# ============================================================================

PYLONTECH_CAN_CURRENT_SIGN = -1.0


def decode_351(d):
    cvl = ((d[0] << 8) | d[1]) * 0.1
    ccl = ((d[2] << 8) | d[3]) * 0.1
    dcl = ((d[4] << 8) | d[5]) * 0.1
    return cvl, ccl, dcl


def decode_355(d):
    return float(d[0]), float(d[1])


def decode_356(d):
    raw_v = (d[0] << 8) | d[1]
    raw_i = (d[2] << 8) | d[3]
    if raw_i > 32767:
        raw_i -= 65536
    raw_t = (d[4] << 8) | d[5]
    if raw_t > 32767:
        raw_t -= 65536
    return raw_v * 0.01, raw_i * 0.1 * PYLONTECH_CAN_CURRENT_SIGN, raw_t * 0.1


def decode_359(d):
    out = []
    for k in range(4):
        mv = (d[k * 2] << 8) | d[k * 2 + 1]
        out.append(mv * 0.001)
    return out


def test_pylontech_can():
    # 0x351: CVL 54.2 V (raw 542 = 0x021E), CCL 100 A (raw 1000), DCL 80 A
    d351 = [0x02, 0x1E, 0x03, 0xE8, 0x03, 0x20, 0x00, 0x00]
    cvl, ccl, dcl = decode_351(d351)
    check("0x351 CVL 54.2V", abs(cvl - 54.2) < 1e-6, f"got {cvl}")
    check("0x351 CCL 100A", abs(ccl - 100.0) < 1e-6)
    check("0x351 DCL 80A", abs(dcl - 80.0) < 1e-6)

    # 0x355: SOC 87 %, SOH 98 %
    soc, soh = decode_355([87, 98, 0, 0, 0, 0])
    check("0x355 SOC 87", soc == 87.0)
    check("0x355 SOH 98", soh == 98.0)

    # 0x356: V=51.00 V (raw 5100 = 0x13EC), I raw -320 (vendor
    # discharge-positive: -32 A = charge 32 A after negation), T=28.5 °C
    v, i, t = decode_356([0x13, 0xEC, 0xFE, 0xC0, 0x01, 0x1D, 0x00, 0x00])
    check("0x356 voltage 51.00V", abs(v - 51.00) < 1e-6, f"got {v}")
    check("0x356 current sign-negated to +32A charge", abs(i - 32.0) < 1e-6, f"got {i}")
    check("0x356 temperature 28.5C", abs(t - 28.5) < 1e-6)

    # Discharge case: raw +250 → vendor +25 A discharge → canonical -25 A.
    _, i2, _ = decode_356([0x13, 0xEC, 0x00, 0xFA, 0x01, 0x1D, 0x00, 0x00])
    check("0x356 discharge negative after sign fix", abs(i2 - (-25.0)) < 1e-6, f"got {i2}")

    # 0x359: 4 cell voltages in mV
    cells = decode_359([0x0C, 0xEE, 0x0C, 0xF8, 0x0C, 0xF3, 0x0C, 0xE9])
    expected = [3.310, 3.320, 3.315, 3.305]
    check("0x359 cell voltages",
          all(abs(a - b) < 1e-9 for a, b in zip(cells, expected)), f"got {cells}")


# ============================================================================
# 4. BatteryCommManager state machine mirror
#    PROBING → LOCKED (2 consecutive successes) → LOST (3 consecutive
#    failures) → PROBING after 60 s cooldown. Hysteresis: one failure resets
#    the lock streak.
# ============================================================================

LOCK_SUCCESSES_REQUIRED = 2
LOST_FAILURES_REQUIRED = 3
REPROBE_COOLDOWN_MS = 60000
MISMATCH_SUSTAIN_POLLS = 3


class ManagerMirror:
    def __init__(self):
        self.state = "PROBING"
        self.lock_successes = 0
        self.poll_failures = 0
        self.lost_since_ms = 0
        self.mismatch_streak = 0
        self.mismatch_active = False
        self.last_mismatch = None
        self.protocol = None
        self.log = []

    def probe_success(self):
        if self.state != "PROBING":
            return
        self.lock_successes += 1
        if self.lock_successes >= LOCK_SUCCESSES_REQUIRED:
            self.state = "LOCKED"
            self.protocol = "PYLONTECH_CAN"
            self.poll_failures = 0
            self.log.append(("BMS_PROTOCOL_LOCKED", self.protocol))

    def probe_failure(self):
        if self.state != "PROBING":
            return
        self.lock_successes = 0        # hysteresis: consecutive only

    def poll_failure(self, now_ms):
        if self.state != "LOCKED":
            return
        self.poll_failures += 1
        if self.poll_failures >= LOST_FAILURES_REQUIRED:
            self.state = "LOST"
            self.lost_since_ms = now_ms
            self.log.append(("BMS_PROTOCOL_LOST", self.protocol))

    def tick_cooldowns(self, now_ms):
        if self.state in ("LOST", "IDLE_NO_BMS"):
            if now_ms - self.lost_since_ms >= REPROBE_COOLDOWN_MS:
                self.state = "PROBING"
                self.protocol = None
                self.lock_successes = 0

    def cross_check(self, shunt_a, bms_a):
        """Mirror of BatteryCommManager::crossCheckShunt."""
        if shunt_a is None or bms_a is None:
            self.mismatch_streak = 0
            self.last_mismatch = None
            self.mismatch_active = False
            return None
        delta = abs(bms_a - shunt_a)
        threshold = max(0.5, 0.05 * abs(shunt_a))
        if delta > threshold:
            if self.mismatch_streak < MISMATCH_SUSTAIN_POLLS:
                self.mismatch_streak += 1
            if self.mismatch_streak >= MISMATCH_SUSTAIN_POLLS:
                self.mismatch_active = True
        else:
            self.mismatch_streak = 0
            self.mismatch_active = False
        self.last_mismatch = delta
        return delta


def test_state_machine():
    m = ManagerMirror()
    # Single success must NOT lock (noise protection).
    m.probe_success()
    check("one success does not lock", m.state == "PROBING")
    # A failure resets the streak — hysteresis.
    m.probe_failure()
    check("failure resets lock streak", m.lock_successes == 0)
    # Two consecutive successes lock.
    m.probe_success()
    m.probe_success()
    check("two consecutive successes lock", m.state == "LOCKED")
    check("lock event logged", m.log[-1][0] == "BMS_PROTOCOL_LOCKED")
    # Two poll failures stay locked (tolerance), third loses.
    m.poll_failure(1000)
    m.poll_failure(2000)
    check("two failures stay locked", m.state == "LOCKED")
    m.poll_failure(3000)
    check("third failure → LOST", m.state == "LOST")
    check("loss event logged", m.log[-1][0] == "BMS_PROTOCOL_LOST")
    # Cooldown: not yet at 60 s.
    m.tick_cooldowns(3000 + REPROBE_COOLDOWN_MS - 1)
    check("no re-probe before cooldown", m.state == "LOST")
    # After cooldown → PROBING again, protocol cleared.
    m.tick_cooldowns(3000 + REPROBE_COOLDOWN_MS)
    check("re-probe after cooldown", m.state == "PROBING")
    check("protocol cleared on re-probe", m.protocol is None)


def test_cross_check():
    m = ManagerMirror()
    # Agreeing instruments → no alarm, small delta reported.
    d = m.cross_check(10.0, 10.2)
    check("agree: mismatch inactive", m.mismatch_active is False)
    check("agree: delta computed", abs(d - 0.2) < 1e-9)
    # Divergence below threshold (5% of 10 A = 0.5) → still fine.
    m.cross_check(10.0, 10.45)
    check("within tolerance", m.mismatch_active is False)
    # Large divergence: needs MISMATCH_SUSTAIN_POLLS consecutive polls.
    for _ in range(MISMATCH_SUSTAIN_POLLS - 1):
        m.cross_check(10.0, 30.0)
    check("divergence not yet active (sustain window)", m.mismatch_active is False)
    m.cross_check(10.0, 30.0)
    check("sustained divergence activates", m.mismatch_active is True)
    check("mismatch delta tracked", abs(m.last_mismatch - 20.0) < 1e-9)
    # One agreeing poll clears it.
    m.cross_check(10.0, 10.1)
    check("agreement clears mismatch", m.mismatch_active is False)
    # Wrong-sign convention scenario (L2-class error): BMS reports +30 while
    # the shunt reads -30 → |Δ|=60 A — caught within the sustain window.
    m2 = ManagerMirror()
    for _ in range(MISMATCH_SUSTAIN_POLLS):
        m2.cross_check(-30.0, 30.0)
    check("wrong sign constant caught by cross-check", m2.mismatch_active is True)
    # Invalid shunt input → returns None, resets state (no fabrication).
    m3 = ManagerMirror()
    m3.cross_check(10.0, 30.0)
    check("invalid input → None + reset", m3.cross_check(None, 30.0) is None
          and m3.mismatch_active is False)


# ============================================================================
# 5. SOC provenance cascade (firmware energyTask merge logic mirror)
# ============================================================================

def test_provenance_cascade():
    def provenance(bms_locked, bms_fresh, soc_plausible, shunt_valid, soc_from_ocv):
        if bms_locked and bms_fresh and soc_plausible:
            return "BMS_DIRECT"
        if shunt_valid:
            return "OCV_ESTIMATED" if soc_from_ocv else "SHUNT_COULOMB"
        return "UNKNOWN"

    check("BMS locked+fresh → BMS_DIRECT",
          provenance(True, True, True, True, False) == "BMS_DIRECT")
    check("BMS stale → shunt fallback",
          provenance(True, False, True, True, False) == "SHUNT_COULOMB")
    check("BMS implausible SOC → shunt fallback",
          provenance(True, True, False, True, False) == "SHUNT_COULOMB")
    check("no BMS, OCV basis → OCV_ESTIMATED",
          provenance(False, False, False, True, True) == "OCV_ESTIMATED")
    check("nothing valid → UNKNOWN (never fabricated)",
          provenance(False, False, False, False, False) == "UNKNOWN")


# ============================================================================
# 6. [2026-09 #4 closure] Plausibility-gate COVERAGE — static source asserts
# Every gated BmsData field must actually pass its gate at EVERY decode site;
# a transport must never smuggle an impossible value past a forgotten gate.
# ============================================================================
def test_gate_coverage():
    here = os.path.dirname(os.path.abspath(__file__))
    fw = os.path.join(here, "..", "firmware", "Comm")
    proto = open(os.path.join(fw, "BatteryProtocol.h"), encoding="utf-8").read()
    rtu = open(os.path.join(fw, "ModbusRtuClient.cpp"), encoding="utf-8").read()
    can = open(os.path.join(fw, "PylontechCanClient.cpp"), encoding="utf-8").read()

    gates = ["bmsSocPlausible", "bmsSohPlausible", "bmsVoltPlausible",
             "bmsCurrentPlausible", "bmsCellVPlausible", "bmsTempPlausible"]
    for g in gates:
        check(f"gate {g} defined in BatteryProtocol.h (single source)",
              ("inline bool " + g) in proto)
    check("Modbus RTU decode gates temperature via bmsTempPlausible",
          "bmsTempPlausible(t)" in rtu)
    check("Pylontech CAN decode gates temperature via bmsTempPlausible",
          "bmsTempPlausible(t)" in can)
    check("no inline duplicated temperature range check left in clients",
          "t > -40.0f" not in rtu and "t > -40.0f" not in can)
    # Every float field assignment in decodeRegisters is gate-wrapped.
    gated = ["out.voltage", "out.current", "out.soc", "out.soh",
             "out.cellVoltageMin", "out.cellVoltageMax", "out.temperature",
             "out.chargeCurrentLimit", "out.dischargeCurrentLimit"]
    for f in gated:
        m = re.search(re.escape(f) + r"\s*=", rtu)
        line_start = rtu.rfind("\n", 0, m.start()) + 1
        line = rtu[line_start:rtu.find("\n", m.end())]
        check(f"RTU decode assigns {f} only through a plausibility gate",
              re.search(r"if \(bms\w+Plausible", line) is not None, line.strip())
    check("cellCount bounded (0 < n < 256) in RTU decode",
          "cells > 0 && cells < 256" in rtu)
    check("moduleCount bounded (0 < n < 64) in CAN decode",
          re.search(r"d\[0\] > 0 && d\[0\] < 64", can) is not None)


if __name__ == "__main__":
    test_crc16()
    test_register_decode()
    test_pylontech_can()
    test_state_machine()
    test_cross_check()
    test_provenance_cascade()
    test_gate_coverage()
    print(f"\nBMS comm logic tests: {PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)
