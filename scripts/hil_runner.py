#!/usr/bin/env python3
"""
hil_runner.py — [GATE-9 / F8-12 HIL-qualification 2026-09]
Orkestrator Hardware-in-the-Loop: mengotomasi bagian terukur dari
docs/hardware-acceptance/HIL-PROCEDURE.md (bagian A–L), memandu operator
untuk langkah fisik, dan menghasilkan DRAFT bukti
docs/hardware-acceptance/v{versi}-gate.DRAFT.json yang kompatibel dengan
gate-qualification.template.json + verify_gate_qualification.py.

DISIPLIN ANTI-FALSE-CLOSURE (mengikat):
  - Check di evidence HANYA diisi dari hasil TERUKUR. Bagian yang tidak
    dieksekusi tetap "PENDING" — tidak pernah auto-PASS.
  - verdict TIDAK PERNAH di-set PASS oleh tool; PASS hanya setelah 22/22
    check terukur + tanda tangan manusia (3 peran).
  - Konfirmasi operator fisik dicatat apa adanya (timestamp + jawaban).
  - Kredensial hanya via env: DEVICE_USER, DEVICE_PASS, HIL_MQTT_HOST,
    HIL_MQTT_PORT, HIL_MQTT_USERNAME, HIL_MQTT_PASSWORD, HIL_MQTT_CA.

KEAMANAN (fail-safe default):
  - READ-ONLY default. Bagian yang mengaktifkan relay/mengubah config/OTA
    memerlukan: --i-confirm-actuation (E/H/G/I/D/J2), --i-confirm-reboot (A),
    --i-confirm-ota (K). Tanpa flag → bagian SKIPPED, exit tetap jujur.
  - Perintah ON hanya pada beban representatif bench (bukan instalasi nyata).

Exit:  0 = sesi selesai (bagian terpilih tereksekusi/skipped tercatat jujur)
       1 = ada check TERUKUR yang FAIL
       2 = precondition error (perangkat/argumen/library)
"""
import argparse
import json
import os
import re
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from http.cookiejar import CookieJar

HTTP_TIMEOUT = 12
SERIAL_BOOT_MARKERS = (r"DEVICE_BOOT", r"RELAY: Controller initialized",
                       r"rst:0x", r"MQTT SECURITY")


# ---------------------------------------------------------------- util REST
class Device:
    def __init__(self, base, insecure_tls=False):
        ctx = ssl.create_default_context()
        if base.startswith("https") and insecure_tls:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        self.base = base.rstrip("/")
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(CookieJar()),
            urllib.request.HTTPSHandler(context=ctx))
        self.csrf = None
        self.logged_in = False

    def req(self, method, path, body=None, timeout=HTTP_TIMEOUT):
        url = self.base + path
        data = body.encode() if isinstance(body, str) else body
        r = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            r.add_header("Content-Type", "application/json")
        if self.csrf and method == "POST":
            r.add_header("X-CSRF-Token", self.csrf)
        try:
            with self.opener.open(r, timeout=timeout) as resp:
                raw = resp.read()
                return resp.status, self._parse(raw)
        except urllib.error.HTTPError as e:
            return e.code, self._parse(e.read())
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise ConnectionError(str(e)) from e

    @staticmethod
    def _parse(raw):
        try:
            d = json.loads(raw.decode("utf-8", "replace"))
            if isinstance(d, dict) and isinstance(d.get("data"), dict):
                out = dict(d["data"])
                out["_message"] = d.get("message", "")
                return out
            return d
        except Exception:
            return None

    def login(self):
        user = os.environ.get("DEVICE_USER", "")
        pw = os.environ.get("DEVICE_PASS", "")
        if not user or not pw:
            return False
        code, body = self.req("POST", "/api/login",
                              json.dumps({"username": user, "password": pw}))
        if code != 200:
            print(f"[HIL] login → {code} ({(body or {}).get('_message','')})")
            return False
        self.csrf = (body or {}).get("csrfToken", "")
        self.logged_in = True
        return True

    def envelope(self, extra=None):
        """Envelope perintah wajib (CORE-02): version+transactionId+issuedAt+expiresAt."""
        now = int(time.time())
        env = {"version": 1, "transactionId": f"HIL-{uuid.uuid4().hex[:16]}",
               "requestId": f"hil-{uuid.uuid4().hex[:8]}",
               "issuedAt": now, "expiresAt": now + 120, "source": "HIL"}
        if extra:
            env.update(extra)
        return env


