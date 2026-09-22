# push-alarm/gas/ — [FROZEN] Duplikat GAS Paralel (keputusan paritas menunggu)

> Status registry: **frozen** (2026-09-22) — lihat
> `docs/surface-governance/SURFACE-STATUS.json`, surface id
> `push-alarm-gas-duplicate`.
> Seluruh berkas di direktori ini dibekukan via content pin SHA-256 dan
> dijaga oleh `scripts/check_surface_policy.py` + job CI
> `surface-policy-gate`.

## Mengapa dibekukan (bukan dihapus)

Direktori ini berisi implementasi GAS kedua (`Code.gs` 1.352 baris +
`WebPushCore.gs` primitif kripto SHA-256/HMAC/HKDF) yang berjalan
**paralel** dengan backend GAS kanonik `code.gs/` (2.878 baris) di
root repo yang sama. Keduanya BUKAN salinan identik (md5 berbeda,
kontrak berbeda): `code.gs/` membawa kontrak identitas
`(device_key, sequence)` [P1-001..004] + autentikasi HMAC-SHA256 v2.1,
sedangkan salinan ini tidak membawa penanda tersebut yang dapat
diverifikasi.

Risiko yang dijaga: perbaikan keamanan yang mendarat di `code.gs/`
(revokasi token, anti-replay nonce, ledger gap) **tidak otomatis**
menyebar ke salinan ini — dan sebaliknya. Dua backend GAS aktif yang
diverifikasi berbeda = permukaan serangan yang tidak dapat diaudit
sebagai satu sistem.

## Keputusan terbuka (pemilik repo)

Pilih SATU dan catat di `docs/surface-governance/SURFACES.md`:

1. **Pensiunkan** salinan ini → status berubah `deprecated`, fleet yang
   menunjuk ke deployment GAS ini dimigrasikan ke `code.gs/`, lalu
   direktori dihapus pada rilis minor berikutnya.
2. **Promosikan jadi kanonik** (jika deployment GAS push-alarm memang
   jalur produksi) → naikkan kontrak P1-00x + HMAC v2.1 ke sini, status
   berubah `active`, dan `code.gs/` yang harus diputuskan.

Sampai keputusan dibuat: tidak ada perubahan konten — pin SHA-256
menolak modifikasi apa pun tanpa pembaruan pin eksplisit di registry.
