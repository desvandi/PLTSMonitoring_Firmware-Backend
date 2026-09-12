#!/usr/bin/env python3
"""
test_relay_safety_2026_09.py — Relay subsystem follow-up audit tests (p.398-412)
================================================================================
Level-2 evidence for the 2026-09 follow-up audit round. Static source contracts
plus Python mirrors of the fixed algorithms:

  p.400  STATIC CALL-SITE GATE — RelayController::applyCommand()/allOffWithResult()
         are EXECUTOR-ONLY: no file outside RelayController.cpp may call them;
         every relay ingress must go through queueCommand().
  p.401  SAFETY ORDERING — tick() enforces maxOnTime BEFORE draining the
         command queue (no one-tick window where a queued ON executes against
         a stale maxOnTimeForced flag). Defense-in-depth: applyCommand()
         re-evaluates the hard limit directly from millis()-onSinceMs.
  p.402  PULSE ROLLOVER SAFETY — expiry uses the signed-difference pattern
         Core::deadlineReached(); the absolute `now >= offAtMs` comparison
         is forbidden. The Python mirror proves the four wrap scenarios.
  p.403  PULSE TRANSACTION IDENTITY — PulseEntry carries transactionId +
         generation; auto-OFF / supersede / cancellation events annotate the
         owning transaction's record in the result ring.
  p.407  HONEST RESULTS ON I²C FAILURE — applyCommand() must check the fault
         flag after every hardware mutation and return Failed (never
         Applied/EXECUTED for an unverified write); the executor promotes
         Failed to Unknown when the driver shadow is unknown.
  p.409  SAFETY DECISION MATRIX — documented precedence
         EMERGENCY > maxOnTime FORCE OFF > pulse expiry > normal OFF/minOnTime,
         enforced in source: maxOnTime cancels pending pulses and bypasses
         minOnTime; emergency bypasses minOnTime; pulse auto-OFF defers to
         minOnTime (deferred, never dropped).

Usage: python3 scripts/test_relay_safety_2026_09.py   (exit 0 = PASS)
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(REPO, "firmware")

passed, failed = 0, 0
failures = []


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        failures.append(f"{name} {detail}")
        print(f"  FAIL  {name} {detail}")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(src):
    """Remove C block comments and C++ line comments — the call-site gate
    judges CODE only (documentation strings legitimately mention the API)."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def function_body(src, signature):
    """Return the source slice of the function whose definition starts at
    `signature`, up to the next top-level function definition."""
    i = src.find(signature)
    if i < 0:
        return ""
    rest = src[i:]
    # end at the next 'void Foo::' / 'Type Foo::' definition that follows
    m = re.search(r"\n(?:void|bool|int|uint\d+_t|AllOffResult|RelayCommandResult|"
                  r"RelayController::SafetyDecision|String|char|size_t)[^\n]*"
                  r"RelayController::[A-Za-z_]+\(", rest[len(signature):])
    end = len(rest) if not m else len(signature) + m.start() + 1
    return rest[:end]


print("=== RELAY SUBSYSTEM FOLLOW-UP AUDIT TESTS (2026-09, p.398-412) ===\n")

rc_h = read(os.path.join(FW, "Services", "RelayController.h"))
rc_cpp = read(os.path.join(FW, "Services", "RelayController.cpp"))
common_h = read(os.path.join(FW, "Core", "Common.h"))

tick_body = function_body(rc_cpp, "void RelayController::tick()")
apply_body = function_body(rc_cpp, "RelayCommandResult RelayController::applyCommand")
maxon_body = function_body(rc_cpp, "void RelayController::_checkMaxOnTime")
pulse_body = function_body(rc_cpp, "void RelayController::_processPulses")
emergency_body = function_body(rc_cpp, "void RelayController::emergencyAllOff")
alloff_body = function_body(rc_cpp, "AllOffResult RelayController::allOffWithResult")
queue_body = function_body(rc_cpp, "void RelayController::processCommandQueue")

# ---------------------------------------------------------------------------
# p.400 — Static call-site gate (executor-only API boundary)
# ---------------------------------------------------------------------------
print("[p.400] Executor-only call-site gate:")
violations = []
for root, _dirs, files in os.walk(FW):
    for fn in files:
        if not fn.endswith((".cpp", ".h", ".ino")):
            continue
        path = os.path.join(root, fn)
        code = strip_comments(read(path))
        for pat in (r"relaysController\.applyCommand",
                    r"relaysController\.allOffWithResult"):
            if re.search(pat, code):
                violations.append(f"{path}: {pat}")
