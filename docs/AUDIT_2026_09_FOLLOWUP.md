# Audit Follow-Up 2026-09 — Relay Subsystem (Poin 398–412)

> **Scope**: jawaban engineer atas audit lanjutan auditor p.398–412 atas basis
> `22a0e001` (firmware) / `f81b7aee` (PWA). Poin 🟢 yang dikonfirmasi RESOLVED
> tidak diulang di sini; dokumen ini menutup 2 temuan korektif (p.401, p.402),
> 1 bug laten yang ditemukan saat remediasi p.407, dan requirement test
> (p.400, p.403, p.407, p.409).
>
> **Bukti**: `scripts/test_relay_safety_2026_09.py` (39 asersi, Level-2
> evidence — masuk CI otomatis via job `test` yang menjalankan semua
> `scripts/test_*.py`), compile `development` + `staging` lokal SUCCESS.

---

## Ringkasan status

| Poin | Temuan auditor | Status engineer | Bukti |
|------|----------------|-----------------|-------|
| 400 | applyCommand executor-only perlu static call-site gate | ✅ Gate implementasi + masuk CI | test `[p.400]` (4 asersi) |
| 401 | 🔴 Queue diproses SEBELUM maxOnTime | ✅ FIXED — reorder + defense-in-depth | test `[p.401]` (6 asersi) |
| 402 | 🔴 Pulse timer tidak rollover-safe | ✅ FIXED — signed-difference pattern | test `[p.402]` (9 asersi) |
| 403 | 🟠 Pulse overwrite belum auditable | ✅ FIXED — TX identity + annotate | test `[p.403]` (10 asersi) |
| 407 | 🟠 Perlu fault-injection test SHADOW_UNKNOWN | ✅ Source fix + test + item acceptance fisik | test `[p.407]` (5 asersi) + T9 |
| 409 | 🟠 Precedence timer belum formal | ✅ Safety Decision Matrix di header + enforce + test | test `[p.409]` (5 asersi) + T10 |

---

## p.401 — Safety ordering (🔴 P1) → FIXED

**Temuan**: `tick()` memproses command queue sebelum `_checkMaxOnTime()`,
menyisakan window satu tick (≤200 ms) di mana command ON bisa dieksekusi
sebelum flag `maxOnTimeForced` diperbarui oleh safety supervisor.

**Remediasi (dua lapis, sesuai rekomendasi auditor):**

1. **Reorder eksekutor** — `RelayController::tick()` sekarang:

   ```
   1. I²C shadow recovery   (executor health dulu)
   2. _checkMaxOnTime()    (safety enforcement — SEBELUM queue)
   3. processCommandQueue() (command normal)
   4. _processPulses()     (pulse expiry)
   5. lockout transitions  (ARMED → NORMAL)
   ```

   Kontrak ini didokumentasikan di header sebagai
   *EXECUTOR TICK ORDERING CONTRACT* — window antara "limit terlampaui" dan
   "flag ter-update" tertutup: command ON yang dequeue selalu diadili dengan
   flag yang sudah segar.

2. **Defense-in-depth di `applyCommand()`** — jalur ON/PULSE kini mengevaluasi
   hard limit **langsung dari `millis() - onSinceMs`**, bukan hanya membaca
   flag hasil tick sebelumnya. Jika limit sudah terlampaui: channel di-force
   OFF saat itu juga, masuk lockout TRIPPED, dan ON di-BLOCK. Invarian tetap
   berlaku walau fungsi ini suatu saat dipanggil di luar kontrak tick
   (call-site baru, harness test).

   Catatan: branch `!_driverAvailable` tetap drain queue terlebih dahulu
   supaya command yang menunggu mendapat hasil final `Failed`, bukan menggantung.

## p.402 — Pulse rollover (🔴 P1) → FIXED

**Temuan**: `if (now >= _pulses[ch].offAtMs)` salah melintasi wrap `millis()`
(~49,7 hari).

**Remediasi**: helper baru `Core::deadlineReached(now, deadline)` di
`Core/Common.h` dengan pola signed-difference
`(int32_t)(now - deadline) >= 0` — valid untuk deadline < 2³¹ ms di masa depan
(batas relay firmware maksimal 60 s). `_processPulses()` menggunakannya;
pembanding absolut dihapus dari source. Test mem-mirror algoritma di Python
dan membuktikan 4 skenario wrap: (a) skenario auditor persis — pulse 100 ms
dijadwalkan 0x10 ms sebelum wrap tidak dianggap expired tepat setelah wrap;
(b) pulse panjang yang deadlinenya wrap tidak langsung dianggap expired
(kesalahan pola lama: OFF terlalu cepat); (c) deadline yang sudah lewat tepat
sebelum wrap tetap terdeteksi expired setelah wrap (kesalahan pola lama: OFF
tertunda ~49,7 hari / tidak pernah); (d) tidak ada false positive jauh dari
deadline.

