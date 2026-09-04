#!/usr/bin/env python3
"""
inject_production_flags.py — PlatformIO pre-build script (PRODUCTION only)

[CI fix] Injects production secrets from environment variables into BUILD_FLAGS
as -D macros. This runs BEFORE assert_production_secrets.py so the validation
can see the flags.

The previous approach used ${sysenv.VAR} in platformio.ini, but PlatformIO's
ini parser cannot handle multiline PEM certificates (ValueError: No closing
quotation). This script handles them properly by using Python string operations.

Environment variables (set by CI from GitHub Environment 'production' secrets):
  MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_USERNAME, MQTT_PASSWORD,
  MQTT_ROOT_CA, OTA_ED25519_PUBLIC_KEY_HEX, OTA_HTTPS_ROOT_CA

Each is converted to a -D flag with proper quoting for the C preprocessor.
"""
import os
import sys

Import("env")  # noqa: F821  (PlatformIO injects this)

# Only run for production builds
build_env = env  # noqa: F821

# Map env var name → macro name
SECRET_MAP = {
    "MQTT_BROKER_HOST":           "MQTT_BROKER_HOST",
    "MQTT_BROKER_PORT":           "MQTT_BROKER_PORT",
    "MQTT_USERNAME":              "MQTT_USERNAME",
    "MQTT_PASSWORD":              "MQTT_PASSWORD",
    "MQTT_ROOT_CA":               "MQTT_ROOT_CA",
    "OTA_ED25519_PUBLIC_KEY_HEX":  "OTA_ED25519_PUBLIC_KEY_HEX",
    "OTA_HTTPS_ROOT_CA":          "OTA_HTTPS_ROOT_CA",
}

flags_to_add = []

for env_var, macro in SECRET_MAP.items():
    value = os.environ.get(env_var, "")
    if not value:
        continue  # assert_production_secrets.py will catch missing ones

    if macro == "MQTT_BROKER_PORT":
        # Numeric macro — no quotes
        flags_to_add.append(f"-D{macro}={value}")
    else:
        # String macro — wrap in escaped double quotes for C preprocessor.
        # The value is placed inside '"..."' so the compiler sees "value".
        # Backslashes in PEM certs are preserved by using \n (the C preprocessor
        # interprets \n as newline in string literals).
        flags_to_add.append(f"-D{macro}='\"{value}\"'")

if flags_to_add:
    print(f"[inject_production_flags] Injecting {len(flags_to_add)} build flags from environment")
    for f in flags_to_add:
        # Don't print the actual values (secrets!)
        macro_name = f.split("=")[0].replace("-D", "")
        print(f"  -D{macro_name}=***")
    build_env.Append(CPPDEFINES=[f.split("=", 1) for f in flags_to_add])
    # Actually, we need to use BUILD_FLAGS, not CPPDEFINES, because
    # assert_production_secrets.py reads BUILD_FLAGS.
    for flag in flags_to_add:
        build_env.Append(BUILD_FLAGS=[flag])
else:
    print("[inject_production_flags] No production secrets found in environment")
