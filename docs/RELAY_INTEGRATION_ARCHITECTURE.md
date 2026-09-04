# RELAY_INTEGRATION_ARCHITECTURE.md

> **PHASE 0 output — mandatory before any relay code is written.**
> This document maps the CURRENT architecture, identifies the integration
> point for 8-channel relay, and defines the NEW relay control path with
> safety, persistence, and network boundaries.

## 1. CURRENT ARCHITECTURE

### 1.1 Firmware (ESP32)

**FreeRTOS tasks (11):** sensor(5Hz), measure(5Hz), energy(1Hz), telemetry(5s),
network(100ms), persist(5min), health(1Hz), ota(2Hz), bmscomm(100ms),
emg(10Hz), gasemg(15s poll). All WDT-subscribed.

**GPIO usage (definitive):**

| GPIO | Role | Owner |
|------|------|-------|
| 2 | E-WAVE LED (OUTPUT) | EmergencySupervisor |
| 4 | RS485 DE (OUTPUT) | ModbusRtuClient |
| 14 | E-WAVE E-stop (INPUT_PULLUP) | EmergencyRelayDriver |
| 16/17 | RS485 TX/RX (UART2) | ModbusRtuClient |
| 18/19 | PZEM RX/TX (UART1, optional) | Pzem004tDriver |
| 21/22 | I²C SDA/SCL | INA219 + SHT31 + DS3231 |
| 25/26 | CAN TX/RX (TWAI) | PylontechCanClient |
| 27 | E-WAVE relay (OUTPUT, active-LOW) | EmergencyRelayDriver |
| 32 | Reserved for future genset ACS712 | — |
| 34/35 | Battery ADC / ACS712 ADC (input-only) | AdcVoltageDriver / Acs712Driver |

**Free safe GPIOs:** 13, 23, 33 (only 3 — NOT enough for 8 direct relays).

**Existing actuator:** E-WAVE emergency relay (GPIO 27, active-LOW, fail-safe).
This is a SAFETY INTERLOCK, NOT a general-purpose actuator. It CANNOT be
extended to 8 channels (binary state machine, single relay, safety-isolated).

**Command pipeline (existing, reusable):**
```
REST/MQTT → auth → CSRF → freshness → CommandCanonicalizer (whitelist+hash)
→ TransactionJournal (decide: NEW/DUPLICATE/CONFLICT) → apply → journal ACK
```

**TransactionJournal:** 16-slot NVS ring, 2-phase commit, CRC32, survives reboot.
**CommandCanonicalizer:** whitelist (fail-closed), SHA-256 hash, cross-transport
dedup (REST + MQTT produce identical hash for same command).

### 1.2 PWA (Next.js)

**Stack:** Next.js 16, React 19, TanStack Query v5, Zustand, mqtt.js, Serwist.

**API split:** `deviceApi.ts` (ESP32 REST) + `backendApi.ts` (GAS aggregation)
+ `apiShared.ts` (CSRF, requestId). `api.ts` is backward-compat façade.

**MQTT:** subscribe-only (status/log/online). NO publish path — commands go via REST.

**Auth:** JWT cookie + CSRF double-submit + JTI revocation. Role: viewer | operator.

**Emergency control (E-WAVE):** browser POSTs directly to GAS Web App URL
(admin_token in body). NO requestId. NO CSRF. This is a SEPARATE command path
from deviceApi REST mutations.

**Compatibility:** `canViewTelemetry` gate exists. NO `canControlRelays` yet.

**Command state model:** NONE (generic). Emergency uses timing-based refresh
(setTimeout 4s+16s). No PENDING/CONFIRMED/FAILED/UNKNOWN tracking.

### 1.3 GAS (Code.gs)

**Actions:** TELEMETRY, PING, OTA_*, CALIBRATION_*, EMERGENCY_*, SEQ_STATUS.
HMAC-SHA256 auth. No relay control actions exist.

---

## 2. RELAY INTEGRATION POINT

### 2.1 Decision: Separate subsystem (NOT E-WAVE extension)

The E-WAVE emergency layer is a safety interlock. It CANNOT be extended:
- Binary state (Run/Emergency) vs 8 independent channels
- Safety-isolated (no REST/MQTT surface) vs operator-controlled
- Single relay fail-safe contract vs multi-channel general purpose

**Architecture:** Build a NEW `RelayController` subsystem. Reuse E-WAVE
*patterns* (driver/supervisor separation, fail-safe pin contract, NVS config)
but NOT its code. E-WAVE stays unchanged.

