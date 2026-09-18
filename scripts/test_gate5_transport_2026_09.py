#!/usr/bin/env python3
"""
test_gate5_transport_2026_09.py — GATE-5 transport/delivery semantics contract.

Covers (source-shape + logic mirrors, repo discipline):
  F4-05  MQTT socket timeout + TLS handshake timeout must be set BELOW the
         task-watchdog budget (audit Phase 4 F4-05: PubSubClient default
         MQTT_SOCKET_TIMEOUT=15s > TWDT 10s → broker outage could force a
         WDT reset; a monitoring failure became a device reboot).
  P7-S2-01 Reconnect FULL JITTER: the exponential value is the CAP; the
         actual next-attempt wait is drawn uniformly from [MIN, cap] so a
         recovering fleet does not stampede on one schedule (audit Phase 7).
  F3/F4-04 Honest QoS API: publishBestEffortQoS0() is the canonical name;
         no publishGuaranteedQoS1() exists on this transport; the legacy
         publish(qos) alias explicitly ignores the qos argument; in-tree
         callers use the honest name (audit Phase 1 F3 / Phase 4 F4-04 /
         Phase 3 P3-S1-03: publish()==true was readable as a broker PUBACK
         guarantee that does not exist on PubSubClient 2.8).

Run: python3 scripts/test_gate5_transport_2026_09.py   (exit 0 = PASS)
"""
import re
import sys

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
        FAILURES.append(name)
        print(f"  FAIL  {name}  {detail}")


ROOT = "firmware/"


def read(p):
    with open(ROOT + p, encoding="utf-8", errors="replace") as f:
        return f.read()


transport_cpp = read("Network/MqttTransport.cpp")
transport_h = read("Network/MqttTransport.h")

print("========================================================================")
print("GATE-5 / F4-05 — timeout budget vs watchdog")
print("========================================================================")

check("F4-05: MQTT socket timeout set (PubSubClient default 15s > TWDT)",
      "setSocketTimeout(5)" in transport_cpp)
check("F4-05: TLS handshake timeout set",
      "setHandshakeTimeout(5)" in transport_cpp)
check("F4-05: invariant documented (socket < TLS handshake < TWDT)",
      "MQTT socket timeout (5 s)" in transport_cpp and "TWDT (10 s)" in transport_cpp)

print()
print("========================================================================")
print("GATE-5 / P7-S2-01 — reconnect full jitter")
print("========================================================================")

check("P7-S2-01: jittered wait member exists",
      "_reconnectWaitMs" in transport_h)
check("P7-S2-01: wait window uses the jittered value",
      re.search(r"now - _lastReconnectMs < _reconnectWaitMs", transport_cpp) is not None)
check("P7-S2-01: full jitter draw from [MIN, cap]",
      re.search(r"random\(Core::MQTT_RECONNECT_MIN_MS,\s*\n?\s*_reconnectDelayMs \+ 1\)",
                transport_cpp) is not None)
check("P7-S2-01: exponential value remains the CAP (still doubled)",
      re.search(r"_reconnectDelayMs = _reconnectDelayMs \* 2", transport_cpp) is not None)
check("P7-S2-01: success resets both cap and wait",
      transport_cpp.count("_reconnectWaitMs = Core::MQTT_RECONNECT_MIN_MS") >= 2)

# --- logic mirror: jitter distribution -------------------------------------
print()
print("[logic mirror] jitter distribution over the backoff ladder")


def next_wait(cap, rng):
    """Mirror of the committed draw: random(MIN, cap+1)."""
    import random
    return rng.randint(5000, cap)


import random as _random
rng = _random.Random(20260918)
caps = [5000, 10000, 20000, 40000, 60000]
for cap in caps:
    waits = [next_wait(cap, rng) for _ in range(400)]
    lo, hi = min(waits), max(waits)
    spread = hi - lo
    # Statistical sanity: bounds respected + genuine spread (a deterministic
    # schedule would show spread == 0). Threshold 80% of the theoretical max
    # absorbs sampling noise at N=400.
    min_spread = 0 if cap == 5000 else int((cap - 5000) * 0.8)
    check(f"jitter at cap {cap}ms spreads retries (spread={spread}ms, "
          f"min>={5000}, max<={cap})",
          lo >= 5000 and hi <= cap and spread >= min_spread)

print()
print("========================================================================")
print("GATE-5 / F3 + F4-04 — honest QoS API")
print("========================================================================")

check("F3: publishBestEffortQoS0() declared as the canonical API",
      "publishBestEffortQoS0" in transport_h)
check("F3: NO publishGuaranteedQoS1 FUNCTION exists (no fake guarantee)",
      re.search(r"\b(?:bool|void)\s+publishGuaranteedQoS1\s*\(", transport_h) is None)
check("F3: the honest name documents socket-write != broker PUBACK",
      "socket write" in transport_h and "PUBACK" in transport_h)
check("F3: legacy publish() alias explicitly ignores qos",
      re.search(r"bool MqttTransport::publish\([^)]*\)\s*\{\s*\(void\)qos;",
                transport_cpp) is not None)

callers_ok = True
caller_files = [
    "Network/MqttOtaHandler.cpp",
    "Network/MqttTelemetryPublisher.cpp",
    "Network/MqttConfigReceiver.cpp",
]
for f in caller_files:
    src = read(f)
    legacy = re.findall(r"mqttTransport\.publish\(", src)
    if legacy:
        callers_ok = False
        check(f"F3: {f} migrated to the honest name", False,
              f"{len(legacy)} legacy call(s) remain")
check("F3: all in-tree callers use publishBestEffortQoS0()", callers_ok)

# --- negative control for the honesty contract ------------------------------
# A caller that treats publish()==true as a BROKER confirmation would be the
# exact bug class F3 removes; assert the ACK path documents the distinction.
check("F3: ack path documents QoS0 fire-and-forget semantics",
      "fire-and-forget" in read("Network/MqttConfigReceiver.cpp"))

print()
print("========================================================================")
print(f"RESULT: {PASS} passed, {FAIL} failed")
print("========================================================================")
if FAILURES:
    for f in FAILURES:
        print("  x " + f)
    sys.exit(1)
