#!/usr/bin/env python3
"""
test_ina219_config_registers.py — [v1.9.2 CANONICAL] Verify INA219 config
register constants match the TI datasheet SBOS448G.

This test uses the CORRECT bit-field layout:
  Bit 15    : RST
  Bit 14    : RESERVED (must be 0)
  Bit 13    : BRNG (0=16V, 1=32V)
  Bits 12:11: PG (00=±40mV, 01=±80mV, 10=±160mV, 11=±320mV)
  Bits 10:7 : BADC (4-bit ADC, 1111=12-bit/128-sample)
  Bits 6:3  : SADC (4-bit ADC, 1111=12-bit/128-sample)
  Bits 2:0  : MODE (111=shunt+bus continuous)

The v1.9.1 version of this test used the WRONG bit-field layout (BRNG=14,
PG=13:12, BADC=11:7, SADC=6:2, MODE=1:0) and therefore PASSED on the buggy
constants 0x152B/0x252B. This rewrite uses the correct layout + adds a
canonical cross-check: the datasheet reset value 0x399F must decode to the
documented defaults.

Usage: python3 scripts/test_ina219_config_registers.py
Exit: 0 = all constants valid, 1 = INVALID (blocker)
"""
import re
import sys
from pathlib import Path

# Expected PGA bit values (bits 12:11) per TI datasheet SBOS448G Table 4
PGA_EXPECTED = {
    "INA219_CONFIG_PGA_80MV":  0b01,   # ±80mV
    "INA219_CONFIG_PGA_160MV": 0b10,   # ±160mV
}

# Expected full register values (v1.9.2 canonical)
EXPECTED_VALUES = {
    "INA219_CONFIG_PGA_80MV":  0x0FFF,
    "INA219_CONFIG_PGA_160MV": 0x17FF,
}

# Datasheet reset value — must decode to documented defaults
DATASHEET_RESET = 0x399F
DATASHEET_RESET_DECODED = {
    "brng": 1,       # 32V FSR (default)
    "pga":  0b11,    # ±320mV (default, gain /8)
    "badc": 0b0011,  # 12-bit/1-sample (default)
    "sadc": 0b0011,  # 12-bit/1-sample (default)
    "mode": 0b111,   # shunt+bus continuous (default)
}

CONFIG_H = Path(__file__).parent.parent / "firmware" / "Core" / "Config.h"


def decode_register(value):
    """Decode INA219 config register into fields using CORRECT bit-field layout."""
    return {
        "rst":      (value >> 15) & 1,
        "reserved": (value >> 14) & 1,
        "brng":     (value >> 13) & 1,
        "pga":      (value >> 11) & 0b11,
        "badc":     (value >> 7) & 0b1111,
        "sadc":     (value >> 3) & 0b1111,
        "mode":     value & 0b111,
    }


