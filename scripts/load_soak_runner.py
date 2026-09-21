#!/usr/bin/env python3
"""
load_soak_runner.py — [GATE-10 / F4 broker-capacity 2026-09]
Orkestrator soak/beban untuk menutup temuan F4: "kapasitas broker produksi
belum diuji". Mensimulasikan armada perangkat virtual N × interval × durasi
terhadap broker MQTT NYATA (TLS), mengukur latensi PUBACK, latensi delivery
end-to-end (via observer), duplikat, drop, disconnect, dan reconnect.

KEAMANAN (fail-safe default):
  - ID perangkat virtual WAJIB berformat SOAK-<runId>-NN (dipaksa script).
  - Publikasi HANYA ke topik sendiri plts/<id>/status (QoS sesuai kontrak).
    Topik config/ota perangkat NYATA tidak pernah disentuh — kontrol tenaga
    tidak mungkin terpicu oleh tool ini.
  - Menjalankan beban nyata memerlukan --i-confirm-publisher (tanpa itu:
    mode --plan saja).
  - Kredensial broker HANYA via env (tidak pernah di argv/log):
      SOAK_MQTT_HOST, SOAK_MQTT_PORT (default 8883),
      SOAK_MQTT_USERNAME, SOAK_MQTT_PASSWORD,
      SOAK_MQTT_CA (path CA, opsional bila CA sistem memadai)
  - TLS aktif default; --insecure-tls memerlukan --i-confirm-insecure-tls
    dan hasil ditandai insecure=true (tidak bisa dipakai bukti gate).

Batasan jujur: mengukur jalur broker dari SATU stasiun uji ini (bukan
distribusi geografis klien).

Exit:  0 = PASS (semua budget terpenuhi, terukur)
       1 = FAIL (ada budget yang dilanggar)
       2 = precondition error (env/broker/library) — tidak silently pass.
"""
import argparse
import json
import os
import random
import statistics
import sys
import threading
import time
import uuid

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("[PRECONDITION] paho-mqtt belum terpasang: pip3 install paho-mqtt. Exit 2.")
    sys.exit(2)

SOAK_PREFIX = "SOAK"
ALLOWED_TOPIC_KINDS = ("status",)     # perintah (config/ota) DILARANG by-design


def percentile(sorted_vals, p):
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(sorted_vals) - 1)
    return round((sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)), 1)


class Metrics:
    def __init__(self):
        self.lock = threading.Lock()
        self.observer_connected = False
        self.device_connected = 0
        self.sent = 0
        self.acked = 0
        self.ack_latencies = []          # ms (publish → PUBACK)
        self.delivered = 0
        self.delivery_latencies = []     # ms (publish → diterima observer)
        self.duplicates = 0
        self.out_of_order = 0
        self.disconnects = 0
        self.reconnect_attempts = 0
        self.errors = 0
        self.disconnect_events = []
        self.last_seq = {}

    def summary(self):
        with self.lock:
            al = sorted(self.ack_latencies)
            dl = sorted(self.delivery_latencies)
            sent = max(self.sent, 1)
            return {
                "sent": self.sent,
                "acked": self.acked,
                "delivered": self.delivered,
                "ackLatencyMs": {"p50": percentile(al, 50), "p95": percentile(al, 95),
                                 "p99": percentile(al, 99), "max": al[-1] if al else None},
                "deliveryLatencyMs": {"p50": percentile(dl, 50), "p95": percentile(dl, 95),
                                      "p99": percentile(dl, 99), "max": dl[-1] if dl else None},
                "duplicates": self.duplicates,
                "outOfOrder": self.out_of_order,
                "dropRatio": round(max(0, self.sent - self.delivered) / sent, 6),
                "disconnects": self.disconnects,
                "reconnectAttempts": self.reconnect_attempts,
                "publishErrors": self.errors,
                "disconnectEvents": self.disconnect_events[:50],
            }


