# GPIO + Hardware Contract — 8-Channel Relay (PCF8574)

> **PHASE C output — hardware contract before any relay code is written.**

## 1. Hardware Interface: PCF8574 I²C Expander

**Decision:** 8-channel relay via PCF8574 I²C port expander.

**Rationale:** Only 3 free safe GPIOs on ESP32 (13, 23, 33) — insufficient
for 8 direct relays. PCF8574 adds 8 channels on the existing I²C bus
(SDA=21, SCL=22) with zero GPIO overlap.

**I²C bus:** Shared with INA219 (0x40), SHT31 (0x44), DS3231 RTC (0x68).
PCF8574 address range: 0x20-0x27 (set by A0/A1/A2 jumpers on module).
Default: 0x20 (all jumpers to GND).

## 2. Channel → PCF8574 Port Map

| Channel | PCF8574 Port | Relay Module Terminal | Active Level | Boot State | Safe State |
|---------|-------------|----------------------|--------------|------------|------------|
| 0 | P0 | IN1 | LOW (active-LOW optocoupler) | OFF | OFF |
| 1 | P1 | IN2 | LOW | OFF | OFF |
| 2 | P2 | IN3 | LOW | OFF | OFF |
| 3 | P3 | IN4 | LOW | OFF | OFF |
| 4 | P4 | IN5 | LOW | OFF | OFF |
| 5 | P5 | IN6 | LOW | OFF | OFF |
| 6 | P6 | IN7 | LOW | OFF | OFF |
| 7 | P7 | IN8 | LOW | OFF | OFF |

**PCF8574 output register at boot:** 0xFF (all HIGH) = all relays OFF
(active-LOW: HIGH = OFF). This is the fail-safe power-on state.

## 3. ESP32 GPIO Usage (NO new GPIO needed)

| ESP32 GPIO | Role | Already Used By |
|------------|------|-----------------|
| 21 (SDA) | I²C data | INA219, SHT31, DS3231 — **SHARED** with PCF8574 |
| 22 (SCL) | I²C clock | INA219, SHT31, DS3231 — **SHARED** with PCF8574 |

**No new ESP32 GPIO pins are consumed.** The PCF8574 is a slave on the
existing I²C bus. Address 0x20 does not conflict with any existing device.

## 4. Compile-Time + Runtime Validation

### 4.1 Compile-time constants (`Core/Config.h`)

```cpp
#if PLTS_ENABLE_RELAYS
  #define RELAY_CHANNEL_COUNT     8
  #define PCF8574_I2C_ADDRESS     0x20   // A0=A1=A2=GND
  #define PCF8574_I2C_ADDRESS_MIN 0x20
  #define PCF8574_I2C_ADDRESS_MAX 0x27
  // Active-LOW: relay ON when PCF8574 port = LOW (0)
  #define RELAY_ACTIVE_LOW        1
  // Boot safe state: ALL OFF
  #define RELAY_BOOT_SAFE_STATE   0x00   // all OFF
  // PCF8574 power-on default: 0xFF (all HIGH = all OFF for active-LOW)
  #define PCF8574_POWER_ON_STATE  0xFF
#endif
```

### 4.2 Runtime validation (`RelayExpanderDriver::begin()`)

```
1. Check I²C address in [0x20, 0x27] → FAIL if out of range
2. Check channel count == 8 → FAIL if not
3. Scan I²C bus for PCF8574 at configured address → FAIL if not found
4. Write 0xFF (all OFF) to PCF8574 output register → FAIL if write error
5. Read back PCF8574 input register → verify 0xFF (all OFF confirmed)
6. If any step fails: set _available = false, raise RELAY_FAULT alarm
```

### 4.3 Reserved GPIO protection

The relay driver does NOT use any ESP32 GPIO directly. It only uses I²C
(which is already initialized for sensors). The following ESP32 GPIOs are
**RESERVED and MUST NOT be used by relay**:

| GPIO | Reserved For | Reason |
|------|-------------|--------|
| 2 | E-WAVE LED | EmergencySupervisor |
| 4 | RS485 DE | ModbusRtuClient |
| 14 | E-WAVE E-stop | EmergencyRelayDriver |
| 16/17 | RS485 UART2 | ModbusRtuClient |
| 18/19 | PZEM UART1 | Pzem004tDriver (optional) |
| 25/26 | CAN TWAI | PylontechCanClient |
| 27 | E-WAVE relay | EmergencyRelayDriver |
| 32 | Future genset ACS712 | Reserved |
| 34/35 | ADC (input-only) | Battery/ACS712 sensors |

The `RelayExpanderDriver` does NOT call `pinMode()` or `digitalWrite()` on
ANY ESP32 GPIO. It only communicates via `Wire.beginTransmission(addr)` /
`Wire.write()` / `Wire.endTransmission()`.

## 5. Boot Glitch Prevention

**PCF8574 power-on state:** 0xFF (all outputs HIGH) = all relays OFF
(active-LOW module). This is hardware-guaranteed by the PCF8574 datasheet.

**Firmware boot sequence:**
1. `Wire.begin()` (already called in `setup()` for sensors)
2. `RelayExpanderDriver::begin()`:
   a. Write 0xFF to PCF8574 output register (re-assert all OFF)
   b. Read back to verify
   c. If mismatch → raise alarm, set `_available = false`
3. `RelayController::begin()`:
   a. Load NVS config (`plts_relays`)
   b. Apply boot policy (BootOff default → all stay OFF)
   c. Restore lockout states (TRIPPED channels stay locked)

**No relay glitch ON during boot** — PCF8574 hardware guarantees 0xFF at
power-on, and firmware re-asserts before any other init.

## 6. I²C Bus Considerations

**Bus loading:** PCF8574 adds 1 device to the I²C bus (total: 4 devices).
At 100 kHz (`I2C_FREQUENCY=100000`), bus capacity is well within spec.

**Pull-up resistors:** PCF8574 module typically includes 10kΩ pull-ups.
If bus errors occur, verify pull-up value (should be 4.7kΩ-10kΩ total
parallel resistance with existing module pull-ups).

**Interrupt pin (optional):** PCF8574 has an INT pin (active-LOW) that can
signal input changes. NOT used in v1 — we don't have physical feedback
inputs. Reserved for future expansion (aux contact readback).

## 7. Relay Module Electrical Spec

| Parameter | Typical 8-CH Optocoupler Relay Module |
|-----------|--------------------------------------|
| Relay type | SRD-05VDC-SL-C (or equivalent) |
| Contact rating | 10A 250VAC / 10A 30VDC |
| Coil voltage | 5V DC |
| Coil current | ~70mA per channel |
| Optocoupler | EL817 (or equivalent) |
| Active level | LOW (LOW = relay ON) |
| Isolation | Optocoupler (galvanic isolation) |
| Flyback diode | Built-in on module (across relay coil) |
| Power | VCC=5V, GND (separate from ESP32 3.3V) |

**IMPORTANT:** Relay module VCC must be 5V (NOT 3.3V — ESP32 GPIO is 3.3V
but PCF8574 can run at 5V VCC, with I²C pulled up to 3.3V for ESP32
compatibility). Verify level shifting on SDA/SCL if running PCF8574 at 5V.
