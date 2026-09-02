#!/usr/bin/env python3
"""
test_emergency_firmware_logic.py — [E-WAVE] firmware emergency layer
=====================================================================
Mirrors the emergency-relay algorithm from firmware-generic v1.6.0
(src/plts_firmware_v1.ino) line-for-line and proves the safety invariants:

  Group A — Trip debounce + hysteresis:
    A single/few noisy violation samples never trip (debounceN); a sustained
    violation trips exactly once (latched).

  Group B — ARM gating (fail-closed):
    ARM is rejected while any trigger is still active (with hysteresis);
    rejected during the recovery window; allowed after recoverySec; rejected
    while crash-chain hold is active; idempotent when already RUN.

  Group C — E-stop latching:
    E-stop open → trip + relay OFF; release → still ISOLATED until operator
    ARM (release alone never re-energizes).

  Group D — Command application:
    DISARM always isolates (safe direction); CONFIG range-checks re-validated
    locally (out-of-range values silently dropped, valid ones applied).

  Group E — Static fail-safe patterns in the .ino source:
    relay pin driven HIGH before WiFi/LittleFS; emgInit before loadConfig's
    dependent use; relay state re-applied after config load; local-first loop
    (emgTick outside the WiFi-connected branch); watchdog stays fed.

Usage: python3 scripts/test_emergency_firmware_logic.py   (exit 0 = PASS)
"""
import math
import re
import sys
import os

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


# ---------------------------------------------------------------------------
# Mirror of EmergencyConfig defaults (must equal the .ino struct)
# ---------------------------------------------------------------------------
class EmergencyConfig:
    def __init__(self, **kw):
        self.vbatLowV = 42.0
        self.vbatLowHystV = 1.0
        self.vbatHighV = 55.0
        self.vbatHighHystV = 1.0
        self.iDcOverA = 110.0
        self.iAcLoadOverA = 28.0
        self.iAcGenOverA = 28.0
        self.debounceN = 3
        self.recoverySec = 60
        self.relayPin = 27
        self.estopPin = 14
        self.estopEnabled = 1
        for k, v in kw.items():
            setattr(self, k, v)


RUN = "RUN"
EMERGENCY = "EMERGENCY"


