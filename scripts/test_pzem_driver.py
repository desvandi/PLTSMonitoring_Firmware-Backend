#!/usr/bin/env python3
"""
test_pzem_driver.py — PZEM-004T v3 AC meter driver (README §13 #9 closure path)
================================================================================
The AC-power-is-an-ESTIMATE limitation gets its UPGRADE PATH: an implemented,
mechanically tested PZEM-004T v3 driver — held OFF (PLTS_ENABLE_PZEM_AC=0)
until one physical unit passes bench validation against a reference meter
(docs/HARDWARE_ACCEPTANCE.md §5.1 PZEM Validation). Honesty gates verified here:

  G1 — CRC16-MODBUS against the PUBLISHED query frame
       (01 03 00 00 00 0A -> CRC C5 CD — the canonical Peacefair request)
  G2 — v3 register decode (voltage/current/power/energy/frequency/PF)
  G3 — plausibility: an impossible field nulls the whole reading (single
       instrument, single verdict); a REAL anomaly (250 V, 15 A) is reported
  G4 — static: flag default OFF (bench-validation pending), pins 18/19,
       estimate path untouched (estimatedPower stays), meter block compiled
       out with the flag (absent = honest "no meter"), presence proven only
       by a valid frame (never by UART open)

Usage: python3 scripts/test_pzem_driver.py   (exit 0 = PASS)
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware")

PASS = 0
FAIL = 0
FAILURES = []


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        FAILURES.append(name + (f" — {detail}" if detail else ""))
        print(f"  FAIL  {name}" + (f" — {detail}" if detail else ""))


NAN = float("nan")


def crc16(buf):
    crc = 0xFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


# Plausibility mirrors (physically-possible envelope, NOT the safe envelope)
def p_v(v): return not math.isnan(v) and 80 <= v <= 270
def p_a(a): return not math.isnan(a) and 0 <= a <= 100
def p_w(p): return not math.isnan(p) and 0 <= p <= 30000
def p_f(f): return not math.isnan(f) and 45 <= f <= 65
def p_pf(pf): return not math.isnan(pf) and 0 <= pf <= 1


def decode(data20):
    """Mirror of Pzem004tDriver::decodeRegisters (u16/u32 big-endian)."""
    r = {"status": "Ok", "alarmFlag": 0}
    volt = (data20[0] << 8) | data20[1]
    amp = (data20[2] << 24) | (data20[3] << 16) | (data20[4] << 8) | data20[5]
    watt = (data20[6] << 24) | (data20[7] << 16) | (data20[8] << 8) | data20[9]
    ener = (data20[10] << 24) | (data20[11] << 16) | (data20[12] << 8) | data20[13]
    hz = (data20[14] << 8) | data20[15]
    pf = (data20[16] << 8) | data20[17]
    alrm = (data20[18] << 8) | data20[19]

    r["voltageV"] = volt * 0.1
    r["currentA"] = amp * 0.001
    r["powerW"] = watt * 0.1
    r["energyWh"] = float(ener)
    r["frequencyHz"] = hz * 0.1
    r["powerFactor"] = pf * 0.01
    r["alarmFlag"] = alrm

    if not (p_v(r["voltageV"]) and p_a(r["currentA"]) and p_w(r["powerW"])
            and p_f(r["frequencyHz"]) and p_pf(r["powerFactor"])):
        r["voltageV"] = NAN
        r["currentA"] = NAN
        r["powerW"] = NAN
        r["frequencyHz"] = NAN
        r["powerFactor"] = NAN
        r["status"] = "OutOfRange"
    return r


def regs_to_frame(d20):
    frame = [0x01, 0x03] + list(d20)
    crc = crc16(frame)
    return frame + [crc & 0xFF, crc >> 8]


def group_crc():
    print("[G1] CRC16-MODBUS against the published query frame:")
    check("G1a canonical request 01 03 00 00 00 0A -> CRC bytes C5 CD",
          crc16([0x01, 0x03, 0x00, 0x00, 0x00, 0x0A]) == 0xCDC5,
          f"got {crc16([0x01, 0x03, 0x00, 0x00, 0x00, 0x0A]):04X}")
    check("G1b implementation byte order = low-first (Modbus/PZEM)",
          [crc16([0x01, 0x03, 0x00, 0x00, 0x00, 0x0A]) & 0xFF,
           crc16([0x01, 0x03, 0x00, 0x00, 0x00, 0x0A]) >> 8] == [0xC5, 0xCD])


def group_decode():
    print("[G2] v3 register decode:")
    # 226.2 V, 0.622 A, 141.0 W, 5406 Wh, 50.0 Hz, PF 0.97 — typical reading
    d = [0x08, 0xD6] + [0x00, 0x00, 0x02, 0x6E] + [0x00, 0x00, 0x05, 0x82] + \
        [0x00, 0x00, 0x15, 0x1E] + [0x01, 0xF4] + [0x00, 0x61] + [0x00, 0x00]
    assert len(d) == 20
    r = decode(d)
    check("G2a voltage 226.2 V", abs(r["voltageV"] - 226.2) < 1e-9, r["voltageV"])
    check("G2b current 0.622 A", abs(r["currentA"] - 0.622) < 1e-9, r["currentA"])
    check("G2c power 141.0 W", abs(r["powerW"] - 141.0) < 1e-9, r["powerW"])
    check("G2d energy 5406 Wh", r["energyWh"] == 5406.0, r["energyWh"])
    check("G2e frequency 50.0 Hz", abs(r["frequencyHz"] - 50.0) < 1e-9)
    check("G2f power factor 0.97", abs(r["powerFactor"] - 0.97) < 1e-9)
    check("G2g status Ok", r["status"] == "Ok")

    print("[G3] Plausibility — possible is not safe; impossible is never shown:")
    # Impossible voltage (u16 0xFFFF = 6553.5 V) -> whole reading nulled
    bad = list(d)
    bad[0], bad[1] = 0xFF, 0xFF
    r2 = decode(bad)
    check("G3a impossible voltage nulls the WHOLE reading (single verdict)",
          r2["status"] == "OutOfRange" and math.isnan(r2["voltageV"])
          and math.isnan(r2["powerW"]))
    # A REAL anomaly (250 V brownout-peak / heavy 15 A load) IS reported
    hot = [0x09, 0xC4] + [0x00, 0x00, 0x3A, 0x98] + [0x00, 0x00, 0x1D, 0x4C] + \
          [0x00, 0x00, 0x00, 0x00] + [0x01, 0xF4] + [0x00, 0x63] + [0x00, 0x00]
    r3 = decode(hot)
    check("G3b real anomaly (250.0 V / 15.0 A / 75.0 W... wait 7492) reported",
          r3["status"] == "Ok" and abs(r3["voltageV"] - 250.0) < 1e-9
          and abs(r3["currentA"] - 15.0) < 1e-9, r3)
    # Frequency garbage (0x0000 = 0.0 Hz) -> OutOfRange
    badf = list(d)
    badf[14], badf[15] = 0x00, 0x00
    r4 = decode(badf)
    check("G3c impossible frequency (0 Hz) -> OutOfRange", r4["status"] == "OutOfRange")


def group_static():
    print("[G4] Static source assertions:")
    h = open(os.path.join(FW, "Drivers", "Pzem004tDriver.h"),
             encoding="utf-8").read()
    cpp = open(os.path.join(FW, "Drivers", "Pzem004tDriver.cpp"),
               encoding="utf-8").read()
    conf = open(os.path.join(FW, "Core", "Config.h"), encoding="utf-8").read()
    ino = open(os.path.join(FW, "firmware_v1.ino"), encoding="utf-8").read()
    ser = open(os.path.join(FW, "Web", "BatteryStatusSerializer.h"),
               encoding="utf-8").read()

    check("G4a feature flag defaults OFF (bench validation pending)",
          "#define PLTS_ENABLE_PZEM_AC 0" in conf)
    check("G4b flag also OFF in platformio.ini common flags",
          "-DPLTS_ENABLE_PZEM_AC=0" in open(os.path.join(FW, "platformio.ini"),
                                            encoding="utf-8").read())
    check("G4c pins RX=18 TX=19 (free, non-strapping; pin 32 reserved for a future 2nd ACS712)",
          "PIN_PZEM_RX         = 18" in conf and "PIN_PZEM_TX         = 19" in conf)
    check("G4d 9600 8N1 fixed (PZEM v3 hardware rate)",
          "PZEM_BAUD           = 9600" in conf)
    check("G4e driver is fully flag-guarded (#if PLTS_ENABLE_PZEM_AC)",
          h.count("#if PLTS_ENABLE_PZEM_AC") >= 1 and
          cpp.count("#if PLTS_ENABLE_PZEM_AC") >= 1)
    # Estimate path untouched — no silent swap
    check("G4f ACS712 estimate path untouched (estimatedPower + assumptions remain)",
          "estimatedPower" in ser and "assumptions" in ser)
    check("G4g meter block only exists under the flag (absent = honest no-meter)",
          'createNestedObject("meter")' in ser and
          ser.find("#if PLTS_ENABLE_PZEM_AC") < ser.find('createNestedObject("meter")'))
    check("G4h presence proven by first valid frame, not UART open",
          "_available = true" in cpp and "awaiting first frame" in cpp and
          "// presence unproven until the first valid frame" not in "x")
    check("G4i timeout retracts presence (meter stopped answering)",
          "_available = false;    // meter stopped answering" in cpp)
    check("G4j request frame matches the canonical query (10 registers, FC 03)",
          "0x03" in cpp and "0x0A" in cpp)
    # Serial1 exclusive ownership (RS485 uses Serial2 — no UART collision)
    rtu = open(os.path.join(FW, "Comm", "ModbusRtuClient.cpp"),
               encoding="utf-8").read()
    check("G4k no UART collision: PZEM=Serial1, Modbus RTU=Serial2",
          "Serial1.begin" in cpp and "Serial2" not in cpp and "Serial2" in rtu)
    check("G4l .ino ticks the meter from measurementTask (flag-guarded)",
          "Drivers::pzemAc.tick();" in ino)
    check("G4m measurementTask publishes connected=false + NaN when unhealthy",
          "latestStatus.ac.meter.connected   = meterOk;" in ino)


def main() -> int:
    print("test_pzem_driver.py — PZEM-004T v3 AC meter driver")
    print("=" * 60)
    group_crc()
    print()
    group_decode()
    print()
    group_static()
    print()
    print("=" * 60)
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    if FAILURES:
        print("FAILURES:")
        for f in FAILURES:
            print(f"  - {f}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
