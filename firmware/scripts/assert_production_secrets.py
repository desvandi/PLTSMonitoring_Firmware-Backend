#!/usr/bin/env python3
"""
assert_production_secrets.py — PlatformIO pre-build script (PRODUCTION only)

REMEDIATION 2026-08 (FW-26): platformio.ini's production env references this
script via `extra_scripts = pre:...` but the file did not exist — `pio run -e
production` failed before compilation. This script enforces the PRODUCTION
fail-closed build contract at CI time:

  - MQTT_BROKER_HOST / MQTT_BROKER_PORT (8883/8884) / MQTT_USERNAME /
    MQTT_PASSWORD / MQTT_ROOT_CA must be defined
  - No public MQTT broker
  - OTA_ED25519_PUBLIC_KEY_HEX must be exactly 64 hex chars
  - OTA_HTTPS_ROOT_CA must be a non-empty PEM
  - ALLOWED_CORS_ORIGINS must not be "*"
  - GAS_INGEST_URL (if used) must be HTTPS

Exit code 1 with a clear message when any check fails.
"""
import re
import sys

Import("env")  # noqa: F821  (PlatformIO injects this)

BUILD_FLAGS = env.ParseFlags(env.get("BUILD_FLAGS"))  # noqa: F821
MACROS = {}
for flag in BUILD_FLAGS.get("CPPDEFINES", []):
    if isinstance(flag, (list, tuple)) and len(flag) == 2:
        MACROS[str(flag[0])] = str(flag[1])
    elif isinstance(flag, str):
        MACROS[flag] = None

# [WAVE-5 / FW-C3] The documented ini form for string macros is
#   -DNAME='"value"'   (single-quoted so spaces in PEMs survive)
# ParseFlags keeps the surrounding double quotes in the value, which broke
# exact-length/hex validators before (a 64-hex key measured as 66 chars).
# Strip ONE layer of surrounding double quotes — the compiler sees the same
# clean string as a C string macro.
def _strip_quotes(v):
    if v and len(v) >= 2 and v[0] == '"' and v[-1] == '"':
        return v[1:-1]
    return v

MACROS = {k: (_strip_quotes(v) if v is not None else None) for k, v in MACROS.items()}

ERRORS = []


def require(name, validator=None, message=None):
    value = MACROS.get(name)
    if value is None or value == "":
        ERRORS.append(message or f"missing required build flag: -D{name}=...")
        return None
    if validator is not None:
        err = validator(value)
        if err:
            ERRORS.append(f"{name}: {err}")
    return value


def is_hex64(v):
    return None if re.fullmatch(r"[0-9a-fA-F]{64}", v) else "must be exactly 64 hex characters"


def is_https(v):
    return None if v.startswith("https://") else "must be an HTTPS URL"


def not_public_broker(v):
    public = ("broker.hivemq.com", "test.mosquitto.org",
              "broker.emqx.io", "public.mqtthq.com")
    return "must not be a public broker" if v in public else None


def not_wildcard(v):
    return "must not be '*'" if v.strip() == "*" else None


print("[assert_production_secrets] validating PRODUCTION build flags ...")

require("MQTT_BROKER_HOST", not_public_broker)
require("MQTT_BROKER_PORT")
require("MQTT_USERNAME")
require("MQTT_PASSWORD")
require("MQTT_ROOT_CA", lambda v: None if "BEGIN CERTIFICATE" in v else "must contain a PEM body")
require("OTA_ED25519_PUBLIC_KEY_HEX", is_hex64)
require("OTA_HTTPS_ROOT_CA", lambda v: None if "BEGIN CERTIFICATE" in v else "must contain a PEM body")
require("ALLOWED_CORS_ORIGINS", not_wildcard)

if "GAS_INGEST_URL" in MACROS and MACROS["GAS_INGEST_URL"]:
    require("GAS_INGEST_URL", is_https)

# Hardcoded-credential scan (defense in depth — mirrors scripts/secret_scan.py)
SUSPICIOUS = {
    "MQTT_PASSWORD": ("plts-dev-", "set-via-secret", "changeme", "password123"),
    "MQTT_USERNAME": ("set-via-secret", "changeme"),
}
for key, banned in SUSPICIOUS.items():
    v = MACROS.get(key) or ""
    for b in banned:
        if b.lower() in v.lower():
            ERRORS.append(f"{key}: contains placeholder value '{b}'")

if ERRORS:
    print("[assert_production_secrets] FAILED — production build refused:")
    for e in ERRORS:
        print(f"  - {e}")
    sys.exit(1)

print("[assert_production_secrets] PASS — all production secrets present and valid")
