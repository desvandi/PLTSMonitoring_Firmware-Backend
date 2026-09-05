#!/usr/bin/env python3
"""
decode_ina219_config_v2.py — CORRECT INA219 config register decoder per TI datasheet SBOS448G.

Bit-field layout (CORRECTED):
  Bit 15    : RST (1 = reset)
  Bit 14    : RESERVED (must be 0)
  Bit 13    : BRNG (0 = 16V FSR, 1 = 32V FSR)
  Bits 12:11: PG (PGA gain)
              00 = ±40mV (gain /1)
              01 = ±80mV (gain /2)
              10 = ±160mV (gain /4)
              11 = ±320mV (gain /8)
  Bits 10:7 : BADC[3:0] (bus ADC — 4-bit field)
  Bits 6:3  : SADC[3:0] (shunt ADC — 4-bit field)
  Bits 2:0  : MODE (3-bit field)

BADC/SADC encoding (4-bit, per Table 5):
  0000 = 9-bit, 1 sample (84µs)
  0001 = 10-bit, 1 sample (148µs)
  0010 = 11-bit, 1 sample (276µs)
  0011 = 12-bit, 1 sample (532µs)
  1000 = 12-bit, 1 sample (532µs, same as 0011)
  1001 = 12-bit, 2 samples (1.06ms)
  1010 = 12-bit, 4 samples (2.13ms)
  1011 = 12-bit, 8 samples (4.26ms)
  1100 = 12-bit, 16 samples (8.51ms)
  1101 = 12-bit, 32 samples (17.02ms)
  1110 = 12-bit, 64 samples (34.05ms)
  1111 = 12-bit, 128 samples (68.10ms)

MODE encoding (3-bit, per Table 6):
  000 = power-down
  001 = shunt voltage, triggered
  010 = bus voltage, triggered
  011 = shunt + bus, triggered
  100 = ADC off
  101 = shunt voltage, continuous
  110 = bus voltage, continuous
  111 = shunt + bus, continuous (default)

Datasheet reset value: 0x399F
  = 32V FSR, ±320mV, 12-bit/1-sample, shunt+bus continuous — matches documented defaults.
"""
import sys

PGA_MAP = {
    0b00: "±40mV (gain /1)",
    0b01: "±80mV (gain /2)",
    0b10: "±160mV (gain /4)",
    0b11: "±320mV (gain /8)",
}

ADC_MAP = {
    0b0000: ("9-bit", 1, "84µs"),
    0b0001: ("10-bit", 1, "148µs"),
    0b0010: ("11-bit", 1, "276µs"),
    0b0011: ("12-bit", 1, "532µs"),
    0b0100: ("9-bit", 1, "84µs"),      # ADC3 = don't-care when ADC4=0
    0b0101: ("10-bit", 1, "148µs"),
    0b0110: ("11-bit", 1, "276µs"),
    0b0111: ("12-bit", 1, "532µs"),
    0b1000: ("12-bit", 1, "532µs"),
    0b1001: ("12-bit", 2, "1.06ms"),
    0b1010: ("12-bit", 4, "2.13ms"),
    0b1011: ("12-bit", 8, "4.26ms"),
    0b1100: ("12-bit", 16, "8.51ms"),
    0b1101: ("12-bit", 32, "17.02ms"),
    0b1110: ("12-bit", 64, "34.05ms"),
    0b1111: ("12-bit", 128, "68.10ms"),
}

MODE_MAP = {
    0b000: "power-down",
    0b001: "shunt voltage, triggered",
    0b010: "bus voltage, triggered",
    0b011: "shunt + bus, triggered",
    0b100: "ADC off",
    0b101: "shunt voltage, continuous",
    0b110: "bus voltage, continuous",
    0b111: "shunt + bus, continuous (default)",
}


def decode(value, label=""):
    v = value & 0xFFFF
    print(f"\n{'='*70}")
    print(f"  {label} = 0x{v:04X} (binary: {v:016b})")
    print(f"{'='*70}")

    rst = (v >> 15) & 1
    reserved = (v >> 14) & 1
    brng = (v >> 13) & 1
    pga = (v >> 11) & 0b11
    badc = (v >> 7) & 0b1111
    sadc = (v >> 3) & 0b1111
    mode = v & 0b111

    print(f"  Bit 15    RST      = {rst}  ({'RESET' if rst else 'normal'})")
    print(f"  Bit 14    RESERVED = {reserved}  ({'⚠ NON-ZERO!' if reserved else 'ok'})")
    print(f"  Bit 13    BRNG     = {brng}  ({'32V FSR' if brng else '16V FSR'})")
    print(f"  Bits 12:11 PGA     = {pga:02b}  ({PGA_MAP[pga]})")
    res, samples, conv_time = ADC_MAP[badc]
    print(f"  Bits 10:7  BADC    = {badc:04b} ({badc:2d})  ({res}, {samples} samples, {conv_time})")
    res, samples, conv_time = ADC_MAP[sadc]
    print(f"  Bits 6:3   SADC    = {sadc:04b} ({sadc:2d})  ({res}, {samples} samples, {conv_time})")
    print(f"  Bits 2:0   MODE    = {mode:03b}  ({MODE_MAP[mode]})")

    issues = []
    if reserved:
        issues.append("Bit 14 (RESERVED) is non-zero — must be 0 per datasheet")
    if issues:
        print(f"\n  ⚠ ISSUES: {', '.join(issues)}")
    else:
        print(f"\n  ✅ All fields valid")


def main():
    print("INA219 Configuration Register Decoder v2 — CORRECTED per TI datasheet SBOS448G")
    print("=" * 70)

    # Datasheet reset value — sanity check
    print("\n--- Datasheet reset value (should match documented defaults) ---")
    decode(0x399F, "INA219 reset value (datasheet)")

    # The CORRECT values per auditor + datasheet
    print("\n--- CORRECT values (per auditor + datasheet) ---")
    decode(0x0FFF, "INA219_CONFIG_PGA_80MV (correct: ±80mV, 12b/128s, cont, 16V)")
    decode(0x17FF, "INA219_CONFIG_PGA_160MV (correct: ±160mV, 12b/128s, cont, 16V)")

    # The BUGGY v1.9.1 values
    print("\n--- BUGGY v1.9.1 values (wrong bit-field layout) ---")
    decode(0x152B, "INA219_CONFIG_PGA_80MV (buggy v1.9.1)")
    decode(0x252B, "INA219_CONFIG_PGA_160MV (buggy v1.9.1)")

    # The BUGGY v1.9.0 values
    print("\n--- BUGGY v1.9.0 values (inverted PGA mapping) ---")
    decode(0x3BFF, "INA219_CONFIG_PGA_80MV (buggy v1.9.0)")
    decode(0x39FF, "INA219_CONFIG_PGA_160MV (buggy v1.9.0)")
    decode(0x3FFB, "INA219_CONFIG_LEGACY (buggy v1.9.0)")

    if len(sys.argv) > 1:
        print("\n--- User-supplied values ---")
        for arg in sys.argv[1:]:
            decode(int(arg, 0), arg)


if __name__ == "__main__":
    main()
