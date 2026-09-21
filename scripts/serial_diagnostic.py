#!/usr/bin/env python3
"""
serial_diagnostic.py — [F-observability / HIL-L 2026-09]
Tool lapangan: capture log serial ESP32, stempel waktu tiap baris,
deteksi event (boot/reboot, panic, watchdog, brownout, kegagalan MQTT,
kegagalan sensor, kebocoran kredensial), tulis log + ringkasan JSON.

Binding temuan/gate: dukungan bukti HIL bagian L (soak: "tidak ada reset
tak terduga / minFreeHeap stabil / tidak ada kebocoran kredensial di log")
dan post-mortem insiden produksi. Output machine-readable untuk CI/HIL.

Dua mode:
  1. CAPTURE : python3 scripts/serial_diagnostic.py --port /dev/ttyUSB0 \
                 --duration-sec 3600 --label PLTS-001
  2. ANALYZE : python3 scripts/serial_diagnostic.py --analyze-existing \
                 logs/PLTS-001/2026-09-18_12-30-00_serial.log

Exit:  0 = log bersih (tidak ada anomali terdeteksi)
       1 = anomali terdeteksi (lihat ringkasan — bukan kegagalan tool)
       2 = precondition error (port tidak ada / pyserial belum terpasang /
           file tidak ditemukan) — tidak pernah silently pass.
"""
import argparse
import json
import os
import re
import sys
import time
from datetime import datetime, timezone

# ---------------------------------------------------------------- pola
PATTERNS = [
    ("reboot", re.compile(r"DEVICE_BOOT|\brst:0x|\bload:0x|REBOOT", re.I)),
    ("panic", re.compile(r"Guru Meditation|panic\(|abort\(\)|Backtrace:|\babort\b", re.I)),
    ("watchdog", re.compile(r"TWDT|Task watchdog|watchdog.*trigger|WDT.*reset", re.I)),
    ("brownout", re.compile(r"brownout|Brownout detector", re.I)),
    ("heap", re.compile(r"heap", re.I)),
    ("heap_warning", re.compile(r"heap.*(low|warn|exhaust|fail)|minFreeHeap.*low", re.I)),
    ("mqtt", re.compile(r"MQTT", re.I)),
    ("mqtt_failure", re.compile(
        r"MQTT.*(reconnect|disconnect|failed|fail|lost|SECURITY|timeout)", re.I)),
    ("sensor_failure", re.compile(r"SENSOR_FAILURE|SHT31.*err|INA219.*err|I2C.*(err|fail|recover)", re.I)),
    ("storage", re.compile(r"STORAGE_ERROR|LittleFS.*(fail|err|corrupt)", re.I)),
    ("alarm", re.compile(r"ALARM_ACTIVE", re.I)),
    ("authority", re.compile(r"command authority (LOST|RESTORED)", re.I)),
    ("auth_fail", re.compile(r"AUTH_FAIL", re.I)),
    ("spool", re.compile(r"EvictedOldest|spool.*(drop|evict|journal)", re.I)),
]

# Kebocoran kredensial di log — kriteria HIL-L: "tidak ada kebocoran
# transaksi/kredensial di log". Heuristik konservatif (false positive lebih
# baik daripada bocor tak terdeteksi).
SECRET_PATTERNS = [
    ("cookie_jwt", re.compile(r"jwt=[A-Za-z0-9._-]{20,}")),
    ("cookie_refresh", re.compile(r"refresh=[A-Za-z0-9._-]{16,}")),
    ("bearer", re.compile(r"Bearer\s+[A-Za-z0-9._-]{20,}")),
    ("password_assignment", re.compile(r"password\s*[:=]\s*\S{6,}", re.I)),
    ("api_key", re.compile(r"(api[-_]?key|vapid)\s*[:=]\s*[A-Za-z0-9+/_-]{16,}", re.I)),
]

RESET_REASONS = {  # best-effort; verifikasi silang dgn esptool utk interpretasi final
    "0x1": "POWERON_RESET", "0x3": "SW_RESET", "0x4": "OWDT_RESET",
    "0x10": "RTCWDT_RTC_RESET", "0x11": "DEEPSLEEP_RESET",
    "0x12": "SW_CPU_RESET", "0x13": "RTC_SW_SYS_RESET",
    "0x14": "RTC_SW_CPU_RESET", "0x15": "RTCWDT_BROWN_OUT_RESET",
    "0x16": "RTCWDT_RTC_RESET", "0x17": "TG0WDT_CPU_RESET",
    "0x18": "TG1WDT_CPU_RESET", "0x19": "SUPER_WDT_RESET",
}