### 2.2 Hardware: I²C expander (PCF8574)

Only 3 free safe GPIOs (13/23/33) — insufficient for 8 direct relays.
**Decision: PCF8574 I²C expander** (8 channels, address 0x20-0x27, shares
SDA/SCL at 21/22). Zero GPIO overlap with existing pins.

Alternative considered: MCP23017 (16-ch, more features), 74HC595 (shift
register, no readback). PCF8574 chosen for simplicity + I²C readback capability.

### 2.3 Integration points (firmware)

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

### 2.4 Integration points (PWA)

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

## 3. NEW RELAY CONTROL PATH

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

**NO BYPASS:** MQTT, REST, Scheduler, Safety, PIR — ALL go through
CommandArbiter → RelayEngine → RelayExpanderDriver → GPIO.
No direct digitalWrite() from any subsystem.

---

## 4. SAFETY BOUNDARY

### 4.1 Boot safety
- `RelayExpanderDriver::begin()` drives ALL channels OFF BEFORE any other
  init (mirror E-WAVE pattern). PCF8574 power-on state is all-HIGH (OFF for
  active-LOW relay modules). Firmware re-asserts OFF immediately.
- Boot policy per channel: `BootOff` (default, hazardous loads) |
  `RestoreLast` (non-hazardous). NO `BootOn` — too dangerous without
  physical verification.

### 4.2 Safety supervisor (per-channel)
- `maxOnTimeSec`: 0=unlimited; >0 = FORCE OFF after N seconds
- `minOnTimeSec`: inhibit OFF before N seconds (protect inductive loads)
- `minOffTimeSec`: inhibit ON before N seconds (cooling)
- `minSwitchIntervalSec`: anti-chatter (min seconds between transitions)
- **FORCE OFF cannot be bypassed** by REST, MQTT, PWA, or Scheduler

### 4.3 Lockout state machine (5-state, NVS-persisted)
```
NORMAL → TRIPPED → ACKNOWLEDGED → CLEARED → ARMED → NORMAL
```
- ACK = operator has seen alarm (NOT permission to re-enable)
- CLEAR requires fault condition resolved
- NVS persistence prevents bypass via power-cycle

### 4.4 E-WAVE safety cascade (one-way gate)
```
EmergencySupervisor::_trip()
  → emergencyRelay.setEnergized(false)   // E-WAVE relay ISOLATED
  → relaysController.emergencyAllOff()    // 8-channel cascade (OPTIONAL)
```
Reverse (RelayController → EmergencySupervisor) is FORBIDDEN.

### 4.5 Interlock
- Declarative groups: MutualExclusion + dead time
- Example: Relay 1 (GRID) + Relay 2 (GENSET) = mutual exclusion
- OFF → dead time → ON (prevents arc short)
- Applies to ALL sources (MANUAL, SCHEDULE, MQTT, REST, AUTOMATION)

---

## 5. PERSISTENCE BOUNDARY

### 5.1 What is persistent (NVS namespace `plts_relays`)

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

### 5.2 Transaction durability boundary
```
Receive command → validate → auth → canonicalize → journal.decide()
  → IF NEW: persist intent (journal valid=0)
  → applyCommand (physical mutation via RelayEngine)
  → persist result (journal valid=1, store ACK)
  → return ACK
```
Crash between persist-intent and persist-result → journal has valid=0
→ on reboot, entry is discarded → command is lost (safe direction: relay
stays in pre-command state, operator retries).

### 5.3 Boot recovery
1. Load `plts_relays` config (channel names, safety limits, lockout states)
2. Restore maxOnTimeForced for TRIPPED/ACKNOWLEDGED channels
3. Apply boot policy (BootOff default → all OFF)
4. Load command sequence (stale detection)
5. RelayExpanderDriver::begin() (I²C init, all OFF)
6. RelayController::begin() (apply boot policy)

---

## 6. NETWORK BOUNDARY

### 6.1 Offline-first
Relay control works WITHOUT Internet/MQTT/PWA/GAS:
- Safety supervisor runs locally (10 Hz tick, no network I/O)
- Scheduler runs locally (RTC-based, no network)
- E-WAVE cascade works locally
- REST works on LAN (direct to ESP32 IP)

### 6.2 REST endpoints (NEW)
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

All POST go through canonical pipeline (auth → CSRF → freshness →
canonicalize → journal → apply → ACK).