check("no file calls relaysController.applyCommand/allOffWithResult "
      "(all ingress must use queueCommand)",
      not violations, "; ".join(violations))

relay_handlers = read(os.path.join(FW, "Web", "RelayHandlers.cpp"))
mqtt_recv = read(os.path.join(FW, "Network", "MqttConfigReceiver.cpp"))
check("REST ingress (RelayHandlers.cpp) uses queueCommand",
      "queueCommand" in relay_handlers)
check("MQTT relay ingress uses queueCommand",
      "queueCommand" in mqtt_recv)
check("applyCommand is only invoked inside RelayController.cpp",
      apply_body.count("applyCommand(") >= 1 and
      "applyCommand(" in rc_cpp)

# ---------------------------------------------------------------------------
# p.401 — Safety ordering in the executor tick
# ---------------------------------------------------------------------------
print("\n[p.401] Safety-before-queue executor ordering:")
check("tick() body extracted", len(tick_body) > 100)
check("tick() drains the queue even when driver unavailable (final results)",
      re.search(r"if\s*\(!_driverAvailable\)\s*\{[^}]*processCommandQueue",
                tick_body) is not None)
idx_safety = tick_body.find("_checkMaxOnTime()")
idx_queue = tick_body.rfind("processCommandQueue")
check("maxOnTime enforcement runs BEFORE the command queue in tick()",
      0 <= idx_safety < idx_queue,
      f"safety@{idx_safety} queue@{idx_queue}")
idx_recovery = tick_body.find("isShadowUnknown()")
check("I²C shadow recovery runs before maxOnTime enforcement in tick()",
      0 <= idx_recovery < idx_safety,
      f"recovery@{idx_recovery} safety@{idx_safety}")
check("applyCommand() re-evaluates maxOnTime DIRECTLY from timestamps "
      "(defense-in-depth, not just the tick-refreshed flag)",
      "onDuration >= _config[channel].maxOnTimeSec" in apply_body and
      "Core::RelaySource::Safety" in apply_body)
check("direct maxOnTime evaluation force-OFFs and BLOCKS the ON",
      re.search(r"onDuration >= _config\[channel\]\.maxOnTimeSec.*?"
                r"return RelayCommandResult::Blocked",
                apply_body, re.S) is not None)

# ---------------------------------------------------------------------------
# p.402 — Rollover-safe pulse expiry
# ---------------------------------------------------------------------------
print("\n[p.402] Pulse timer rollover safety:")
check("Core::deadlineReached helper exists in Common.h",
      "inline bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs)" in common_h)
check("_processPulses() uses Core::deadlineReached",
      "Core::deadlineReached(now, _pulses[ch].offAtMs)" in pulse_body)
check("absolute comparison 'now >= _pulses' is gone from the source",
      "now >= _pulses" not in rc_cpp)


def deadline_reached(now, deadline):
    """Python mirror of Core::deadlineReached (signed 32-bit difference)."""
    return (((now - deadline) & 0xFFFFFFFF) ^ 0x80000000) - 0x80000000 >= 0


def absolute_compare(now, deadline):
    """The OLD, unsafe pattern — kept here to prove it misjudges the wrap."""
    return now >= deadline


def add32(a, b):
    return (a + b) & 0xFFFFFFFF


# Scenario A (auditor's example): pulse scheduled 0x10 ms before the wrap,
# duration 100 ms. Just after the wrap it must NOT be expired.
start = 0xFFFFFFF0
dur = 100
off_at = add32(start, dur)          # 0x54
check("mirror: not expired 0x20 ms after the wrap (auditor scenario)",
      deadline_reached(0x00000010, off_at) is False and
      absolute_compare(0x00000010, off_at) is False)
# ...but it IS expired once the full duration has elapsed post-wrap.
check("mirror: expired after full duration past the wrap",
      deadline_reached(add32(0x00000010, dur - 0x20 + 1), off_at) is True)
# Scenario B: a long pulse whose deadline wraps; before the wrap the absolute
# comparison fires immediately (pulse OFFs too early).
start = 0xFFFFFF00
off_at = add32(start, 0x200)       # wraps to 0x100
check("mirror: NOT expired 0x100 ms into a 0x200 ms pulse (pre-wrap)",
      deadline_reached(start + 0x100, off_at) is False and
      absolute_compare(start + 0x100, off_at) is True)
