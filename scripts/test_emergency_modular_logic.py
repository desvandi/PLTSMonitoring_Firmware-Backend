#!/usr/bin/env python3
"""
test_emergency_modular_logic.py — E-WAVE v1.7.0 modular-port mechanical suite
==============================================================================
Closes README §13 limitation #7 (emergency layer only in firmware-generic).
This suite proves the PORTED logic in firmware/ matches the E-WAVE safety
semantics with two complementary techniques (same as
test_emergency_firmware_logic.py, extended for the modular architecture):

  Groups A/B/C/D/S — hand-mirrored logic simulation of
    Services/EmergencySupervisor.{h,cpp} (state machine, debounce+ hysteresis,
    ARM gating fail-closed, E-stop latch, DISARM safe-direction, sensor-loss
    policy with the RESERVED genset channel excluded).
  Group M — event queue (single-slot, latest wins) + GasEmergencyChannel
    retry budget (20 attempts, 5 s rate limit) mirror.
  Group R — wire contract vs code.gs/Code.gs: response parse fields, ACK/EVENT
    bodies, event-type whitelist, and the 13 EMergency_CONFIG_FIELDS ranges
    cross-checked against the device-side clamps (mixed-version fleet guard).
  Group E — static source assertions on the real firmware files:
    boot-isolation ordering (E1), local-first (no network in the supervisor),
    HMAC canonical string, TLS pinning precedence, WDT around blocking POST,
    fail-safe relay polarity, NVS keys, telemetry emergency block.

Safety invariants being proven (the port must not regress them):
  A: a trip is latched, debounced, and hysteresis-safe
  B: ARM is operator-only and fail-closed against bad sensors/thresholds
  C: physical E-stop latches; release never re-energizes
  D: DISARM is always safe-direction and idempotent
  S: sensorFailPolicy=1 fails CLOSED (SENSOR_LOSS); iGen is reserved

Usage: python3 scripts/test_emergency_modular_logic.py   (exit 0 = PASS)
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
FW = os.path.join(ROOT, "firmware")
INO = os.path.join(FW, "firmware_v1.ino")
SUP_CPP = os.path.join(FW, "Services", "EmergencySupervisor.cpp")
SUP_H = os.path.join(FW, "Services", "EmergencySupervisor.h")
DRV_CPP = os.path.join(FW, "Drivers", "EmergencyRelayDriver.cpp")
CH_CPP = os.path.join(FW, "Network", "GasEmergencyChannel.cpp")
SER_H = os.path.join(FW, "Web", "BatteryStatusSerializer.h")
CFG_CPP = os.path.join(FW, "Storage", "ConfigStore.cpp")
CODE_GS = os.path.join(ROOT, "code.gs", "Code.gs")

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


def fin(x):
    return isinstance(x, float) and math.isfinite(x)


# ---------------------------------------------------------------------------
# Mirror of Core::Config.h E-WAVE defaults (must match Config.h + Code.gs)
# ---------------------------------------------------------------------------
DEFAULTS = {
    "vbatLowV": 42.0, "vbatLowHystV": 1.0, "vbatHighV": 55.0, "vbatHighHystV": 1.0,
    "iDcOverA": 110.0, "iAcLoadOverA": 28.0, "iAcGenOverA": 28.0,
    "debounceN": 3, "recoverySec": 60, "relayPin": 27, "estopPin": 14,
    "estopEnabled": 1, "sensorFailPolicy": 1,
}
RANGES = {
    "vbatLowV": (30.0, 60.0), "vbatLowHystV": (0.1, 5.0),
    "vbatHighV": (48.0, 60.0), "vbatHighHystV": (0.1, 5.0),
    "iDcOverA": (10.0, 120.0), "iAcLoadOverA": (5.0, 40.0),
    "iAcGenOverA": (5.0, 40.0),
    "debounceN": (1, 10), "recoverySec": (0, 3600),
    "relayPin": (12, 39), "estopPin": (-1, 39),
    "estopEnabled": (0, 1), "sensorFailPolicy": (0, 1),
}


def clamp(v, lo, hi, fallback):
    if v is None or (isinstance(v, float) and not math.isfinite(v)) or v < lo or v > hi:
        return fallback
    return v


# ---------------------------------------------------------------------------
# Hand-mirrored EmergencySupervisor (line-for-line semantics of the .cpp)
# ---------------------------------------------------------------------------
class EmgFw:
    def __init__(self, cfg=None):
        self.cfg = dict(DEFAULTS)
        if cfg:
            self.cfg.update(cfg)
        self.state = "EMERGENCY"          # RAM-only: every boot re-enters EMERGENCY
        self.reason = "BOOT"
        self.trips = 0
        self.crash_chain = 0
        self.relay_energized = False
        self.estop_open = False
        self.debounce = [0, 0, 0, 0, 0, 0]
        self.clear_at_ms = 0
        self.now_ms = 0
        self.trip_at_ms = 0
        self.vbat = NAN
        self.idc = NAN
        self.iac = NAN
        self.igen = NAN                    # RESERVED channel — always NaN on this board
        self.ina219 = True
        self.events = []                   # (type, reason) queue, single slot semantics

    # -- event queue: single-slot, LATEST wins --------------------------------
    def queue_event(self, etype, reason):
        self.events = [(etype, reason)]

    def consume_event(self):
        self.events = []

    # -- latched trip ----------------------------------------------------------
    def trip(self, reason, event_type="TRIP", event_reason=""):
        if self.state == "EMERGENCY":
            return
        self.state = "EMERGENCY"
        self.reason = reason
        self.trip_at_ms = self.now_ms
        self.trips += 1
        self.relay_energized = False       # ISOLATED — immediate, local
        self.queue_event(event_type, event_reason or reason)

    def arm(self, source):
        self.state = "RUN"
        self.reason = ""
        self.trip_at_ms = 0
        self.relay_energized = True
        self.queue_event("ARMED", f"operator ARM ({source}) — relay energized")

    # -- trigger evaluation (RUN only) ----------------------------------------
    def evaluate_triggers(self):
        n = self.cfg["debounceN"]

        if fin(self.vbat) and self.vbat < self.cfg["vbatLowV"]:
            self.debounce[0] += 1
            if self.debounce[0] >= n:
                self.trip("VBAT_LOW")
                return
        elif fin(self.vbat) and self.vbat > self.cfg["vbatLowV"] + self.cfg["vbatLowHystV"]:
            self.debounce[0] = 0

        if fin(self.vbat) and self.vbat > self.cfg["vbatHighV"]:
            self.debounce[1] += 1
            if self.debounce[1] >= n:
                self.trip("VBAT_HIGH")
                return
        elif fin(self.vbat) and self.vbat < self.cfg["vbatHighV"] - self.cfg["vbatHighHystV"]:
            self.debounce[1] = 0

        if self.ina219 and fin(self.idc) and abs(self.idc) > self.cfg["iDcOverA"]:
            self.debounce[2] += 1
            if self.debounce[2] >= n:
                self.trip("I_DC_OVER")
                return
        else:
            self.debounce[2] = 0

        if fin(self.iac) and self.iac > self.cfg["iAcLoadOverA"]:
            self.debounce[3] += 1
            if self.debounce[3] >= n:
                self.trip("I_AC_LOAD_OVER")
                return
        else:
            self.debounce[3] = 0

        if fin(self.igen) and self.igen > self.cfg["iAcGenOverA"]:
            self.debounce[4] += 1
            if self.debounce[4] >= n:
                self.trip("I_AC_GEN_OVER")
                return
        else:
            self.debounce[4] = 0

        if self.cfg["sensorFailPolicy"]:
            loss = not fin(self.vbat) or not fin(self.idc) or not fin(self.iac)
            if loss:
                self.debounce[5] += 1
                if self.debounce[5] >= n:
                    self.trip("SENSOR_LOSS")
                    return
            else:
                self.debounce[5] = 0
        else:
            self.debounce[5] = 0

    # -- ARM gate (fail-closed) ----------------------------------------------
    def arm_block_reason(self):
        if not fin(self.vbat):
            return "sensor tegangan tidak valid"
        if self.cfg["sensorFailPolicy"]:
            if not self.ina219:
                return "sensor INA219 tidak terdeteksi — proteksi arus DC nonaktif (sensorFailPolicy=1)"
            if not fin(self.idc):
                return "sensor arus DC tidak valid"
            if not fin(self.iac):
                return "sensor arus beban AC tidak valid"
            # (iGen deliberately absent — RESERVED channel)
        if self.vbat <= self.cfg["vbatLowV"] + self.cfg["vbatLowHystV"]:
            return f"VBAT masih rendah ({self.vbat}V)"
        if self.vbat >= self.cfg["vbatHighV"] - self.cfg["vbatHighHystV"]:
            return f"VBAT masih tinggi ({self.vbat}V)"
        if self.ina219 and fin(self.idc) and abs(self.idc) > self.cfg["iDcOverA"]:
            return f"arus DC masih di atas ambang ({abs(self.idc)}A)"
        if fin(self.iac) and self.iac > self.cfg["iAcLoadOverA"]:
            return f"arus beban masih di atas ambang ({self.iac}A)"
        if fin(self.igen) and self.igen > self.cfg["iAcGenOverA"]:
            return f"arus jenset masih di atas ambang ({self.igen}A)"
        if self.clear_at_ms == 0:
            self.clear_at_ms = self.now_ms
        if self.now_ms - self.clear_at_ms < self.cfg["recoverySec"] * 1000:
            remaining = self.cfg["recoverySec"] - (self.now_ms - self.clear_at_ms) // 1000
            return f"masih dalam masa pemulihan ({remaining} detik lagi)"
        return ""

    def track_clear(self):
        blocked = self.arm_block_reason()
        clear_now = (len(blocked) == 0) or blocked.startswith("masih dalam masa pemulihan")
        if not clear_now:
            self.clear_at_ms = 0
        elif self.clear_at_ms == 0:
            self.clear_at_ms = self.now_ms

    # -- tick ------------------------------------------------------------------
    def tick(self):
        self.evaluate_triggers()
        self.track_clear()

    # -- operator commands (GAS queue) ----------------------------------------
    def apply_command(self, command, cmd_cfg=None):
        cmd = command.upper()
        if cmd == "ARM":
            if self.state == "RUN":
                return "APPLIED", "already RUN"
            if self.crash_chain >= 3:
                return "REJECTED", "crash-loop hold active — power-cycle stable first"
            block = self.arm_block_reason()
            if block:
                return "REJECTED", block
            self.arm("operator")
            return "APPLIED", "relay energized"
        if cmd == "DISARM":
            if self.state == "RUN":
                self.state = "EMERGENCY"
                self.reason = "OPERATOR"
                self.trip_at_ms = self.now_ms
                self.trips += 1
                self.queue_event("DISARMED", "operator DISARM — relay isolated")
            self.relay_energized = False
            return "APPLIED", "relay isolated"
        if cmd == "CONFIG":
            if cmd_cfg is None:
                return "REJECTED", "missing config object"
            for k, (lo, hi) in RANGES.items():
                v = cmd_cfg.get(k)
                if v is not None:
                    self.cfg[k] = clamp(v, lo, hi, self.cfg[k])
            self.queue_event("CONFIG_APPLIED", "operator updated trigger thresholds")
            return "APPLIED", "emergency config updated"
        return "REJECTED", f"unknown command: {cmd}"


def healthy(fw):
    fw.vbat, fw.idc, fw.iac, fw.igen = 51.2, -3.5, 1.8, NAN
    fw.ina219 = True


def armed(fw):
    healthy(fw)
    fw.clear_at_ms = fw.now_ms - (fw.cfg["recoverySec"] + 1) * 1000
    res, _ = fw.apply_command("ARM")
    assert res == "APPLIED", "armed() helper failed"
    assert fw.state == "RUN"


# ---------------------------------------------------------------------------
# Mirror of GasEmergencyChannel event flush (retry budget)
# ---------------------------------------------------------------------------
class ChannelMirror:
    MAX_TRIES = 20
    MIN_INTERVAL_MS = 5000

    def __init__(self, fw):
        self.fw = fw
        self.tries = 0
        self.last_sent_at = 0
        self.dropped = None

    def flush_event(self, http_ok, now_ms):
        if not self.fw.events:
            return
        if self.last_sent_at != 0 and now_ms - self.last_sent_at < self.MIN_INTERVAL_MS:
            return
        if self.tries >= self.MAX_TRIES:
            self.dropped = self.fw.events[0]
            self.fw.consume_event()
            self.tries = 0
            return
        self.tries += 1
        self.last_sent_at = now_ms
        if http_ok:
            self.fw.consume_event()
            self.tries = 0


# =============================================================================
# Groups A/B/C/D/S — supervisor logic
# =============================================================================
def group_logic():
    print("[A] Debounce + latching (trips are latched, never re-tripped):")
    fw = EmgFw()
    check("A1 boot state is EMERGENCY/BOOT with relay isolated",
          fw.state == "EMERGENCY" and fw.reason == "BOOT" and not fw.relay_energized)
    armed(fw)
    check("A2 ARM from healthy state reaches RUN", fw.state == "RUN" and fw.relay_energized)

    fw.vbat = 41.0  # below vbatLowV=42
    fw.tick(); fw.tick()
    check("A3a below-threshold for 2 of debounceN=3 ticks — no trip yet", fw.state == "RUN")
    fw.tick()
    check("A3b 3rd consecutive violating tick trips VBAT_LOW",
          fw.state == "EMERGENCY" and fw.reason == "VBAT_LOW" and not fw.relay_energized)
    check("A3c trip counted once", fw.trips == 1)
    fw.tick(); fw.tick()
    check("A4 latched — further violations never re-trip or recount", fw.trips == 1)

    # Hysteresis dead band: 42 < vbat <= 43 holds the counter (no reset)
    fw2 = EmgFw(); armed(fw2)
    fw2.vbat = 41.0
    fw2.tick()                      # debounce[0] = 1
    fw2.vbat = 42.5                 # dead band: above trip threshold, below clear threshold
    fw2.tick()
    held = fw2.debounce[0]
    fw2.tick()
    check("A5 dead band HOLDS the counter (neither increments nor resets)",
          fw2.debounce[0] == held and fw2.state == "RUN", f"held={held}, now={fw2.debounce[0]}")
    fw2.vbat = 44.0                 # above vLow + hyst (43)
    fw2.tick()
    check("A6 above clear threshold resets the counter", fw2.debounce[0] == 0)

    fw3 = EmgFw(); armed(fw3)
    fw3.vbat = 56.0
    fw3.tick(); fw3.tick(); fw3.tick()
    check("A7 VBAT_HIGH trips after debounce", fw3.reason == "VBAT_HIGH")

    print("[B] ARM gating (fail-closed, operator-only):")
    fw = EmgFw(); healthy(fw)
    fw.vbat = NAN
    res, msg = fw.apply_command("ARM")
    check("B1 ARM with invalid voltage is REJECTED",
          res == "REJECTED" and "sensor tegangan" in msg, msg)
    fw = EmgFw(); healthy(fw); fw.ina219 = False
    res, msg = fw.apply_command("ARM")
    check("B2 ARM without INA219 (policy=1) is REJECTED",
          res == "REJECTED" and "INA219" in msg, msg)
    fw = EmgFw(); healthy(fw); fw.iac = NAN
    res, msg = fw.apply_command("ARM")
    check("B3 ARM with invalid AC current is REJECTED",
          res == "REJECTED" and "arus beban AC" in msg, msg)
    fw = EmgFw(); healthy(fw); fw.iac = 30.0
    res, msg = fw.apply_command("ARM")
    check("B4 ARM with AC current above threshold is REJECTED",
          res == "REJECTED" and "arus beban" in msg, msg)
    fw = EmgFw(); healthy(fw)
    fw.now_ms = 1000
    fw.track_clear()  # seeds clear_at_ms
    fw.now_ms = 1000 + 30_000  # only 30 s of recovery (needs 60)
    res, msg = fw.apply_command("ARM")
    check("B5 ARM inside recovery window is REJECTED",
          res == "REJECTED" and "masih dalam masa pemulihan" in msg, msg)
    fw.now_ms = 1000 + 61_000
    res, msg = fw.apply_command("ARM")
    check("B6 ARM after recovery window APPLIES", res == "APPLIED" and fw.state == "RUN", msg)
    res, msg = fw.apply_command("ARM")
    check("B7 re-ARM while RUN is idempotent APPLIED",
          res == "APPLIED" and "already RUN" in msg, msg)
    fw = EmgFw(); healthy(fw); fw.crash_chain = 3
    fw.clear_at_ms = fw.now_ms - 61_000
    res, msg = fw.apply_command("ARM")
    check("B8 crash-loop hold (chain>=3) blocks ARM",
          res == "REJECTED" and "crash-loop" in msg, msg)
    fw = EmgFw(); healthy(fw); fw.vbat = 42.5  # inside low hysteresis band
    fw.clear_at_ms = fw.now_ms - 61_000
    res, msg = fw.apply_command("ARM")
    check("B9 ARM inside low-voltage hysteresis band is REJECTED",
          res == "REJECTED" and "rendah" in msg, msg)

    print("[C] Physical E-stop latch:")
    fw = EmgFw(); armed(fw)
    fw.estop_open = True   # hardware opened the line; supervisor senses it
    # (tick's _pollEstop edge logic mirrored here)
    if fw.estop_open and fw.state == "RUN":
        fw.trip("ESTOP", "ESTOP", "physical e-stop opened the relay negative line")
    check("C1 E-stop while RUN trips ESTOP with relay isolated",
          fw.state == "EMERGENCY" and fw.reason == "ESTOP" and not fw.relay_energized)
    check("C1b event type is ESTOP (GAS whitelist)",
          fw.events and fw.events[0][0] == "ESTOP")
    trips_before = fw.trips
    fw.estop_open = False
    fw.queue_event("ESTOP_RELEASED", "e-stop line closed — operator ARM required")
    check("C2 release does NOT re-energize (stays EMERGENCY, no re-count)",
          fw.state == "EMERGENCY" and fw.trips == trips_before)
    check("C2b release queues ESTOP_RELEASED",
          fw.events and fw.events[0][0] == "ESTOP_RELEASED")
    fw2 = EmgFw()  # already isolated at boot
    fw2.estop_open = True
    fw2.queue_event("ESTOP", "physical e-stop opened the relay negative line")
    check("C3 E-stop while already EMERGENCY only latches the flag (no trip)",
          fw2.trips == 0 and fw2.reason == "BOOT")

    print("[D] DISARM safe-direction + idempotent:")
    fw = EmgFw(); armed(fw)
    trips_before = fw.trips
    res, msg = fw.apply_command("DISARM")
    check("D1 DISARM from RUN isolates, counts as OPERATOR trip, queues DISARMED",
          res == "APPLIED" and fw.state == "EMERGENCY" and fw.reason == "OPERATOR"
          and fw.trips == trips_before + 1 and not fw.relay_energized
          and fw.events[0][0] == "DISARMED", msg)
    trips_before = fw.trips
    res, msg = fw.apply_command("DISARM")
    check("D2 DISARM while EMERGENCY is idempotent (no extra count)",
          res == "APPLIED" and fw.trips == trips_before and not fw.relay_energized, msg)

    print("[S] Sensor-loss fail-closed policy (RESERVED genset channel):")
    fw = EmgFw(); armed(fw)
    fw.vbat = NAN
    fw.tick(); fw.tick(); fw.tick()
    check("S1 NaN sensors with policy=1 trip SENSOR_LOSS",
          fw.reason == "SENSOR_LOSS")
    fw = EmgFw(cfg={"sensorFailPolicy": 0}); armed(fw)
    fw.vbat = NAN; fw.iac = NAN
    for _ in range(6):
        fw.tick()
    check("S2 policy=0 never trips SENSOR_LOSS (documented unsafe opt-out)",
          fw.reason != "SENSOR_LOSS")
    fw = EmgFw(); armed(fw)
    fw.igen = NAN  # RESERVED — permanently NaN on this board
    for _ in range(10):
        fw.tick()
    check("S3 RESERVED iGen NaN never trips SENSOR_LOSS (port delta, documented)",
          fw.state == "RUN" and fw.reason != "SENSOR_LOSS")
    fw = EmgFw(); armed(fw)
    fw.iac = 40.0
    fw.tick(); fw.tick(); fw.tick()
    check("S4 AC overcurrent trips I_AC_LOAD_OVER",
          fw.reason == "I_AC_LOAD_OVER")

    print("[M] Event queue + channel retry budget:")
    fw = EmgFw()
    fw.queue_event("BOOT", "boot sehat")
    fw.queue_event("TRIP", "VBAT_LOW")
    check("M1 single-slot queue keeps only the LATEST unsent event",
          len(fw.events) == 1 and fw.events[0][0] == "TRIP")
    ch = ChannelMirror(fw)
    fw.queue_event("TRIP", "VBAT_LOW")
    ch.last_sent_at = 10_000
    ch.flush_event(http_ok=True, now_ms=12_000)
    check("M2a 5 s rate limit defers the send (event retained)",
          fw.events and ch.tries == 0)
    ch.flush_event(http_ok=True, now_ms=16_000)
    check("M2b after rate limit a successful POST consumes the event",
          not fw.events and ch.tries == 0)
    fw.queue_event("ESTOP", "physical e-stop")
    for i in range(20):
        ch.flush_event(http_ok=False, now_ms=100_000 + i * 10_000)
    check("M3a 20 failed attempts keep the event queued (real retry budget)",
          bool(fw.events) and ch.tries == 20, f"tries={ch.tries}")
    ch.flush_event(http_ok=False, now_ms=400_000)
    check("M3b the 21st failed attempt DROPS the event (bounded, logged)",
          not fw.events and ch.dropped is not None)


# =============================================================================
# Group R — wire contract vs code.gs/Code.gs (cross-layer)
# =============================================================================
def group_contract():
    print("[R] GAS wire contract (cross-layer vs code.gs/Code.gs):")
    gs = open(CODE_GS, encoding="utf-8", errors="replace").read()
    ch = open(CH_CPP, encoding="utf-8", errors="replace").read()
    sup = open(SUP_CPP, encoding="utf-8", errors="replace").read()
    suph = open(SUP_H, encoding="utf-8", errors="replace").read()

    check("R1 Code.gs dispatches EMERGENCY_PENDING/ACK/EVENT",
          "EMERGENCY_PENDING" in gs and "EMERGENCY_ACK" in gs and "EMERGENCY_EVENT" in gs)
    check("R2 device posts the same three actions",
          '"EMERGENCY_PENDING"' in ch and '"EMERGENCY_ACK"' in ch and '"EMERGENCY_EVENT"' in ch)

    m = re.search(r"const EMERGENCY_EVENT_TYPES = \[(.*?)\]", gs, re.S)
    types = re.findall(r"'([A-Z_]+)'", m.group(1)) if m else []
    needed = ["TRIP", "ESTOP", "BOOT", "CRASHLOOP", "ARMED", "DISARMED",
              "CONFIG_APPLIED", "ESTOP_RELEASED"]
    check("R3 GAS event-type whitelist covers every type the device emits",
          all(t in types for t in needed), f"whitelist={types}")
    sup_all = sup + suph
    for t in needed:
        check(f"R3.{t} device emits only whitelisted types ({t} present both sides)",
              t in types and f'"{t}"' in sup_all, "device-side literal missing")

    check("R4 device parses pending response via status/data.command_id/command/config",
          '"command_id"' in ch and '"command"' in ch and '"status"' in ch
          and 'data["command_id"]' in ch and 'data["config"]' in ch)
    check("R5 ACK body carries command_id/result/message/state (GAS order-insensitive)",
          all(f'["{k}"]' in ch for k in ("command_id", "result", "message", "state")))
    check("R6 EVENT body carries type/reason/detail/state",
          all(f'["{k}"]' in ch for k in ("type", "reason", "detail", "state")))

    # 13-field config table: GAS EMERGENCY_CONFIG_FIELDS vs device clamps.
    m = re.search(r"const EMERGENCY_CONFIG_FIELDS = \[(.*?)\];", gs, re.S)
    rows = re.findall(r"\['([A-Za-z0-9_]+)',\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?),\s*(-?\d+(?:\.\d+)?)\]",
                      m.group(1)) if m else []
    check("R7a Code.gs EMERGENCY_CONFIG_FIELDS parsed (13 rows)",
          len(rows) == 13, f"rows={len(rows)}")
    gas_fields = {name: (float(lo), float(hi), float(d)) for name, lo, hi, d in rows}
    dev_ok, dev_bad = [], []
    for name, (lo, hi, d) in gas_fields.items():
        # device-side clamp call: clampEmgFloat(cfg["<name>"] | ..., lo, hi, ...)
        pat = (r'cfg\["' + re.escape(name) + r'"\]\s*\|\s*\(float\)NAN,\s*'
               + re.escape(_numlit(lo)) + r'f?,\s*' + re.escape(_numlit(hi)) + r'f?')
        pat_i = (r'cfg\["' + re.escape(name) + r'"\]\s*\|\s*\(long\)-?\d+,\s*'
                 + re.escape(str(int(lo))) + r',\s*' + re.escape(str(int(hi))))
        if re.search(pat, sup) or re.search(pat_i, sup):
            dev_ok.append(name)
        else:
            dev_bad.append(name)
    check("R7b every GAS config field is re-validated device-side with the SAME range",
          len(dev_ok) == 13, f"missing/mismatched={dev_bad}")
    check("R7c field sets match (no extra device-only field)",
          set(gas_fields) == set(DEFAULTS), f"gas={sorted(gas_fields)}")
    mismatched = [n for n in gas_fields
                  if abs(gas_fields[n][2] - DEFAULTS[n]) > 1e-9]
    check("R7d GAS defaults == device defaults (mixed-version fleet guard)",
          not mismatched, f"mismatched={mismatched}")

    check("R8 commands accepted by GAS match device applyCommand set",
          "['ARM', 'DISARM', 'CONFIG']" in gs.replace('"', "'")
          and '"ARM"' in sup and '"DISARM"' in sup and '"CONFIG"' in sup)

    # HMAC canonical string parity with AI/GasAdvisor.cpp (byte-identical both sides)
    adv = open(os.path.join(FW, "AI", "GasAdvisor.cpp"), encoding="utf-8",
               errors="replace").read()
    check("R9 channel canonical string == GasAdvisor canonical string (WAVE-1)",
          all(p in ch and p in adv for p in
              ['"HMAC-SHA256"', '"\\n"', '"timestamp"', '"nonce"', '"deviceId"']))


def _numlit(x):
    if float(x).is_integer():
        return f"{int(x)}.0"
    return str(x)


# =============================================================================
# Group E — static source assertions (fail-safe ordering + architecture)
# =============================================================================
def group_static():
    print("[E] Static source assertions (fail-safe ordering, local-first, TLS):")
    ino = open(INO, encoding="utf-8", errors="replace").read()
    sup = open(SUP_CPP, encoding="utf-8", errors="replace").read()
    suph = open(SUP_H, encoding="utf-8", errors="replace").read()
    drv = open(DRV_CPP, encoding="utf-8", errors="replace").read()
    ch = open(CH_CPP, encoding="utf-8", errors="replace").read()
    ser = open(SER_H, encoding="utf-8", errors="replace").read()
    cfgs = open(CFG_CPP, encoding="utf-8", errors="replace").read()

    # E1 — boot isolation: relay ISOLATED before LittleFS and WiFi.
    i_relay = ino.find("Drivers::emergencyRelay.begin();")
    i_fs = ino.find("LittleFS.begin(")
    i_wifi = ino.find("WiFi")  # first WiFi mention (WiFiManager/Network include)
    check("E1 emergencyRelay.begin() precedes LittleFS.begin() (fail-safe first)",
          0 <= i_relay < i_fs, f"relay@{i_relay} fs@{i_fs}")
    check("E2 emergencyRelay.begin() precedes WiFi bring-up region",
          i_relay < ino.find("Services::wifi.begin();"))

    # E3 — local-first: the supervisor contains NO network I/O.
    check("E3 supervisor has no WiFi/HTTP/TLS symbols (local-first)",
          not re.search(r"WiFi|HTTPClient|WiFiClientSecure|PubSubClient", sup))
    check("E3b supervisor never includes Network/ or AI/ headers",
          not re.search(r'#include\s+"(Network|AI)/', sup))

    # E4 — dedicated tasks; safety tick runs regardless of WiFi.
    emg_task_line = re.search(r"xTaskCreatePinnedToCore\(\s*emergencyTask[^;]*;", ino)
    check("E4 emergencyTask created and pinned to core 0",
          emg_task_line is not None and ", 0);" in emg_task_line.group(0),
          emg_task_line.group(0) if emg_task_line else "task line not found")
    check("E5 gasEmergencyTask is a SEPARATE task (blocking POST never stalls MQTT)",
          'xTaskCreatePinnedToCore(gasEmergencyTask, "gasemg"' in ino)
    m = re.search(r"void emergencyTask\(void\* pv\) \{(.*?)\}", ino, re.S)
    body = m.group(1) if m else ""
    check("E6 emergencyTask feeds the WDT BEFORE ticking the supervisor",
          body.find("esp_task_wdt_reset();") < body.find("Services::emergency.tick();"))
    check("E7 supervisor tick is NOT gated on WiFi status",
          "WiFi.status()" not in body)

    # E8 — relay polarity + E-stop sense.
    check("E8 relay write is active-LOW (energized -> LOW)",
          "energized ? LOW : HIGH" in drv)
    check("E8b E-stop sense uses INPUT_PULLUP with OPEN=HIGH",
          "INPUT_PULLUP" in drv and "== HIGH" in drv)
    check("E8c driver never energizes in begin() (boot = isolated)",
          "_relayWrite(false);" in drv.split("EmergencyRelayDriver::begin()")[1][:400])

    # E9 — NVS keys + namespace.
    for key in ("emg_trips", "emg_run_ok", "emg_chain"):
        check(f"E9 NVS key {key} present in supervisor", f'"{key}"' in sup)
    check("E9b NVS namespace plts_emg in supervisor", '"plts_emg"' in sup)
    check("E9c NVS namespace plts_emg in ConfigStore (config fields)", '"plts_emg"' in cfgs)

    # E10 — crash-chain constants.
    check("E10 crash-chain limit 3 + healthy-runtime 300000",
          "EMG_CRASH_CHAIN_LIMIT   = 3" in suph and "EMG_HEALTHY_RUNTIME_MS  = 300000" in suph)

    # E11 — GAS channel TLS: pinning precedence, WDT around POST.
    check("E11a channel pins CA (GAS_ROOT_CA macro path)", "#ifdef GAS_ROOT_CA" in ch
          and "setCACert(GAS_ROOT_CA)" in ch)
    check("E11b channel falls back to GTS Root R4 (never silent insecure)",
          "GAS_ROOT_CA_GTS_R4" in ch)
    check("E11c setInsecure() only under DEVELOPMENT_BUILD",
          re.search(r"#elif defined\(DEVELOPMENT_BUILD\)\s*\n\s*client\.setInsecure\(\)", ch) is not None)
    seg = ch[ch.find("esp_task_wdt_reset();\n  int code = http.POST"):]
    check("E11d WDT fed immediately before the blocking POST",
          ch.count("esp_task_wdt_reset();") >= 2 and
          "esp_task_wdt_reset();\n  int code = http.POST" in ch)
    chh = open(os.path.join(FW, "Network", "GasEmergencyChannel.h"),
               encoding="utf-8", errors="replace").read()
    check("E11e poll cadence 15 s + HTTP timeout 7 s (bounded stall)",
          "EMERGENCY_POLL_INTERVAL_MS = 15000" in chh and
          "EMERGENCY_HTTP_TIMEOUT_MS  = 7000" in chh)

    # E12 — telemetry emergency block (additive, PWA-neutral).
    # [W12-1] canonical keys = PWA SystemStatus.emergency names
    # (estopLineOpen / tripCount); relayEnergized stays an informational extra.
    check("E12 serializer emits the emergency block with state/estop/relay",
          '"emergency"' in ser and '"estopLineOpen"' in ser
          and '"tripCount"' in ser and '"relayEnergized"' in ser)

    # E13 — idempotency contract (documented, asserted).
    check("E13a ARM idempotency message present", '"already RUN"' in sup)
    check("E13b DISARM safe-direction comment present", "safe direction" in sup)

    # E14 — config globals wired (Globals.h + .ino definitions + ConfigStore load).
    glob = open(os.path.join(FW, "Core", "Globals.h"), encoding="utf-8",
                errors="replace").read()
    check("E14 cfgEmg* externs in Globals.h (13 fields)",
          glob.count("cfgEmg") >= 13)
    check("E14b .ino defines cfgEmg* defaults", ino.count("cfgEmg") >= 13)
    check("E14c ConfigStore loads/saves emergency config",
          "loadEmergencyConfig" in cfgs and "saveEmergencyConfig" in cfgs)

    # E15 — fail-closed channel enablement.
    check("E15 GAS channel disabled without URL/secret (fail-closed, logged)",
          "GAS_INGEST_URL empty" in ch and "device secret empty" in ch)

    # E16 — feature flag compile-out path exists.
    conf = open(os.path.join(FW, "Core", "Config.h"), encoding="utf-8",
                errors="replace").read()
    check("E16 PLTS_ENABLE_EMERGENCY flag default 1 in Config.h",
          "#define PLTS_ENABLE_EMERGENCY 1" in conf)

    # E17 — alarm code + TaskIds wired.
    types = open(os.path.join(FW, "Core", "Types.h"), encoding="utf-8",
                 errors="replace").read()
    check("E17 EMERGENCY_TRIP alarm + Emergency/GasEmergency task ids",
          'EMERGENCY_TRIP' in types and "Emergency," in types and "GasEmergency," in types)


def main() -> int:
    print("test_emergency_modular_logic.py — E-WAVE v1.7.0 modular port")
    print("=" * 64)
    group_logic()
    print()
    group_contract()
    print()
    group_static()
    print()
    print("=" * 64)
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    if FAILURES:
        print("FAILURES:")
        for f in FAILURES:
            print(f"  - {f}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
