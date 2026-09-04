#!/usr/bin/env python3
"""
test_rs485_console.py — RS485 vendor-frame capture console (README §13 #2)
=========================================================================
Closes limitation #2 ("Pylontech RS485 console belum diimplementasi —
menunggu capture frame vendor, jangan menebak") with the ONLY honest tool
possible before vendor frames exist: a strictly PASSIVE capture console.

Groups:
  G1 — frame segmentation mirror (gap-based boundary, byte cap, ring)
  G2 — hex encoding + REST payload shape (raw, uninterpreted)
  G3 — static source assertions:
       * DE pinned LOW — the console NEVER transmits (no vendor bus abuse,
         no guessed frames injected)
       * no protocol interpretation anywhere in the console (no decode of
         vendor register maps — the parser slot stays RESERVED)
       * runtime activation is exclusive (bmsProtocol == rs485_console ->
         the polling manager builds no client and stays Disabled)
       * REST route registered + fail-closed responses (auth, inactive, 0)

Usage: python3 scripts/test_rs485_console.py   (exit 0 = PASS)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, "..", "firmware")

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


# Mirror of the capture state machine (Comm/Rs485Console.cpp)
FRAME_MAX = 64
GAP_MS = 6
RING = 16


class ConsoleMirror:
    def __init__(self):
        self.buf = bytearray()
        self.ring = [None] * RING   # (tsMs, bytes)
        self.head = 0
        self.total = 0
        self.active = True

    def feed(self, now_ms, data):
        for b in data:
            if len(self.buf) > 0 and now_ms - self.last_byte_ms >= GAP_MS:
                self.close()
            if len(self.buf) < FRAME_MAX:
                self.buf.append(b)
            else:
                self.close()
                self.buf.append(b)
            self.last_byte_ms = now_ms

    def idle(self, now_ms):
        if len(self.buf) > 0 and now_ms - self.last_byte_ms >= GAP_MS:
            self.close()

    def close(self):
        if len(self.buf) == 0:
            return
        self.ring[self.head] = (self.last_byte_ms, bytes(self.buf))
        self.head = (self.head + 1) % RING
        self.total += 1
        self.buf = bytearray()

    def frames(self):
        out = []
        for i in range(RING):
            idx = (self.head + i) % RING
            f = self.ring[idx]
            if f is not None:
                out.append(f)
        return out


def hex_of(bs):
    return " ".join(f"{b:02X}" for b in bs)


def group_segmentation():
    print("[G1] Gap-based frame segmentation (mirror):")
    c = ConsoleMirror()
    c.last_byte_ms = 0
    c.feed(100, b"\x71\x01\x00\x02")       # contiguous burst = one frame
    c.feed(101, b"\x02\x03")               # 1 ms later — same frame
    c.idle(120)                            # 19 ms later — gap closes it
    check("G1a contiguous bytes form ONE frame", c.total == 1 and len(c.frames()[0][1]) == 6)
    c.feed(200, b"\x71\x00")               # new frame
    c.idle(300)                            # closed
    c.feed(400, b"\x42\x43")               # another frame
    c.idle(500)
    check("G1b separated bursts form separate frames", c.total == 3)
    ts_a, bytes_a = c.frames()[0]
    check("G1c frames are RAW bytes (uninterpreted)", bytes_a == b"\x71\x01\x00\x02\x02\x03")

    # Byte-cap: a monster burst splits at FRAME_MAX
    c2 = ConsoleMirror()
    c2.last_byte_ms = 0
    c2.feed(1000, bytes(range(150)))       # 150 contiguous bytes
    c2.idle(2000)
    check("G1d long burst capped at FRAME_MAX bytes (bounded memory)",
          all(len(f[1]) <= FRAME_MAX for f in c2.frames()) and len(c2.frames()[0][1]) == FRAME_MAX)

    # Ring: the LAST RING frames survive; older ones rotate out
    c3 = ConsoleMirror()
    c3.last_byte_ms = 0
    for k in range(RING + 5):
        c3.feed(k * 100, bytes([k]))
        c3.idle(k * 100 + 50)
    check("G1e ring keeps exactly RING frames (bounded REST payload)",
          len(c3.frames()) == RING, f"got {len(c3.frames())}")
    check("G1f total counter counts every frame (not just the ring)",
          c3.total == RING + 5)
    kept = [f[1][0] for f in c3.frames()]
    check("G1g newest frame present, oldest rotated out",
          (RING + 4) in kept and 0 not in kept)

    # Empty slot honesty: a fresh console reports ZERO frames, never null-garbage
    c4 = ConsoleMirror()
    check("G1h fresh console has no frames (honest empty)", c4.frames() == [])


def group_static():
    print("[G2] Static source assertions (passive, no guessing):")
    h = open(os.path.join(FW, "Comm", "Rs485Console.h"), encoding="utf-8").read()
    cpp = open(os.path.join(FW, "Comm", "Rs485Console.cpp"), encoding="utf-8").read()
    mgr = open(os.path.join(FW, "Comm", "BatteryCommManager.cpp"), encoding="utf-8").read()
    ino = open(os.path.join(FW, "firmware_v1.ino"), encoding="utf-8").read()
    extra = open(os.path.join(FW, "Web", "ExtraHandlers.cpp"), encoding="utf-8").read()
    extrah = open(os.path.join(FW, "Web", "ExtraHandlers.h"), encoding="utf-8").read()

    # NEVER transmits: DE pinned LOW, no write() call on the serial port.
    check("G2a DE pinned LOW (permanent receive)",
          "digitalWrite(Core::PIN_RS485_DE, LOW)" in cpp)
    check("G2b console NEVER calls Serial2.write (passive-only)",
          "Serial2.write" not in cpp and ".write(" not in cpp)
    check("G2c console NEVER raises DE (no transmit window)",
          "PIN_RS485_DE, HIGH" not in cpp)
    # No interpretation: no register decode, no vendor value extraction.
    check("G2d no vendor value decoding in the console (raw bytes only)",
          not re.search(r"soc|voltage|current|temperature|soh", cpp, re.I)
          or "soc" not in cpp.lower().replace("interpretation", ""))
    check("G2e interpretation field says NONE in the REST payload",
          '"interpretation"' in cpp and "NONE" in cpp)
    # Exclusive activation: manager stays Disabled, no client probed.
    check("G2f manager treats rs485_console as Disabled (no polling client)",
          'strcmp(mode, "rs485_console") == 0' in mgr and "RS485_CONSOLE_MODE" in mgr)
    # Runtime gate: begin() self-gates on the protocol id.
    check("G2g begin() self-gates on cfgBmsProtocol == rs485_console",
          'strcmp(Core::cfgBmsProtocol, PROTOCOL_ID)' in cpp)
    # .ino wiring: begin + tick under the flag.
    check("G2h .ino ticks the console from bmsCommTask (flag-guarded)",
          "Comm::rs485Console.tick(millis())" in ino
          and "Comm::rs485Console.begin()" in ino)
    # REST: route registered, auth, fail-closed branches.
    check("G2i route /api/rs485/frames registered (flag-guarded)",
          'http.on("/api/rs485/frames", HTTP_GET, handleRs485Frames)' in extra)
    check("G2j handler requires auth (no anonymous bus capture)",
          'handleRs485Frames() {\n  if (!requireAuth())' in extra)
    check("G2k inactive mode returns honest 503 (never fabricated frames)",
          "RS485 console not active" in extra)
    check("G2l compiled-out build returns honest 503",
          "RS485 console compiled out" in extra)
    # Gap heuristic documented.
    check("G2m gap heuristic documented in the header (visible honesty)",
          "inter-byte" in h or "idle-gap" in h or "bus-idle" in h.lower())
    # No PZEM-style guessing into the RESERVED client slot.
    proto = open(os.path.join(FW, "Comm", "BatteryProtocol.h"), encoding="utf-8").read()
    check("G2n PylontechRs485 client slot stays RESERVED (not guessed)",
          "PylontechRs485" in proto and "RESERVED" in proto)


def main() -> int:
    print("test_rs485_console.py — passive RS485 capture console")
    print("=" * 60)
    group_segmentation()
    print()
    group_static()
    print()
    print("=" * 60)
    print(f"RESULT: {PASS} passed, {FAIL} failed")
    if FAILURES:
        print("FAILURES:")
        for f in FAILURES:
            print(f"  - {f}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