CRITICAL_CATS = {"panic", "watchdog", "brownout", "secrets_leak"}
WARNING_CATS = {"mqtt_failure", "sensor_failure", "storage", "heap_warning", "auth_fail"}

HEAP_NUM = re.compile(r"(?:free)?\s*heap\D{0,12}(\d{2,7})(?:\s*(?:B|b|KB|bytes))?", re.I)
RST_LINE = re.compile(r"rst:(0x[0-9A-Fa-f]+)")


def now_iso():
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


class Analyzer:
    def __init__(self):
        self.counts = {name: 0 for name, _ in PATTERNS}
        self.secrets = []
        self.first_seen = {}
        self.last_seen = {}
        self.heap_samples = []          # (epoch, bytes)
        self.reset_reasons = {}
        self.lines = 0
        self.mqtt_reconnects = 0
        self.sensor_errors = 0

    def feed(self, raw, ts_epoch):
        self.lines += 1
        for name, rx in PATTERNS:
            if rx.search(raw):
                self.counts[name] += 1
                self.first_seen.setdefault(name, ts_epoch)
                self.last_seen[name] = ts_epoch
        for label, rx in SECRET_PATTERNS:
            if rx.search(raw):
                self.counts.setdefault("secrets_leak", 0)
                self.counts["secrets_leak"] = self.counts.get("secrets_leak", 0) + 1
                self.secrets.append({"label": label, "at": ts_epoch,
                                     "excerpt": self._redact(raw)})
        m = RST_LINE.search(raw)
        if m:
            code = m.group(1).lower()
            self.reset_reasons[code] = self.reset_reasons.get(code, 0) + 1
        if re.search(r"MQTT.*reconnect", raw, re.I):
            self.mqtt_reconnects += 1
        if "SENSOR_FAILURE" in raw:
            self.sensor_errors += 1
        if "heap" in raw.lower():
            hm = HEAP_NUM.search(raw)
            if hm:
                try:
                    self.heap_samples.append((ts_epoch, int(hm.group(1))))
                except ValueError:
                    pass

    @staticmethod
    def _redact(raw):
        out = raw
        for rx in (re.compile(r"jwt=\S+"), re.compile(r"refresh=\S+"),
                   re.compile(r"Bearer\s+\S+"), re.compile(r"(password|api[-_]?key|vapid)\s*[:=]\s*\S+", re.I)):
            out = rx.sub("<REDACTED>", out)
        return out[:120]

    def heap_trend(self):
        """Deteksi tren penurunan monoton (best-effort, HIL-L criterion).
        Hanya WARNING — pola, bukan diagnosis memory-leak definitif."""
        if len(self.heap_samples) < 4:
            return {"verdict": "INSUFFICIENT_DATA", "note": "< 4 sampel heap"}
        vals = [v for _, v in self.heap_samples]
        drops = sum(1 for i in range(1, len(vals)) if vals[i] < vals[i - 1])
        monotonic_ratio = drops / (len(vals) - 1)
        span_kb = (max(vals) - min(vals)) / 1024.0
        if monotonic_ratio >= 0.85 and span_kb > 8:
            return {"verdict": "WARNING", "monotonicRatio": round(monotonic_ratio, 2),
                    "spanKB": round(span_kb, 1),
                    "note": "Heap menurun konsisten — periksa potensi kebocoran (bukan diagnosis final)"}
        return {"verdict": "STABLE", "monotonicRatio": round(monotonic_ratio, 2),
                "spanKB": round(span_kb, 1),
                "minKB": round(min(vals) / 1024.0, 1), "maxKB": round(max(vals) / 1024.0, 1)}

    def verdict(self):
        cats = set(k for k, v in self.counts.items() if v > 0)
        if cats & CRITICAL_CATS:
            return "CRITICAL", 1
        if cats & WARNING_CATS:
            return "WARNING", 1
        if self.counts.get("reboot", 0) > 0 and self.lines > 0:
            return "NOTE_REBOOT_OBSERVED", 0
        return "PASS", 0

    def summary(self, label, duration_sec, source):
        verdict, code = self.verdict()
        return {
            "schemaVersion": 1,
            "tool": "serial_diagnostic.py",
            "label": label,
            "source": source,
            "capturedAt": now_iso(),
            "durationSec": duration_sec,
            "lines": self.lines,
            "categories": {k: v for k, v in self.counts.items() if v > 0},
            "mqttReconnects": self.mqtt_reconnects,
            "sensorErrors": self.sensor_errors,
            "resetReasons": {c: {"count": n, "decodedBestEffort": RESET_REASONS.get(c, "UNKNOWN")}
                             for c, n in self.reset_reasons.items()},
            "heapTrend": self.heap_trend(),
            "secretsLeakage": {"count": len(self.secrets), "occurrences": self.secrets[:20]},
            "verdict": verdict,
            "exit": code,
        }


