#!/usr/bin/env python3
"""
decode_ina219_config.py — Decode INA219 configuration register bit-fields.

Verifies register values against the TI INA219 datasheet (SBOS397D).

Register layout (16 bits):
  Bit 15    : RST (1 = reset)
  Bit 14    : BRNG (0 = 16V FSR, 1 = 32V FSR)
  Bits 13:12: PGA (shunt voltage gain)
              00 = ±40mV (gain /1)
              01 = ±80mV (gain /2)
              10 = ±160mV (gain /4)
              11 = ±320mV (gain /8)
  Bits 11:7 : BADC[4:0] (bus ADC resolution + averaging)
              00000 = 9-bit, 1 sample (84µs)
              00001 = 10-bit, 1 sample (148µs)
              00010 = 11-bit, 1 sample (276µs)
              00011 = 12-bit, 1 sample (532µs)
              00100 = 12-bit, 2 samples (1.06ms)
              00101 = 12-bit, 4 samples (2.13ms)
              00110 = 12-bit, 8 samples (4.26ms)
              00111 = 12-bit, 16 samples (8.51ms)
              01000 = 12-bit, 32 samples (17.02ms)
              01001 = 12-bit, 64 samples (34.05ms)
              01010 = 12-bit, 128 samples (68.10ms)
              01011-11111 = RESERVED
  Bits 6:2  : SADC[4:0] (shunt ADC — same encoding as BADC)
  Bits 1:0  : MODE
              00 = power-down
              01 = shunt voltage, triggered
              10 = bus voltage, triggered
              11 = shunt + bus, continuous

Usage: python3 decode_ina219_config.py 0x3BFF 0x39FF 0x3FFB
"""
import sys

PGA_MAP = {
    0b00: ("±40mV", "/1"),
    0b01: ("±80mV", "/2"),
    0b10: ("±160mV", "/4"),
    0b11: ("±320mV", "/8"),
}

ADC_MAP = {
    0b00000: ("9-bit", 1, "84µs"),
    0b00001: ("10-bit", 1, "148µs"),
    0b00010: ("11-bit", 1, "276µs"),
    0b00011: ("12-bit", 1, "532µs"),
    0b00100: ("12-bit", 2, "1.06ms"),
    0b00101: ("12-bit", 4, "2.13ms"),
    0b00110: ("12-bit", 8, "4.26ms"),
    0b00111: ("12-bit", 16, "8.51ms"),
    0b01000: ("12-bit", 32, "17.02ms"),
    0b01001: ("12-bit", 64, "34.05ms"),
    0b01010: ("12-bit", 128, "68.10ms"),
}

MODE_MAP = {
    0b00: "power-down",
    0b01: "shunt voltage, triggered",
    0b10: "bus voltage, triggered",
    0b11: "shunt + bus, continuous",
}


def decode(value, label=""):
    v = value & 0xFFFF
    print(f"\n{'='*60}")
    print(f"  {label} = 0x{v:04X} (binary: {v:016b})")
    print(f"{'='*60}")

    rst = (v >> 15) & 1
    brng = (v >> 14) & 1
    pga = (v >> 12) & 0b11
    badc = (v >> 7) & 0b11111
    sadc = (v >> 2) & 0b11111
    mode = v & 0b11

    print(f"  Bit 15    RST   = {rst}  ({'RESET' if rst else 'normal'})")
    print(f"  Bit 14    BRNG  = {brng}  ({'32V FSR' if brng else '16V FSR'})")

    pga_range, pga_gain = PGA_MAP[pga]
    print(f"  Bits 13:12 PGA  = {pga:02b}  ({pga_range}, gain {pga_gain})")

    if badc in ADC_MAP:
        res, samples, conv_time = ADC_MAP[badc]
        print(f"  Bits 11:7  BADC = {badc:05b} ({badc:2d})  ({res}, {samples} samples, {conv_time})")
    else:
        print(f"  Bits 11:7  BADC = {badc:05b} ({badc:2d})  ⚠ RESERVED!")

    if sadc in ADC_MAP:
        res, samples, conv_time = ADC_MAP[sadc]
        print(f"  Bits 6:2   SADC = {sadc:05b} ({sadc:2d})  ({res}, {samples} samples, {conv_time})")
    else:
        print(f"  Bits 6:2   SADC = {sadc:05b} ({sadc:2d})  ⚠ RESERVED!")

    print(f"  Bits 1:0   MODE = {mode:02b}  ({MODE_MAP[mode]})")

    # Validation
    issues = []
    if badc not in ADC_MAP:
        issues.append(f"BADC={badc} is RESERVED")
    if sadc not in ADC_MAP:
        issues.append(f"SADC={sadc} is RESERVED")
    if issues:
        print(f"\n  ⚠ ISSUES: {', '.join(issues)}")
    else:
        print(f"\n  ✅ All fields valid")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 decode_ina219_config.py 0x3BFF 0x39FF 0x3FFB [0x152B] [0x252B]")
        # Decode the current (buggy) values + proposed fixes
        print("\n--- CURRENT (buggy) values from Config.h ---")
        decode(0x3BFF, "INA219_CONFIG_PGA_80MV (current)")
        decode(0x39FF, "INA219_CONFIG_PGA_160MV (current)")
        decode(0x3FFB, "INA219_CONFIG_LEGACY (current)")
        print("\n--- PROPOSED CORRECT values ---")
        decode(0x152B, "INA219_CONFIG_PGA_80MV (corrected)")
        decode(0x252B, "INA219_CONFIG_PGA_160MV (corrected)")
        decode(0x152B, "INA219_CONFIG_LEGACY (corrected, ±80mV default)")
        return

    for arg in sys.argv[1:]:
        val = int(arg, 0)
        decode(val, arg)


if __name__ == "__main__":
    main()