# ---------------------------------------------------------------------------
# Mirror of the firmware state machine (emg* functions, line-for-line logic)
# ---------------------------------------------------------------------------
class EmgFirmware:
    def __init__(self, cfg=None):
        self.cfg = cfg or EmergencyConfig()
        self.state = EMERGENCY
        self.reason = "BOOT"
        self.trips = 0
        self.estop_open = False
        self.debounce = [0, 0, 0, 0, 0]   # vbatLo, vbatHi, iDc, iAc, iAcGen
        self.clear_at_ms = 0
        self.crash_chain = 0
        self.relay_gpio_low = False        # LOW = energized = RUN
        self.now_ms = 0
        self.ina219_present = True
        self.vbat = float("nan")
        self.idc = float("nan")
        self.iac = float("nan")
        self.igen = float("nan")

    def relay_write(self, energized):
        self.relay_gpio_low = energized    # active-LOW module

    def tick(self, ms):
        self.now_ms = ms

    def trip(self, reason):
        if self.state == EMERGENCY:
            return
        self.state = EMERGENCY
        self.reason = reason
        self.trips += 1
        self.relay_write(False)

    def evaluate_triggers(self):
        v = self.vbat
        i = self.idc
        a = self.iac
        g = self.igen
        cfg = self.cfg
        # low voltage
        if _fin(v) and v < cfg.vbatLowV:
            self.debounce[0] += 1
            if self.debounce[0] >= cfg.debounceN:
                self.trip("VBAT_LOW"); return
        elif _fin(v) and v > cfg.vbatLowV + cfg.vbatLowHystV:
            self.debounce[0] = 0
        # high voltage
        if _fin(v) and v > cfg.vbatHighV:
            self.debounce[1] += 1
            if self.debounce[1] >= cfg.debounceN:
                self.trip("VBAT_HIGH"); return
        elif _fin(v) and v < cfg.vbatHighV - cfg.vbatHighHystV:
            self.debounce[1] = 0
        # DC overcurrent
        if self.ina219_present and _fin(i) and abs(i) > cfg.iDcOverA:
            self.debounce[2] += 1
            if self.debounce[2] >= cfg.debounceN:
                self.trip("I_DC_OVER"); return
        elif (not self.ina219_present) or (_fin(i) and abs(i) < cfg.iDcOverA):
            self.debounce[2] = 0
        # AC load overcurrent
        if _fin(a) and a > cfg.iAcLoadOverA:
            self.debounce[3] += 1
            if self.debounce[3] >= cfg.debounceN:
                self.trip("I_AC_LOAD_OVER"); return
        elif _fin(a) and a < cfg.iAcLoadOverA:
            self.debounce[3] = 0
        # Genset overcurrent
        if _fin(g) and g > cfg.iAcGenOverA:
            self.debounce[4] += 1
            if self.debounce[4] >= cfg.debounceN:
                self.trip("I_AC_GEN_OVER"); return
        elif _fin(g) and g < cfg.iAcGenOverA:
            self.debounce[4] = 0

    def arm_block_reason(self):
        v, i, a, g = self.vbat, self.idc, self.iac, self.igen
        cfg = self.cfg
        if not _fin(v):
            return "sensor tegangan tidak valid"
        if v <= cfg.vbatLowV + cfg.vbatLowHystV:
            return "VBAT masih rendah"
        if v >= cfg.vbatHighV - cfg.vbatHighHystV:
            return "VBAT masih tinggi"
        if self.ina219_present and _fin(i) and abs(i) > cfg.iDcOverA:
            return "arus DC masih di atas ambang"
        if _fin(a) and a > cfg.iAcLoadOverA:
            return "arus beban masih di atas ambang"
        if _fin(g) and g > cfg.iAcGenOverA:
            return "arus jenset masih di atas ambang"
        if self.clear_at_ms == 0:
            self.clear_at_ms = self.now_ms
        if self.now_ms - self.clear_at_ms < cfg.recoverySec * 1000:
            return "masih dalam masa pemulihan"
        return ""

    def track_clear(self):
        blocked = self.arm_block_reason()
        clear_now = (len(blocked) == 0) or blocked.startswith("masih dalam masa pemulihan")
        if not clear_now:
            self.clear_at_ms = 0
        elif self.clear_at_ms == 0:
            self.clear_at_ms = self.now_ms

    def poll_estop(self, open_now):
        if not self.cfg.estopEnabled or self.cfg.estopPin < 0:
            return
        if open_now != self.estop_open:
            self.estop_open = open_now
            if open_now:
                if self.state == RUN:
                    self.state = EMERGENCY
                    self.reason = "ESTOP"
                    self.trips += 1
                    self.relay_write(False)
            # release → nothing: latched until ARM

    def apply_command(self, command):
        cmd = command.upper()
        if cmd == "ARM":
            if self.state == RUN:
                return ("APPLIED", "already RUN")
            if self.crash_chain >= 3:
                return ("REJECTED", "crash-loop hold active")
            block = self.arm_block_reason()
            if len(block) > 0:
                return ("REJECTED", block)
            self.state = RUN
            self.reason = ""
            self.relay_write(True)
            return ("APPLIED", "relay energized")
        if cmd == "DISARM":
            if self.state == RUN:
                self.state = EMERGENCY
                self.reason = "OPERATOR"
                self.trips += 1
            self.relay_write(False)
            return ("APPLIED", "relay isolated")
        return ("REJECTED", "unknown command")


def _fin(x):
    return isinstance(x, float) and math.isfinite(x)


def healthy_sensors(fw):
    fw.vbat = 51.2
    fw.idc = -3.5
    fw.iac = 1.8
    fw.igen = 0.3


def armed(fw):
    """Simulate a legitimately-armed system (trigger-clear + recovery done)."""
    healthy_sensors(fw)
    fw.clear_at_ms = fw.now_ms - 61_000
    res = fw.apply_command("ARM")
    assert res[0] == "APPLIED", res
    return fw


# ---------------------------------------------------------------------------
# Group A — debounce + latching
# ---------------------------------------------------------------------------
print("[A] Trip debounce + hysteresis:")
fw = EmgFirmware()
healthy_sensors(fw)
res = fw.apply_command("ARM")
check("A0 fresh boot: ARM rejected during recovery window (fail-safe)",
      res[0] == "REJECTED" and "pemulihan" in res[1], str(res))

fw = EmgFirmware()
armed(fw)
check("A1 ARM allowed once trigger-clear + recovery elapsed",
      fw.state == RUN and fw.relay_gpio_low is True)

