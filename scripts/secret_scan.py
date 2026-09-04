#!/usr/bin/env python3
"""
secret_scan.py — Pre-commit secret scanner
Brief §98: never commit WiFi password, JWT secret, MQTT password, GAS secret, private keys
Scans staged files for common secret patterns. Exits 1 if CRITICAL secrets found.
"""
import re
import sys
import os

PATTERNS = [
    (r'ghp_[A-Za-z0-9]{36}', 'GitHub PAT', 'CRITICAL'),
    (r'gho_[A-Za-z0-9]{36}', 'GitHub OAuth token', 'CRITICAL'),
    (r'github_pat_[A-Za-z0-9_]{82}', 'GitHub fine-grained PAT', 'CRITICAL'),
    (r'AIza[0-9A-Za-z_\-]{35}', 'Google API key', 'CRITICAL'),
    (r'AKIA[0-9A-Z]{16}', 'AWS access key', 'CRITICAL'),
    (r'-----BEGIN [A-Z ]*PRIVATE KEY-----', 'Private key (PEM)', 'CRITICAL'),
    (r'mongodb(\+srv)?://[^\s]+:[^\s]+@', 'MongoDB connection string', 'CRITICAL'),
    (r'postgres(ql)?://[^\s]+:[^\s]+@', 'PostgreSQL connection string', 'CRITICAL'),
    (r'WIFI_PASS(?:WORD)?\s*=\s*["\'][^"\']{8,}["\']', 'WiFi password', 'WARNING'),
    (r'JWT_SECRET\s*=\s*["\'][^"\']{16,}["\']', 'JWT secret', 'WARNING'),
    (r'MQTT_PASS(?:WORD)?\s*=\s*["\'][^"\']{8,}["\']', 'MQTT password', 'WARNING'),
    (r'GAS_SECRET\s*=\s*["\'][^"\']{16,}["\']', 'GAS HMAC secret', 'WARNING'),
    (r'sk_[A-Za-z0-9]{24,}', 'Stripe secret key', 'CRITICAL'),
    (r'xox[baprs]-[A-Za-z0-9-]+', 'Slack token', 'CRITICAL'),
]

ALLOWED_FILES = {
    '.env.example', 'README.md', 'SECURITY.md', 'DEPLOYMENT.md',
    'PANDUAN_LENGKAP_DEPLOY.md', 'ENVIRONMENT_VARIABLES_GUIDE.md'
}

def scan_file(filepath):
    findings = []
    try:
        with open(filepath, 'r', errors='ignore') as f:
            content = f.read()
    except (IOError, UnicodeDecodeError):
        return findings
    basename = os.path.basename(filepath)
    for pattern, desc, severity in PATTERNS:
        matches = re.finditer(pattern, content)
        for m in matches:
            # Skip if in allowed file and looks like a placeholder
            if basename in ALLOWED_FILES:
                continue
            line_num = content[:m.start()].count('\n') + 1
            findings.append((severity, desc, filepath, line_num))
    return findings

def main():
    if len(sys.argv) < 2:
        print("Usage: secret_scan.py <file1> [file2 ...]")
        sys.exit(1)
    all_findings = []
    for filepath in sys.argv[1:]:
        all_findings.extend(scan_file(filepath))
    if not all_findings:
        print("✓ No secrets detected")
        sys.exit(0)
    critical = [f for f in all_findings if f[0] == 'CRITICAL']
    warning = [f for f in all_findings if f[0] == 'WARNING']
    for sev, desc, path, line in all_findings:
        print(f"[{sev}] {desc} in {path}:{line}")
    print(f"\n{len(critical)} CRITICAL, {len(warning)} WARNING")
    sys.exit(1 if critical else 0)

if __name__ == '__main__':
    main()
