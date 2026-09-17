# PUSH_DELIVERY_SEMANTICS.md — Kontrak Semantika Pengiriman Push

> [AUDIT ROUND-3 — 2026-09-17] Dokumen ini menutup permintaan auditor:
> "sistem ini secara efektif at-least-once delivery, bukan exactly-once —
> itu harus dinyatakan sebagai kontrak". Sebelum dokumen ini, semantika
> tersebut hanya implisit dalam urutan tulis-ke-store vs kirim.

## 1. Kontrak resmi

| Properti | Nilai | Catatan |
|---|---|---|
| Delivery guarantee | **at-least-once per subscriber** | Bukan exactly-once; bukan at-most-once |
| Ordering | best-effort (urutan outbox) | Tidak ada jaminan urutan lintas event |
| Idempotency klien | tag notification + eventId | Browser mengganti notifikasi bertag sama |
| Jendela duplikat | hanya subscriber in-flight saat eksekusi mati antara kirim dan merge | Dipersempit dari "semua subscriber" (pra-round-3) |
| Retensi ACK | selama event masih di outbox (≤ 200 event, eviksi terminal-tertua-dulu) | Lihat §4 |

## 2. Mengapa bukan exactly-once

Web Push (RFC 8030) tidak memiliki idempotency key di protokol: push service
menerima POST terenkripsi dan tidak mendedupe. Di sisi GAS, urutan
durable-first yang benar adalah:

```
(1) tulis event PENDING ke store        ← durable sebelum efek samping
(2) kirim Web Push ke subscriber
(3) tulis marker SENT per subscriber
```

Bila eksekusi GAS mati di antara (2) dan (3), subscriber sudah menerima
notifikasi tetapi store belum mencatatnya. Retry akan mengirim ULANG ke
subscriber itu. Membalik urutan (tandai SENT sebelum kirim) justru
menghasilkan kehilangan senyap (silent loss) — lebih buruk untuk alarm
keselamatan. Maka at-least-once adalah kontrak yang benar untuk kelas
layanan ini; duplikat notifikasi KRITIS lebih aman daripada notifikasi hilang.

## 3. Bagaimana round-3 mempersempit jendela duplikat

Pra-round-3, status delivery adalah AGREGAT (sent/failed) — satu subscriber
gagal membuat SELURUH event di-retry ke SEMUA subscriber, menduplikasi push
ke yang sudah sukses. Round-3 menggantinya dengan **cursor per-subscription**:

- `delivery.perSub[subId] = 'SENT'` dicatat per langganan pada merge.
- `pushDeliverEvent_()` SKIP subscriber yang sudah bertanda SENT.
- Retry hanya menargetkan subscriber yang belum terkirim.

Sisa jendela duplikat: eksekusi mati TEPAT di antara `UrlFetchApp.fetch`
sukses dan `pushRecordDelivery_()` — dan hanya untuk subscriber yang sedang
in-flight pada saat itu. Klaim delivery (claim-token + TTL 90 detik)
mencegah dua round pengiriman berjalan bersamaan untuk event yang sama pada
eksekusi yang berbeda.

## 4. Retensi & jendela ACK (temuan P2 auditor)

Outbox dibatasi `OUTBOX_MAX_EVENTS = 200` dengan eviksi **terminal tertua
dulu** (SENT/EXPIRED/FAILED). Konsekuensi yang kini eksplisit:

- `PUSH_ACK` mengembalikan 404 untuk event yang sudah terevisi — **jendela
  ACK = jendela retensi outbox**, bukan waktu absolut.
- Event PENDING tidak dievisi selama masih ada event terminal yang lebih tua;
  bila TIDAK ada, event PENDING tertua bisa terevisi dan **dihitung** di
  `PUSH_CAPACITY_DROPPED` (dilaporkan `PUSH_STATUS.storage.capacityDropped` —
  kehilangan tidak pernah senyap).
- Untuk kebutuhan "ACK harus tersedia 24 jam", kapasitas 200 event dengan
  laju alarm normal (<< 200 event/hari) memenuhi; deployment dengan laju
  alarm lebih tinggi harus menaikkan `OUTBOX_MAX_EVENTS` DAN memverifikasi
  anggaran `STORAGE_TOTAL_LIMIT` (lihat §5).

## 5. Kapasitas Script Properties (temuan P1 auditor)

Google Apps Script membatasi property store: **9 KB per value**,
**500 KB total**. Round-3 mengubah layout penyimpanan:

| Pra-round-3 | Round-3 |
|---|---|
| `PUSH_ALARM_EVENTS` = SATU array JSON seluruh event | SATU property per event `PUSH_EVT_<eventId>` |
| `PUSH_SUBSCRIPTIONS` = SATU array JSON | SATU property per langganan `PUSH_SUB_<subId>` |
| State alarm global satu property | Per-device `PUSH_ST_<hash16>` |
| Tanpa pemeriksaan batas | Anggaran mekanis: `STORAGE_VALUE_LIMIT` 8.500 char/value, `STORAGE_TOTAL_LIMIT` 450.000 char total, eviksi + penghitung jujur |

Worst-case yang DIUJI mekanis di CI (K10, mock yang MENEGAKKAN batas nyata):
205 event ber-payload maksimum + 50 langganan + state + token
→ 0 pelanggaran 9 KB/value, 0 pelanggaran 500 KB total, retensi tetap ≤ 200.

## 6. Ekonomi kuota PropertiesService (QUOTA)

Jalur tenang (ingest tanpa tepi alarm) kini membaca O(1)–O(beberapa)
property lewat gerbang indeks `PUSH_PEND_IDX` (dibuktikan K10: ≤ 40 read,
≤ 3 write per ingest tenang), bukan membaca seluruh outbox. Ini menjaga
kuota harian PropertiesService pada fleet multi-device dengan telemetri
per-menit.

## 7. Referensi uji

- `push-alarm/tests/cross-audit-test.js` — kontrak K10 (54 asersi):
  partial-retry tanpa duplikat, lifecycle RAISED→CLEARED, isolasi
  multi-device, disiplin lock, kapasitas, quota.
- `scripts/make-gas-bundle.sh` + `scripts/verify-gas-deployment.js` —
  pengikatan revision sumber ↔ deployment live.
