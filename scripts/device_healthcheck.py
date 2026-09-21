#!/usr/bin/env python3
"""
device_healthcheck.py — [COMMISSIONING / F5-03 telemetry-freshness 2026-09]
Healthcheck READ-ONLY untuk perangkat PLTS-ESP32 nyata (commissioning
lapangan + pemantauan Gate 10). Semua permintaan hanya GET + satu POST
login; TIDAK ADA mutasi relay/konfigurasi/OTA. Logout hanya dijalankan
bila diminta eksplisit (--logout-revoke-sessions) karena logout mencabut
SEMUA refresh slot server-side (AUTH-GATE-07).

Binding temuan/gate: F5-03 (kesegaran telemetri), F16 observability,
Gate 10 pemantauan soak. Menghasilkan JSON machine-readable (--json).

Kredensial HANYA via environment (tidak pernah di argv/log):
  DEVICE_USER, DEVICE_PASS       (fase terautentikasi; tanpa ini hanya
                                  fase read-only tak terautentikasi)

Exit:  0 = HEALTHY / DEGRADED (tidak ada kegagalan kritikal)
       1 = UNHEALTHY (kegagalan kritikal TERUKUR)
       2 = precondition error (tidak terjangkau / argumen / kredensial
           salah) — TIDAK PERNAH silently pass (disiplin anti-false-closure)
"""
import argparse
import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
from http.cookiejar import CookieJar

# ---------------------------------------------------------------- konstanta
FRESHNESS_BUDGET_SEC = 60        # F5-03: usia telemetry maksimum yang sehat
HEAP_CRITICAL = 16 * 1024        # byte
HEAP_WARNING = 32 * 1024
RSSI_CRITICAL = -85
RSSI_WARNING = -75
HTTP_TIMEOUT = 10
CRASHLOOP_MARKERS = ("CRASHLOOP", "BOOT_GUARD")

CRITICAL = "CRITICAL"
WARNING = "WARNING"
INFO = "INFO"


class Checks:
    def __init__(self):
        self.items = []

    def add(self, name, status, detail, value=None):
        self.items.append({
            "check": name, "status": status, "detail": detail, "value": value,
        })
        mark = {"CRITICAL": "!!", "WARNING": " !", "INFO": " ·", "PASS": "OK"}[status]
        print(f"  {mark}  {name:<38} {detail}")

    @property
    def criticals(self):
        return [c for c in self.items if c["status"] == CRITICAL]

    @property
    def warnings(self):
        return [c for c in self.items if c["status"] == WARNING]


def make_opener(base_url, insecure_tls, confirm_insecure):
    ctx = ssl.create_default_context()
    if base_url.startswith("https"):
        if insecure_tls:
            if not confirm_insecure:
                print("[PRECONDITION] --insecure-tls memerlukan --i-confirm-insecure-tls "
                      "(sertifikat self-signed LAN). Exit 2.")
                sys.exit(2)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
    jar = CookieJar()
    return urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(jar),
        urllib.request.HTTPSHandler(context=ctx),
    )


def request(opener, method, url, body=None, headers=None, timeout=HTTP_TIMEOUT):
    """Kembalikan (status_code, headers, parsed_json_or_None)."""
    data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with opener.open(req, timeout=timeout) as resp:
            raw = resp.read()
            try:
                parsed = json.loads(raw.decode("utf-8", "replace"))
            except Exception:
                parsed = None
            return resp.status, dict(resp.headers), parsed
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            parsed = json.loads(raw.decode("utf-8", "replace"))
        except Exception:
            parsed = None
        return e.code, dict(e.headers), parsed
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise ConnectionError(str(e)) from e


def unwrap(parsed):
    """Firmware membungkus respons: {success, message, data}. Terima keduanya."""
    if isinstance(parsed, dict) and isinstance(parsed.get("data"), dict):
        merged = dict(parsed["data"])
        for k in ("success", "message"):
            if k in parsed:
                merged.setdefault("_" + k, parsed[k])
        return merged
    return parsed if isinstance(parsed, dict) else {}