def main():
    if not CONFIG_H.is_file():
        print(f"[FAIL] Config.h not found: {CONFIG_H}")
        return 1

    src = CONFIG_H.read_text()
    failures = []

    # --- CANONICAL CROSS-CHECK: datasheet reset value ---
    print("=" * 72)
    print("CANONICAL CROSS-CHECK: Datasheet reset value 0x399F")
    print("=" * 72)
    reset_fields = decode_register(DATASHEET_RESET)
    print(f"  0x{DATASHEET_RESET:04X} → BRNG={reset_fields['brng']}, PGA={reset_fields['pga']:02b}, "
          f"BADC={reset_fields['badc']:04b}, SADC={reset_fields['sadc']:04b}, MODE={reset_fields['mode']:03b}")
    if reset_fields != {**DATASHEET_RESET_DECODED, "rst": 0, "reserved": 0}:
        failures.append(f"Datasheet reset 0x{DATASHEET_RESET:04X} does NOT decode to documented defaults! "
                        f"Got {reset_fields}, expected {DATASHEET_RESET_DECODED}")
        print(f"  [FAIL] Reset value does NOT match documented defaults — bit-field layout is wrong!")
        print(f"         This means the decoder itself is broken. Fix decode_register() first.")
        return 1
    else:
        print(f"  [PASS] 0x399F = 32V/±320mV/12b-1s/cont — matches TI documented defaults")
        print(f"         (This confirms the bit-field layout is correct.)")

    # --- Verify PGA constants ---
    print()
    print("=" * 72)
    print("PGA CONSTANT VERIFICATION")
    print("=" * 72)

    for const_name, expected_pga in PGA_EXPECTED.items():
        m = re.search(rf"static constexpr uint16_t\s+{const_name}\s*=\s*(0x[0-9A-Fa-f]+)", src)
        if not m:
            failures.append(f"{const_name}: constant not found in Config.h")
            print(f"\n[FAIL] {const_name}: not found in Config.h")
            continue

        value = int(m.group(1), 16)
        fields = decode_register(value)

        print(f"\n[{const_name}] = 0x{value:04X}")
        print(f"  BRNG={fields['brng']} ({'32V' if fields['brng'] else '16V'}), "
              f"PGA={fields['pga']:02b} (expected {expected_pga:02b}), "
              f"BADC={fields['badc']:04b}, SADC={fields['sadc']:04b}, "
              f"MODE={fields['mode']:03b}, RST={fields['rst']}, RESERVED={fields['reserved']}")

        # Check exact value
        expected_val = EXPECTED_VALUES[const_name]
        if value != expected_val:
            failures.append(f"{const_name}: value 0x{value:04X} != expected 0x{expected_val:04X}")
            print(f"  [FAIL] Value mismatch: got 0x{value:04X}, expected 0x{expected_val:04X}")
        else:
            print(f"  [PASS] Exact value matches 0x{expected_val:04X}")

        # Check PGA bits
        if fields["pga"] != expected_pga:
            failures.append(f"{const_name}: PGA={fields['pga']} (expected {expected_pga})")
            print(f"  [FAIL] PGA mismatch: got {fields['pga']}, expected {expected_pga}")
        else:
            pga_ranges = {0: "±40mV", 1: "±80mV", 2: "±160mV", 3: "±320mV"}
            print(f"  [PASS] PGA = {pga_ranges[fields['pga']]}")

        # Check BADC = 1111 (12-bit/128-sample)
        if fields["badc"] != 0b1111:
            failures.append(f"{const_name}: BADC={fields['badc']:04b} (expected 1111 = 12-bit/128-sample)")
            print(f"  [FAIL] BADC={fields['badc']:04b} (expected 1111)")
        else:
            print(f"  [PASS] BADC=1111 (12-bit/128-sample)")

        # Check SADC = 1111 (12-bit/128-sample)
        if fields["sadc"] != 0b1111:
            failures.append(f"{const_name}: SADC={fields['sadc']:04b} (expected 1111 = 12-bit/128-sample)")
            print(f"  [FAIL] SADC={fields['sadc']:04b} (expected 1111)")
        else:
            print(f"  [PASS] SADC=1111 (12-bit/128-sample)")

        # Check MODE = 111 (continuous)
        if fields["mode"] != 0b111:
            failures.append(f"{const_name}: MODE={fields['mode']:03b} (expected 111 = continuous)")
            print(f"  [FAIL] MODE={fields['mode']:03b} (expected 111 — 011 is TRIGGERED, not continuous!)")
        else:
            print(f"  [PASS] MODE=111 (shunt+bus continuous)")

        # Check RST = 0
        if fields["rst"] != 0:
            failures.append(f"{const_name}: RST=1 (should be 0)")
            print(f"  [FAIL] RST=1 (should be 0)")

        # Check RESERVED bit 14 = 0
        if fields["reserved"] != 0:
            failures.append(f"{const_name}: bit 14 (RESERVED) = 1 (must be 0 per datasheet)")
            print(f"  [FAIL] bit 14 (RESERVED) = 1 (must be 0)")

    # --- Verify the two modes are DIFFERENT ---
    print()
    print("=" * 72)
    print("DYNAMIC SWITCHING SANITY CHECK")
    print("=" * 72)
    m80 = re.search(r"INA219_CONFIG_PGA_80MV\s*=\s*(0x[0-9A-Fa-f]+)", src)
    m160 = re.search(r"INA219_CONFIG_PGA_160MV\s*=\s*(0x[0-9A-Fa-f]+)", src)
    if m80 and m160:
        v80 = int(m80.group(1), 16)
        v160 = int(m160.group(1), 16)
        pga80 = decode_register(v80)["pga"]
        pga160 = decode_register(v160)["pga"]
        if v80 == v160:
            failures.append("PGA_80MV and PGA_160MV are the SAME value — dynamic switching is a no-op!")
            print(f"[FAIL] PGA_80MV (0x{v80:04X}) == PGA_160MV (0x{v160:04X}) — switching is a no-op!")
        elif pga80 == pga160:
            failures.append(f"PGA bits are the same ({pga80}) for both modes — switching is a no-op!")
            print(f"[FAIL] PGA bits identical ({pga80}) for both modes — switching is a no-op!")
        else:
            print(f"[PASS] PGA bits differ: 80mV mode PGA={pga80}, 160mV mode PGA={pga160}")
            print(f"       Dynamic switching will actually change the gain range.")

    print()
    if failures:
        print("=" * 72)
        print("INA219 CONFIG REGISTER TEST = FAILED")
        print("=" * 72)
        for f in failures:
            print(f"  - {f}")
        return 1
    print("=" * 72)
    print("INA219 CONFIG REGISTER TEST = PASS")
    print("=" * 72)
    print("  Datasheet reset value 0x399F decodes correctly (layout verified).")
    print("  All PGA constants have correct bit-fields per TI datasheet SBOS448G.")
    print("  BADC/SADC = 1111 (12-bit/128-sample). MODE = 111 (continuous).")
    print("  The two PGA modes use different PGA bits (dynamic switching works).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