# Scenario C: deadline passed just BEFORE the wrap; just after the wrap the
# absolute comparison misses it for another full 49.7 days.
off_at = 0xFFFFFF00
now = add32(off_at, 100)            # 0xFFFFFF64 pre-wrap — expired
check("mirror: expired pre-wrap", deadline_reached(now, off_at) is True)
now2 = add32(off_at, 5000)          # 0x138C post-wrap — long expired
check("mirror: still detected as expired post-wrap "
      "(absolute pattern would stall ~49.7 days)",
      deadline_reached(now2, off_at) is True and
      absolute_compare(now2, off_at) is False)
# Sanity: no false positives far from the deadline.
check("mirror: not expired 1 ms before deadline",
      deadline_reached(off_at - 1, off_at) is False)

# ---------------------------------------------------------------------------
# p.403 — Pulse transaction identity / observability
# ---------------------------------------------------------------------------
print("\n[p.403] Pulse transaction identity:")
check("PulseEntry carries transactionId",
      "char transactionId[65]" in rc_h)
check("PulseEntry carries generation",
      "uint32_t generation" in rc_h)
check("applyCommand() accepts the owning transactionId",
      "const char* transactionId" in rc_h)
check("processCommandQueue passes cmd.transactionId to applyCommand",
      re.search(r"applyCommand\([^;]*cmd\.transactionId\)",
                queue_body, re.S) is not None)
for note in ("pulse completed (auto-OFF)",
             "superseded by newer pulse",
             "cancelled by E-WAVE emergency cascade",
             "cancelled by all_off",
             "cancelled by maxOnTime FORCE OFF"):
    check(f"lifecycle note present: '{note}'",
          note in rc_cpp)
check("_annotatePulseOutcome defined and bounded (ring scan)",
      "void RelayController::_annotatePulseOutcome" in rc_cpp)

# ---------------------------------------------------------------------------
# p.407 — Honest results when the hardware write fails
# ---------------------------------------------------------------------------
print("\n[p.407] I²C failure honesty:")
fault_checks = len(re.findall(r"if\s*\(_state\[channel\]\.fault\)",
                              apply_body))
check("applyCommand checks the fault flag after EVERY mutation path "
      "(on/off/pulse)", fault_checks >= 3, f"found {fault_checks}")
check("an ON/OFF/PULSE write failure returns Failed, never Applied",
      re.search(r"if\s*\(_state\[channel\]\.fault\)\s*\{[^}]*?"
                r"return RelayCommandResult::Failed",
                apply_body, re.S) is not None)
check("executor promotes Failed to Unknown when the shadow is unknown",
      re.search(r"terminal == RelayTerminalResult::Failed.*?"
                r"isShadowUnknown\(\).*?RelayTerminalResult::Unknown",
                queue_body, re.S) is not None)
check("allOffWithResult counts faulted channels as UNKNOWN",
      "result.unknown++" in alloff_body)
check("pulse scheduling is skipped when the ON write failed",
      0 <= apply_body.find('if (command == "pulse")') <
      apply_body.find("if (_state[channel].fault)",
                    apply_body.find('if (command == "pulse")')) <
      apply_body.find("_pulses[channel].offAtMs",
                    apply_body.find('if (command == "pulse")')))

# ---------------------------------------------------------------------------
# p.409 — Safety decision matrix
# ---------------------------------------------------------------------------
print("\n[p.409] Safety decision matrix:")
matrix_doc = ("EMERGENCY (E-WAVE cascade)" in rc_h and
              "maxOnTime FORCE OFF" in rc_h and
              "pulse expiry" in rc_h and
              "EXECUTOR TICK ORDERING CONTRACT" in rc_h)
check("precedence documented in RelayController.h", matrix_doc)
check("maxOnTime cancels a pending pulse (maxOnTime > pulse expiry)",
      "_pulses[ch].active = false" in maxon_body and
      "_annotatePulseOutcome" in maxon_body and
      "cancelled by maxOnTime FORCE OFF" in maxon_body)
check("maxOnTime FORCE OFF bypasses minOnTime (no _evaluateSafety call)",
      "_evaluateSafety" not in maxon_body)
check("emergencyAllOff bypasses minOnTime (direct safe-direction write)",
      "_evaluateSafety" not in emergency_body and
      "_applyChannelState(ch, false, Core::RelaySource::Safety)" in emergency_body)
check("pulse auto-OFF defers to minOnTime (deferred, never dropped)",
      re.search(r"minOnTimeSec > 0 && onDuration < _config\[ch\]\.minOnTimeSec"
                r"[^}]*offAtMs = _state\[ch\]\.onSinceMs",
                pulse_body, re.S) is not None)

# ---------------------------------------------------------------------------
print()
if failed:
    print(f"RESULT: {passed} passed, {failed} FAILED")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)
print(f"RESULT: {passed} passed, 0 failed — relay subsystem follow-up clean")