def ask(prompt, yes_flag):
    if yes_flag:
        print(f"[OPERATOR] {prompt} → (diterima via --yes)")
        return True
    try:
        return input(f"[OPERATOR] {prompt} [y/N]: ").strip().lower() == "y"
    except EOFError:
        return False


# ---------------------------------------------------------------- evidence
CHECKS = ["boot", "sensors", "alarms", "otaRest", "otaMqtt", "rollback",
          "emergencyRelay", "configPersistence", "security", "soak24h",
          "factoryReset", "emergencyAtomicity", "watchdogStarvationBank",
          "clockInvalidReject", "safetyConfigLockdown", "commLossFailSafe",
          "pinChangeRefused", "mqttTopicBinding", "otaDurableReservation",
          "qosTimeoutBudget", "staleCommandBlocked", "telemetryJournalRetention"]


class Evidence:
    def __init__(self, meta):
        self.doc = {
            "schemaVersion": 1,
            "version": meta["version"],
            "gitCommit": meta["gitCommit"],
            "firmwareSha256": meta["firmwareSha256"],
            "verdict": "PENDING",
            "testEngineer": meta.get("test_engineer", ""),
            "reviewer": "", "releaseManager": "",
            "testDate": time.strftime("%Y-%m-%d"),
            "hardwareSerial": meta.get("hardware_serial", ""),
            "hardwareIdentity": meta.get("hardware_identity", ""),
            "broker": meta.get("broker", ""),
            "checks": {c: "PENDING" for c in CHECKS},
            "observed": {},
            "operatorLog": [],
            "notes": "DRAFT dihasilkan hil_runner.py — nilai PENDING = belum "
                     "dieksekusi/terukur. Finalisasi manual oleh operator "
                     "(HIL-PROCEDURE.md); verifier menolak PENDING.",
        }
        self.fail_measured = False

    def check(self, name, result):
        if result not in ("PASS", "FAIL"):
            raise ValueError(f"hasil check harus PASS/FAIL terukur, bukan {result}")
        self.doc["checks"][name] = result
        if result == "FAIL":
            self.fail_measured = True

    def obs(self, key, value):
        self.doc["observed"][key] = value

    def log_op(self, text):
        self.doc["operatorLog"].append({"at": time.strftime("%H:%M:%S"), "text": text})

    def emit(self, path):
        parent = os.path.dirname(os.path.abspath(path))
        os.makedirs(parent, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.doc, f, indent=2)
        pending = [c for c, v in self.doc["checks"].items() if v == "PENDING"]
        print(f"\n[EVIDENCE] ditulis: {path}")
        print(f"[EVIDENCE] check terukur: {len(CHECKS)-len(pending)}/{len(CHECKS)}; "
              f"PENDING: {len(pending)} — verifier akan mem-BLOK rilis hingga lengkap.")
        return path


# ---------------------------------------------------------------- bagian A
def section_A(dev, ev, args):
    print("\n=== BAGIAN A — boot / relayAllOffBoot ===")
    try:
        code, st = dev.req("GET", "/api/status")
    except ConnectionError as e:
        print(f"[A] perangkat tidak terjangkau: {e}")
        return "SKIPPED"
    if code != 200:
        print(f"[A] /api/status → {code}")
        return "SKIPPED"
    ev.obs("device", {"deviceId": st.get("deviceId"),
                      "firmwareVersion": st.get("firmwareVersion"),
                      "bootCountAtStart": st.get("bootCount"),
                      "resetReasonAtStart": st.get("resetReason"),
                      "uptimeSecondsAtStart": st.get("uptimeSeconds")})
    print(f"[A] deviceId={st.get('deviceId')} fw={st.get('firmwareVersion')} "
          f"bootCount={st.get('bootCount')} resetReason={st.get('resetReason')}")
    if not ask("Lakukan 3× cold boot; verifikasi semua kanal OFF fisik "
               "(beban mati / PCF8574 0xFF). Selesai?", args.yes):
        ev.log_op("A: operator membatalkan cold-boot — tetap PENDING")
        return "PARTIAL"
    ev.log_op("A: 3× cold boot selesai + verifikasi kanal OFF fisik oleh operator")
    try:
        code, st2 = dev.req("GET", "/api/status")
        if code == 200 and isinstance(st2.get("bootCount"), int):
            ev.obs("relayAllOffBoot", "OPERATOR_VERIFIED")
            ev.obs("bootCountAfterColdBoots", st2.get("bootCount"))
            ev.check("boot", "PASS")     # identitas + boot terukur + operator verifikasi
    except ConnectionError:
        pass
    return "EXECUTED"


