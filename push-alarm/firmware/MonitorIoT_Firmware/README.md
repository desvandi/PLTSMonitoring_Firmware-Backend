# MonitorIoT_Firmware — [DEPRECATED] Permukaan Legacy

> Status registry: **deprecated** (2026-09-22) — lihat
> `docs/surface-governance/SURFACE-STATUS.json`, surface id
> `push-alarm-firmware-monitoriot`.
> Seluruh berkas di direktori ini dibekukan via content pin SHA-256 dan
> dijaga oleh `scripts/check_surface_policy.py` + job CI
> `surface-policy-gate`. Modifikasi senyap ditolak fail-closed.

## Mengapa dibekukan

Direktori ini adalah firmware ekosistem **MonitorIoT (greenhouse)** —
garis keturunan tertua yang berjalan paralel dengan jalur PLTS aktif.
Ketika sebuah perbaikan keamanan mendarat di `code.gs/`,
`firmware-generic/`, atau `firmware/`, perbaikan itu **tidak sampai** ke
sini. Sudut keamanan yang tidak dimiliki permukaan ini:

| Kemampuan | code.gs/ (aktif) | firmware-generic/ (aktif) | MonitorIoT (ini) |
|---|---|---|---|
| Identitas (device_key, sequence) + ledger gap [P1-00x] | ya | ya | **tidak** |
| Autentikasi HMAC-SHA256 kontrak v2.1 | ya | — (token+device_key) | **tidak** |
| OTA bertanda-tangan Ed25519 + rollback otomatis | ya (sisi GAS) | ya | **tidak** |
| Pinning TLS root CA (GTS R1+R4) | — | ya (GAS_ROOT_CA) | ya (v1.1.0) |
| Verifikasi tulis NVS read-back [FW6-6] | — | ya | **tidak** |

Placeholder kredensial (`GANTI_PASSWORD_WIFI`, `AKfycbxGANTI_DENGAN_ID_…`)
menandakan basis kode contoh, bukan jalur produksi.

## Jalur migrasi

- Telemetri ke GAS PLTS → `firmware-generic/` (kontrak TELEMETRY v2,
  identitas `(device_key, sequence)`, OTA bertanda-tangan).
- Dashboard lokal REST + MQTT → `firmware/` (auth web + CSRF + transaksi
  relay).
- Keputusan pensiun final (hapus direktori) menunggu konfirmasi bahwa
  tidak ada fleet greenhouse yang masih flash build dari sini —
  lihat `docs/surface-governance/SURFACES.md` §Keputusan Terbuka.

## Jika benar-benar harus mengubah berkas ini

1. Buka/tulis issue dengan alasan + tinjauan keamanan.
2. Perbarui content pin berkas di
   `docs/surface-governance/SURFACE-STATUS.json` pada PR yang sama
   (perubahan pin = tanda tangan eksplisit reviewer, terlihat di diff).
3. CI `surface-policy-gate` akan tetap gagal untuk penambahan berkas baru
   yang tidak terdaftar — daftarkan juga berkas baru di registry.