# ---------------------------------------------------------------- fase 1
def phase_unauthenticated(opener, base, ck):
    print("\n[FASE 1] probe read-only (tanpa kredensial)")
    try:
        code, hdrs, _ = request(opener, "GET", base + "/api/version")
    except ConnectionError as e:
        ck.add("device.reachable", CRITICAL, f"tidak terjangkau: {e}")
        return False
    ck.add("device.reachable", "PASS", f"HTTP {code}", code)

    code, hdrs, _ = request(opener, "GET", base + "/api/status")
    if code == 401:
        ck.add("auth.enforced", "PASS", "GET /api/status tanpa sesi ditolak 401")
    elif code == 200:
        ck.add("auth.enforced", WARNING, "/api/status dapat diakses tanpa sesi — "
              "verifikasi apakah memang endpoint publik by-design")
    else:
        ck.add("auth.enforced", INFO, f"GET /api/status → {code}")

    lower = {k.lower(): v for k, v in hdrs.items()}
    sec_ok = True
    for h in ("x-content-type-options", "x-frame-options"):
        if h not in lower:
            ck.add(f"security.header.{h}", WARNING, "tidak ada di respons")
            sec_ok = False
    if sec_ok:
        ck.add("security.headers", "PASS",
               "nosniff + XFO hadir di respons firmware")
    if "x-powered-by" in lower:
        ck.add("security.header.xpoweredby", WARNING,
               f"x-powered-by bocor: {lower['x-powered-by'][:40]}")
    else:
        ck.add("security.header.xpoweredby", "PASS", "tidak ada x-powered-by")

    # Probe malformed-JSON pada /api/login: gagal SEBELUM cek kredensial,
    # tidak menyentuh rate-limit/AUTH_FAIL (banding AuthHandlers.cpp:56-60).
    code, _, _ = request(opener, "POST", base + "/api/login", body="{invalid-json")
    if code == 400:
        ck.add("auth.malformedRejected", "PASS", "payload rusak ditolak 400")
    elif code == 503:
        ck.add("auth.provisioned", CRITICAL,
               "503 auth system not ready — perangkat belum terprovision (fail-closed teramati)")
    else:
        ck.add("auth.malformedRejected", INFO, f"POST /api/login malformed → {code}")
    return True


# ---------------------------------------------------------------- fase 2
def phase_authenticated(opener, base, ck):
    user = os.environ.get("DEVICE_USER", "")
    pw = os.environ.get("DEVICE_PASS", "")
    if not user or not pw:
        ck.add("auth.deepChecks", INFO,
               "DEVICE_USER/DEVICE_PASS tidak diset — hanya probe fase 1")
        return None
    print("\n[FASE 2] login + healthcheck terautentikasi (READ-ONLY)")
    code, hdrs, parsed = request(opener, "POST", base + "/api/login",
                                 body=json.dumps({"username": user, "password": pw}))
    if code == 401:
        ck.add("auth.login", INFO, "kredensial ditolak (401) — precondition gagal")
        return None
    if code == 429:
        ck.add("auth.login", INFO, "rate-limit aktif (429) — coba lagi nanti")
        return None
    if code == 503:
        ck.add("auth.login", CRITICAL, "auth not ready (503)")
        return None
    if code != 200:
        ck.add("auth.login", CRITICAL, f"login HTTP {code}")
        return None
    ck.add("auth.login", "PASS", "200 + sesi diterbitkan")
    csrf = (unwrap(parsed).get("csrfToken")
            or unwrap(parsed).get("_data", {}).get("csrfToken", ""))
    return {"csrf": csrf}


def collect_status(opener, base, ck):
    code, _, parsed = request(opener, "GET", base + "/api/status")
    if code != 200:
        ck.add("status.endpoint", CRITICAL, f"GET /api/status → {code}")
        return None
    ck.add("status.endpoint", "PASS", "200")
    return unwrap(parsed)