### 6.3 MQTT command (equivalent to REST)
Topic: `plts/{deviceId}/config` (existing command topic)
Payload: `{ type: "relay", action: "on", channel: 1, requestId: "...", ... }`
ACK: `plts/{deviceId}/ack` (existing ACK topic)

REST and MQTT produce IDENTICAL canonical hash → cross-transport dedup.

### 6.4 Telemetry (additive block in SystemStatus)
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

### 6.5 PWA command state model (NEW)
8-state discriminator:
```
COMMAND_PENDING | CONFIRMED_ON | CONFIRMED_OFF | TIMEOUT | FAILED |
DEVICE_OFFLINE | UNKNOWN | STATE_DRIFT
```
- TIMEOUT ≠ FAILED (non-negotiable)
- After reconnect: GET current state → reconcile (NOT blind retry)
- Zustand store `Map<requestId, RelayCommandState>` with expiry timer

### 6.6 Capability discovery
```json
{
  "relay": {
    "supported": true,
    "channels": 8,
    "capabilities": ["SET_STATE", "PULSE", "ALL_OFF", "INTERLOCK", "MAX_ON_TIME"]
  }
}
```
PWA does NOT hard-code "8 relays always exist". Compatibility gate:
`canControlRelays` flag based on firmware version ≥ 1.8.0.

---

## 7. COMMAND MODEL (Canonical)

### 7.1 Idempotent state command (NOT toggle)
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

`toggle` is REJECTED. `on` / `off` are idempotent — replay is safe:
`ON → ON` stays ON (no double-flip).

### 7.2 Command hash (SHA-256, canonical)
```
SHA-256("v1|relay|on|channel=1|desiredState=true|semantics=IDEMPOTENT_STATE")
```
- `requestId` and `transactionId` EXCLUDED from hash (identifies transaction,
  not command)
- `expiresAt` EXCLUDED (envelope, not command)
- Field order FIXED per type (not JSON property order)
- REST and MQTT produce IDENTICAL hash → cross-transport dedup

### 7.3 ACK semantics
```
RECEIVED → ACCEPTED → EXECUTED (success)
                  → REJECTED (invalid command)
                  → BLOCKED (safety/interlock veto)
                  → FAILED (driver error)
                  → UNKNOWN (timeout — no confirmation)
```

---

## 8. SOURCE + PRIORITY

| Source | Priority | Use case |
|--------|----------|----------|
| SAFETY | 1000 | maxOnTime FORCE OFF, fault, interlock |
| SYSTEM | 900 | E-WAVE cascade, boot policy |
| MANUAL | 800 | Operator REST/MQTT command (authorized) |
| AUTOMATION | 600 | Remote automation rule |
| SCHEDULE | 500 | RTC-based schedule |
| DEFAULT | 100 | Default OFF |

**Safety authority > manual control.** FORCE OFF cannot be overridden by
REST, MQTT, PWA, or Scheduler.

---

## 9. AUTHORIZATION

### 9.1 Role model (extend existing)
- `viewer` — can VIEW relay status, CANNOT control
- `operator` — can CONTROL relays (on/off/pulse/config)

`'relays'` added to `OPERATOR_ONLY_VIEWS` in PWA app-shell.

### 9.2 Per-channel ACL
**Decision: NOT implemented in v1.** Documented as limitation.
All 8 channels are operator-controllable. Per-channel ACL requires schema
extension to auth model — deferred to v2 if needed.

---

## 10. COMPATIBILITY MATRIX

| PWA | Firmware | Relay Capability |
|-----|----------|-----------------|
| old (≤1.7.x) | old (≤1.7.x) | N/A (no relay view) |
| new (1.8+) | old (≤1.7.x) | relay view hidden (`canControlRelays=false`) |
| old (≤1.7.x) | new (1.8+) | relay block in telemetry ignored (additive) |
| new (1.8+) | new (1.8+) | 8-channel relay fully functional |

Firmware version bump: 1.7.1 → 1.8.0 (relay feature = minor version).
Protocol version bump: 1 → 2 (new relay command types).
Config schema version: stays 1 (relay config is separate namespace).

---

## 11. BUILD FLAG

`PLTS_ENABLE_RELAYS=1` (default 1 in `platformio.ini`).

When OFF: byte-equivalent to current build (no relay code compiled in).
Telemetry `relays[]` block absent (additive, `#if PLTS_ENABLE_RELAYS`).

---

## 12. ELECTRICAL SAFETY (documentation requirement)

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

**Firmware Production Grade ≠ electrical safety.** Operator MUST verify
electrical installation per local code.
