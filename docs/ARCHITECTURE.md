# Architecture — PLTS Monitoring System

> **Consolidated from:** `RELAY_INTEGRATION_ARCHITECTURE.md` + `RELAY_GPIO_HARDWARE_CONTRACT.md`
> **Applies to:** Firmware v1.8.0+ (relay subsystem) through v1.9.2 (current)

---

## Daftar Isi

1. [Arsitektur Firmware (ESP32)](#1-arsitektur-firmware-esp32)
2. [Arsitektur PWA (Next.js)](#2-arsitektur-pwa-nextjs)
3. [Arsitektur Backend (Google Apps Script)](#3-arsitektur-backend-google-apps-script)
4. [Integrasi Relay 8-Channel](#4-integrasi-relay-8-channel)
5. [Hardware Interface: PCF8574 I²C Expander](#5-hardware-interface-pcf8574-i²c-expander)
6. [Relay Control Path](#6-relay-control-path)
7. [Safety Boundary](#7-safety-boundary)
8. [Persistence Boundary](#8-persistence-boundary)
9. [Network Boundary](#9-network-boundary)
10. [Command Model (Canonical)](#10-command-model-canonical)
11. [Source + Priority](#11-source--priority)
12. [Authorization](#12-authorization)
13. [Compatibility Matrix](#13-compatibility-matrix)
14. [Build Flag](#14-build-flag)
15. [Electrical Safety](#15-electrical-safety)

---

## 1. Arsitektur Firmware (ESP32)

### FreeRTOS Tasks (11)

sensor(5Hz), measure(5Hz), energy(1Hz), telemetry(5s), network(100ms), persist(5min), health(1Hz), ota(2Hz), bmscomm(100ms), emg(10Hz), gasemg(15s poll). Semua WDT-subscribed.

### GPIO Usage (Definitive)

| GPIO | Role | Owner |
|------|------|-------|
| 2 | E-WAVE LED (OUTPUT) | EmergencySupervisor |
| 4 | RS485 DE (OUTPUT) | ModbusRtuClient |
| 14 | E-WAVE E-stop (INPUT_PULLUP) | EmergencyRelayDriver |
| 16/17 | RS485 TX/RX (UART2) | ModbusRtuClient |
| 18/19 | PZEM RX/TX (UART1, optional) | Pzem004tDriver |
| 21/22 | I²C SDA/SCL | INA219 + SHT31 + DS3231 + PCF8574 |
| 25/26 | CAN TX/RX (TWAI) | PylontechCanClient |
| 27 | E-WAVE relay (OUTPUT, active-LOW) | EmergencyRelayDriver |
| 32 | Reserved for future genset ACS712 | — |
| 34/35 | Battery ADC / ACS712 ADC (input-only) | AdcVoltageDriver / Acs712Driver |

**Free safe GPIOs:** 13, 23, 33 (only 3 — NOT enough for 8 direct relays).

### Existing Actuator

E-WAVE emergency relay (GPIO 27, active-LOW, fail-safe). Ini adalah **SAFETY INTERLOCK**, BUKAN aktuator general-purpose. TIDAK bisa diperluas ke 8 channel (binary state machine, single relay, safety-isolated).

### Command Pipeline (Existing, Reusable)

```
REST/MQTT → auth → CSRF → freshness → CommandCanonicalizer (whitelist+hash)
→ TransactionJournal (decide: NEW/DUPLICATE/CONFLICT) → apply → journal ACK
```

- **TransactionJournal:** 16-slot NVS ring, 2-phase commit, CRC32, survives reboot.
- **CommandCanonicalizer:** whitelist (fail-closed), SHA-256 hash, cross-transport dedup (REST + MQTT produce identical hash for same command).

---

## 2. Arsitektur PWA (Next.js)

**Stack:** Next.js 16, React 19, TanStack Query v5, Zustand, mqtt.js, Serwist.

**API split:** `deviceApi.ts` (ESP32 REST) + `backendApi.ts` (GAS aggregation) + `apiShared.ts` (CSRF, requestId). `api.ts` is backward-compat façade.

**MQTT:** subscribe-only (status/log/online). NO publish path — commands go via REST.

**Auth:** JWT cookie + CSRF double-submit + JTI revocation. Role: viewer | operator.

**Emergency control (E-WAVE):** browser POSTs directly to GAS Web App URL (admin_token in body). NO requestId. NO CSRF. This is a SEPARATE command path from deviceApi REST mutations.

**Compatibility:** `canViewTelemetry` gate exists. `canControlRelays` added in v1.8.0 (firmware version ≥ 1.8.0).

---

## 3. Arsitektur Backend (Google Apps Script)

**Actions:** TELEMETRY, PING, OTA_*, CALIBRATION_*, EMERGENCY_*, SEQ_STATUS. HMAC-SHA256 auth. Relay control actions tidak ada (relay commands go direct to ESP32 via REST/MQTT, tidak melalui GAS).

---

## 4. Integrasi Relay 8-Channel

### 4.1 Decision: Separate Subsystem (NOT E-WAVE Extension)

E-WAVE emergency layer adalah safety interlock. TIDAK bisa diperluas:
- Binary state (Run/Emergency) vs 8 independent channels
- Safety-isolated (no REST/MQTT surface) vs operator-controlled
- Single relay fail-safe contract vs multi-channel general purpose

**Architecture:** Build a NEW `RelayController` subsystem. Reuse E-WAVE *patterns* (driver/supervisor separation, fail-safe pin contract, NVS config) tapi BUKAN code-nya. E-WAVE stays unchanged.

### 4.2 Hardware: I²C Expander (PCF8574)

Only 3 free safe GPIOs (13/23/33) — insufficient for 8 direct relays. **Decision: PCF8574 I²C expander** (8 channels, address 0x20-0x27, shares SDA/SCL at 21/22). Zero GPIO overlap with existing pins.

Alternative considered: MCP23017 (16-ch, more features), 74HC595 (shift register, no readback). PCF8574 chosen for simplicity + I²C readback capability.

### 4.3 Integration Points (Firmware)

| Layer | File | Change |
|-------|------|--------|
| Driver | `Drivers/RelayExpanderDriver.{h,cpp}` (NEW) | PCF8574 I²C, fail-safe OFF at boot, setChannel() |
| Service | `Services/RelayController.{h,cpp}` (NEW) | State machine, applyCommand(), tick(), publishStatus() |
| Safety | `Services/RelaySafety.{h,cpp}` (NEW) | maxOnTime, minOnTime, antiChatter, lockout 5-state |
| Interlock | `Services/RelayInterlock.{h,cpp}` (NEW) | Mutual exclusion groups, dead time |
| Config | `Storage/ConfigStore.{h,cpp}` (EDIT) | loadRelayConfig/saveRelayConfig (NVS `plts_relays`) |
| Command | `Services/CommandCanonicalizer.cpp` (EDIT) | Add `relay.*` to whitelist |
| MQTT | `Network/MqttConfigReceiver.cpp` (EDIT) | Add `relay.*` dispatch |
| REST | `Web/RelayHandlers.{h,cpp}` (NEW) | GET/POST /api/relays/* |
| Telemetry | `Web/BatteryStatusSerializer.h` (EDIT) | Add `relays[]` block |
| Types | `Core/Types.h` (EDIT) | RelayState, RelayMode, TaskId::Relay, alarm codes |
| Config | `Core/Config.h` (EDIT) | PLTS_ENABLE_RELAYS, PCF8574 addr, channel count |
| Main | `firmware_v1.ino` (EDIT) | relayTask, relaysController.begin() |
| Safety cascade | `Services/EmergencySupervisor.cpp` (EDIT) | _trip() → relaysController.emergencyAllOff() |

### 4.4 Integration Points (PWA)

| Layer | File | Change |
|-------|------|--------|
| Types | `src/lib/types.ts` (EDIT) | RelayChannelStatus, RelayCommandState, RelayConfig |
| Command | `src/lib/relays.ts` (NEW) | sendRelayCommand(), fetchRelayStatus(), config schema |
| Hooks | `src/hooks/useRelays.ts` (NEW) | useRelayStatus(), useRelayCommand() |
| State | `src/lib/relayCommandStore.ts` (NEW) | Map<requestId, state> with expiry |
| API | `src/lib/deviceApi.ts` (EDIT) | Add relay endpoints |
| Compat | `src/lib/compatibility.ts` (EDIT) | canControlRelays flag |
| View | `src/components/relays/` (NEW) | relay-control-view, channel-card, config-panel |
| Nav | `src/lib/store.ts` + `app-shell.tsx` (EDIT) | Add 'relays' ViewKey + nav item |
| Routes | `src/app/api/relays/` (NEW) | REST proxy routes |

---

## 5. Hardware Interface: PCF8574 I²C Expander

> **Source code reference:** `firmware/Core/Config.h` — see `RELAY_*` constants

### 5.1 I²C Bus Configuration

**I²C bus:** Shared with INA219 (0x40), SHT31 (0x44), DS3231 RTC (0x68). PCF8574 address range: 0x20-0x27 (set by A0/A1/A2 jumpers on module). Default: 0x20 (all jumpers to GND).

**No new ESP32 GPIO pins are consumed.** PCF8574 is a slave on the existing I²C bus. Address 0x20 does not conflict with any existing device.

### 5.2 Channel → PCF8574 Port Map

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

**PCF8574 output register at boot:** 0xFF (all HIGH) = all relays OFF (active-LOW: HIGH = OFF). This is the fail-safe power-on state.

### 5.3 Compile-Time Constants (`Core/Config.h`)

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

### 5.4 Runtime Validation (`RelayExpanderDriver::begin()`)

```
1. Check I²C address in [0x20, 0x27] → FAIL if out of range
2. Check channel count == 8 → FAIL if not
3. Scan I²C bus for PCF8574 at configured address → FAIL if not found
4. Write 0xFF (all OFF) to PCF8574 output register → FAIL if write error
5. Read back PCF8574 input register → verify 0xFF (all OFF confirmed)
6. If any step fails: set _available = false, raise RELAY_FAULT alarm
```

### 5.5 Reserved GPIO Protection

Relay driver TIDAK menggunakan ESP32 GPIO secara langsung. Hanya komunikasi via I²C. GPIO berikut **RESERVED dan TIDAK BOLEH digunakan relay**:

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

### 5.6 Boot Glitch Prevention

**PCF8574 power-on state:** 0xFF (all outputs HIGH) = all relays OFF (active-LOW module). Hardware-guaranteed by PCF8574 datasheet.

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

**No relay glitch ON during boot** — PCF8574 hardware guarantees 0xFF at power-on, and firmware re-asserts before any other init.

### 5.7 I²C Bus Considerations

**Bus loading:** PCF8574 adds 1 device to the I²C bus (total: 4 devices). At 100 kHz (`I2C_FREQUENCY=100000`), bus capacity is well within spec.

**Pull-up resistors:** PCF8574 module typically includes 10kΩ pull-ups. If bus errors occur, verify pull-up value (should be 4.7kΩ-10kΩ total parallel resistance with existing module pull-ups).

**Interrupt pin (optional):** PCF8574 has an INT pin (active-LOW) that can signal input changes. NOT used in v1 — reserved for future expansion (aux contact readback).

---

## 6. Relay Control Path

```
     REST POST /api/relays/{id}/on
     MQTT plts/{dev}/command  (relay.set)
              ↓               ↓
         ┌─────────────────────┐
         │ Command Normalizer  │  (inject type="relay", action="on")
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ Auth + CSRF         │  (JWT + double-submit)
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ Freshness Gate      │  (expiresAt check)
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ CommandCanonicalizer│  (whitelist + SHA-256 hash)
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ TransactionJournal  │  (NEW/DUPLICATE/CONFLICT)
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ RelayController     │  (applyCommand)
         │   .applyCommand()   │
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ RelaySafety         │  (maxOnTime, lockout, antiChatter)
         │   .evaluate()       │
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ RelayInterlock      │  (mutual exclusion, dead time)
         │   .evaluate()       │
         └─────────┬───────────┘
                   ↓
         ┌─────────────────────┐
         │ RelayEngine         │  SINGLE GPIO MUTATION PATH
         │   .applyChannelState│  ( RelayExpanderDriver.setChannel()
         └─────────┬───────────┘    → PCF8574 → I²C → GPIO )
                   ↓
         ┌─────────────────────┐
         │ Persist ACK         │  (journal.storeTransaction)
         └─────────┬───────────┘
                   ↓
              ACK to caller
```

**NO BYPASS:** MQTT, REST, Scheduler, Safety, PIR — ALL go through CommandArbiter → RelayEngine → RelayExpanderDriver → GPIO. No direct digitalWrite() from any subsystem.

---

## 7. Safety Boundary

### 7.1 Boot Safety

- `RelayExpanderDriver::begin()` drives ALL channels OFF BEFORE any other init (mirror E-WAVE pattern). PCF8574 power-on state is all-HIGH (OFF for active-LOW relay modules). Firmware re-asserts OFF immediately.
- Boot policy per channel: `BootOff` (default, hazardous loads) | `RestoreLast` (non-hazardous). NO `BootOn` — too dangerous without physical verification.

### 7.2 Safety Supervisor (Per-Channel)

- `maxOnTimeSec`: 0=unlimited; >0 = FORCE OFF after N seconds
- `minOnTimeSec`: inhibit OFF before N seconds (protect inductive loads)
- `minOffTimeSec`: inhibit ON before N seconds (cooling)
- `minSwitchIntervalSec`: anti-chatter (min seconds between transitions)
- **FORCE OFF cannot be bypassed** by REST, MQTT, PWA, or Scheduler

### 7.3 Lockout State Machine (5-State, NVS-Persisted)

```
NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
```
- ACK = operator has seen alarm (NOT permission to re-enable)
- CLEAR requires fault condition resolved
- NVS persistence prevents bypass via power-cycle

### 7.4 E-WAVE Safety Cascade (One-Way Gate)

```
EmergencySupervisor::_trip()
  → emergencyRelay.setEnergized(false)   // E-WAVE relay ISOLATED
  → relaysController.emergencyAllOff()    // 8-channel cascade (OPTIONAL)
```
Reverse (RelayController → EmergencySupervisor) is FORBIDDEN.

### 7.5 Interlock

- Declarative groups: MutualExclusion + dead time
- Example: Relay 1 (GRID) + Relay 2 (GENSET) = mutual exclusion
- OFF → dead time → ON (prevents arc short)
- Applies to ALL sources (MANUAL, SCHEDULE, MQTT, REST, AUTOMATION)

---

## 8. Persistence Boundary

### 8.1 What is Persistent (NVS namespace `plts_relays`)

| Data | Persistent? | Why |
|------|-------------|-----|
| Channel config (name, pin, mode, safety limits) | YES | Operator configuration survives reboot |
| Safety lockout state (5-state per channel) | YES | Prevent bypass via power-cycle |
| maxOnTimeForced flag | YES | Force-OFF survives reboot |
| Command sequence (per-channel monotonic) | YES | Stale detection survives reboot |
| Transaction journal (16-slot ring) | YES (existing `plts_txn`) | Cross-channel dedup |
| **desiredState** | NO (RAM only) | Boot policy decides, not last desired |
| **reportedState** | NO (RAM only) | Recomputed on boot via boot policy |
| relayStateSequence | NO (RAM only) | Reset on boot |

### 8.2 Transaction Durability Boundary

```
Receive command → validate → auth → canonicalize → journal.decide()
  → IF NEW: persist intent (journal valid=0)
  → applyCommand (physical mutation via RelayEngine)
  → persist result (journal valid=1, store ACK)
  → return ACK
```

Crash between persist-intent and persist-result → journal has valid=0 → on reboot, entry is discarded → command is lost (safe direction: relay stays in pre-command state, operator retries).

### 8.3 Boot Recovery

1. Load `plts_relays` config (channel names, safety limits, lockout states)
2. Restore maxOnTimeForced for TRIPPED/ACKNOWLEDGED channels
3. Apply boot policy (BootOff default → all OFF)
4. Load command sequence (stale detection)
5. RelayExpanderDriver::begin() (I²C init, all OFF)
6. RelayController::begin() (apply boot policy)

---

## 9. Network Boundary

### 9.1 Offline-First

Relay control works WITHOUT Internet/MQTT/PWA/GAS:
- Safety supervisor runs locally (10 Hz tick, no network I/O)
- Scheduler runs locally (RTC-based, no network)
- E-WAVE cascade works locally
- REST works on LAN (direct to ESP32 IP)

### 9.2 REST Endpoints

```
GET  /api/relays                    — list all 8 channels (state + config)
POST /api/relays/{id}/on            — set channel ON (idempotent)
POST /api/relays/{id}/off           — set channel OFF (idempotent)
POST /api/relays/{id}/pulse         — momentary ON with duration
POST /api/relays/all_off            — emergency all-off
GET  /api/relays/config             — read relay config
POST /api/relays/config             — update relay config
POST /api/relays/{id}/acknowledge   — acknowledge safety alarm
POST /api/relays/{id}/clear         — clear safety lockout
```

All POST go through canonical pipeline (auth → CSRF → freshness → canonicalize → journal → apply → ACK).

### 9.3 MQTT Command (Equivalent to REST)

Topic: `plts/{deviceId}/config` (existing command topic)
Payload: `{ type: "relay", action: "on", channel: 1, requestId: "...", ... }`
ACK: `plts/{deviceId}/ack` (existing ACK topic)

REST and MQTT produce IDENTICAL canonical hash → cross-transport dedup.

### 9.4 Telemetry (Additive Block in SystemStatus)

```json
{
  "relays": [
    {
      "channel": 0,
      "name": "Load 1",
      "desiredState": true,
      "reportedState": true,
      "physicalState": null,
      "stateConfidence": "SOFTWARE_ONLY",
      "fault": false,
      "lockoutState": "NORMAL",
      "source": "MANUAL",
      "lastChangedAt": 123456789
    }
  ]
}
```

### 9.5 PWA Command State Model (8-State)

```
COMMAND_PENDING | CONFIRMED_ON | CONFIRMED_OFF | TIMEOUT | FAILED |
DEVICE_OFFLINE | UNKNOWN | STATE_DRIFT
```
- TIMEOUT ≠ FAILED (non-negotiable)
- After reconnect: GET current state → reconcile (NOT blind retry)
- Zustand store `Map<requestId, RelayCommandState>` with expiry timer

### 9.6 Capability Discovery

```json
{
  "relay": {
    "supported": true,
    "channels": 8,
    "capabilities": ["SET_STATE", "PULSE", "ALL_OFF", "INTERLOCK", "MAX_ON_TIME"]
  }
}
```
PWA does NOT hard-code "8 relays always exist". Compatibility gate: `canControlRelays` flag based on firmware version ≥ 1.8.0.

---

## 10. Command Model (Canonical)

### 10.1 Idempotent State Command (NOT Toggle)

```json
{
  "transactionId": "uuid-...",
  "requestId": "uuid-...",
  "commandSequence": 42,
  "deviceId": "PLTS-A1B2C3",
  "type": "relay",
  "action": "on",
  "channel": 1,
  "desiredState": true,
  "semantics": "IDEMPOTENT_STATE",
  "source": "MANUAL",
  "expiresAt": 123456789
}
```

`toggle` is REJECTED. `on` / `off` are idempotent — replay is safe: `ON → ON` stays ON (no double-flip).

### 10.2 Command Hash (SHA-256, Canonical)

```
SHA-256("v1|relay|on|channel=1|desiredState=true|semantics=IDEMPOTENT_STATE")
```
- `requestId` and `transactionId` EXCLUDED from hash (identifies transaction, not command)
- `expiresAt` EXCLUDED (envelope, not command)
- Field order FIXED per type (not JSON property order)
- REST and MQTT produce IDENTICAL hash → cross-transport dedup

### 10.3 ACK Semantics

```
RECEIVED → ACCEPTED → EXECUTED (success)
                  → REJECTED (invalid command)
                  → BLOCKED (safety/interlock veto)
                  → FAILED (driver error)
                  → UNKNOWN (timeout — no confirmation)
```

---

## 11. Source + Priority

| Source | Priority | Use case |
|--------|----------|----------|
| SAFETY | 1000 | maxOnTime FORCE OFF, fault, interlock |
| SYSTEM | 900 | E-WAVE cascade, boot policy |
| MANUAL | 800 | Operator REST/MQTT command (authorized) |
| AUTOMATION | 600 | Remote automation rule |
| SCHEDULE | 500 | RTC-based schedule |
| DEFAULT | 100 | Default OFF |

**Safety authority > manual control.** FORCE OFF cannot be overridden by REST, MQTT, PWA, or Scheduler.

---

## 12. Authorization

### 12.1 Role Model (Extend Existing)

- `viewer` — can VIEW relay status, CANNOT control
- `operator` — can CONTROL relays (on/off/pulse/config)

`'relays'` added to `OPERATOR_ONLY_VIEWS` in PWA app-shell.

### 12.2 Per-Channel ACL

**Decision: NOT implemented in v1.** Documented as limitation. All 8 channels are operator-controllable. Per-channel ACL requires schema extension to auth model — deferred to v2 if needed.

---

## 13. Compatibility Matrix

| PWA | Firmware | Relay Capability |
|-----|----------|-----------------|
| old (≤1.7.x) | old (≤1.7.x) | N/A (no relay view) |
| new (1.8+) | old (≤1.7.x) | relay view hidden (`canControlRelays=false`) |
| old (≤1.7.x) | new (1.8+) | relay block in telemetry ignored (additive) |
| new (1.8+) | new (1.8+) | 8-channel relay fully functional |

Firmware version bump: 1.7.1 → 1.8.0 (relay feature = minor version). Protocol version bump: 1 → 2 (new relay command types). Config schema version: stays 1 (relay config is separate namespace).

---

## 14. Build Flag

`PLTS_ENABLE_RELAYS=1` (default 1 in `platformio.ini`).

When OFF: byte-equivalent to current build (no relay code compiled in). Telemetry `relays[]` block absent (additive, `#if PLTS_ENABLE_RELAYS`).

---

## 15. Electrical Safety

### 15.1 Relay Module Electrical Spec

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

**IMPORTANT:** Relay module VCC must be 5V (NOT 3.3V — ESP32 GPIO is 3.3V but PCF8574 can run at 5V VCC, with I²C pulled up to 3.3V for ESP32 compatibility). Verify level shifting on SDA/SCL if running PCF8574 at 5V.

### 15.2 Installation Safety Requirements

| Parameter | Requirement |
|-----------|-------------|
| Relay contact rating | Per relay module spec (typically 10A 250VAC) |
| Load type | Resistive (lights, fans) or inductive (motors, contactors) |
| Inductive load | Flyback diode (DC) or snubber (AC) REQUIRED |
| Inrush current | Verify relay contact rating > inrush peak |
| Fuse/MCB | Per-channel fuse sized to load |
| Isolation | Optocoupler relay module (standard) |
| Active level | LOW = ON (standard optocoupler relay module) |
| Boot behavior | ALL OFF at boot (fail-safe) |

**Firmware Production Grade ≠ electrical safety.** Operator MUST verify electrical installation per local code.