## p.407 — Hasil jujur saat I²C gagal (🟠 P1 test req) → SOURCE FIX + TEST

**Temuan auditor**: TX yang dequeue saat `SHADOW_UNKNOWN` harus berakhir
`FAILED/UNKNOWN`, bukan `EXECUTED` — perlu dibuktikan.

**Bug laten yang ditemukan saat remediasi**: `applyCommand()` TIDAK
memeriksa flag fault setelah `_applyChannelState()` — jika write I²C gagal,
transaksi tetap dilaporkan `Applied` → tercatat **EXECUTED** di result ring.
Ini melanggar kontrak kejujuran hasil yang didokumentasikan sendiri.

**Remediasi**:
- Ketiga jalur mutasi (ON / OFF / PULSE) kini memeriksa `_state[ch].fault`
  setelah write dan mengembalikan `Failed` dengan pesan eksplisit
  "I²C write FAILED, state UNKNOWN". Executor mempromosikan `Failed` →
  `Unknown` saat shadow unknown (mekanisme yang sudah ada).
- Jalur PULSE: write ON yang gagal → pulse **tidak dijadwalkan** sama sekali.
- Auto-OFF pulse yang write-nya gagal → pulse dihentikan dan dianotasi
  "auto-OFF write FAILED — deferred to I²C recovery" (recovery di tick
  berikutnya meng-drive ALL OFF — arah aman).
- I²C recovery dipindah ke urutan pertama tick (lihat p.401) sehingga window
  SHADOW_UNKNOWN yang bisa memproses command tersisa maksimal satu tick.
- **Fault-injection fisik** ditambahkan sebagai acceptance test T9 (bawah).

## p.403 — Pulse transaction identity (🟠 P2) → FIXED

`PulseEntry` kini membawa `transactionId` + `generation` (safety generation
saat penjadwalan). `applyCommand()` menerima `transactionId` dari
`processCommandQueue`. Semua event lifecycle pulse menganotasi record TX
pemiliknya di result ring (hasil terminal TIDAK ditulis ulang — hanya
memperkaya jejak audit):

| Event | Anotasi di record TX |
|---|---|
| Auto-OFF normal | `pulse completed (auto-OFF)` |
| Pulse baru menimpa pulse lama | TX lama: `superseded by newer pulse` |
| E-WAVE | `cancelled by E-WAVE emergency cascade` |
| all_off | `cancelled by all_off` |
| maxOnTime FORCE OFF | `cancelled by maxOnTime FORCE OFF` |
| Recovery I²C | `cancelled by I²C shadow recovery (ALL OFF)` |
| Auto-OFF gagal write | `auto-OFF write FAILED — deferred to I²C recovery` |

Overwrite semantics kini eksplisit dan dapat diaudit: TX-A "selesai" hanya
setelah anotasinya menunjukkan ia digantikan TX-B.

## p.409 — Safety Decision Matrix (🟠 P1) → DOKUMENTASI + ENFORCE + TEST

Precedence formal didokumentasikan di `RelayController.h` dan ditegakkan
di source (masing-masing diverifikasi oleh test):

```
1. EMERGENCY (E-WAVE)      — bypass minOnTime/lockout/queue, bump generation
2. maxOnTime FORCE OFF     — mengevaluasi SEBELUM queue; mem-cancel pulse
                             pending; bypass minOnTime
3. pulse expiry            — tunduk minOnTime (DEFERRED, tidak pernah
                             dibatalkan diam-diam); dibatalkan oleh 1/2
4. normal OFF / minOnTime / interlock / antiChatter
```

Kasus auditor (pulse 60 s + maxOnTime 30 s + minOnTime 40 s): pada detik ke-30
maxOnTime menang — channel force OFF, pulse dibatalk, lockout TRIPPED.
minOnTime tidak menghambat force-off (aman untuk beban: OFF adalah arah aman;
minOnTime hanya melindungi dari OFF *normal* yang premature).

## p.400 — Static call-site gate (🟡) → IMPLEMENTASI