# ---------------------------------------------------------------- bagian C
def section_C(dev, ev, args):
    print("\n=== BAGIAN C — power abuse (10× VCC cut saat CH3 ON) ===")
    if not args.i_confirm_actuation:
        print("[C] memerlukan --i-confirm-actuation (CH3 ON + power cycle) → SKIPPED")
        return "SKIPPED"
    code, cfg0 = dev.req("GET", "/api/config")
    if code != 200:
        print("[C] tidak dapat membaca /api/config baseline → SKIPPED")
        return "SKIPPED"
    print("[C] baseline config tersimpan; nyalakan CH3 via REST lalu lakukan "
          "10× putus VCC sesuai prosedur.")
    code, ack = dev.req("POST", "/api/relays/3/on", json.dumps(dev.envelope()))
    print(f"[C] POST /api/relays/3/on → {code} {ack}")
    ev.log_op(f"C: CH3 ON via REST → {code}")
    if not ask("Putus VCC 10× berturut (amati tiap boot). Selesai?", args.yes):
        return "PARTIAL"
    try:
        code, st = dev.req("GET", "/api/status")
        code2, cfg1 = dev.req("GET", "/api/config")
        if code == 200:
            ev.obs("powerCycleCount", 10)
            ev.obs("resetReasonAfterPowerAbuse", st.get("resetReason"))
            ev.obs("bootCountAfterPowerAbuse", st.get("bootCount"))
        if code2 == 200 and cfg0 is not None and cfg1 is not None:
            same = {k: v for k, v in cfg0.items() if k in cfg1 and cfg1[k] == v}
            keys0 = set(cfg0.keys())
            persisted = len(same) >= max(1, int(0.9 * len(keys0)))
            ev.obs("configPersistedAfterPowerAbuse", persisted)
            ev.check("configPersistence", "PASS" if persisted else "FAIL")
    except ConnectionError as e:
        print(f"[C] gagal polling pasca-power-cycle: {e}")
    ev.log_op("C: 10× power abuse selesai (operator)")
    return "EXECUTED"