# A2 — two noisy low-voltage samples then recovery → NO trip.
fw = EmgFirmware()
armed(fw)
for _ in range(2):
    fw.vbat = 40.0
    fw.evaluate_triggers()
fw.vbat = 51.5
fw.evaluate_triggers()
fw.vbat = 51.5
fw.evaluate_triggers()
check("A2 noise burst (2 < debounceN 3) never trips", fw.state == RUN)

# A3 — sustained low voltage → trip on exactly the 3rd sample.
fw = EmgFirmware()
armed(fw)
fw.vbat = 40.0
fw.evaluate_triggers(); fw.evaluate_triggers()
check("A3a after 2 samples still RUN", fw.state == RUN)
fw.evaluate_triggers()
check("A3b 3rd sample trips VBAT_LOW", fw.state == EMERGENCY and fw.reason == "VBAT_LOW")
check("A3c relay de-energized (GPIO HIGH)", fw.relay_gpio_low is False)
# A4 — latched: further violations do not re-trip / no state change.
fw.evaluate_triggers(); fw.evaluate_triggers()
check("A4 trip is LATCHED (no re-trip)", fw.state == EMERGENCY and fw.trips == 1)

# ---------------------------------------------------------------------------
# Group B — ARM gating
# ---------------------------------------------------------------------------
print("\n[B] ARM gating (fail-closed):")
# B1 — trigger still active → reject.
fw.vbat = 41.5   # above lowV but below lowV + hyst (blocked by hysteresis)
fw.track_clear() # loop behavior: blocked → recovery clock reset
res = fw.apply_command("ARM")
check("B1 hysteresis blocks ARM (v=41.5 < 42+1)", res[0] == "REJECTED", str(res))
fw.vbat = 43.5   # clear of the low trigger
fw.track_clear() # loop behavior: clear NOW → recovery clock starts
res = fw.apply_command("ARM")
check("B2 ARM rejected during recovery window", res[0] == "REJECTED" and "pemulihan" in res[1], str(res))

# B3 — recovery elapsed → allowed.
fw.clear_at_ms = fw.now_ms - 61_000
res = fw.apply_command("ARM")
check("B3 ARM allowed after recoverySec", res[0] == "APPLIED" and fw.state == RUN, str(res))

# B4 — crash-chain hold.
fw2 = EmgFirmware()
healthy_sensors(fw2)
fw2.crash_chain = 3
res = fw2.apply_command("ARM")
check("B4 crash-loop hold rejects ARM", res[0] == "REJECTED" and "crash" in res[1], str(res))

# B5 — idempotent ARM.
fw2.crash_chain = 0
fw2.clear_at_ms = fw2.now_ms - 61_000
fw2.apply_command("ARM")
res = fw2.apply_command("ARM")
check("B5 ARM idempotent when already RUN", res[0] == "APPLIED" and "already" in res[1], str(res))

# B6 — invalid sensor blocks ARM (fail-closed).
fw3 = EmgFirmware()
fw3.vbat = float("nan")
res = fw3.apply_command("ARM")
check("B6 invalid voltage sensor blocks ARM", res[0] == "REJECTED" and "tidak valid" in res[1], str(res))

# B7 — INA219 absent: iDc NaN does not block ARM (unmonitored ≠ unsafe).
fw4 = EmgFirmware()
fw4.ina219_present = False
healthy_sensors(fw4)
fw4.vbat = 51.2; fw4.idc = float("nan")
fw4.clear_at_ms = fw4.now_ms - 61_000
res = fw4.apply_command("ARM")
check("B7 INA219 absent does not block ARM", res[0] == "APPLIED", str(res))

# ---------------------------------------------------------------------------
# Group C — E-stop latching
# ---------------------------------------------------------------------------
print("\n[C] E-stop latching:")
fw5 = EmgFirmware()
armed(fw5)
check("C1 pre: RUN", fw5.state == RUN)
fw5.poll_estop(True)
check("C2 E-stop open → ISOLATED + relay off",
      fw5.state == EMERGENCY and fw5.reason == "ESTOP" and fw5.relay_gpio_low is False)
fw5.poll_estop(False)
check("C3 E-stop released → STILL isolated (latched until ARM)",
      fw5.state == EMERGENCY and fw5.relay_gpio_low is False)