`scripts/test_relay_safety_2026_09.py` bagian `[p.400]` memindai seluruh
file `.cpp/.h/.ino` (setelah strip komentar): tidak boleh ada
`relaysController.applyCommand(...)` / `relaysController.allOffWithResult(...)`
di mana pun — semua ingress wajib `queueCommand()`. Gate ini berjalan otomatis
di CI pada setiap push/PR, sehingga call-site ilegal akan mem-break build.

## Item acceptance fisik baru (lanjutan T1–T8)

### T9 — Relay fault-injection I²C (membuktikan p.407 di hardware)
1. Perangkat normal, relay CH0 ON → konfirmasi EXECUTED via
   `GET /api/relays/transactions/{id}`.
2. Putus / short SDA-SCL (atau tarik PCF8574 keluar) → kirim command ON CH1.
3. **PASS jika**: transaksi berakhir `FAILED` atau `UNKNOWN` (bukan
   `EXECUTED`), alarm RELAY_FAULT Critical ter-raise, PWA menampilkan
   state UNKNOWN/fault.
4. Pasang kembali → tunggu ≤ 1 tick recovery → kirim ON CH1 → EXECUTED,
   alarm fault hilang.

### T10 — Safety matrix timing (membuktikan p.401 + p.409 di hardware)
1. Set maxOnTime CH0 = 5 s. ON CH0. Tunggu 6 s.
2. Dalam window ≤ 200 ms setelah batas terlampaui, kirim command ON CH0
   (enqueue sebelum force-off terjadi).
3. **PASS jika**: transaksi ON berakhir `BLOCKED` (bukan EXECUTED),
   `maxOnTimeForced=true`, channel tetap OFF.
4. Ulangi dengan pulse: pulse 10 s + maxOnTime 3 s → pada ~3 s channel
   force OFF dan record pulse ber-anotasi `cancelled by maxOnTime FORCE OFF`.

### T11 — Pulse lifecycle audit trail (membuktikan p.403 di hardware)
1. Kirim pulse CH0 10 s → catat TX-A.
2. Setelah 2 s, kirim pulse CH0 30 s → catat TX-B.
3. **PASS jika**: query TX-A menunjukkan `superseded by newer pulse`,
   dan setelah ~30 s TX-B menunjukkan `pulse completed (auto-OFF)`.

---

## Verifikasi

- Compile: `pio run -e development -e staging` → SUCCESS.
- `scripts/test_relay_safety_2026_09.py`: **39/39 PASS** (dan seluruh 27 file
  `scripts/test_*.py` lama tetap hijau).
- Scheduler/Automation (p.408): tetap *future* — tidak diklaim Production
  Grade; konfirmasi klasifikasi auditor dihargai.

---

# Round 2 — p.413–415: Transaction durability & reconciliation (12 Sep 2026)

Auditor menutup p.398–412 (commit `5d36f166`) dan mengangkat tiga temuan
baru pada lapisan transaction relay. Round ini menutup ketiganya.

## p.413 — Result ring 8 entry bukan authoritative (🔴 P1) → SELESAI

**Temuan**: `getTransactionResult()` hanya membaca RAM ring 8 entry; setelah
eviction, transaksi terminal bisa terlihat `PENDING` — "terminal transaction
terlihat non-terminal setelah record dieviction".

**Remediasi**:
1. `RelayController::_recordTransactionResult()` kini menulis **dua kali**:
   ring RAM (fast cache) **dan** `TransactionJournal::updateAck()` (NVS,
   authoritative). Setiap verdict terminal durable sejak momen eksekusi.
2. `GET /api/relays/transactions/{id}` memakai rantai resolusi:
   `ring (fast) → journal (durable) → UNKNOWN jujur`. Tidak ada jalur yang
   mengembalikan `PENDING` abadi; respons membawa `source: ring|journal`
   untuk observabilitas.
3. **Create-on-demand** di `updateAck()`: verdict executor yang *menang*
   dari journal-write ingress (race `queueCommand()` vs
   `storeTransaction()` di handler REST — queue masuk duluan, journal
   belakangan) tetap durable: entri terminal dibuat dengan identitas sama;
   `storeTransaction()` ingress yang datang terlambat jatuh ke cabang
   idempotent-duplicate dan **tidak pernah menimpa** ack terminal dengan
   ack QUEUED basi.

## p.414 — Reboot menghapus seluruh result ring (🔴 P1) → SELESAI