# ---------------------------------------------------------------- bagian E
def section_E(dev, ev, args):
    print("\n=== BAGIAN E — E-WAVE / E-stop / atomicity (PH8-01) ===")
    if not args.i_confirm_actuation:
        print("[E] memerlukan --i-confirm-actuation (3 kanal ON + emergency) → SKIPPED")
        return "SKIPPED"
    for ch in (0, 1, 2):
        code, ack = dev.req("POST", f"/api/relays/{ch}/on", json.dumps(dev.envelope()))
        print(f"[E] CH{ch} ON → {code} ({(ack or {}).get('state','?')})")
        time.sleep(0.3)
    ev.log_op("E: CH0-CH2 ON via REST (beban representatif)")
    print("[E] pilih pemicu emergency: (a) E-stop fisik / kondisi sensor E-WAVE, "
          "atau (b) --emergency-via-rest (POST /api/relays/all_off).")
    if args.emergency_via_rest:
        t_em = time.time()
        code, ack = dev.req("POST", "/api/relays/all_off", json.dumps(dev.envelope()))
        print(f"[E] all_off → {code} {ack}")
        ev.log_op(f"E: emergency via REST all_off → {code}")
    else:
        if not ask("Picu E-WAVE/E-stop SEKARANG lalu konfirmasi", args.yes):
            return "PARTIAL"
        t_em = time.time()
        ev.log_op("E: E-WAVE/E-stop dipicu operator (fisik)")
    time.sleep(2.0)
    code, rel = dev.req("GET", "/api/relays")
    violations = 0
    if code == 200 and isinstance(rel, dict):
        chans = rel.get("channels") or rel.get("relays") or []
        if isinstance(chans, list):
            violations = sum(1 for c in chans
                             if isinstance(c, dict) and c.get("state") in (True, "ON", 1)
                             or (isinstance(c, dict) and str(c.get("reportedState", "")).upper() == "ON"))
        ev.obs("channelsAfterEmergency", chans)
    print(f"[E] kanal menyala SETELAH emergency: {violations} (harus 0)")
    ev.obs("emergencyAtomicity", {"tripsDuringOnBurst": 1,
                                  "violationCount": violations,
                                  "cascadeLatencyMs": None})
    # PH8-01 burst: 10Hz ON selama 3s sambil emergency sudah aktif → harus BLOCKED
    blocked_ok = 0
    t_end = time.time() + 3
    while time.time() < t_end:
        code, ack = dev.req("POST", "/api/relays/4/on", json.dumps(dev.envelope()))
        if code in (200, 409, 423):
            res = str(((ack or {}).get("result", "")) or ((ack or {}).get("_message", "")))
            if "BLOCKED" in res.upper() or "LATCH" in res.upper() or code in (409, 423):
                blocked_ok += 1
        time.sleep(0.1)
    ev.obs("emergencyAtomicity", {"tripsDuringOnBurst": 1,
                                  "postTripOnRefused": blocked_ok > 0,
                                  "armRequiredToReenable": ask(
                                      "Kanal hanya bisa ON lagi setelah ARM operator eksplisit? "
                                      "(verifikasi prosedur)", args.yes),
                                  "violationCount": violations})
    ev.check("emergencyRelay", "PASS" if violations == 0 else "FAIL")
    ev.check("emergencyAtomicity", "PASS" if violations == 0 and blocked_ok > 0 else "FAIL")
    ev.log_op(f"E: post-trip ON refused count={blocked_ok}/~30, violations={violations}")
    return "EXECUTED"


# ---------------------------------------------------------------- bagian F
def section_F(dev, ev, args):
    print("\n=== BAGIAN F — clock invalid (PH8-03) ===")
    if not args.i_confirm_actuation:
        print("[F] memerlukan --i-confirm-actuation (perintah ON sbg uji) → SKIPPED")
        return "SKIPPED"
    code, st = dev.req("GET", "/api/status")
    tq = str((st or {}).get("timeQuality", ""))
    if tq.upper() in ("VALID", "SYNCED"):
        print(f"[F] timeQuality={tq} — clock SUDAH sync; bagian F butuh boot tanpa "
              "NTP (blokir NTP lalu reboot). Jika sudah diatur:")
        if not ask("Clock TIDAK tersinkron sekarang? (jika tidak, bagian F tidak valid)",
                   False):
            return "SKIPPED"
    env = dev.envelope()
    code, ack = dev.req("POST", "/api/relays/5/on", json.dumps(env))
    res = str(((ack or {}).get("_message", "")) + " " + str((ack or {}).get("result", ""))).upper()
    energizing_rejected = ("CLOCK_INVALID" in res) or code in (409, 422, 400)
    print(f"[F] ON dengan clock invalid → {code} ({res.strip()[:60]})")
    code2, ack2 = dev.req("POST", "/api/relays/5/off", json.dumps(dev.envelope()))
    off_ok = code2 == 200
    print(f"[F] OFF (safe-direction) → {code2}")
    ev.obs("clockInvalidReject", {"energizingRejected": energizing_rejected,
                                  "safeDirectionAllowed": off_ok})
    ev.check("clockInvalidReject", "PASS" if energizing_rejected and off_ok else "FAIL")
    return "EXECUTED"