def collect_diagnostics(opener, base, ck):
    code, _, parsed = request(opener, "GET", base + "/api/diagnostics")
    if code != 200:
        ck.add("diagnostics.endpoint", WARNING, f"GET /api/diagnostics → {code}")
        return None
    ck.add("diagnostics.endpoint", "PASS", "200")
    return unwrap(parsed)


def collect_alarms(opener, base, ck):
    code, _, parsed = request(opener, "GET", base + "/api/alarms")
    if code != 200:
        ck.add("alarms.endpoint", WARNING, f"GET /api/alarms → {code}")
        return []
    return parsed if isinstance(parsed, list) else (
        parsed.get("data") if isinstance(parsed, dict) else [])


def evaluate(status, diag, alarms, ck, freshness_budget=FRESHNESS_BUDGET_SEC):
    print("\n[EVALUASI]")
    if status:
        ts = status.get("timestamp")
        if isinstance(ts, (int, float)):
            age = time.time() - float(ts) if float(ts) < 10**11 else time.time() - float(ts) / 1000.0
            if age > freshness_budget:
                ck.add("telemetry.freshness", CRITICAL,
                       f"telemetry basi {int(age)}s > {freshness_budget}s (F5-03)", round(age, 1))
            else:
                ck.add("telemetry.freshness", "PASS", f"usia {int(age)}s", round(age, 1))
        tq = str(status.get("timeQuality", ""))
        if tq and tq.upper() not in ("VALID", "SYNCED", "NTP", "OK"):
            ck.add("clock.quality", CRITICAL, f"timeQuality={tq} — perintah "
                   "ber-expiresAt akan ditolak CLOCK_INVALID (GATE-1 PH8-03)", tq)
        else:
            ck.add("clock.quality", "PASS", f"timeQuality={tq}", tq)
        rr = str(status.get("resetReason", ""))
        if any(m in rr.upper() for m in CRASHLOOP_MARKERS):
            ck.add("boot.stability", CRITICAL, f"resetReason={rr}")
        elif rr:
            ck.add("boot.stability", INFO, f"resetReason={rr}", rr)
        up = status.get("uptimeSeconds")
        if isinstance(up, (int, float)):
            ck.add("boot.uptime", INFO, f"{int(up)}s ({up/3600:.1f}j)", int(up))
        if status.get("relayAvailable") is False:
            ck.add("relay.available", CRITICAL, "relay expander tidak tersedia")
        elif status.get("relayAvailable") is True:
            ck.add("relay.available", "PASS", "PCF8574 tersedia")

    if diag:
        heap = diag.get("minFreeHeap")
        if isinstance(heap, (int, float)):
            if heap < HEAP_CRITICAL:
                ck.add("heap.minFree", CRITICAL, f"{int(heap)}B < {HEAP_CRITICAL}B", heap)
            elif heap < HEAP_WARNING:
                ck.add("heap.minFree", WARNING, f"{int(heap)}B < {HEAP_WARNING}B", heap)
            else:
                ck.add("heap.minFree", "PASS", f"{int(heap)}B", heap)
        rssi = diag.get("wifiRssi")
        if isinstance(rssi, (int, float)):
            if rssi < RSSI_CRITICAL:
                ck.add("wifi.rssi", CRITICAL, f"{rssi} dBm", rssi)
            elif rssi < RSSI_WARNING:
                ck.add("wifi.rssi", WARNING, f"{rssi} dBm", rssi)
            else:
                ck.add("wifi.rssi", "PASS", f"{rssi} dBm", rssi)
        if diag.get("spoolJournalDegraded") is True:
            ck.add("spool.journalDegraded", WARNING,
                   "retensi jujur terdegradasi (GATE-7b) — kapasitas < target")
        spool = diag.get("spoolSize")
        if isinstance(spool, (int, float)):
            ck.add("spool.backlog", INFO, f"{int(spool)} record tertunda", spool)
        for key in sorted(diag):
            if "mqtt" in key.lower():
                ck.add(f"mqtt.{key}", INFO, str(diag[key])[:80], diag[key])

    if isinstance(alarms, list):
        crit = [a for a in alarms if str(a.get("severity", "")).upper() == "CRITICAL"]
        warn = [a for a in alarms if str(a.get("severity", "")).upper() == "WARNING"]
        if crit:
            ck.add("alarms.active", CRITICAL, f"{len(crit)} alarm Critical aktif", len(crit))
        elif warn:
            ck.add("alarms.active", WARNING, f"{len(warn)} alarm Warning aktif", len(warn))
        elif alarms is not None:
            ck.add("alarms.active", "PASS", "tidak ada alarm aktif", 0)