def build_client(client_id, ctx, m, on_disconnect_cb=None, on_message_cb=None):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id,
                    protocol=mqtt.MQTTv311)
    user = os.environ.get("SOAK_MQTT_USERNAME", "")
    pw = os.environ.get("SOAK_MQTT_PASSWORD", "")
    if user:
        c.username_pw_set(user, pw)
    if ctx is not None:
        c.tls_set(ca_certs=os.environ.get("SOAK_MQTT_CA") or None, context=ctx)
    c.reconnect_delay_set(min_delay=1, max_delay=30)   # backoff w/ jitter bawaan
    if on_disconnect_cb:
        c.on_disconnect = on_disconnect_cb
    if on_message_cb:
        c.on_message = on_message_cb
    return c


def on_disconnect_factory(m, label):
    def cb(client, userdata, *args):
        with m.lock:
            m.disconnects += 1
            m.disconnect_events.append({"who": label, "at": time.time()})
    return cb


def device_worker(idx, run_id, cfg, m, ctx, stop_evt):
    dev_id = f"{SOAK_PREFIX}-{run_id}-{idx:03d}"
    topic = f"plts/{dev_id}/{ALLOWED_TOPIC_KINDS[0]}"
    c = build_client(dev_id, ctx, m, on_disconnect_cb=on_disconnect_factory(m, dev_id))
    try:
        c.connect(cfg["host"], cfg["port"], keepalive=30)
        c.loop_start()
        deadline = time.time() + 10
        while not c.is_connected() and time.time() < deadline:
            time.sleep(0.1)
        if c.is_connected():
            with m.lock:
                m.device_connected += 1
    except Exception as e:
        with m.lock:
            m.errors += 1
        print(f"[DEVICE {dev_id}] connect gagal: {type(e).__name__}")
        return
    seq = 0
    rng = random.Random(idx)
    while not stop_evt.is_set():
        seq += 1
        payload = json.dumps({"seq": seq, "sentTsMs": int(time.time() * 1000),
                              "deviceId": dev_id})
        t0 = time.time()
        try:
            info = c.publish(topic, payload, qos=cfg["qos"])
            with m.lock:
                m.sent += 1
            if cfg["qos"] > 0:
                ok = info.wait_for_publish(timeout=cfg["ack_timeout"])
                with m.lock:
                    if ok:
                        m.acked += 1
                        m.ack_latencies.append((time.time() - t0) * 1000.0)
                    else:
                        m.errors += 1
        except Exception:
            with m.lock:
                m.errors += 1
        # jitter ±10% (hindari thundering-herd sinkron)
        stop_evt.wait(cfg["interval"] * (0.9 + 0.2 * rng.random()))
    try:
        c.disconnect()
        c.loop_stop()
    except Exception:
        pass