**Temuan**: `begin()` menghapus ring → outcome prareboot tak terjawab.

**Remediasi**:
1. `TransactionJournal::begin()` memuat ulang entri dari NVS; terminal
   outcome prareboot terjawab dari journal (`source: journal`).
2. **Boot marker**: journal menyimpan boot counter monotonik (NVS,
   `putUInt("boot")`). Kedua ack submission relay (`handleRelayCommand` +
   `handleAllOff`) membubuhkan `boot: N`. Resolusi GET:
   - ack submission + `boot == boot sekarang` → `QUEUED` (in-flight jujur);
   - ack submission + `boot != boot sekarang` (atau legacy tanpa marker,
     yang pasti prareboot) → **`TERMINAL/UNKNOWN`** — "lost at reboot /
     interrupted mid-execution" — rekonsiliasi berhenti, tidak berlarut
     menjadi PENDING. (Antrean command bersifat RAM-only; tanpa marker,
     entri lama tak mungkin masih dieksekusi.)

## p.415 — Queue-8 vs Result-8 vs lifecycle (🟠) → SELESAI

**Remediasi** (model lifecycle jurnalistik, diadaptasi dari usulan auditor):

```
RECEIVED → DURABLE → QUEUED   = satu tulisan NVS di ingress (submission ack)
EXECUTING                      = antrean RAM → executor (implisit)
EXECUTED/BLOCKED/REJECTED/
FAILED/UNKNOWN                 = tulisan NVS kedua di verdict executor
```

- `RESULT_RING_SIZE=8` dipertahankan **hanya** sebagai fast RAM cache —
  auditor eksplisit membolehkannya; sumber authoritative = journal 16 slot
  (retensi didokumentasikan di `TransactionJournal.h`).
- Identitas immutabel: `commandAck` tidak pernah ditulis ulang dengan hash
  berbeda (`updateAck` menolak mismatch); `commandHash` tidak berubah.
- **Mutasi lintas-task diserialisasi**: `storeTransaction` (networkTask,
  ingress) vs `updateAck` (relayTask, verdict) kini dilindungi mutex
  internal — tanpa ini, eviksi slot oleh ingress bisa berpacu dengan
  re-commit slot oleh executor dan RAM/NVS saling kontradiksi.
- `relayTask` stack 4K→6K (frame `StaticJsonDocument<512>` baru di jalur
  executor; preseden yang sama dengan `otaTask` [v1.6.3]).

## Deployment status (Vercel FAILURE pada `5d36f166`) → SELESAI

Akar masalah: project Vercel `plts-monitor-push-alarm` **ter-link ke repo
yang salah** (`PLTSMonitoring_Firmware-Backend`, tanpa rootDirectory) —
setiap push firmware memicu build push-alarm yang pasti ERROR (fail-closed;
alias produksi tidak pernah tertimpa). Perbaikan via Vercel API:
project di-delete + di-recreate dengan nama sama (domain default
`plts-monitor-push-alarm.vercel.app` tetap), link benar ke
`PLTSMonitoring_PWA` + `rootDirectory=pwa-push-alarm`, lalu produksi
di-restore via deployment git (sha `f81b7aee`, READY). Semua rute statis
200 terverifikasi ulang. Push ke repo firmware tidak lagi memicu konteks
Vercel — combined GitHub status firmware kini bersih.

## Item acceptance fisik baru (lanjutan T9–T11)

### T12 — Durabilitas transaksi lintas reboot (membuktikan p.413/414 di hardware)
1. Kirim ON CH0 → catat TX-R; konfirmasi `GET /transactions/TX-R` =
   `TERMINAL/EXECUTED` (`source: ring`).
2. Reboot ESP32 (power-cycle).
3. **PASS jika**: `GET /transactions/TX-R` tetap `TERMINAL/EXECUTED` dengan
   `source: journal` (bukan PENDING/UNKNOWN).

### T13 — Eviksi ring >8 transaksi (membuktikan p.413 di hardware)
1. Kirim 10 perintah relay berurutan (TX-1 … TX-10), semua EXECUTED.
2. **PASS jika**: `GET /transactions/TX-1` tetap `TERMINAL/EXECUTED`
   (`source: journal`); `GET /transactions/TX-9/TX-10` `source: ring`.
3. (Di luar jendela retensi 16: transaksi ke-17+ mengevict TX-1 →
   `GET /transactions/TX-1` = `TERMINAL/UNKNOWN` jujur, bukan PENDING.)