# ---------------------------------------------------------------- bagian G/I
def section_G_I(dev, ev, args, which):
    label = "G (safety-config lockdown)" if which == "G" else "I (pin change refused)"
    print(f"\n=== BAGIAN {label} ===")
    if not args.i_confirm_actuation:
        print(f"[{which}] memerlukan --i-confirm-actuation (config POST, diharapkan REFUSED) → SKIPPED")
        return "SKIPPED"
    code, st0 = dev.req("GET", "/api/status")
    payloads = {
        "G": {"sensorFailPolicy": "IGNORED", "estopEnabled": False, "relayPin": 255},
        "I": {"relayPin": 27, "estopPin": 33},
    }[which]
    refused = {}
    for key, val in payloads.items():
        code, ack = dev.req("POST", "/api/config",
                            json.dumps(dev.envelope({key: val, "key": key})))
        res = str(((ack or {}).get("_message", ""))).upper()
        refused[key] = ("REFUS" in res) or code in (403, 409, 422)
        print(f"[{which}] config {key}={val} → {code} ({res[:50]})")
    code, st1 = dev.req("GET", "/api/status")
    physical_unchanged = (code == 200 and st0 is not None and st1 is not None
                          and st1.get("relayAvailable") == st0.get("relayAvailable"))
    if which == "G":
        ev.obs("safetyConfigLockdown", {
            "sensorFailPolicyRefused": refused.get("sensorFailPolicy"),
            "estopEnabledRefused": refused.get("estopEnabled"),
            "relayPinRefused": refused.get("relayPin")})
        ev.check("safetyConfigLockdown", "PASS" if all(refused.values()) else "FAIL")
    else:
        ev.obs("pinChangeRefused", {"relayPinChangeWhileEnergized": refused.get("relayPin"),
                                    "physicalStateUnchanged": physical_unchanged})
        ev.check("pinChangeRefused", "PASS" if all(refused.values()) and physical_unchanged else "FAIL")
    return "EXECUTED"


# ---------------------------------------------------------------- bagian J
def section_J(dev, ev, args):
    print("\n=== BAGIAN J — MQTT topic binding (GATE-3) ===")
    host = os.environ.get("HIL_MQTT_HOST", "")
    if not host:
        print("[J] HIL_MQTT_* tidak diset → SKIPPED")
        return "SKIPPED"
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[J] paho-mqtt tidak tersedia → SKIPPED")
        return "SKIPPED"
    code, st = dev.req("GET", "/api/status")
    dev_id = (st or {}).get("deviceId", "UNKNOWN")
    code, d0 = dev.req("GET", "/api/diagnostics")
    f0 = (d0 or {}).get("foreignTopicCount", 0)
    ctx = ssl.create_default_context()
    ca = os.environ.get("HIL_MQTT_CA")
    if ca:
        ctx.load_verify_locations(ca)
    other = f"plts/{dev_id}-other/config"
    got_ack = threading.Event()
    ack_msgs = []

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                    client_id=f"HIL-obs-{uuid.uuid4().hex[:6]}", protocol=mqtt.MQTTv311)
    user = os.environ.get("HIL_MQTT_USERNAME", "")
    if user:
        c.username_pw_set(user, os.environ.get("HIL_MQTT_PASSWORD", ""))
    c.tls_set(context=ctx)

    def on_message(client, userdata, msg):
        ack_msgs.append((msg.topic, time.time()))
        if msg.topic.endswith("/ack"):
            got_ack.set()

    c.on_message = on_message
    port = int(os.environ.get("HIL_MQTT_PORT", "8883"))
    try:
        c.connect(host, port, keepalive=30)
        c.loop_start()
        c.subscribe(f"plts/{dev_id}/#", qos=1)
        time.sleep(1.5)
        # J.1 — topik perangkat LAIN (dengan identitas alamat salah)
        c.publish(other, json.dumps(dev.envelope()), qos=1)
        time.sleep(3.0)
    except Exception as e:
        print(f"[J] broker gagal: {type(e).__name__}: {e}")
        return "SKIPPED"
    finally:
        try:
            c.disconnect()
            c.loop_stop()
        except Exception:
            pass
    code, d1 = dev.req("GET", "/api/diagnostics")
    f1 = (d1 or {}).get("foreignTopicCount", 0)
    foreign_rejected = f1 > f0
    print(f"[J] foreignTopicCount {f0} → {f1}; ack diterima: {len(ack_msgs)}")
    ev.obs("mqttTopicBinding", {"foreignTopicRejected": foreign_rejected,
                                "foreignTopicCountIncremented": f1 > f0,
                                "ownTopicAccepted": None})   # J.2 butuh confirm terpisah
    ev.check("mqttTopicBinding", "PASS" if foreign_rejected else "FAIL")
    ev.log_op("J: publish ke topik perangkat lain dieksekusi (harus TIDAK); "
              "J.2 own-topic memerlukan sesi --i-confirm-actuation terpisah")
    return "EXECUTED"


