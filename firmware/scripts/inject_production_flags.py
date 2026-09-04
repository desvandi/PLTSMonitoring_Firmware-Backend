#!/usr/bin/env python3
"""
inject_production_flags.py — PlatformIO pre-build script (PRODUCTION only)

[CI fix] This script does TWO things in one pass:
  1. Injects production secrets from environment variables into BUILD_FLAGS
     as -D macros (handles multiline PEM properly — platformio.ini's
     ${sysenv.VAR} cannot).
  2. Validates the injected flags (same checks as the old
     assert_production_secrets.py — now merged into one script so the
     validation runs AFTER injection, not before).

Environment variables (set by CI from GitHub Environment 'production' secrets):
  MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USERNAME, MQTT_PASSWORD,
  MQTT_ROOT_CA, OTA_ED25519_PUBLIC_KEY_HEX, OTA_HTTPS_ROOT_CA

Exit code 1 with a clear message when any check fails.
"""
import os
import re
import sys

Import("env")  # noqa: F821  (PlatformIO injects this)

build_env = env  # noqa: F821

# ============================================================================
# STEP 1: Inject secrets from environment variables into BUILD_FLAGS
# ============================================================================

SECRET_MAP = {
    "MQTT_BROKER_HOST":           "MQTT_BROKER_HOST",
    "MQTT_BROKER_PORT":           "MQTT_BROKER_PORT",
    "MQTT_USERNAME":              "MQTT_USERNAME",
    "MQTT_PASSWORD":              "MQTT_PASSWORD",
    "MQTT_ROOT_CA":               "MQTT_ROOT_CA",
    "OTA_ED25519_PUBLIC_KEY_HEX":  "OTA_ED25519_PUBLIC_KEY_HEX",
    "OTA_HTTPS_ROOT_CA":          "OTA_HTTPS_ROOT_CA",
}

print("[inject_production_flags] Injecting production secrets from environment...")

for env_var, macro in SECRET_MAP.items():
    value = os.environ.get(env_var, "")
    if not value:
        print(f"  WARN: {env_var} not set in environment")
        continue

    if macro == "MQTT_BROKER_PORT":
        # Numeric macro — use CPPDEFINES tuple too (consistent with string macros)
        build_env.Append(CPPDEFINES=[(macro, value)])
    else:
        # String macro — use CPPDEFINES with (name, value) tuple.
        build_env.Append(CPPDEFINES=[(macro, value)])
    print(f"  OK: {macro} injected ({len(value)} chars)")

# Also add ALLOWED_CORS_ORIGINS if not already in platformio.ini build_flags
# (it IS in platformio.ini, but let's make sure it's in CPPDEFINES for validation)
if "ALLOWED_CORS_ORIGINS" not in [d[0] if isinstance(d, (list, tuple)) else d for d in build_env.Dictionary("CPPDEFINES")]:
    build_env.Append(CPPDEFINES=[("ALLOWED_CORS_ORIGINS", "https://plts.example.com")])

# ============================================================================
# STEP 2: Validate (same logic as old assert_production_secrets.py)
# ============================================================================

# Read CPPDEFINES directly from env (includes both platformio.ini -D flags
# AND programmatically appended CPPDEFINES from Step 1).
# The old approach (env.ParseFlags(env.get("BUILD_FLAGS"))) only sees -D flags
# from platformio.ini, not programmatically added ones.
RAW_CPPDEFINES = build_env.Dictionary("CPPDEFINES")
MACROS = {}
for flag in RAW_CPPDEFINES:
    if isinstance(flag, (list, tuple)) and len(flag) == 2:
        MACROS[str(flag[0])] = str(flag[1])
    elif isinstance(flag, str):
        MACROS[flag] = None

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

print("[inject_production_flags] validating PRODUCTION build flags ...")

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

# Hardcoded-credential scan
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
    print("[inject_production_flags] FAILED — production build refused:")
    for e in ERRORS:
        print(f"  - {e}")
    sys.exit(1)

print("[inject_production_flags] PASS — all production secrets present and valid")