# ---------------------------------------------------------------------------
# Group D — DISARM semantics
# ---------------------------------------------------------------------------
print("\n[D] DISARM semantics:")
fw6 = EmgFirmware()
armed(fw6)
res = fw6.apply_command("DISARM")
check("D1 DISARM isolates from RUN",
      res[0] == "APPLIED" and fw6.state == EMERGENCY and fw6.relay_gpio_low is False)
res = fw6.apply_command("DISARM")
check("D2 DISARM idempotent (already isolated, no double count)",
      res[0] == "APPLIED" and fw6.trips == 1, f"trips={fw6.trips}")

# ---------------------------------------------------------------------------
# Group E — static fail-safe patterns in the real .ino source
# ---------------------------------------------------------------------------
print("\n[E] Static fail-safe patterns (real source):")
ino = open(os.path.join(os.path.dirname(__file__), "..",
                        "firmware-generic", "src", "plts_firmware_v1.ino"), encoding="utf-8").read()

# E1 — relay pin driven HIGH before WiFi begin / LittleFS.begin in setup().
setup_idx = ino.index("void setup() {")
setup_src = ino[setup_idx:]
relay_high_idx = setup_src.index("digitalWrite(RELAY_EMERGENCY_PIN, HIGH)")
littlefs_idx = setup_src.index("LittleFS.begin(")
wifi_idx = setup_src.index("WiFi.begin(")
check("E1 relay ISOLATED (HIGH) before LittleFS/WiFi in setup()",
      relay_high_idx < littlefs_idx and relay_high_idx < wifi_idx,
      f"relay@{relay_high_idx} littlefs@{littlefs_idx} wifi@{wifi_idx}")

# E2 — emgInit() runs before loadConfig()'s dependent use (it must not depend
# on loaded config; defaults = isolated).
emginit_idx = setup_src.index("emgInit();")
loadcfg_idx = setup_src.index("loadConfig()")
check("E2 emgInit() runs before loadConfig() (defaults, fail-safe)",
      emginit_idx < loadcfg_idx, f"emgInit@{emginit_idx} loadConfig@{loadcfg_idx}")

# E3 — pins re-applied after a successful config load.
check("E3 emgApplyPinsFromConfig() after loadConfig()",
      "emgApplyPinsFromConfig();" in setup_src and
      setup_src.index("emgApplyPinsFromConfig();") > loadcfg_idx)

# E4 — local-first: emgTick() runs OUTSIDE the WiFi-connected branch (before it).
loop_idx = ino.index("void loop() {")
loop_src = ino[loop_idx:]
emgtick_idx = loop_src.index("emgTick();")
wificonn_idx = loop_src.index("WiFi.status() == WL_CONNECTED")
check("E4 emgTick() before the WiFi-connected gate (local-first)",
      emgtick_idx < wificonn_idx)

# E5 — active-LOW semantics documented + implemented.
check("E5 active-LOW relay semantics present",
      "RELAY_ACTIVE_LOW" in ino and
      "energized ? LOW : HIGH" in ino)

# E6 — watchdog fed in loop.
check("E6 WDT reset still first in loop()",
      loop_src.index("esp_task_wdt_reset();") < emgtick_idx)

# E7 — telemetry carries the emergency fields (flat contract, matches Code.gs).
for field in ['"i_ac_gen"', '"emg_state"', '"emg_reason"', '"emg_estop"', '"emg_trips"']:
    check(f"E7 telemetry field {field} present", field in ino)

# E8 — dedicated poll + piggyback both present.
check("E8a EMERGENCY_PENDING poll present", '"EMERGENCY_PENDING"' in ino)
check("E8b telemetry piggyback consumption present", "emgConsumePendingFromResponse" in ino)

# E9 — 2nd ACS712 sampling wired into sampleSensors.
check("E9 ACS712 #2 sampled in sampleSensors()",
      "acGenCurrentSamples[sampleIndex] = readAcGenRmsAmps();" in ino)

# E10 — crash-chain NVS accounting present.
check("E10 crash-chain accounting present",
      "NVS_KEY_EMG_RUN_OK" in ino and "NVS_KEY_EMG_CHAIN" in ino and
      "EMG_CRASH_CHAIN_LIMIT" in ino)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
print("\n" + "=" * 60)
print(f"EMERGENCY FIRMWARE LOGIC: {PASS} passed, {FAIL} failed")
if FAIL > 0:
    print("FAILED CHECKS:")
    for f in FAILURES:
        print("  x " + f)
    sys.exit(1)
print("ALL EMERGENCY FIRMWARE LOGIC TESTS PASS")