TS_PREFIX = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[^\s]*)\s(.*)$")


def parse_line(line):
    """Terima baris dgn/tanpa stempel waktu ISO (output mode CAPTURE)."""
    m = TS_PREFIX.match(line)
    if m:
        try:
            dt = datetime.fromisoformat(m.group(1))
            return dt.timestamp(), m.group(2)
        except ValueError:
            pass
    return time.time(), line


def capture(args, analyzer):
    try:
        import serial  # pyserial
    except ImportError:
        print("[PRECONDITION] pyserial belum terpasang: pip3 install pyserial. Exit 2.")
        return 2
    if not os.path.exists(args.port):
        print(f"[PRECONDITION] port serial tidak ditemukan: {args.port}. Exit 2.")
        return 2
    outdir = os.path.join(args.outdir, args.label or os.path.basename(args.port))
    os.makedirs(outdir, exist_ok=True)
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    log_path = os.path.join(outdir, f"{stamp}_serial.log")
    sum_path = os.path.join(outdir, f"{stamp}_summary.json")
    print(f"[CAPTURE] {args.port} @ {args.baud} → {log_path} ({args.duration_sec}s)")
    start = time.time()
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:
        print(f"[PRECONDITION] gagal membuka port: {e}. Exit 2.")
        return 2
    with ser, open(log_path, "w", encoding="utf-8", errors="replace") as logf:
        while time.time() - start < args.duration_sec:
            try:
                raw = ser.readline().decode("utf-8", "replace").rstrip("\r\n")
            except Exception as e:
                print(f"[WARN] baca serial gagal: {e}")
                break
            if not raw:
                continue
            ts = now_iso()
            logf.write(f"{ts} {raw}\n")
            logf.flush()
            analyzer.feed(raw, time.time())
    summary = analyzer.summary(args.label or args.port, args.duration_sec, log_path)
    with open(sum_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print(f"[SUMMARY] {json.dumps(summary['verdict'])} → {sum_path}")
    return summary["exit"]


def analyze_file(args, analyzer):
    if not os.path.isfile(args.analyze_existing):
        print(f"[PRECONDITION] file tidak ditemukan: {args.analyze_existing}. Exit 2.")
        return 2
    t0 = None
    with open(args.analyze_existing, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                continue
            ts, raw = parse_line(line)
            t0 = t0 or ts
            analyzer.feed(raw, ts)
    duration = max(0, int((t0 or time.time()) and (analyzer.last_seen.get("reboot", 0) or t0) - t0)) \
        if t0 else 0
    summary = analyzer.summary(args.label or os.path.basename(args.analyze_existing),
                               duration, args.analyze_existing)
    print(json.dumps(summary, indent=2))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
    return summary["exit"]


def main():
    ap = argparse.ArgumentParser(description="Capture + analisis log serial ESP32 (HIL-L / observability)")
    ap.add_argument("--port", help="port serial, mis. /dev/ttyUSB0 (mode CAPTURE)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--duration-sec", type=int, default=600)
    ap.add_argument("--label", help="label perangkat (mis. PLTS-001) untuk penamaan direktori")
    ap.add_argument("--analyze-existing", help="file log untuk analisis offline (mode ANALYZE)")
    ap.add_argument("--outdir", default="logs")
    ap.add_argument("--out", help="tulis ringkasan JSON ke path ini (mode ANALYZE)")
    args = ap.parse_args()
    if not args.port and not args.analyze_existing:
        print("[PRECONDITION] berikan --port (capture) atau --analyze-existing (analisis). Exit 2.")
        return 2
    analyzer = Analyzer()
    if args.analyze_existing:
        return analyze_file(args, analyzer)
    return capture(args, analyzer)


if __name__ == "__main__":
    sys.exit(main())