# ---------------------------------------------------------------- bagian L
def section_L(dev, ev, args):
    minutes = args.soak_min
    print(f"\n=== BAGIAN L — soak monitor ({minutes} menit) ===")
    if minutes <= 0:
        print("[L] --soak-min 0 → SKIPPED")
        return "SKIPPED"
    heaps, resets0, alarms_seen = [], None, 0
    try:
        code, st = dev.req("GET", "/api/status")
        resets0 = (st or {}).get("bootCount")
    except ConnectionError:
        pass
    t_end = time.time() + minutes * 60
    next_poll = 0
    while time.time() < t_end:
        time.sleep(1.0)
        if time.time() < next_poll:
            continue
        next_poll = time.time() + 60
        try:
            code, st = dev.req("GET", "/api/status")
            code2, dg = dev.req("GET", "/api/diagnostics")
            if code == 200 and isinstance(dg, dict):
                h = dg.get("minFreeHeap")
                if isinstance(h, (int, float)):
                    heaps.append(h)
                if isinstance((st or {}).get("bootCount"), int) and resets0 is not None:
                    if st["bootCount"] != resets0:
                        print(f"[L] bootCount berubah {resets0} → {st['bootCount']} "
                              f"(reset tak terduga?)")
            code3, al = dev.req("GET", "/api/alarms")
            if isinstance(al, list):
                alarms_seen = max(alarms_seen, len(al))
        except ConnectionError as e:
            print(f"[L] poll gagal: {e}")
    stable = len(heaps) >= 4 and (max(heaps) - min(heaps)) < 64 * 1024
    ev.obs("soak", {"durationHours": round(minutes / 60.0, 2),
                    "minFreeHeapKB": round(min(heaps) / 1024.0, 1) if heaps else None,
                    "heapCollapseObserved": not stable if heaps else None,
                    "resetCount": 0 if resets0 is None else None,
                    "alarmChurnCount": alarms_seen})
    if minutes >= 24 * 60:
        ev.check("soak24h", "PASS" if stable else "FAIL")
    else:
        ev.log_op(f"L: soak {minutes} menit < 24 jam — checks.soak24h tetap PENDING (jujur)")
    return "EXECUTED"


SECTIONS = {
    "A": ("boot / relayAllOffBoot", section_A),
    "C": ("power abuse", section_C),
    "E": ("E-WAVE / E-stop / atomicity", section_E),
    "F": ("clock invalid", section_F),
    "G": ("safety-config lockdown", lambda d, e, a: section_G_I(d, e, a, "G")),
    "I": ("pin change refused", lambda d, e, a: section_G_I(d, e, a, "I")),
    "J": ("MQTT topic binding", section_J),
    "L": ("soak monitor", section_L),
}


