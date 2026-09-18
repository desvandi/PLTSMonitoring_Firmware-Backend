#!/usr/bin/env python3
"""
test_gate1b_commloss_2026_09.py — [GATE-1b / PH8-05] contract tests.

Mirrors the PH8-05 communication-loss fail-safe remediation logic 1:1 (same
discipline as the 2026-09 mirror suites — mirrors prove the ALGORITHM; the
physical HIL acceptance [WiFi OFF / MQTT OFF per channel CH0-CH7] is the
Gate 9 runtime qualification):

  PH8-05  per-channel fail-safe policy on communication loss:
    - commLossPolicy enum: OFF (default) / HOLD_SAFE
    - command lease: ON persists at most commandLeaseSec beyond authority
      loss; executed operator commands (incl. local REST during an outage)
      re-extend the lease (operator-presence evidence)
    - policy OFF  -> lease expiry => FORCE OFF (safety grade, bypasses
      minOnTime, cancels pending pulse; source=COMM_LOSS)
    - policy HOLD -> hold + ONE-SHOT warning per episode (visible, not
      silent)
    - NOT a global kill-switch: a HOLD channel stays ON while an OFF
      channel trips in the same episode (negative control for the audit's
      "Jangan membuat MQTT disconnected -> semua relay OFF" distinction)
    - authority restore => NO automatic re-energize; commLossForced stays
      as honest visibility until a NEW command executes
    - production guard: setChannelConfig refuses HOLD_SAFE without
      PLTS_ALLOW_HOLD_SAFE; lease bounded 0..86400; NVS-corrupt policy
      values fall back to OFF (fail-closed)
    - lease 0 = immediate (fail-closed convention; the OPPOSITE of
      maxOnTime's 0=unlimited, by design)

Run: python3 scripts/test_gate1b_commloss_2026_09.py   (exit 0 = PASS)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

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
# SOURCE-SHAPE CONTRACTS (structure present in the production source)
# ---------------------------------------------------------------------------
print("[S] Source-shape — PH8-05 structures")

h = (FW / "Services" / "RelayController.h").read_text(encoding="utf-8", errors="replace")
cpp = (FW / "Services" / "RelayController.cpp").read_text(encoding="utf-8", errors="replace")
ino = (FW / "firmware_v1.ino").read_text(encoding="utf-8", errors="replace")
types = (FW / "Core" / "Types.h").read_text(encoding="utf-8", errors="replace")
cfg = (FW / "Core" / "Config.h").read_text(encoding="utf-8", errors="replace")
web = (FW / "Web" / "RelayHandlers.cpp").read_text(encoding="utf-8", errors="replace")

check("S1  enum RelayFailSafePolicy {Off, HoldSafe} ada di RelayController.h",
      "enum class RelayFailSafePolicy" in h and "HoldSafe" in h)
check("S2  RelayChannelConfig memuat commLossPolicy + commandLeaseSec",
      "commLossPolicy" in h and "commandLeaseSec" in h)
check("S3  default production = OFF (fail-closed)",
      re.search(r"commLossPolicy\s*=\s*RelayFailSafePolicy::Off", h) is not None)
check("S4  default lease 900s dideklarasikan di Core/Config.h",
      "RELAY_DEFAULT_COMMAND_LEASE_SEC" in cfg and "900" in cfg)
check("S5  RelayChannelState memuat commLossForced + lastCommandAtMs",
      "commLossForced" in h and "lastCommandAtMs" in h)
check("S6  setCommandAuthority + isCommandAuthorityLost + lostSinceMs API publik",
      "setCommandAuthority" in h and "isCommandAuthorityLost" in h and
      "commandAuthorityLostSinceMs" in h)
check("S7  _authorityLost atomic (cross-core single-flag, single-writer episode)",
      "std::atomic<bool> _authorityLost" in h)
check("S8  _checkCommLossFailSafe dipanggil di tick() SETELAH _checkMaxOnTime "
      "dan SEBELUM processCommandQueue",
      re.search(r"_checkMaxOnTime\(\);.*?_checkCommLossFailSafe\(\);.*?processCommandQueue\(\);",
                cpp, re.S) is not None)
check("S9  networkTask memberi makan authority DI LUAR guard WiFi "
      "(WiFi down = authority lost segera)",
      re.search(r"setCommandAuthority\(\s*WiFi\.status\(\) == WL_CONNECTED &&",
                ino) is not None and
      ino.count("setCommandAuthority") == 1)
check("S10 RelaySource::CommLoss + string COMM_LOSS di Core/Types.h",
      "CommLoss" in types and '"COMM_LOSS"' in types)
check("S11 serializeStatus mengekspos commLossPolicy/commandLeaseSec/commLossForced",
      all(k in cpp for k in ('"commLossPolicy"', '"commandLeaseSec"', '"commLossForced"')))
check("S12 GET /api/relays mengekspos commandAuthorityLost(+SinceMs)",
      '"commandAuthorityLost"' in web)
check("S13 guard produksi HOLD_SAFE (PRODUCTION_BUILD && !PLTS_ALLOW_HOLD_SAFE) "
      "di setChannelConfig",
      re.search(r"#if defined\(PRODUCTION_BUILD\) && !defined\(PLTS_ALLOW_HOLD_SAFE\)",
                cpp) is not None and
      "HOLD_SAFE" in cpp.split("setChannelConfig")[1][:2000])
check("S14 lease di-clamp 0..86400 (setChannelConfig + _loadConfig)",
      cpp.count("86400") >= 2)
check("S15 NVS keys fsP + lease dipersist (save & load)",
      '"fsP"' in cpp and '"lease"' in cpp)
check("S16 NVS-corrupt policy value > HoldSafe -> fallback OFF",
      re.search(r"commLossPolicy\s*=\s*RelayFailSafePolicy::Off", cpp) is not None)
check("S17 command Applied (on/off/pulse/all_off) merefresh lastCommandAtMs "
      "+ clear commLossForced",
      cpp.count("lastCommandAtMs = millis()") >= 4)
check("S18 FORCE OFF comm-loss membatalkan pulse aktif (race dua timer)",
      "cancelled by comm-loss fail-safe FORCE OFF" in cpp)
check("S19 restore: episode di-reset, commLossForced TIDAK di-clear "
      "(no auto-replay, jujur)",
      re.search(r"commLossForced is intentionally NOT cleared", cpp, re.I) is not None)
check("S20 HOLD_SAFE: warning satu kali per episode (one-shot)",
      "_holdSafeWarned" in cpp)


# ---------------------------------------------------------------------------
# MIRROR: PH8-05 decision logic (1:1 with _checkCommLossFailSafe)
# ---------------------------------------------------------------------------
print()
print("[M] Logic mirror — PH8-05 decision table")


class MirrorChannel:
    def __init__(self, policy="OFF", lease=900):
        self.policy = policy
        self.lease = lease
        self.on = False
        self.comm_loss_forced = False
        self.last_command_at = 0


class MirrorController:
    def __init__(self, channels):
        self.channels = channels
        self.authority_lost = False
        self.lost_since = 0          # 0 = healthy
        self.episode_logged = False
        self.hold_warned = {i: False for i in channels}
        self.events = []

    def set_authority(self, lost, now):
        self.authority_lost = lost

    def command(self, ch_id, now, desired=True):
        """Executed operator command (Applied) — lease evidence + marker clear."""
        c = self.channels[ch_id]
        c.on = desired
        c.last_command_at = now
        c.comm_loss_forced = False

    def tick(self, now):
        """Mirror of _checkCommLossFailSafe()."""
        if self.authority_lost:
            if self.lost_since == 0:
                self.lost_since = now
                self.episode_logged = False
                self.hold_warned = {i: False for i in self.channels}
            if not self.episode_logged:
                self.episode_logged = True
                self.events.append(("EPISODE_START", now))
            for ch_id, c in self.channels.items():
                if not c.on or c.comm_loss_forced:
                    continue
                if c.policy == "OFF":
                    base = max(self.lost_since, c.last_command_at)
                    lease_ms = c.lease * 1000
                    if (now - base) >= lease_ms:
                        c.on = False
                        c.comm_loss_forced = True
                        self.events.append(("FORCE_OFF", ch_id, now))
                else:  # HOLD_SAFE
                    if not self.hold_warned[ch_id]:
                        self.hold_warned[ch_id] = True
                        self.events.append(("HOLD_WARN", ch_id, now))
        elif self.lost_since != 0:
            self.events.append(("RESTORE", now))
            self.lost_since = 0
            self.episode_logged = False
            self.hold_warned = {i: False for i in self.channels}
            # commLossForced intentionally NOT cleared


# --- M1: policy OFF trips at lease expiry; OFF channel untouched ----------
ch = {0: MirrorChannel("OFF", 900), 1: MirrorChannel("OFF", 900)}
m = MirrorController(ch)
m.command(0, 1000, True)
m.command(1, 1000, False)
m.set_authority(True, 0)
m.tick(2000)                       # episode start
check("M1a episode terlatch saat authority hilang", m.lost_since == 2000)
m.tick(2000 + 899 * 1000)
check("M1b ON channel bertahan sampai T-1s sebelum lease",
      ch[0].on is True and ch[0].comm_loss_forced is False)
m.tick(2000 + 900 * 1000)
check("M1c ON channel FORCE OFF tepat saat lease kedaluwarsa",
      ch[0].on is False and ch[0].comm_loss_forced is True)
check("M1d channel yang memang OFF tidak menghasilkan event FORCE_OFF",
      ("FORCE_OFF", 1, 2000 + 900 * 1000) not in m.events)

# --- M2: HOLD channel holds while OFF channel trips (bukan kill-switch) ---
ch = {0: MirrorChannel("OFF", 900), 3: MirrorChannel("HOLD_SAFE", 900)}
m = MirrorController(ch)
m.command(0, 1000, True)
m.command(3, 1000, True)
m.set_authority(True, 0)
m.tick(2000)
m.tick(2000 + 901 * 1000)
check("M2a channel policy OFF ter-trip, channel HOLD tetap ON "
      "(negative control audit: BUKAN global all-off)",
      ch[0].on is False and ch[3].on is True)
check("M2b HOLD warning tepat satu kali per episode",
      sum(1 for e in m.events if e[0] == "HOLD_WARN") == 1)

# --- M3: operator command during outage extends the lease ------------------
ch = {0: MirrorChannel("OFF", 900)}
m = MirrorController(ch)
m.command(0, 1000, True)
m.set_authority(True, 0)
m.tick(2000)                       # episode start t=2000
m.tick(2000 + 600 * 1000)          # t+600s: masih dalam lease
m.command(0, 2000 + 600 * 1000, True)   # local REST executed during outage
m.tick(2000 + 600 * 1000 + 899 * 1000)  # 899s after the command
check("M3a lease diperpanjang oleh perintah operator selama outage",
      ch[0].on is True)
m.tick(2000 + 600 * 1000 + 900 * 1000)
check("M3b lease baru tetap berakhir (FORCE OFF pada batas baru)",
      ch[0].on is False and ch[0].comm_loss_forced is True)

# --- M4: lease 0 = immediate (fail-closed) ----------------------------------
ch = {0: MirrorChannel("OFF", 0)}
m = MirrorController(ch)
m.command(0, 1000, True)
m.set_authority(True, 0)
m.tick(2000)
check("M4 lease 0 = FORCE OFF seketika (konvensi fail-closed, lawan dari "
      "maxOnTime 0=unlimited)",
      ch[0].on is False)

# --- M5: restore does NOT re-energize; marker stays honest ------------------
ch = {0: MirrorChannel("OFF", 300)}
m = MirrorController(ch)
m.command(0, 1000, True)
m.set_authority(True, 0)
m.tick(2000)
m.tick(2000 + 301 * 1000)
check("M5a ter-trip sebelum restore", ch[0].on is False and ch[0].comm_loss_forced is True)
m.set_authority(False, 0)
m.tick(2000 + 400 * 1000)
check("M5b RESTORE tercatat", any(e[0] == "RESTORE" for e in m.events))
check("M5c TIDAK ada re-energize otomatis setelah restore",
      ch[0].on is False)
check("M5d commLossForced tetap jujur sampai perintah BARU",
      ch[0].comm_loss_forced is True)
m.command(0, 2000 + 500 * 1000, True)
check("M5e perintah baru membersihkan marker (intent operator eksplisit)",
      ch[0].on is True and ch[0].comm_loss_forced is False)

# --- M6: repeated episodes re-arm warnings ----------------------------------
ch = {2: MirrorChannel("HOLD_SAFE", 60)}
m = MirrorController(ch)
m.command(2, 1000, True)
m.set_authority(True, 0)
m.tick(2000)
m.tick(2000 + 1000)
m.set_authority(False, 0)
m.tick(2000 + 2000)
m.set_authority(True, 0)
m.tick(2000 + 3000)
check("M6 episode kedua kembali mempersenjatai warning HOLD (one-shot per "
      "episode)",
      sum(1 for e in m.events if e[0] == "HOLD_WARN") == 2)

# --- M7: WiFi down = authority lost immediately (feed contract) -------------
check("M7 feed: authority = WiFi && MQTT fully operational "
      "(implementasi M-mirror konsisten dengan wiring S9)",
      re.search(r"WiFi\.status\(\)\s*==\s*WL_CONNECTED\s*&&\s*Network::mqttTransport\.isFullyOperational\(\)",
                ino) is not None)


# ---------------------------------------------------------------------------
print()
print("=" * 72)
print(f"RESULT: {PASS} passed, {FAIL} failed")
print("=" * 72)
sys.exit(0 if FAIL == 0 else 1)
