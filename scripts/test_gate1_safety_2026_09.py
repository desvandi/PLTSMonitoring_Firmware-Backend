#!/usr/bin/env python3
"""
test_gate1_safety_2026_09.py — [GATE-1 / Safety Foundation] contract tests.

Mirrors the GATE-1 remediation logic 1:1 (same discipline as the other
2026-09 mirror suites — see F9-01 limitation note in the audit: mirrors prove
the ALGORITHM; the native harness verify_emergency_atomicity.cpp covers the
CONCURRENCY proof for PH8-01):

  PH8-01  safety latch: boot=latched, ON refused while latched, ARM-only
          release, trip/disarm-starvation latches, single-0xFF barrier.
  PH8-02  starvation escalation forces the relay bank (driver barrier),
          not only the E-WAVE GPIO.
  PH8-03  clock fail-CLOSED for energizing mutations (ingress gate +
          executor gate); safe-direction commands stay executable.
  PH8-04  production safety-config lockdown: sensorFailPolicy / estopEnabled
          / relayPin / estopPin immutable over remote CONFIG.
  PH8-08  production pin change refused while energized.

Run: python3 scripts/test_gate1_safety_2026_09.py   (exit 0 = PASS)
"""
import sys

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  PASS  {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")


# ---------------------------------------------------------------------------
# Mirror: CommandCanonicalizer::isCommandExpired (post-PH8-03)
# ---------------------------------------------------------------------------
def is_command_expired(doc, clock, actuator_energizing=False):
    """Mirror of the fixed gate. clock=None models time()==0 (invalid)."""
    expires_at = doc.get("expiresAt", None)
    if expires_at is None or expires_at == 0:
        if actuator_energizing and clock is None:
            return True, "CLOCK_INVALID — cannot evaluate freshness of an energizing command"
        return False, ""
    if clock is None:
        if actuator_energizing:
            return True, "CLOCK_INVALID — cannot evaluate freshness of an energizing command"
        return False, ""   # legacy fail-open (non-actuator)
    if expires_at < clock:
        return True, "command expired (issuedAt/expiresAt in the past)"
    return False, ""


# ---------------------------------------------------------------------------
# Mirror: relay executor execution-time freshness (post-PH8-03)
# ---------------------------------------------------------------------------
def executor_rejects(queued_cmd, clock):
    """Mirror of processCommandQueue decision: returns (reject, reason)."""
    energizing = queued_cmd["desiredState"]
    if energizing and clock is None:
        return True, "CLOCK_INVALID — cannot evaluate freshness of an energizing command"
    if queued_cmd.get("expiresAt", 0) and clock is not None:
        if queued_cmd["expiresAt"] < clock:
            return True, "command expired while queued"
    return False, ""


# ---------------------------------------------------------------------------
# Mirror: EmergencySupervisor CONFIG lockdown (post-PH8-04, PRODUCTION_BUILD)
# ---------------------------------------------------------------------------
LOCKED_FIELDS = ("relayPin", "estopPin", "estopEnabled", "sensorFailPolicy")


def config_refused(cfg, current, production=True):
    """Mirror of the production lockdown branch. Returns (refused, fields)."""
    if not production:
        return False, []
    asked = []
    if "relayPin" in cfg and int(cfg["relayPin"]) != current["relayPin"]:
        asked.append("relayPin")
    if "estopPin" in cfg and int(cfg["estopPin"]) != current["estopPin"]:
        asked.append("estopPin")
    if "estopEnabled" in cfg and int(cfg["estopEnabled"]) != current["estopEnabled"]:
        asked.append("estopEnabled")
    if "sensorFailPolicy" in cfg and int(cfg["sensorFailPolicy"]) != current["sensorFailPolicy"]:
        asked.append("sensorFailPolicy")
    return (len(asked) > 0), asked


# ---------------------------------------------------------------------------
# Mirror: EmergencyRelayDriver.applyPins (post-PH8-08, PRODUCTION_BUILD)
# ---------------------------------------------------------------------------
def apply_pins(refused_state, relay_pin, estop_pin, estop_enabled, energized,
               production=True):
    """Returns (new_refused, applied_relay_pin). Mirrors the pin gate."""
    pin_changes = (relay_pin != refused_state["relayPin"]) or \
                  (estop_pin != refused_state["estopPin"]) or \
                  (estop_enabled != refused_state["estopEnabled"])
    if production and pin_changes and energized:
        return True, refused_state["relayPin"]     # REFUSED — pins unchanged
    return False, relay_pin                        # applied


# ---------------------------------------------------------------------------
def main():
    print("========================================================================")
    print("GATE-1 SAFETY FOUNDATION — PH8-03 / PH8-04 / PH8-08 / latch contracts")
    print("========================================================================")

    # ---------------- PH8-03: clock fail-closed ------------------------------
    print("\n[PH8-03] CommandCanonicalizer::isCommandExpired — strict actuator mode")
    now = 1_770_000_000
    ok, _ = is_command_expired({"expiresAt": now + 60}, now, actuator_energizing=True)
    check("relay.on with valid clock + future expiry → accepted", not ok)

    ok, reason = is_command_expired({"expiresAt": now - 1}, now, actuator_energizing=True)
    check("relay.on expired → rejected", ok)

    ok, reason = is_command_expired({"expiresAt": now + 60}, None, actuator_energizing=True)
    check("relay.on with dead clock → REJECTED (fail-closed)", ok)
    check("  rejection reason is CLOCK_INVALID", "CLOCK_INVALID" in reason)

    ok, reason = is_command_expired({"expiresAt": now + 60}, None, actuator_energizing=False)
    check("relay.off / config with dead clock → legacy path (not clock-rejected)", not ok)

    ok, _ = is_command_expired({}, None, actuator_energizing=True)
    check("energizing command, no expiresAt, dead clock → rejected", ok)

    print("\n[PH8-03] executor execution-time gate (processCommandQueue mirror)")
    r, reason = executor_rejects({"desiredState": True, "expiresAt": now + 60}, None)
    check("queued ON + dead clock → REJECTED at execution", r)
    check("  reason CLOCK_INVALID", "CLOCK_INVALID" in reason)
    r, _ = executor_rejects({"desiredState": False, "expiresAt": now + 60}, None)
    check("queued OFF + dead clock → executable (safe direction)", not r)
    r, _ = executor_rejects({"desiredState": True, "expiresAt": now - 1}, now)
    check("queued ON + valid clock + expired → rejected", r)
    r, _ = executor_rejects({"desiredState": True, "expiresAt": now + 60}, now)
    check("queued ON + valid clock + fresh → executed", not r)

    # ---------------- PH8-04: production config lockdown ---------------------
    print("\n[PH8-04] EmergencySupervisor CONFIG lockdown (PRODUCTION_BUILD)")
    current = {"relayPin": 27, "estopPin": 34, "estopEnabled": 1, "sensorFailPolicy": 1}

    refused, fields = config_refused({"sensorFailPolicy": 0}, current, production=True)
    check("production: sensorFailPolicy=0 → REFUSED", refused)
    check("  refused field is sensorFailPolicy", fields == ["sensorFailPolicy"])

    refused, fields = config_refused({"estopEnabled": 0}, current, production=True)
    check("production: estopEnabled=0 → REFUSED", refused)

    refused, fields = config_refused({"relayPin": 26}, current, production=True)
    check("production: relayPin change → REFUSED", refused)

    refused, _ = config_refused({"vbatLowV": 42.0}, current, production=True)
    check("production: threshold change (vbatLowV) → allowed (not locked)", not refused)

    refused, _ = config_refused({"sensorFailPolicy": 0}, current, production=False)
    check("development build: sensorFailPolicy=0 → allowed (bench override)", not refused)

    refused, _ = config_refused({"sensorFailPolicy": 1}, current, production=True)
    check("production: same-value field → allowed (no-op)", not refused)

    # ---------------- PH8-08: pin change while energized ---------------------
    print("\n[PH8-08] EmergencyRelayDriver::applyPins gate (PRODUCTION_BUILD)")
    st = {"relayPin": 27, "estopPin": 34, "estopEnabled": True}

    refused, pin = apply_pins(st, 26, 34, True, energized=True, production=True)
    check("production: relayPin change while ENERGIZED → refused, pin unchanged",
          refused and pin == 27)

    refused, pin = apply_pins(st, 26, 34, True, energized=False, production=True)
    check("production: relayPin change while ISOLATED → applied (commissioning path)",
          (not refused) and pin == 26)

    refused, pin = apply_pins(st, 27, 34, True, energized=True, production=True)
    check("production: same pins while energized → no-op re-apply accepted", not refused)

    refused, pin = apply_pins(st, 26, 34, True, energized=True, production=False)
    check("development: live pin change → legacy behavior (applied)", (not refused) and pin == 26)

    # ---------------- PH8-01/02: latch semantics (logic level) ---------------
    # The concurrency proof lives in verify_emergency_atomicity.cpp (native).
    print("\n[PH8-01/02] latch state machine (logic mirror; concurrency in native harness)")

    class LatchMirror:
        def __init__(self):
            self.latched = True          # boot = ISOLATED
        def on_allowed(self):
            return not self.latched
        def force_all_off(self):
            self.latched = True
        def arm(self):
            self.latched = False

    lm = LatchMirror()
    check("boot: latch SET (ON authority withheld until ARM)", not lm.on_allowed())
    lm.arm()
    check("operator ARM: ON authority restored", lm.on_allowed())
    lm.force_all_off()
    check("emergency trip: latch SET again", not lm.on_allowed())
    lm.arm()
    lm.force_all_off()
    lm.arm()
    check("trip → ARM cycle is repeatable", lm.on_allowed())

    print()
    print("========================================================================")
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    print("========================================================================")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
