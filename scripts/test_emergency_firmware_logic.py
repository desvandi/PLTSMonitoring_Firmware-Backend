#!/usr/bin/env python3
"""
test_emergency_firmware_logic.py — [E-WAVE] firmware emergency layer
=====================================================================
Mirrors the emergency-relay algorithm from firmware-generic v1.7.0
(src/plts_firmware_v1.ino) line-for-line and proves the safety invariants:

  Group A — Trip debounce + hysteresis:
    A single/few noisy violation samples never trip (debounceN); a sustained
    violation trips exactly once (latched).

  Group B — ARM gating (fail-closed):
    ARM is rejected while any trigger is still active (with hysteresis);
    rejected during the recovery window; allowed after recoverySec; rejected
    while crash-chain hold is active; idempotent when already RUN.
    v1.7.0 [P1-SC2]: under sensorFailPolicy=1 (default) ARM is ALSO rejected
    while any safety sensor is absent/invalid — unmonitored IS unsafe for a
    safety interlock. Policy 0 = explicit legacy opt-out (bench only).

  Group C — E-stop latching:
    E-stop open → trip + relay OFF; release → still ISOLATED until operator
    ARM (release alone never re-energizes).

  Group D — Command application:
    DISARM always isolates (safe direction); CONFIG range-checks re-validated
    locally (out-of-range values silently dropped, valid ones applied).

  Group S — Safety-sensor loss (v1.7.0 [P1-SC3], policy-gated):
    A running system that loses a safety sensor TRIPS to ISOLATED after the
    debounce window (fail-closed); with policy 0 it keeps running (legacy,
    documented unsafe).

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
        # v1.7.0 [P1-SC1] — 13th schema field (0/1, default 1 = fail-closed)
        self.sensorFailPolicy = 1
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
        # v1.7.0 — slot 5 = safety-sensor loss (policy-gated)
        self.debounce = [0, 0, 0, 0, 0, 0]   # vbatLo, vbatHi, iDc, iAc, iAcGen, sensorLoss
        self.clear_at_ms = 0
        self.crash_chain = 0
        self.relay_gpio_low = False        # LOW = energized = RUN
        self.now_ms = 0
        self.ina219_present = True         # INA219 detected at boot
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
        # v1.7.0 [P1-SC3] — safety-sensor loss (fail-closed, policy-gated)
        if cfg.sensorFailPolicy:
            sensor_loss = (not _fin(v)) or (not _fin(i)) or (not _fin(a)) or (not _fin(g))
            if sensor_loss:
                self.debounce[5] += 1
                if self.debounce[5] >= cfg.debounceN:
                    self.trip("SENSOR_LOSS"); return
            else:
                self.debounce[5] = 0
        else:
            self.debounce[5] = 0

    def arm_block_reason(self):
        v, i, a, g = self.vbat, self.idc, self.iac, self.igen
        cfg = self.cfg
        if not _fin(v):
            return "sensor tegangan tidak valid"
        # v1.7.0 [P1-SC2] — fail-closed: safety sensors are mandatory inputs.
        if cfg.sensorFailPolicy:
            if not self.ina219_present:
                return "sensor INA219 tidak terdeteksi — proteksi arus DC nonaktif (sensorFailPolicy=1)"
            if not _fin(i):
                return "sensor arus DC tidak valid"
            if not _fin(a):
                return "sensor arus beban AC tidak valid"
            if not _fin(g):
                return "sensor arus genset tidak valid"
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

# B7 — v1.7.0 [P1-SC2] INA219 absent now BLOCKS ARM (fail-closed default).
# The old "unmonitored ≠ unsafe" stance silently disabled I_DC_OVER
# protection; under sensorFailPolicy=1 an absent safety sensor is a hard
# ARM block with an honest, actionable reason.
fw4 = EmgFirmware()
fw4.ina219_present = False
healthy_sensors(fw4)
fw4.vbat = 51.2; fw4.idc = float("nan")
fw4.clear_at_ms = fw4.now_ms - 61_000
res = fw4.apply_command("ARM")
check("B7 INA219 absent BLOCKS ARM under sensorFailPolicy=1 (fail-closed)",
      res[0] == "REJECTED" and "INA219" in res[1], str(res))

# B7b — invalid AC current also blocks ARM (same fail-closed contract).
fw4b = EmgFirmware()
healthy_sensors(fw4b)
fw4b.iac = float("nan")
fw4b.clear_at_ms = fw4b.now_ms - 61_000
res = fw4b.apply_command("ARM")
check("B7b invalid AC-load sensor BLOCKS ARM (fail-closed)",
      res[0] == "REJECTED" and "arus beban AC" in res[1], str(res))

# B8 — policy 0 = explicit operator opt-out (legacy bench behavior).
fw4c = EmgFirmware(EmergencyConfig(sensorFailPolicy=0))
fw4c.ina219_present = False
healthy_sensors(fw4c)
fw4c.vbat = 51.2; fw4c.idc = float("nan")
fw4c.clear_at_ms = fw4c.now_ms - 61_000
res = fw4c.apply_command("ARM")
check("B8 sensorFailPolicy=0 restores legacy (unmonitored ≠ unsafe, opt-out)",
      res[0] == "APPLIED", str(res))

# B9 — healthy sensors + policy 1 still ARM normally (no regression).
fw4d = EmgFirmware()
healthy_sensors(fw4d)
fw4d.clear_at_ms = fw4d.now_ms - 61_000
res = fw4d.apply_command("ARM")
check("B9 healthy sensors + policy 1 → ARM allowed", res[0] == "APPLIED", str(res))

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
# Group S — Safety-sensor loss while RUN (v1.7.0 [P1-SC3], fail-closed)
# ---------------------------------------------------------------------------
print("\n[S] Safety-sensor loss while RUN (fail-closed):")
# S1 — a running system that loses the INA219 (iDc → NaN) trips to ISOLATED
# after the debounce window — protection must not silently vanish.
fwS = EmgFirmware()
armed(fwS)
fwS.idc = float("nan")
fwS.evaluate_triggers(); fwS.evaluate_triggers()
check("S1a 2 lost samples still RUN (debounce)", fwS.state == RUN)
fwS.evaluate_triggers()
check("S1b 3rd lost sample trips SENSOR_LOSS",
      fwS.state == EMERGENCY and fwS.reason == "SENSOR_LOSS" and fwS.relay_gpio_low is False)

# S2 — noise burst (2 lost samples then recovery) never trips.
fwS2 = EmgFirmware()
armed(fwS2)
for _ in range(2):
    fwS2.iac = float("nan")
    fwS2.evaluate_triggers()
fwS2.iac = 1.8
fwS2.evaluate_triggers()
check("S2 noise burst (2 < debounceN) never trips SENSOR_LOSS", fwS2.state == RUN)

# S3 — trip is latched (idempotent, single trip count).
fwS.evaluate_triggers(); fwS.evaluate_triggers()
check("S3 SENSOR_LOSS trip is LATCHED (no re-trip)",
      fwS.state == EMERGENCY and fwS.trips == 1)

# S4 — with policy 0 the same runtime loss does NOT trip (legacy opt-out).
fwS3 = EmgFirmware(EmergencyConfig(sensorFailPolicy=0))
armed(fwS3)
fwS3.idc = float("nan")
for _ in range(5):
    fwS3.evaluate_triggers()
check("S4 policy=0: runtime sensor loss does NOT trip (legacy, unsafe-by-record)",
      fwS3.state == RUN)

# S5 — voltage NaN is ALSO a sensor-loss trip while RUN (previously only an
# ARM-gate check; now the running system isolates too).
fwS5 = EmgFirmware()
armed(fwS5)
fwS5.vbat = float("nan")
fwS5.evaluate_triggers(); fwS5.evaluate_triggers(); fwS5.evaluate_triggers()
check("S5 voltage NaN while RUN trips SENSOR_LOSS",
      fwS5.state == EMERGENCY and fwS5.reason == "SENSOR_LOSS")

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

# E11 — v1.7.0 [P1-SC1] sensorFailPolicy is a first-class schema field:
# struct member + CONFIG parse (both load + apply paths) + persistence.
check("E11a sensorFailPolicy in EmergencyConfig struct",
      "sensorFailPolicy = 1;" in ino)
check("E11b sensorFailPolicy parsed from persisted config (loadConfig)",
      ino.count('emg["sensorFailPolicy"] | -1') == 1)
check("E11c sensorFailPolicy parsed from operator CONFIG command",
      ino.count('cfg["sensorFailPolicy"]') == 1)
check("E11d sensorFailPolicy persisted in saveConfig()",
      'emg["sensorFailPolicy"] = config.emg.sensorFailPolicy;' in ino)

# E12 — v1.7.0 [P1-SC2/SC3] the fail-closed policy is actually wired into
# the ARM gate and the trigger evaluator (not just declared).
check("E12a ARM gate consults sensorFailPolicy",
      "if (config.emg.sensorFailPolicy) {" in ino and
      "sensor INA219 tidak terdeteksi" in ino)
check("E12b SENSOR_LOSS runtime trip present (debounced)",
      'emgTrip("SENSOR_LOSS")' in ino)
check("E12c debounce array extended to 6 slots",
      "emgDebounce[6]" in ino)

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