## Verifikasi Round 2

- Compile: `pio run -e development -e staging` → SUCCESS.
- `scripts/test_transaction_durability_2026_09.py`: **30/30 PASS**
  (kontrak statis p.413/414/415 + Python mirror rantai resolusi:
  eviksi, reboot, in-flight, lost-at-reboot, race create-on-demand,
  never-accepted id).
- Seluruh 26 file `scripts/test_*.py` lama tetap hijau (termasuk 39/39
  dari `test_relay_safety_2026_09.py`).
- T9–T13 tetap menunggu bukti fisik (lihat catatan auditor: source test ≠
  physical acceptance evidence).

---

# Round 3 — p.418/425/427/432/434: Cross-subsystem re-audit remediation (12 Sep 2026)

Re-audit auditor atas commit `97caa646` menutup p.398–417 dan membuka fokus
baru. Round ini menutup semua temuan source-level yang dapat ditindaklanjuti
tanpa hardware: requestId transport identity (p.418), TLS variant gate
(p.425), factory reset single-source (p.427), kontrak delivery MQTT yang
jujur (p.432), dan relay.config yang jujur fail-closed (p.434).

## p.418 — requestId diisi transactionId (🟠 P2 metadata) → SELESAI

**Temuan**: 3 call-site mengisi `qc.requestId` dengan transactionId
(RelayHandlers ×2, MqttConfigReceiver ×1) — metadata transport identity
menjadi salah, observability tidak bisa membedakan attempt retry.

**Perbaikan**:
- `CanonicalResult` kini membawa `requestId` (dikeluarkan dari hash —
  envelope-only). `validateCommandEnvelope()` tidak lagi menolak
  requestId ≠ transactionId: transactionId = identitas logis/dedup
  (wajib), requestId = identitas transport (opsional, boleh berbeda,
  fallback ke transactionId untuk klien v2).
- Ketiga call-site mengisi `qc.requestId` dari identitas transport yang
  benar (REST: `canon.requestId`; MQTT: `doc["requestId"]` fallback tid).
- `RelayTransactionRecord` + journal terminal ack + GET
  `/api/relays/transactions/{id}` + ack QUEUED semuanya membawa
  `requestId`.
- Replay DUPLICATE (REST + MQTT) me-LOG `attempt requestId=` dari retry —
  jumlah attempt per transaksi logis kini terlacak di device log, sementara
  ACK asli tetap direplay verbatim.
- PWA `apiShared.ts` meng-document kontrak v3 (requestId boleh beda);
  perilaku klien tetap requestId === transactionId (kompatibel v2+v3).

## p.425 — TLS gate hanya satu .ino (🟠 P1 build-variant) → SELESAI

**Temuan**: gate statis M3 hanya memindai `MqttTransport.cpp`; varian
push-alarm (`MonitorIoT_Firmware.ino`) dan firmware-generic tidak
tercakup.

**Perbaikan**: `scripts/test_audit_round3_2026_09.py` memindai SEMUA target
(134 file di `firmware/`, `firmware-generic/src/`, `push-alarm/firmware/`):
- Setiap `setInsecure()` LIVE (9 ditemukan) wajib di balik guard
  compile-time (`#elif defined(DEVELOPMENT_BUILD)` / `#else` dari guard
  produksi (`PRODUCTION_BUILD`/`*_ROOT_CA`) / `#ifdef TLS_SKIP_CERT_VERIFY`).
- `#define TLS_SKIP_CERT_VERIFY` push-alarm wajib tetap OFF default
  (commented).
- Regresi M3 modular tetap di-assert (paritas dengan
  `test_wave7_10_crosslayer.py`).

## p.427 — Factory reset tidak menyapu seluruh namespace (🟠 P1) → SELESAI

**Temuan**: REST menyapu 13 namespace + LittleFS.format + audit
preservation; MQTT hanya 9 (meninggalkan `plts_time`, `plts_emg`,
`plts_auth`, `plts_relays` — lockout relai & counter emergency selamat
dari "factory reset"); boot completion memegang salinan list ketiga.