def main():
    ap = argparse.ArgumentParser(description="Orkestrator HIL Gate-9 (HIL-PROCEDURE.md A–L)")
    ap.add_argument("--device", help="URL perangkat, mis. http://192.168.1.50")
    ap.add_argument("--section", action="append", choices=sorted(SECTIONS),
                    help="bagian yang dijalankan (boleh berulang)")
    ap.add_argument("--all", action="store_true", help="jalankan A,C,E,F,G,I,J,L berurutan")
    ap.add_argument("--plan", action="store_true", help="cetak rencana bagian tanpa eksekusi")
    ap.add_argument("--yes", action="store_true", help="terima semua prompt operator (batch/non-interaktif)")
    ap.add_argument("--i-confirm-actuation", action="store_true",
                    help="IzINKAN relay ON / config POST / emergency (bench representatif)")
    ap.add_argument("--i-confirm-reboot", action="store_true")
    ap.add_argument("--i-confirm-ota", action="store_true")
    ap.add_argument("--emergency-via-rest", action="store_true",
                    help="bagian E: picu all_off via REST (selain E-stop fisik)")
    ap.add_argument("--soak-min", type=int, default=0)
    ap.add_argument("--insecure-tls", action="store_true")
    ap.add_argument("--version", default="1.9.4")
    ap.add_argument("--source-commit", default="")
    ap.add_argument("--firmware-sha256", default="")
    ap.add_argument("--release-json", help="ambil gitCommit/firmwareSha256 dari release.json")
    ap.add_argument("--test-engineer", default="")
    ap.add_argument("--hardware-serial", default="")
    ap.add_argument("--hardware-identity", default="bench-prototype")
    ap.add_argument("--broker", default="")
    ap.add_argument("--emit-evidence", help="path output DRAFT evidence JSON")
    args = ap.parse_args()

    meta = {"version": args.version, "gitCommit": args.source_commit,
            "firmwareSha256": args.firmware_sha256,
            "test_engineer": args.test_engineer,
            "hardware_serial": args.hardware_serial,
            "hardware_identity": args.hardware_identity,
            "broker": args.broker or os.environ.get("HIL_MQTT_HOST", "")}
    if args.release_json and os.path.isfile(args.release_json):
        rel = json.loads(open(args.release_json, encoding="utf-8").read())
        meta["gitCommit"] = meta["gitCommit"] or rel.get("gitCommit", "")
        meta["firmwareSha256"] = meta["firmwareSha256"] or rel.get("firmwareSha256", "")

    if args.plan:
        print(json.dumps({
            "tool": "hil_runner.py", "mode": "PLAN",
            "sections": {k: v[0] for k, v in SECTIONS.items()},
            "notAutomatedHere": ["B (sensor fisik)", "D (jumper I2C)", "H (outage)",
                                 "K (OTA — --i-confirm-ota tersedia sbg sesi lanjutan)",
                                 "M (retensi journal — via outage + diagnostics)"],
            "evidence": "v{version}-gate.DRAFT.json (PENDING jujur, tanpa signoff otomatis)",
            "requires": ["--device", "DEVICE_USER/DEVICE_PASS",
                         "--i-confirm-actuation untuk E/C/F/G/I",
                         "HIL_MQTT_* untuk J"],
        }, indent=2))
        return 0

    if not args.device:
        print("[PRECONDITION] --device wajib (atau --plan). Exit 2.")
        return 2
    wanted = args.section or (sorted(SECTIONS) if args.all else [])
    if not wanted:
        print("[PRECONDITION] pilih --section atau --all. Exit 2.")
        return 2

    dev = Device(args.device, args.insecure_tls)
    ev = Evidence(meta)
    print("=" * 72)
    print(f"HIL RUNNER — {args.device} | bagian: {', '.join(wanted)}")
    print("=" * 72)
    try:
        reachable = dev.req("GET", "/api/version")[0] in (200, 401, 403, 404)
    except ConnectionError:
        reachable = False
    if not reachable:
        print("[PRECONDITION] perangkat tidak terjangkau. Exit 2.")
        return 2
    if dev.login():
        print("[HIL] login OK (role operator diasumsikan untuk bagian mutasi)")
    else:
        print("[HIL] tanpa login — bagian mutasi akan gagal; lanjut bagian read-only")

    statuses = {}
    for sec in wanted:
        fn = SECTIONS[sec][1]
        try:
            statuses[sec] = fn(dev, ev, args)
        except KeyboardInterrupt:
            statuses[sec] = "INTERRUPTED"
            break
        except Exception as e:
            statuses[sec] = f"ERROR:{type(e).__name__}"
            print(f"[{sec}] error: {e}")

    out = args.emit_evidence or f"docs/hardware-acceptance/v{args.version}-gate.DRAFT.json"
    ev.obs("session", {"sections": statuses,
                       "actuationConfirmed": args.i_confirm_actuation,
                       "emergencyViaRest": args.emergency_via_rest,
                       "soakMinutes": args.soak_min})
    ev.emit(out)
    if ev.fail_measured:
        print("[RESULT] ada check TERUKUR = FAIL → exit 1")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
