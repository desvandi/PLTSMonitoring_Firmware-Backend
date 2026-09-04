# Backend GAS Push-Alarm MonitorIoT

Dua file Google Apps Script yang ditempel ke SATU proyek Apps Script:

| File | Peran |
|---|---|
| `Code.gs` | Lapisan layanan: `doGet`/`doPost`, endpoint ingest firmware (validasi token), registri langganan push, kirim alarm terenkripsi ke semua pelanggan, ACK, snapshot, rate-limit `testPush`. Identik dengan `PushService.gs` pada rilis paket - hanya nama file yang mengikuti konvensi Apps Script. |
| `WebPushCore.gs` | Implementasi kripto Web Push murni-JS (kompatibel runtime GAS): SHA-256/HMAC/HKDF, ECDH P-256, ECDSA RFC 6979, AES-128-GCM, JWT VAPID RFC 8292, enkripsi payload aes128gcm RFC 8291/8188. |

## Penyiapan singkat

1. Buah proyek baru di [script.google.com](https://script.google.com).
2. Tempel `Code.gs` dan `WebPushCore.gs` sebagai dua file terpisah.
3. Jalankan `../tools/generate-vapid-keys.js` (Node 18+) lalu isi
   **Script Properties**: `VAPID_PUBLIC_KEY`, `VAPID_PRIVATE_KEY` (rahasia),
   `FW_DEVICE_TOKEN` (dari `../tools/prep-deploy-secrets.js`).
4. Deploy sebagai **Web App**: Execute as *Me*, Access *Anyone*.
5. Salin URL `/exec` - itulah `API_BASE` untuk PWA dan firmware.

Prosedur lengkap + kamus seluruh Script Properties:
`../docs/Panduan_Deploy_Production_MonitorIoT.pdf` (Bab 4).

## Uji cepat dari editor GAS

- `simulateAlarmPush()` - kirim alarm uji ke semua pelanggan (rate-limit 60 dtk).
- `latestAlarm()` - alarm aktif terakhir (dipakai SW saat push tanpa payload).