def observer_worker(run_id, cfg, m, ctx, stop_evt):
    obs_id = f"{SOAK_PREFIX}-{run_id}-obs"
    seen = {}

    def on_message(client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        dev = data.get("deviceId", "")
        seq = data.get("seq")
        ts = data.get("sentTsMs")
        if not dev or not isinstance(seq, int):
            return
        with m.lock:
            key = (dev, seq)
            if key in seen:
                m.duplicates += 1
                return
            seen[key] = True
            last = m.last_seq.get(dev)
            if last is not None and seq < last:
                m.out_of_order += 1
            m.last_seq[dev] = seq
            m.delivered += 1
            if isinstance(ts, (int, float)):
                m.delivery_latencies.append(max(0.0, time.time() * 1000.0 - ts))
        if len(seen) > 400_000:      # proteksi memori pada soak panjang
            seen.clear()

    c = build_client(obs_id, ctx, m, on_disconnect_cb=on_disconnect_factory(m, "observer"),
                     on_message_cb=on_message)
    try:
        c.connect(cfg["host"], cfg["port"], keepalive=30)
        c.loop_start()
        deadline = time.time() + 10
        while not c.is_connected() and time.time() < deadline:
            time.sleep(0.1)
        if c.is_connected():
            with m.lock:
                m.observer_connected = True
            c.subscribe(f"plts/{SOAK_PREFIX}-{run_id}/#", qos=cfg["qos"])
    except Exception as e:
        print(f"[OBSERVER] connect gagal: {type(e).__name__}")
        return
    while not stop_evt.is_set():
        time.sleep(0.5)
    try:
        c.disconnect()
        c.loop_stop()
    except Exception:
        pass


def run_soak(cfg):
    import ssl
    run_id = uuid.uuid4().hex[:8]
    print(f"[SOAK] runId={run_id} devices={cfg['devices']} interval={cfg['interval']}s "
          f"duration={cfg['duration']}s qos={cfg['qos']} "
          f"tls={'off' if cfg['insecure_tls'] else 'on(verify)'}")
    ctx = None
    if not cfg["insecure_tls"]:
        ctx = ssl.create_default_context()
        ca = os.environ.get("SOAK_MQTT_CA")
        if ca:
            ctx.load_verify_locations(ca)
    m = Metrics()
    stop_evt = threading.Event()
    obs = threading.Thread(target=observer_worker, args=(run_id, cfg, m, ctx, stop_evt), daemon=True)
    obs.start()
    time.sleep(1.0)     # observer siap lebih dulu (kontrol positif delivery)
    workers = [threading.Thread(target=device_worker, args=(i, run_id, cfg, m, ctx, stop_evt),
                                daemon=True) for i in range(cfg["devices"])]
    for w in workers:
        w.start()
    start = time.time()
    next_report = start + 30
    while time.time() - start < cfg["duration"]:
        time.sleep(0.5)
        if time.time() >= next_report:
            s = m.summary()
            print(f"  [{int(time.time() - start):>5}s] sent={s['sent']} delivered={s['delivered']} "
                  f"drop={s['dropRatio']} dup={s['duplicates']} disc={s['disconnects']} "
                  f"ackP95={s['ackLatencyMs']['p95']}ms")
            next_report += 30
    print("[SOAK] drain window 10s (QoS1 in-flight settle)…")
    stop_evt.set()
    time.sleep(10)
    for w in workers:
        w.join(timeout=5)
    obs.join(timeout=5)
    return run_id, m


def main():
    ap = argparse.ArgumentParser(description="Load/soak broker MQTT (GATE-10 / F4)")
    ap.add_argument("--devices", type=int, default=10, help="jumlah perangkat virtual (default 10)")
    ap.add_argument("--interval", type=float, default=5.0, help="detik antar telemetry (default 5)")
    ap.add_argument("--duration", type=float, default=600.0, help="detik durasi soak (default 600)")
    ap.add_argument("--qos", type=int, default=1, choices=(0, 1))
    ap.add_argument("--ack-timeout", type=float, default=10.0, help="timeout PUBACK detik")
    ap.add_argument("--latency-p95-budget-ms", type=float, default=2000.0)
    ap.add_argument("--max-drop-ratio", type=float, default=0.001)
    ap.add_argument("--max-disconnects", type=int, default=0)
    ap.add_argument("--max-duplicates", type=int, default=0)
    ap.add_argument("--insecure-tls", action="store_true", help="matikan verifikasi TLS (uji)")
    ap.add_argument("--i-confirm-insecure-tls", action="store_true")
    ap.add_argument("--i-confirm-publisher", action="store_true",
                    help="konfirmasi mempublikasi beban ke namespace plts/SOAK-*/status")
    ap.add_argument("--plan", action="store_true", help="cetak rencana run tanpa menjalankan")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--out", help="tulis JSON hasil ke file")
    args = ap.parse_args()

    host = os.environ.get("SOAK_MQTT_HOST", "")
    port = int(os.environ.get("SOAK_MQTT_PORT", "8883"))
    cfg = vars(args).copy()
    cfg.update({"host": host, "port": port})

    if args.plan:
        plan = {
            "schemaVersion": 1, "tool": "load_soak_runner.py", "mode": "PLAN",
            "virtualDevices": args.devices, "intervalSec": args.interval,
            "durationSec": args.duration, "qos": args.qos,
            "topicNamespace": f"plts/{SOAK_PREFIX}-<runId>-NNN/status (SAJA — kontrol dilarang)",
            "requires": ["SOAK_MQTT_HOST", "SOAK_MQTT_USERNAME", "SOAK_MQTT_PASSWORD",
                         "--i-confirm-publisher"],
            "budgets": {"latencyP95Ms": args.latency_p95_budget_ms,
                        "dropRatio": args.max_drop_ratio,
                        "disconnects": args.max_disconnects},
        }
        print(json.dumps(plan, indent=2))
        return 0

    if not host:
        print("[PRECONDITION] SOAK_MQTT_HOST tidak diset (lihat --plan). Exit 2.")
        return 2
    if not os.environ.get("SOAK_MQTT_USERNAME"):
        print("[PRECONDITION] SOAK_MQTT_USERNAME tidak diset. Exit 2.")
        return 2
    if args.insecure_tls and not args.i_confirm_insecure_tls:
        print("[PRECONDITION] --insecure-tls memerlukan --i-confirm-insecure-tls. Exit 2.")
        return 2
    if not args.i_confirm_publisher:
        print("[PRECONDITION] beban publikasi memerlukan --i-confirm-publisher "
              "(namespace plts/SOAK-*/status — tidak menyentuh perangkat nyata). "
              "Gunakan --plan untuk melihat rencana. Exit 2.")
        return 2
    if args.devices < 1 or args.devices > 500:
        print("[PRECONDITION] --devices harus 1..500. Exit 2.")
        return 2

    run_id, m = run_soak(cfg)
    # [ANTI-FALSE-CLOSURE] run tanpa pengukuran TIDAK BOLEH PASS:
    # observer/device gagal terhubung atau 0 pesan terukur = precondition gagal.
    if not m.observer_connected or m.device_connected == 0 or m.sent == 0:
        print(f"[PRECONDITION] broker tidak terjangkau/pesan tidak terukur "
              f"(observer={m.observer_connected}, devices={m.device_connected}, "
              f"sent={m.sent}) — hasil TIDAK sah. Exit 2.")
        return 2
    s = m.summary()
    violations = []
    if s["ackLatencyMs"]["p95"] is not None and s["ackLatencyMs"]["p95"] > args.latency_p95_budget_ms:
        violations.append(f"ackP95 {s['ackLatencyMs']['p95']}ms > {args.latency_p95_budget_ms}ms")
    if s["deliveryLatencyMs"]["p95"] is not None and s["deliveryLatencyMs"]["p95"] > args.latency_p95_budget_ms:
        violations.append(f"deliveryP95 {s['deliveryLatencyMs']['p95']}ms > {args.latency_p95_budget_ms}ms")
    if s["dropRatio"] > args.max_drop_ratio:
        violations.append(f"dropRatio {s['dropRatio']} > {args.max_drop_ratio}")
    if s["disconnects"] > args.max_disconnects:
        violations.append(f"disconnects {s['disconnects']} > {args.max_disconnects}")
    if s["duplicates"] > args.max_duplicates:
        violations.append(f"duplicates {s['duplicates']} > {args.max_duplicates}")

    verdict = "PASS" if not violations else "FAIL"
    result = {
        "schemaVersion": 1, "tool": "load_soak_runner.py",
        "runId": run_id,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "config": {"devices": args.devices, "intervalSec": args.interval,
                   "durationSec": args.duration, "qos": args.qos,
                   "insecureTls": bool(args.insecure_tls),
                   "originNote": "diukur dari satu stasiun uji"},
        "metrics": s,
        "budgets": {"latencyP95Ms": args.latency_p95_budget_ms,
                    "dropRatio": args.max_drop_ratio,
                    "disconnects": args.max_disconnects,
                    "duplicates": args.max_duplicates},
        "violations": violations,
        "verdict": verdict,
    }
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
    if args.json:
        print(json.dumps(result, indent=2))
    print("\n" + "=" * 72)
    print(f"LOAD/SOAK RESULT: {verdict}  (exit {0 if verdict == 'PASS' else 1})")
    for v in violations:
        print(f"  - {v}")
    print(f"  sent={s['sent']} delivered={s['delivered']} dup={s['duplicates']} "
          f"disc={s['disconnects']} ackP95={s['ackLatencyMs']['p95']}ms "
          f"deliveryP95={s['deliveryLatencyMs']['p95']}ms")
    print("=" * 72)
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
