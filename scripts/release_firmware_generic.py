#!/usr/bin/env python3
"""
release_firmware_generic.py — Rilis firmware-generic + sinkron ke PWA

Audit 2026-08-27 (temuan F-D): README lama mengklaim CI menyinkronkan binari
ke PWA /public/firmware/ — mekanisme itu TIDAK ADA. Skrip ini adalah mekanisme
rilis resmi yang menggantikan klaim tersebut.

Prosedur yang dijalankan (fail-closed di setiap langkah):
  1. Build firmware-generic (pio run -e esp32dev) — bisa dilewati --skip-build
  2. Baca versi dari manifest.json
  3. GUARD KEJUJURAN: versi manifest HARUS sama dengan FIRMWARE_VERSION di
     src/plts_firmware_v1.ino — mismatch = abort (manifest bohong pada operator)
  4. Salin bootloader.bin + partitions.bin + firmware.bin (dinamai versi) ke bin/
  5. Hapus binari versi lama di bin/ (hanya satu versi aktif)
  6. Bila --pwa-path diberikan: salin 3 binari ke <pwa>/public/firmware/ dan
     tulis manifest versi PWA (path TANPA prefiks "bin/" — file duduk di
     folder yang sama dengan manifest), lalu hapus binari versi lama di sana.

Cara pakai:
  python3 scripts/release_firmware_generic.py --pwa-path ../plts_monitor_PWA_only

Setelah skrip sukses, commit kedua repo:
  (firmware repo)  git add firmware-generic && git commit -m "release: generic v<V>"
  (PWA repo)        git add public/firmware && git commit -m "feat(firmware): generic v<V>"
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # repo firmware
GENERIC = ROOT / "firmware-generic"
BUILD = GENERIC / ".pio" / "build" / "esp32dev"
INO = GENERIC / "src" / "plts_firmware_v1.ino"


def die(msg: str) -> None:
    print(f"[RELEASE] GAGAL: {msg}")
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description="Rilis firmware-generic + sinkron PWA")
    ap.add_argument("--pwa-path", type=str, default=None,
                    help="Path checkout repo PWA (untuk sinkron public/firmware/)")
    ap.add_argument("--skip-build", action="store_true",
                    help="Lewati build PlatformIO (pakai .pio/build yang ada)")
    args = ap.parse_args()

    # --- 1. Build -----------------------------------------------------------
    if not args.skip_build:
        print("[RELEASE] Building firmware-generic (pio run -e esp32dev)...")
        r = subprocess.run(["pio", "run", "-e", "esp32dev"], cwd=GENERIC)
        if r.returncode != 0:
            die("build PlatformIO gagal — perbaiki dulu, jangan rilis binari rusak")

    # --- 2. Versi dari manifest --------------------------------------------
    manifest_path = GENERIC / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        version = manifest["version"]
    except Exception as e:
        die(f"manifest.json tidak terbaca/valid: {e}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", str(version)):
        die(f"versi manifest '{version}' bukan semver X.Y.Z")

    # --- 3. GUARD KEJUJURAN: manifest == source ------------------------------
    ino_text = INO.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', ino_text)
    if not m:
        die("FIRMWARE_VERSION tidak ditemukan di src/plts_firmware_v1.ino")
    src_version = m.group(1)
    if src_version != version:
        die(
            f"MISMATCH VERSI: manifest.json={version} tapi source={src_version}. "
            f"Samakan keduanya (bump FIRMWARE_VERSION dan manifest bersamaan) "
            f"sebelum rilis — manifest tidak boleh bohong tentang isi binari."
        )

    # --- 3b. GUARD KEJUJURAN (extra, P1-remediation): header changelog ---------
    # Audit finding "version identity drift": the .ino header comment
    # (top-of-file changelog) stayed at v1.5.4 while the constant moved to
    # 1.6.0. A stale header lies to every reader doing a release diff.
    hm = re.search(r"Generic Firmware v(\d+\.\d+\.\d+)", ino_text)
    header_version = hm.group(1) if hm else None
    if header_version != version:
        die(
            f"MISMATCH VERSI HEADER: manifest.json={version} tapi header .ino="
            f"{header_version}. Perbarui komentar changelog teratas di "
            f"src/plts_firmware_v1.ino agar sama dengan versi rilis "
            f"(header basi = identitas versi drift, guard ini menutupnya)."
        )

    # --- 4. Salin binari ke bin/ --------------------------------------------
    fw_name = f"plts_firmware_v{version}.bin"
    sources = {
        "bootloader.bin": BUILD / "bootloader.bin",
        "partitions.bin": BUILD / "partitions.bin",
        fw_name: BUILD / "firmware.bin",
    }
    for name, src in sources.items():
        if not src.is_file():
            die(f"binari build tidak ditemukan: {src} (jalankan tanpa --skip-build)")
        dst = GENERIC / "bin" / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        print(f"[RELEASE] bin/{name}  ({dst.stat().st_size:,} B)")

    # --- 5. Hapus versi lama di bin/ ----------------------------------------
    removed = 0
    for old in (GENERIC / "bin").glob("plts_firmware_v*.bin"):
        if old.name != fw_name:
            old.unlink()
            print(f"[RELEASE] hapus binari lama: bin/{old.name}")
            removed += 1

    # Manifest generic: path DENGAN prefiks bin/
    generic_parts = [
        {"path": f"bin/{n}", "offset": off}
        for n, off in (("bootloader.bin", 4096), ("partitions.bin", 32768), (fw_name, 65536))
    ]
    manifest["builds"] = [{"chipFamily": "ESP32", "parts": generic_parts}]
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"[RELEASE] manifest.json (generic) → v{version}, {len(generic_parts)} parts")

    # --- 6. Sinkron ke PWA (opsional) ----------------------------------------
    if args.pwa_path:
        pwa_fw = Path(args.pwa_path) / "public" / "firmware"
        if not pwa_fw.parent.is_dir():
            die(f"folder PWA tidak ditemukan: {pwa_fw.parent} (--pwa-path salah?)")
        pwa_fw.mkdir(parents=True, exist_ok=True)
        for name in ("bootloader.bin", "partitions.bin", fw_name):
            shutil.copy2(GENERIC / "bin" / name, pwa_fw / name)
            print(f"[RELEASE] PWA public/firmware/{name}  ({(pwa_fw / name).stat().st_size:,} B)")
        for old in pwa_fw.glob("plts_firmware_v*.bin"):
            if old.name != fw_name:
                old.unlink()
                print(f"[RELEASE] hapus binari lama PWA: public/firmware/{old.name}")
        # Manifest PWA: path TANPA prefiks bin/ (file sejajar manifest)
        pwa_manifest = json.loads(json.dumps(manifest))  # deep copy
        pwa_manifest["builds"] = [{
            "chipFamily": "ESP32",
            "parts": [
                {"path": n, "offset": off}
                for n, off in (("bootloader.bin", 4096), ("partitions.bin", 32768), (fw_name, 65536))
            ],
        }]
        (pwa_fw / "manifest.json").write_text(
            json.dumps(pwa_manifest, indent=2) + "\n", encoding="utf-8"
        )
        print(f"[RELEASE] PWA public/firmware/manifest.json → v{version} (path tanpa 'bin/')")

    print(
        f"\n[RELEASE] SELESAI — firmware-generic v{version} "
        f"({'+ sinkron PWA' if args.pwa_path else 'TANPA sinkron PWA'})."
    )
    print("[RELEASE] Commit yang perlu dijalankan:")
    print(f'  firmware repo : git add firmware-generic && git commit -m "release: generic v{version}"')
    if args.pwa_path:
        print(f'  PWA repo      : git add public/firmware && git commit -m "feat(firmware): generic v{version}"')


if __name__ == "__main__":
    main()