**Perbaikan**: `Services/FactoryReset.{h,cpp}` = satu-satunya sumber daftar
namespace (13, dengan `"plts"` DIURUTKAN TERAKHIR — sebelumnya marker
`ftr_p` dihapus oleh sweep namespace pertama sendiri, mempersempit jendela
recoverable-reset ke mikrodetik; kini marker ditulis ulang setelah sweep
dan bertahan sampai format + restore selesai). Ketiga jalur (REST confirm,
MQTT confirm, boot completion) memanggil `executeFactoryResetWipe()` yang
sama: marker → sweep → re-marker → preserve audit → `LittleFS.format()` →
restore audit → clear marker. Test mirror memverifikasi setiap namespace
yang pernah di-`begin()` firmware tercakup dalam sweep (kecuali
`plts_audit` — vessel preserve yang dibersihkan oleh restore).

## p.432 — Klaim delivery guarantee MQTT (🟠 P1 contract) → SELESAI

**Temuan**: sisa overclaim "QoS 1 PUBACK" di TelemetrySpool, tabel topik
Config.h mencampur arah, dokumen remediasi menulis "at QoS 1" untuk
publish yang mustahil QoS 1 di PubSubClient 2.8.

**Perbaikan** (kontrak jujur, selaras REMEDIATION_REPORT_v1.9.3 p.79 yang
diklaim tapi belum tuntas):
- TelemetrySpool.h/.cpp: penghapusan record = "confirmed socket write
  (PubSubClient QoS-0 — NOT a broker PUBACK)"; delivery = at-least-once
  via spool replay + dedup sekuens GAS.
- Config.h tabel topik kini direction-accurate: `config`/`ota` = QoS 1
  SUBSCRIBE (downlink); `ack`/`status`/`log` = QoS 0 publish; `online` =
  QoS 1 retain via LWT broker-side.
- AUDIT_2026_09_REMEDIATION.md P1-6 dan GasOtaReporter.h dikoreksi.
- Jalur ACK MQTT relai kini men-document eksplisit: QoS 0 fire-and-forget,
  journal durable + REST reconciliation = sumber outcome authoritative;
  ACK yang hilang dipulihkan dengan re-issue transactionId sama (replay
  idempotent).

## p.434 — Config mutation atomicity (🟠 P1) → VERIFIED + HONEST-FAIL FIX

**Temuan auditor**: konfigurasi keselamatan (maxOnTime/minOnTime/
interlock/...) harus atomic. **Temuan laten baru saat verifikasi**: aksi
`relay.config` di MQTT masuk queue lalu stub `applyCommand("config")`
membalas "Config updated" TANPA menerapkan apa pun (misleading success).

**Perbaikan**:
- MQTT relay ingress kini whitelist aksi executable (on/off/pulse/all_off/
  acknowledge/clear) — "config" ditolak jujur sebelum queue.
- Stub `applyCommand("config")` fail-closed: `Rejected` + pesan jujur
  (relay config = provisioning NVS, bukan jalur runtime). Entri registry
  relay.config dipertahankan sebagai schema-canonical untuk ingress
  provisioning masa depan, dengan komentar statusnya.
- Atomicity jalur yang ADA diverifikasi ulang: ConfigUpdater (2-phase,
  zero RAM mutation sebelum semua field + invariant lolos) untuk
  battery/BMS/alarm; calibration `setPoint`/`captureZeroOffset`
  validate-before-mutate. MQTT endpoint & OTA policy bukan mutasi runtime.

## Item yang tetap menunggu (di luar scope source)

- **T9–T13 physical acceptance** — tidak berubah (Gate F).
- **Secure Boot + Flash Encryption + eFuse anti-rollback provisioning**
  (p.422/423) — bukti perangkat, bukan source.
- **p.429 retention journal** — keputusan produk: Opsi A (retensi 16
  entry terdokumentasi; endpoint jujur UNKNOWN setelah eviksi) vs Opsi B
  (audit permanen ke GAS dengan transactionId/commandHash/result/reason/
  eventTime/deviceId/firmwareVersion/stateSequence). Status quo = Opsi A
  terdokumentasi di TransactionJournal.h + respons GET.
- **p.430/431 telemetry contract & clock authority** — butuh verifikasi
  lintas-layer firmware–GAS–PWA dan keputusan NTP-first policy;
  direkomendasikan masuk audit cycle berikutnya.

## Verifikasi Round 3

- `scripts/test_audit_round3_2026_09.py`: **41/41 PASS** (statis p.418/
  427/432/434 + Python mirror envelope/requestId + TLS scanner 134 file,
  9 live setInsecure, 0 unguarded).
- Seluruh 28 file `scripts/test_*.py` hijau (termasuk 39/39 relay safety
  dan 30/30 transaction durability).
- Compile: `pio run -e development -e staging` → SUCCESS.
