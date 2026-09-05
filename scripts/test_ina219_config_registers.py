#!/usr/bin/env python3
"""
test_ina219_config_registers.py — [v1.9.1 / INA-01] Verify INA219 config
register constants match the TI datasheet (SBOS397D).

This test prevents the v1.9.0 bug from recurring: register constants had
inverted PGA bit-mapping, causing both "±80mV" and "±160mV" modes to
actually use ±320mV.

The test reads the constants from firmware/Core/Config.h and decodes each
bit-field, asserting:
  1. PGA bits (13:12) match the expected range for each mode
  2. BADC/SADC values are NOT in the reserved range (01011..11111)
  3. MODE bits = 11 (shunt+bus continuous)
  4. RST bit = 0 (not reset)

Usage: python3 scripts/test_ina219_config_registers.py
Exit: 0 = all constants valid, 1 = INVALID (blocker)
"""
import re
import sys
from pathlib import Path

# Expected PGA bit values (bits 13:12) per TI datasheet
PGA_EXPECTED = {
    "INA219_CONFIG_PGA_80MV":  0b01,   # ±80mV
    "INA219_CONFIG_PGA_160MV": 0b10,   # ±160mV
}

# Reserved ADC range: 01011 (11) through 11111 (31)
ADC_RESERVED_MIN = 0b01011  # 11
ADC_RESERVED_MAX = 0b11111  # 31

CONFIG_H = Path(__file__).parent.parent / "firmware" / "Core" / "Config.h"


def decode_register(value):
    """Decode INA219 config register into fields."""
    return {
        "rst":   (value >> 15) & 1,
        "brng":  (value >> 14) & 1,
        "pga":   (value >> 12) & 0b11,
        "badc":  (value >> 7) & 0b11111,
        "sadc":  (value >> 2) & 0b11111,
        "mode":  value & 0b11,
    }


def main():
    if not CONFIG_H.is_file():
        print(f"[FAIL] Config.h not found: {CONFIG_H}")
        return 1

    src = CONFIG_H.read_text()
    failures = []

    for const_name, expected_pga in PGA_EXPECTED.items():
        # Find the constant definition
        m = re.search(rf"static constexpr uint16_t\s+{const_name}\s*=\s*(0x[0-9A-Fa-f]+)", src)
        if not m:
            failures.append(f"{const_name}: constant not found in Config.h")
            print(f"[FAIL] {const_name}: not found in Config.h")
            continue

        value = int(m.group(1), 16)
        fields = decode_register(value)

        print(f"\n[{const_name}] = 0x{value:04X}")
        print(f"  PGA={fields['pga']:02b} (expected {expected_pga:02b}), "
              f"BADC={fields['badc']:05b}, SADC={fields['sadc']:05b}, "
              f"MODE={fields['mode']:02b}, RST={fields['rst']}, BRNG={fields['brng']}")

        # Check PGA
        if fields["pga"] != expected_pga:
            failures.append(f"{const_name}: PGA={fields['pga']} (expected {expected_pga})")
            print(f"  [FAIL] PGA mismatch: got {fields['pga']}, expected {expected_pga}")
        else:
            pga_ranges = {0: "±40mV", 1: "±80mV", 2: "±160mV", 3: "±320mV"}
            print(f"  [PASS] PGA = {pga_ranges[fields['pga']]}")

        # Check BADC not reserved
        if ADC_RESERVED_MIN <= fields["badc"] <= ADC_RESERVED_MAX:
            failures.append(f"{const_name}: BADC={fields['badc']} is RESERVED")
            print(f"  [FAIL] BADC={fields['badc']} is RESERVED (must be 0-10)")
        else:
            print(f"  [PASS] BADC={fields['badc']} is valid")

        # Check SADC not reserved
        if ADC_RESERVED_MIN <= fields["sadc"] <= ADC_RESERVED_MAX:
            failures.append(f"{const_name}: SADC={fields['sadc']} is RESERVED")
            print(f"  [FAIL] SADC={fields['sadc']} is RESERVED (must be 0-10)")
        else:
            print(f"  [PASS] SADC={fields['sadc']} is valid")

        # Check MODE = 11 (continuous)
        if fields["mode"] != 0b11:
            failures.append(f"{const_name}: MODE={fields['mode']} (expected 11 = continuous)")
            print(f"  [FAIL] MODE={fields['mode']} (expected 11)")
        else:
            print(f"  [PASS] MODE=11 (shunt+bus continuous)")

        # Check RST = 0
        if fields["rst"] != 0:
            failures.append(f"{const_name}: RST=1 (should be 0)")
            print(f"  [FAIL] RST=1 (should be 0)")

    # Also verify the two modes are DIFFERENT (the v1.9.0 bug had both = ±320mV)
    m80 = re.search(r"INA219_CONFIG_PGA_80MV\s*=\s*(0x[0-9A-Fa-f]+)", src)
    m160 = re.search(r"INA219_CONFIG_PGA_160MV\s*=\s*(0x[0-9A-Fa-f]+)", src)
    if m80 and m160:
        v80 = int(m80.group(1), 16)
        v160 = int(m160.group(1), 16)
        if v80 == v160:
            failures.append("PGA_80MV and PGA_160MV are the SAME value — dynamic switching is a no-op!")
            print(f"\n[FAIL] PGA_80MV (0x{v80:04X}) == PGA_160MV (0x{v160:04X}) — switching is a no-op!")
        else:
            pga80 = decode_register(v80)["pga"]
            pga160 = decode_register(v160)["pga"]
            if pga80 == pga160:
                failures.append(f"PGA bits are the same ({pga80}) for both modes — switching is a no-op!")
                print(f"\n[FAIL] PGA bits identical ({pga80}) for both modes — switching is a no-op!")
            else:
                print(f"\n[PASS] PGA bits differ: 80mV={pga80}, 160mV={pga160} — switching works")

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
    print("  All PGA constants have correct bit-fields per TI datasheet SBOS397D.")
    print("  BADC/SADC values are valid (not RESERVED).")
    print("  The two PGA modes use different PGA bits (dynamic switching works).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