def maybe_logout(opener, base, csrf, do_logout, ck):
    if not do_logout:
        ck.add("session.hygiene", INFO,
               "sesi dibiarkan kedaluwarsa alami (logout mencabut SEMUA sesi — "
               "gunakan --logout-revoke-sessions bila commissioning selesai)")
        return
    code, _, _ = request(opener, "POST", base + "/api/logout", body="{}",
                         headers={"X-CSRF-Token": csrf or ""})
    ck.add("session.hygiene", INFO, f"logout → {code} (semua refresh slot dicabut)", code)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--device", required=True, help="URL basis perangkat, mis. http://192.168.1.50")
    ap.add_argument("--json", action="store_true", help="keluarkan JSON machine-readable")
    ap.add_argument("--out", help="tulis JSON ke file ini")
    ap.add_argument("--freshness-budget", type=int, default=FRESHNESS_BUDGET_SEC)
    ap.add_argument("--insecure-tls", action="store_true",
                    help="perangkat HTTPS dengan sertifikat self-signed (LAN)")
    ap.add_argument("--i-confirm-insecure-tls", action="store_true")
    ap.add_argument("--logout-revoke-sessions", action="store_true",
                    help="logout di akhir (mencabut SEMUA sesi server-side)")
    args = ap.parse_args()

    base = args.device.rstrip("/")
    if not base.startswith(("http://", "https://")):
        base = "http://" + base

    print("=" * 72)
    print(f"DEVICE HEALTHCHECK — {base}")
    print("=" * 72)
    ck = Checks()
    opener = make_opener(base, args.insecure_tls, args.i_confirm_insecure_tls)

    reachable = phase_unauthenticated(opener, base, ck)
    session = None
    status = diag = None
    alarms = []
    if reachable:
        session = phase_authenticated(opener, base, ck)
        if session is not None:
            status = collect_status(opener, base, ck)
            diag = collect_diagnostics(opener, base, ck)
            alarms = collect_alarms(opener, base, ck)
            evaluate(status, diag, alarms, ck, args.freshness_budget)
            maybe_logout(opener, base, session.get("csrf"),
                         args.logout_revoke_sessions, ck)

    crit_n, warn_n = len(ck.criticals), len(ck.warnings)
    if not reachable:
        verdict, exit_code = "PRECONDITION_FAILED", 2
    elif crit_n:
        verdict, exit_code = "UNHEALTHY", 1
    elif warn_n:
        verdict, exit_code = "DEGRADED", 0
    else:
        verdict, exit_code = "HEALTHY", 0

    report = {
        "schemaVersion": 1,
        "tool": "device_healthcheck.py",
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "device": base,
        "deviceId": (status or {}).get("deviceId"),
        "firmware": (status or {}).get("firmwareVersion"),
        "verdict": verdict,
        "criticalCount": crit_n,
        "warningCount": warn_n,
        "checks": ck.items,
        "statusSnapshot": status,
        "diagnosticsSnapshot": diag,
    }
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    if args.json:
        print(json.dumps(report, indent=2, default=str))

    print("\n" + "=" * 72)
    print(f"RESULT: {verdict}  (critical={crit_n}, warning={warn_n})  exit={exit_code}")
    print("=" * 72)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
