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

---

# Round 4 — Device security provisioning + hardware acceptance (12 Sep 2026)

Audit round 4 mengonfirmasi penutupan p.398–415, p.418/425/427/432/434 di
commit 18af0ae. Temuan tersisa auditor bukan lagi arsitektur software,
melainkan **security provisioning perangkat + hardware acceptance**. Round
ini menjawab poin-poin tersebut pada level yang bisa dijawab dari source,
dan memberikan runbook + tooling untuk bagian yang menuntut tindakan fisik.

## Ringkasan status round 4

| Temuan auditor | Status | Bentuk jawaban |
|---|---|---|
| OTA anti-rollback hanya software-level (tanpa eFuse secure_version) | 🟠→🟢 app-layer **TERPASANG** (bootloader-layer = Gate B) | SecurityPosture + OtaManager floor gate |
| Secure Boot + Flash Encryption belum terbukti aktif | 🟠→🟡 fail-closed gate + evidence endpoint + runbook | Gate A runbook + `/api/security` |
| OTA rollback software matang | 🟢 disetujui auditor | — |
| INA219_SIGN_CORRECTION masih ASSUMED | 🟠 **BENAR & DIPERTAHANKAN** (honesty contract) | acceptance endpoint, bukan perubahan konstanta |
| Dynamic INA219 gain perlu acceptance fisik | 🟠 **BENAR** — tooling acceptance diberikan | `/api/diagnostics/ina219` |
| MQTT honest (QoS 0 + spool) | 🟢 disetujui auditor | — |
| Factory reset 13 namespace | 🟢 disetujui auditor | — |
| Build variant TLS | 🟢 disetujui auditor | sisa "pastikan binary production" → dijawab §bawah |

## 1. Hardware-rooted anti-rollback (p.436) → APP-LAYER CLOSED

Kritik auditor tepat: `new version must be > current` adalah software gate;
tidak ada mekanisme yang membuat device *secara cryptographic* menolak
security version lebih rendah. Round ini menutupnya pada lapis aplikasi
dengan jangkar hardware:

- **Floor**: `esp_efuse_read_secure_version()` (BLK3 SECURE_VERSION, 32 bit,
  one-way) — semantik **sama dengan bootloader ESP-IDF** (popcount/unary,
  `esp_efuse_check_secure_version`). Diverifikasi terhadap source IDF v4.4.7
  (core Arduino prebuilt) — API ini berfungsi tanpa config bootloader.
- **Ledger**: NVS namespace `plts_sec` (sengaja di luar sweep factory
  reset) menginterpretasikan tiap bit terbakar sebagai satu security epoch
  dan merekam versi (major, minor) yang diwakilinya. Boot melakukan
  **reconcile** ledger vs floor: selisih satu epoch + running image lebih
  baru → self-heal terbukti aman; selisih lain → `TAMPER`/`NO_LEDGER`
  (bukti rollback) → **OTA ditolak**.
- **Gate**: kedua jalur OTA (REST upload + MQTT download) kini juga
  menjalankan `_validateSecurityFloor()` — kandidat dengan (major, minor)
  di bawah epoch terbakar ditolak, *termasuk* saat running image lebih
  tua darinya (skenario post-rollback). Patch dalam epoch tetap bebas.
- **Burn**: satu bit eFuse dibakar **hanya saat aktivasi** (healthy window
  lolos, `esp_ota_mark_app_valid_cancel_rollback`), **hanya build
  production** (`PLTS_DISABLE_EFUSE_BURN` = escape hatch terdokumentasi).
  Gagal burn dilaporkan jujur (`BURN_FAIL`, verdict DEGRADED), tidak
  pernah dipalsukan sukses.

**Batas yang diakui secara eksplisit**: ini menutup jalur OTA; jalur fisik
(UART re-flash) ditutup oleh flash encryption + secure boot (Gate A/B),
bukan oleh gate aplikasi. Kepatuhan penuh bootloader (ABS_DONE +
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) membutuhkan migrasi build yang
didokumentasikan sebagai **Gate B** di `docs/SECURE_PROVISIONING.md` §6 —
dan **tidak boleh** diaktifkan sebelum pipeline release menstempel
`secure_version` ke app descriptor (image Arduino hari ini bernilai 0).

## 2. Secure Boot + Flash Encryption (p.437) → GATE + BUKTI + RUNBOOK

Auditor benar bahwa sebelumnya tidak ada evidence per-device. Jawaban
round ini tiga lapis:

1. **Fail-closed gate** — build PRODUCTION menolak SELURUH OTA selama flash
   encryption belum diaktifkan pada device (`otaProvisioningOk`).
   Artinya: binary production *tidak bisa lagi dipakai* pada device yang
   tidak diprovision — persis "production provisioning gate" yang diminta.
2. **Evidence endpoint** — `GET /api/security` (authenticated):
   flashEncryption (paritas FLASH_CRYPT_CNT), secureBoot V1/V2 (ABS_DONE),
   floor eFuse + kapasitas + coding scheme, ledger status, build identity,
   **self-measurement SHA-256 image berjalan** (mmap lewat cache yang
   mendekripsi transparan — cocok byte-per-byte dengan `release.json`
   firmwareSha256), plus verdict provisioning terkomputasi
   (PASS/DEGRADED/FAIL + alasan). Tidak ada nilai yang dipalsukan; yang
   tak terukur dilaporkan apa adanya.
3. **Runbook** — `docs/SECURE_PROVISIONING.md`: layer map ancaman,
   prosedur per-device Gate A (generate key → burn BLK1 → FLASH_CRYPT_CNT
   → re-flash terenkripsi → verifikasi), arsip baseline/post eFuse,
   checklist acceptance per device, batas jujur (NVS tidak terenkripsi;
   bootloader belum secure-boot), dan Gate B (migrasi secure boot).

## 3. Bukti binary production = artifact teraudit (kesimpulan auditor #4)

- **Sisi device**: `/api/security` → `runningImage.sha256` (self-measured).
- **Sisi operator**: `scripts/verify_flashed_image.py`:
  - mode `--device-url`: bandingkan digest device dengan `release.json`
    (berlaku untuk device terenkripsi sekalipun — device mengukur dirinya
    melalui cache);
  - mode `--port` (esptool read-back): hash kedua slot app pada flash
    plaintext dan cocokkan dengan manifest (untuk bench pra-provisioning);
  - `--self-test` masuk CI.
- **Sisi build**: tidak berubah dari v1.9.3 — reproducible-build 2x
  (REL-04) + provenance + Ed25519 + tag-signed release chain sudah hijau;
  build profile kini juga diverifiable at-runtime via `/api/security`
  (`buildProfile` + digest self-measured).

## 4. INA219 (item 4 & 5 auditor) — kejujuran dipertahankan, tooling diberikan

Auditor menyebut `INA219_SIGN_CORRECTION = -1.0f` berstatus
**ASSUMED — NOT HARDWARE VERIFIED** sebagai P1 acceptance. Kami **setuju
dan sengaja tidak mengubahnya** — flag itu hanya boleh turun setelah
bukti INA-004 berada di `docs/hardware-acceptance/`. Yang diberikan round
ini adalah jalur eksekusinya:

- `GET /api/diagnostics/ina219` (authenticated): readback register config
  (bukti bit PGA), raw shunt/bus segar + decode, rantai konstanta lengkap
  (shunt Ω, sign correction + status ASSUMED, LSB, EMA), kebijakan PGA
  (threshold up/down, max), nilai pipeline, dan **cross-check BMS live**
  (konvensi tanda sama: +charging) termasuk kesepakatan tanda dan status
  alarm mismatch.
- Endpoint ini dirancang untuk prosedur INA-001..INA-004 (low-current
  accuracy, transisi PGA + hysteresis, saturasi + transient, tanda) tanpa
  debugger — mengurangi biaya eksekusi acceptance fisik tanpa mengklaim
  hasil yang belum diukur.

## 5. Factory reset & ledger (amplifikasi p.427)

Namespace ledger keamanan `plts_sec` sengaja **tidak** masuk sweep 13
namespace: state epoch adalah state perangkat, bukan konfigurasi user.
Menghapusnya bersama factory reset akan membuat setiap device pasca-reset
terlihat sebagai rollback (ledger absen, floor > 0) dan menolak OTA
selamanya. Kontrak ini di-enforce statis oleh test round-4 (C1–C3).

## 6. Verifikasi Round 4

- `scripts/test_audit_round4_2026_09.py`: **83/83 PASS** (gate wiring,
  burn hanya di production, endpoint, ledger survival, honesty INA219,
  tooling + runbook, mirror keputusan ledger/floor/paritas/verdict).
- `scripts/verify_flashed_image.py --self-test`: PASS (parser header +
  partition table + perbandingan digest).
- Regresi: seluruh 29 skrip `scripts/test_*.py` hijau (termasuk round 1–3).
- Compile: `pio run -e development -e staging` SUCCESS.

## 7. Yang tetap menunggu tindakan fisik (tidak berubah, kini ber-runbook)

1. **Gate A** — provisioning flash encryption per device produksi
   (runbook §3; bukti via `/api/security` + `verify_flashed_image.py`).
2. **Gate B** — migrasi secure boot + bootloader anti-rollback (runbook §6).
3. **T9–T13** — physical acceptance relay/journal (Gate F rilis).
4. **INA-001..INA-004** — pengukuran fisik rantai INA219 (endianness
   konstanta tanda baru boleh berubah SETELAH bukti; endpoint eksekusi
   kini tersedia).
5. Keputusan produk p.429 (retensi journal) dan p.430/431 (kontrak
   telemetry lintas-layer) — tetap di cycle berikutnya.

---

# Round 5 — Telemetry durability, persistence failure propagation, BMS/shunt authority, alarm safety policy (13 Sep 2026)

Baseline auditor: `6dd991e` (merge PR #33). Auditor membedah area baru:
telemetry/spool, energy persistence, storage/auth, lalu SocStateMachine +
BMS + alarm registry — total 16 temuan p.437–p.452. Semua diverifikasi di
source sebelum diperbaiki; prioritas diikuti sesuai urutan auditor
(p.451 → p.445 → p.448/449 → p.439/452 → sisanya).

## Ringkasan status round 5

| Temuan | Severity | Status |
|---|---|---|
| p.437 spool reguler hilang saat reboot | 🔴 P1* | ✅ CLOSED — regular ring kini persist ke LittleFS |
| p.438 spool()==true menyembunyikan eviksi | 🟠 P2 | ✅ CLOSED — SpoolResult eksplisit |
| p.439 energy NVS failure tak terpropagasi | 🟠/🔴 P1 | ✅ CLOSED — bool + read-back verify + alarm |
| p.440 legacy fallback mixed-generation | 🟠 P2 | ✅ CLOSED — migration-only + auto-normalisasi |
| p.441 kontrak waktu lintas-layer | 🟠 P1 | 🔶 firmware-side evidence lengkap; GAS/PWA = audit berikutnya (sesuai rencana auditor) |
| p.442 gap sequence vs spool vs reboot | 🟡 | ✅ firmware-side CLOSED — spoolDrops di envelope |
| p.443 credential plaintext (app-level) | 🟠 P2 | ✅ CLOSED sbg posture alarm + dokumentasi urutan provisioning |
| p.444 UART one-time reveal boundary | 🟠 P2 | ✅ CLOSED — auditable event + alarm DEFAULT_CREDENTIALS_ACTIVE |
| p.445 SOC lastSyncTs campur domain waktu | 🔴 P1 | ✅ CLOSED — epoch domain tunggal + sanitasi legacy |
| p.446 OCV kurva kasar | 🟠 P1 | ✅ gates + confidence LOW (kurva tetap honest-rough, menunggu data karakterisasi fisik) |
| p.447 full-charge 100% hanya dari V+I | 🟠 P1 | ✅ CLOSED — BMS authority + evidence gates |
| p.448 BMS authoritative abaikan faultFlags | 🟠 P1 | ✅ CLOSED — faultFlags==0 masuk gate |
| p.449 cross-check hanya detector | 🟠 P1 | ✅ CLOSED — interlock (Suspect + authority suspension) |
| p.450 update alarm existing tak durable | 🟠 P2 | ✅ CLOSED — persist saat eskalasi severity/pesan |
| p.451 registry buang alarm aktif | 🔴 P1 | ✅ CLOSED — eviksi CLEARED-only, reject jujur |
| p.452 SOC NVS failure tak dilaporkan | 🟠 P1 | ✅ CLOSED — bool + counter + alarm |

## 1. Alarm registry tidak pernah mengorbankan state safety aktif (p.451, p.450)

Kebijakan eviksi baru di `AlarmRegistry::raise()`:

- Kandidat eviksi **hanya** entri CLEARED (tertua `clearedAt`, tie-break
  `raisedAt` — memperbaiki pemilihan "oldest" yang sebelumnya tidak
  terjamin).
- Registry penuh entri ACTIVE/ACKNOWLEDGED → alarm baru **REJECTED**
  jujur: `raise()` mengembalikan `false`, `overflowCount++`, log
  rate-limited (60 s / kode berbeda). Rejection recoverable — evaluator
  re-raise tiap tick; alarm masuk begitu slot dibebaskan. Eviksi alarm
  aktif adalah kehilangan permanen dan silent — itu yang dihilangkan.
- Saturasi dapat diamati: `overflowCount` di `/api/alarms` dan
  `/api/diagnostics` (`alarmRegistryOverflow`).
- Update existing yang MEANINGFUL (eskalasi severity / pesan berubah)
  kini persist langsung — simetri dengan clear()/acknowledge() [p.450].
  Pure refresh tetap menunggu checkpoint periodik (wear bound).

## 2. Domain waktu SOC tunggal (p.445)

Jalur full-charge semula menulis `_lastSyncTs = monotonicMs / 1000`
(uptime-seconds) ke field yang dipersistenkan sebagai `lastSyncUnix` dan
dibaca PWA sebagai epoch — setelah reboot tampil sebagai tanggal 1970.
Fix: semua jalur sync (OCV, full-charge, manual, BMS) kini seragam
`rtc.getUnixTime()` (0 = unsynced-honest, konvensi driver RTC).
Load dari firmware lama membersihkan nilai implausibel (< 2020-01-01)
menjadi 0 dengan log — nilai SOC sendiri tetap valid (CRC + basis lolos).

## 3. Otoritas BMS vs INA219 ditegakkan (p.448, p.449)

`BatteryCommManager::socAuthoritative()` kini menuntut
`faultFlags == 0` DAN `!_mismatchActive` di samping locked + plausible +
fresh. BMS yang melaporkan fault tidak lagi dikonsumsi sebagai kebenaran
SOC; alarm BMS_FAULT tetap dinaikkan terpisah.

Mismatch kini INTERLOCK, bukan sekadar detektor:

- energyTask mengevaluasi cross-check **sebelum** tick integrasi
  (keputusan arbitrase cycle ini menggate cycle ini).
- Saat mismatch aktif: `snap.batteryCurrent.quality` didemote ke
  **Suspect** → quality gate SocStateMachine (SOC FREEZE) dan
  EnergyCounters (integrasi diblokir) sudah menolaknya — arus salah-tanda
  tidak bisa lagi masuk senyap ke jalur SOC/energi.
- Re-baseline `setSoc(bms.soc, "BMS_SYNC")` unreachable saat mismatch
  (gate authority menolak BMS yang berselisih dengan shunt).
- Konfirmasi full-charge juga defer saat mismatch [p.447].
- Kedua instrumen non-authoritative sampai sepakat lagi; alarm
  BMS_CURRENT_MISMATCH memberi tahu operator alasannya.

## 4. Persistence fail-closed (p.439, p.452)

`EnergyCounters::saveToNVS()`, `SocStateMachine::saveToNVS()`, dan
`AlarmRegistry::saveToNVS()` kini mengembalikan `bool` dan memverifikasi:

1. `Preferences::begin()` sukses,
2. `putBytes` menulis penuh (size check),
3. energy: **read-back memcmp** — menangkap korupsi bit yang lolos
   size-only check.

persistenceTask mengonversi kegagalan menjadi alarm STORAGE_ERROR
(Critical) sekali per transisi gagal→pulih. Counter `persistFailures()`
diekspos di `/api/diagnostics`. Device dengan NVS sekarat tidak lagi
menunggu reboot untuk ketahuan.

## 5. Spool reguler tahan reboot + semantik eksplisit (p.437, p.438)

- Regular ring (8 × ~2.6 KB) kini di-flush ke snapshot LittleFS
  `/spool_reg.bin` — atomic `.tmp → rename`, header magic/version +
  CRC32 atas seluruh array, per-record CRC saat restore (record korup
  di-drop jujur, dihitung).
- Kebijakan wear-bounded: flush saat record pertama masuk episode
  buffering, lalu maksimal 1× per 30 s selama ada pending; file dihapus
  saat ring kosong (tidak ada resurreksi record yang sudah terkirim).
  Jalur sehat tidak menyentuh file sama sekali. Reboot di tengah outage
  kehilangan maksimal 1 interval flush (30 s) — kontrak yang sebelumnya
  "hilang seluruhnya" (RAM-only).
- `spool()` mengembalikan `SpoolResult`:
  `Stored | EvictedOldest | Rejected`. Caller (publishTelemetry)
  mem-log transisi eviksi; `dropCount` masuk envelope sebagai
  `health.spoolDrops` [p.442] — bersama sequence + reboot margin, backend
  kini bisa membedakan gap outage vs overflow vs reboot.

## 6. Evidence gates untuk sinkronisasi V-based (p.446, p.447)

`SocSyncEvidence` (plain data, engine bebas dependensi Comm):

- **Full-charge** (event basis SOC "100%"): BMS sehat = otoritas —
  konfirmasi memerlukan persetujuan (SOC ≥ `BMS_FULL_AGREE_PCT` 95%).
  BMS sehat bilang "belum penuh" → DEFER (tetap FullCandidate, tidak
  pernah paksa 100%). Cells buruk (overvoltage > 3.65 V / imbalance >
  250 mV) → defer. Mismatch aktif → defer. Suhu di luar [0, 45] °C →
  defer. Bukti tak diketahui (tanpa BMS/sensor) → jalur V+I legacy
  berlaku (fallback terdokumentasi).
- **OCV-at-rest**: resolusi SOC dari OCV ditolak saat cells buruk atau
  suhu di luar jendela — rejeksi, bukan kurva kompensasi rekaan (tidak
  ada dataset karakterisasi; kejujuran dipertahankan). Provenance
  OCV ditandai `method=OCV_AT_REST, confidence=LOW`.
- Kurva kasar TIDAK diubah/dipalsukan — tetap menunggu data
  karakterisasi fisik (sejalan kejujuran INA219 round 4).

## 7. Legacy energy = migration-only (p.440)

Path legacy 6 key kini: deteksi kehadiran key (`isKey`, bukan
default-vs-nol), lalu **normalisasi langsung** ke blob atomik — path
dieksekusi tepat sekali per device. `loadedFromLegacy()` diekspos di
diagnostics untuk fleet visibility.

## 8. Boundary credential operasional (p.443, p.444)

- **p.443**: PRODUCTION build di flash tak terenkripsi → alarm Critical
  `SECURITY_PROVISIONING` saat boot (di samping OTA yang sudah menolak).
  Urutan provisioning yang salah sekarang teriak di telemetry, bukan
  hanya di `/api/security`.
- **p.444**: reveal UART satu kali tetap (kontrak commissioning F-G18)
  namun kini: (1) event teraudit dengan marker SECURITY; (2) alarm
  Warning `DEFAULT_CREDENTIALS_ACTIVE` aktif sampai password diganti —
  flag `credentialsProvisioned` dipersist di config.json (`credProv`),
  ditutup di endpoint ganti password, diekspos di `/api/security`
  (`credentialBoundary`). Config lama tanpa flag dianggap
  sudah-provisioned (tidak ada nagging retroaktif).

## 9. Verifikasi Round 5

- `scripts/test_audit_round5_2026_09.py`: **71/71 PASS** (wiring statis
  8 grup + mirror Python: kebijakan eviksi alarm 4 kasus, tabel keputusan
  full-charge defer 8 kasus, interlock mismatch 3 kasus, sanitasi
  timestamp 3 kasus, semantik SpoolResult 3 kasus).
- Regresi: seluruh 30 skrip `scripts/test_*.py` hijau.
- Compile: `pio run -e development -e staging -e production` SUCCESS
  (RAM 36.8%, flash dev 92.6%).

## 10. Yang tetap menunggu (tidak berubah dari round 4)

1. **Gate A/B provisioning** + T9–T13 + INA-001..004 (fisik).
2. **p.441/p.442 lintas-layer**: firmware-side evidence kini lengkap
   (timeQuality, uptimeSeconds, spoolDrops, sequence, monotonicMs
   tersedia di envelope) — bedah GAS/Sheets/PWA menjadi audit
   berikutnya persis seperti rencana auditor (p.430/431).
3. Keputusan produk p.429 (retensi journal) — kini disertai bukti bahwa
   alarm/energy/SOC persistence sudah fail-closed di sisi device.

## 11. Addendum post-merge (2026-09-14): verifikasi ulang p.451 + p.445

Atas permintaan operator, kedua prioritas #1 dan #2 auditor diverifikasi ulang
setelah merge PR #34. Hasil: kebijakan p.451 (eviksi CLEARED-only, reject
jujur) dan p.445 (epoch tunggal di firmware) terkonfirmasi benar — namun
verifikasi ulang menemukan **bug laten P0 yang telah ada sejak commit
pertama** pada jalur yang TIDAK diaudit p.451 (auditor menganalisis jalur
registry-penuh; jalur normal justru yang rusak):

### 11.1 Bug P0: append out-of-bounds di `AlarmRegistry::raise()` (jalur normal)

- **Akar masalah**: `idx` dari `_findIdx()` adalah `0xFF` (255) untuk alarm
  baru, dan hanya cabang eviksi (`_count >= MAX_ALARMS`) yang meng-assign
  slot. Di jalur normal (registry TIDAK penuh — jalur yang paling sering
  dilalui), `idx` tetap 255 → `_alarms[255]` menulis ±33 KB melewati array
  24 entri (korupsi globals yang tidak terduga), `_count` tidak pernah
  bertambah, `find()`/`countActive()` tidak pernah melihat alarm tersebut,
  dan `raise()` tetap return `true` — kehilangan senyap yang dilaporkan
  sukses.
- **Mengapa lolos**: mirror Python round-5 mengimplementasikan *intent*
  (`registry.append`) bukan struktur slot nyata; seluruh test adalah
  statis/mirror; firmware round-5 belum pernah dijalankan di perangkat
  (T9–T13 masih menunggu fisik).
- **Bukti**: harness C++ native + AddressSanitizer mereplikasi logika
  persis — versi lama: `stored at idx=255 (count=0)`, alarm tidak
  ditemukan kembali; versi fixed: `idx=0 (count=1)` ditemukan. Siklus
  hidup penuh (24 raise → reject ke-25 → clear 3 → evict cleared terlama →
  tidak ada alarm aktif yang hilang) lulus bersih ASAN.
- **Fix**: `if (idx == 0xFF) idx = _count;` eksplisit sebelum akses array
  + komentar penuh; pesan reject dikoreksi ("Clear alarms to free slots
  (acknowledge keeps the slot occupied...)" — acknowledge TIDAK membebaskan
  slot, hanya CLEARED yang bisa dievict).
- **Regresi guard**: `test_audit_round5_2026_09.py` G8 (assignment append
  wajib ada sebelum akses array) + G9 (pesan guidance akurat). Suite: 74/74.
- **Konsekuensi retrospektif**: seluruh alarm runtime yang pernah diamati
  di bench/mock berasal dari mock JS — alarm firmware asli tidak pernah
  tersimpan sebelumnya; perbaikan ini adalah prasyarat T9–T13.

### 11.2 p.445 celah lintas-layer (PWA): domain detik vs milidetik

- Firmware (post-fix) mengirim `soc.lastSync` sebagai **epoch DETIK**
  (`uint32_t`; ms tidak muat 32-bit) — sedangkan `types.ts` PWA
  mendokumentasikan "ms epoch", `formatLastSync()` mengurangkan dari
  `Date.now()` (ms), dan mock menghasilkan domain ms. Perangkat nyata akan
  menampilkan label seperti "20274d ago"; mock menutupi bug.
- **Fix (PWA PR)**: `normalizeLastSyncMs()` di `src/lib/soc.ts` —
  normalisasi deterministik di batas tampilan (nilai < 1e11 = detik →
  ×1000; lebih besar = ms; 0/negatif/null = unknown). Kontrak tipe
  didokumentasikan ulang; 6 test baru (detik/ms/unknown/mock/firmware).
- Kontrak domain tunggal lintas-producer (device REST/MQTT/GAS) tetap
  milik audit round 6 (p.441) — normalisasi ini menjaga tampilan benar
  sampai kontrak itu dipatenkan.

### 11.3 Verifikasi addendum

- `test_audit_round5_2026_09.py` 74/74; seluruh 30 skrip regresi hijau.
- `pio run -e development -e staging` SUCCESS.
- PWA: vitest 201/201, lint 0 error, typecheck bersih.

## 12. Round 6 (2026-09-14): eligibility keamanan, quality gate anomali, atomicity persistensi alarm

Audit round-6 (p.453–p.466) membedah AnomalyDetector → sensor-quality
pipeline → EmergencySupervisor plus persistensi alarm. Empat temuan
p.445–p.452-family dari round 5 diverifikasi CLOSED oleh auditor; round ini
menutup seluruh temuan terbuka:

### 12.1 p.457 + p.466 (P1): predikat kelayakan kelas-keselamatan

- **Akar masalah**: satu API `Measurement::isValid()` melayani dua kebutuhan
  yang berbeda — dashboard (Valid/Derived/Estimated = usable) dan interlock
  keselamatan (harusnya hanya pengukuran langsung yang segar). Emergency
  `_readSensors()` memakai `isValid()`, sehingga nilai Derived/Estimated
  bisa menjadi input trip/ARM.
- **Fix (API-level, arsitektural)**: `Measurement::isSafetyUsable()` —
  `quality == Valid && source == Measured && isfinite(value)` — dengan
  kontrak terdokumentasi eksplisit: `isValid()` = "usable untuk dashboard",
  `isSafetyUsable()` = "boleh menjadi input interlock keselamatan".
  `_readSensors()` memakai predikat strict untuk vbat/idc/iac; degradasi
  kini menjadi NaN → SENSOR_LOSS fail-closed (sensorFailPolicy=1) dan ARM
  ditolak sampai pengukuran nyata kembali. Kontrak dashboard TIDAK berubah.
- **Laten bug tambahan (ditemukan saat remediasi)**: raise() pada entri
  CLEARED tidak pernah mengembalikan lifecycle ke Active — alarm yang
  pernah di-clear TIDAK PERNAH bisa aktif lagi sepanjang boot (kondisi
  berulang, mis. overcurrent ke-2, tak terlihat di alarm center). Fixed
  bersamaan karena menjadi prasyarat semantik p.460; `raisedAt` baru,
  `clearedAt` reset, severity dimulai dari nilai raise baru.

### 12.2 p.458 (P1/P2): quality gate per-kuantitas di AnomalyDetector

- `AnomalyContext` kini membawa `temperatureQ`/`humidityQ`/`socQ`; call
  site mengisinya dari kualitas pipeline + `getSocQuality()`.
- Evaluasi T/H digerbangkan `envEligible()` (hanya Valid) — sensor yang
  mati/stale tidak lagi dievaluasi sebagai pembacaan hidup.
- SOC digerbangkan "known" (Estimated tetap eligible — jalur normal shunt
  coulomb TIDAK dibungkam; NotAvailable/SensorError/Invalid/OutOfRange
  menangguhkan evaluasi).

### 12.3 p.459 (P1): baseline hanya dari sampel eligible

- Baseline (`_last*`) hanya ditulis dari sampel yang eligible, masing-masing
  dengan stempel waktu sendiri (`_last*Sec`) — rate detector membagi dengan
  selisih waktu ANTARA DUA SAMPEL ELIGIBLE, bukan selisih tick terakhir.
  Sekuens valid → SensorError → valid tidak lagi menghasilkan alarm
  voltage-jump/current-spike palsu (bug lama: |Δ| dihitung terhadap sampel
  rusak).
- Stuck-window arus di-reset pada quality break — sampel dari era sensor
  berbeda tidak pernah bercampur dalam satu verdict "stuck".

### 12.4 p.460 (P2): semantik jujur saat sensor tidak dapat dinilai

- Ketika kualitas tidak eligible: alarm numerik kuantity tersebut
  di-clear (clear-if-active — persist NVS hanya sekali pada transisi),
  alarm sensor-error milik producer tetap menjadi kebenaran yang hidup,
  dan emergency layer tetap fail-closed independen (SENSOR_LOSS).
  Operator melihat "OVERCURRENT tidak lagi diklaim" — bukan verdict zombie
  dari data yang tidak ada. Log transisi tercatat di event log.

### 12.5 p.461 (P2): TELEMETRY_STALE — gap + stall + clear

- Gap sequence: tetap raise.
- Aliran sehat (konsekutif +1): alarm di-clear jujur (sebelumnya zombie
  permanen).
- Stream yang berhenti total (seq beku): dideteksi di jalur queue-timeout
  measurementTask (`MEAS_STALL_ALARM_MS` 15 s, selaras jendela §6.1) —
  detektor internal tidak mungkin melihatnya karena tick() hanya berjalan
  saat sampel TIBA. Pesan alarm membedakan STALL vs GAP.

### 12.6 p.453 + p.455 (P2): persistensi alarm = SATU record NVS atomik

- Layout v2 kunci "state": `magic + version + generation + count + crc32 +
  alarms[]` — satu `putBytes` = satu transaksi NVS ter-journal (boot melihat
  generasi lama utuh ATAU generasi baru utuh — tidak pernah campuran).
- **p.453**: cek panjang STRICT (`w == blobLen`, bukan `w2 != 0`) +
  read-back verification (`getBytes` + `memcmp`) — short write / media robek
  dilaporkan gagal, tidak pernah dianggap tersimpan.
- **p.455**: `generation` (u16, wrap) menjadi penanda transaksi; dikomit
  HANYA setelah write terverifikasi, di-adopsi kembali saat load; diagnostik
  boot mencantumkan generasi.
- Migrasi legacy dua-record: dibaca SEKALI (validasi CRC sama seperti v1),
  langsung ditulis ulang sebagai record tunggal; pasangan legacy dibiarkan
  (stale, self-consistent) agar firmware rollback masih menemukan data.
  Skenario mixed-generation legacy ditolak jujur (empty) oleh CRC.

### 12.7 p.454 (P2): kontrak raise yang jujur

- `RaiseResult` (Rejected / AcceptedRam / AcceptedPersisted /
  AcceptedPersistFailed) via `raiseTracked()`; `bool raise()` tetap sebagai
  wrapper TIDAK MENGUBAH caller lama dengan kontrak terdokumentasi: TRUE =
  "diterima di RAM — BUKAN klaim durability".
- Kegagalan persist immediate kini tercatat sebagai event StorageError
  (terlihat di log), dihitung di `persistFailures()`, dan di-retry oleh
  checkpoint periodik; persistenceTask tetap menaikkan STORAGE_ERROR.

### 12.8 p.465 (P2): watchdog starvation mutex emergency

- `tick()` menghitung starve beruntun; ≥ `EMG_LOCK_STARVE_LIMIT` (20 tick ≈
  2 s @ 10 Hz) → **force physical fail-safe TANPA mutex** (driver relay =
  tulisan GPIO murni), alarm EMERGENCY_TRIP Critical, log. State mutex-owned
  sengaja tidak disentuh dari luar lock.
- Siklus berikutnya yang berhasil mengunci me-reconcile menjadi trip
  ter-latch: event type `TRIP` (whitelist GAS — type TIDAK ditambah;
  reason `INTERNAL` string passthrough, wire-additive), reason vocabulary
  dipatenkan sebagai pengecualian terdokumentasi di X7.
- Kontensi transien (< limit) reset bersih — tidak ada alarm palsu.
  Fault-injection terbukti di harness native (D/E).

### 12.9 p.456: checklist acceptance E2E live (operator)

Deployment READY membuktikan DNS→Vercel→HTML, bukan rantai fungsional.
Acceptance penuh menunggu perangkat fisik; checklist berikut adalah gerbang
T14 yang harus dieksekusi dengan device live (bukti per langkah masuk
`docs/hardware-acceptance/`):

1. **Browser → auth**: login PWA produksi, sesi JWT tersimpan, logout bersih.
2. **PWA → GAS**: LATEST telemetry muncul < 10 s; envelope HMAC diverifikasi
   GAS (cek row device di Sheets: kolom signature/nonce terisi).
3. **GAS → device (jalur perintah)**: ARM/DISARM dari PWA → ACK APPLIED di
   UI; status relay terkonfirmasi via telemetry `emergency.relayEnergized`.
4. **Device → telemetry**: sequence monotonic, `soc.lastSync` tampil umur
   masuk akal (p.445), kualitas sensor (VALID/STALE/...) sesuai kondisi.
5. **Transaksi relay**: ON/OFF via PWA → `result: EXECUTED` + lifecycle
   event utuh (pending→executed) + reconciliation status match.
6. **Alarm end-to-end**: picu kondisi (mis. lepas SHT31) → alarm muncul di
   alarm center PWA < 15 s + push notification; clear → status konsisten;
   reboot device → alarm bertahan (blob v2; catat `generation` naik).
7. **Persistence**: setelah OTA/rollback, alarm history selamat (migrasi v2)
   dan `overflowCount` == 0 di /api/diagnostics.

### 12.10 Verifikasi round 6

- Native ASAN+UBSAN (struktur 1:1 dengan source): `verify_alarm_blob_atomicity`
  (35 check), `verify_emg_safety_gate` (30 check), `verify_anomaly_quality_gates`
  (19 check) — semua lulus, termasuk skenario short-write, CRC-corrupt,
  mixed-generation legacy, reaktivasi, starvation 20-tick, dan phantom-jump.
- Gate statis baru `scripts/test_audit_round6_2026_09.py`: 30/30.
- Regresi: round5 74/74, round4 83/83, round3 42, remediation 65, relay
  safety 39, transaction durability 30, emg modular 85, emg firmware 50,
  emg GAS 57, wave12 26/26, phase13d1 9/9, soc 6/6, w7-10 24/24, parity
  & ack-contract PASS.
- `pio run -e development -e staging` SUCCESS.

### 12.11 Addendum self-review (2026-09-14, pra audit ulang)

Verifikasi ulang menyeluruh atas PR #36 menemukan **tiga cacat — dua di
remediasi round-6 sendiri, satu bug laten lama** — semuanya diperbaiki
sebelum diserahkan ke auditor:

1. **Baseline V/I bisa diracuni NaN** (cacat remediasi p.459 saya sendiri):
   update baseline berada di dalam guard `envEligible` tetapi DI LUAR
   `isfinite` — producer yang melanggar kontrak (quality=Valid, value=NaN)
   membuat `_lastVoltage`/`_lastCurrent` menjadi NaN. Arah kegagalannya
   konservatif (`NaN > threshold` = false → tidak ada alarm palsu), tetapi
   rate detector buta selama interval itu dan invariant p.459 rusak.
   Fix: update baseline dipindah ke dalam guard `isfinite`; baseline finite
   sebelumnya dipertahankan. Test regresi: lompatan nyata SETELAH sampel
   Valid+NaN harus tetap terdeteksi.
2. **Priming suhu tidak menstempel `_lastTempSec`** (cacat minor): deteksi
   temp-rise skip tepat satu interval pertama setelah boot. Fix + test.
3. **BUG LATEN (pre-existing): alarm rapid-rise tidak pernah terlihat** —
   detektor laju suhu me-raise `TEMPERATURE_HIGH`, lalu blok threshold di
   tick yang sama men-clear kode itu setiap kali T < warn−hyst. Akibatnya
   early-warning thermal justru invisible persis di rentang di bawah
   threshold statis (rentang di mana deteksi dini penting), plus churn NVS
   2×/tick saat kondisi berlanjut. Fix: kode terpisah
   `TEMPERATURE_RAPID_RISE` (pola yang sama dengan split
   `BATTERY_SOC_DISCONTINUITY`, PARITY-4 — "dua kondisi, dua kode").
   Wire-additive: PWA me-render `alarm.code` sebagai string mentah.

Hardening observabilitas: `/api/diagnostics` kini mengekspos
`alarmStateGeneration` (klaim "diagnostics" p.455 menjadi konkret), dan
harness native ASAN/UBSAN masuk ke CI (job `test`, langkah baru) — pelajaran
round-5: mirror intent saja melewatkan P0 OOB; struktur 1:1 di bawah
sanitizer menangkapnya.

Gate: `test_audit_round6_2026_09.py` kini 34/34 (4 pin self-review baru:
I1–I4). Harness anomaly diperluas (5 ekspektasi baru). Seluruh regresi
tetap hijau; `pio run -e development -e staging` SUCCESS.

## 13. Round 7 (2026-09-14): serialisasi AlarmRegistry lintas task (p.467, p.468, p.469)

Auditor round-7 menutup p.453–p.466 (verifikasi independen atas PR #36/#37)
namun menemukan satu kelas masalah baru yang tidak tersentuh remediasi
sebelumnya: **AlarmRegistry adalah singleton global yang dimutasi dari
banyak execution context tanpa serialisasi internal apa pun**. Temuan:

- **p.467 (P1)** — data race lintas task pada state registry. `raiseTracked()`
  menjalankan `_findIdx() → modifikasi _alarms[] → _count → saveToNVS()`
  tanpa lock, sementara caller tersebar di measurementTask, anomaly/health,
  energyTask, networkTask, task web async, relayTask, emergencyTask, dan
  persistenceTask. Dua raise bersamaan yang sama-sama melihat
  `_findIdx()==0xFF` dapat menulis slot append yang sama (alarm hilang /
  duplikat); clear yang interleaved dengan eviksi menggeser array di bawah
  indeks writer.
- **p.468 (P1)** — race persistensi. `saveToNVS()` dipicu dari semua jalur
  immediate-save raise/clear/ack DAN checkpoint persistenceTask; dua save
  paralel dapat membangun blob dari snapshot RAM berbeda dan saling
  menimpa dengan generation yang tidak konsisten. Atomicity record
  (round-6) menyelesaikan tulisan tunggal, bukan serialisasi produser.
- **p.469 (P2)** — pembacaan tidak koheren. `countAll()+getAlarm(i)` di
  GET /api/alarms dan pasangan `highestActiveSeverity()` +
  `copyActiveAlarms()` di envelope telemetry adalah multi-call reads yang
  bisa menyandingkan dua state berbeda; `find()`/`getAlarm()` mengembalikan
  pointer interior yang bisa divalidasi oleh eviksi bersamaan.

### 13.1 Arsitektur fix — serialisasi DI DALAM registry

Alternatif mutex per-caller ditolak (auditor: call site terlalu tersebar
untuk dijaga benar). Yang diimplementasikan persis sesuai rekomendasi:

```
public API  →  lock  →  private *Unlocked() implementation
  raise()           → _raiseTrackedUnlocked()   → _saveToNVSUnlocked()
  clear()           → _clearUnlocked()          → _saveToNVSUnlocked()
  clearIfActive()   → _clearIfActiveUnlocked()  → _saveToNVSUnlocked()
  acknowledge()     → _acknowledgeUnlocked()    → _saveToNVSUnlocked()
  acknowledgeAll()  → _acknowledgeAllUnlocked() → _saveToNVSUnlocked()
  snapshotInto()    → (salinan utuh state dalam satu lock)
```

- **Satu mutex FreeRTOS non-rekursif**, dibuat di `begin()` (setup(),
  sebelum task apa pun lahir — terverifikasi `alarms.begin()` line 343,
  `xTaskCreatePinnedToCore` pertama line 508); fallback lazy-create di
  `_lock()` mengikuti pola `AuthManager`/`LogService`/`BatteryCommManager`
  yang sudah diaudit. Lock diambil HANYA di entry publik — tidak ada
  implementasi Unlocked yang bisa mengambil lock (deadlock mustahil secara
  konstruksi; mutex non-rekursif membuat nested acquisition tidak
  berekspresi).
- **Transaksi persistensi = bagian dari critical section (p.468)**: mutasi
  RAM + build blob + write + read-back + commit generation berjalan dalam
  satu sesi lock. Interleaving "Save A build gen N+1 / Save B build gen
  N+1 / A write / B write / A read-back / B read-back" yang dicontohkan
  auditor tidak lagi mungkin.
- **Priority inheritance** bawaan mutex FreeRTOS menaikkan prioritas holder
  saat emergencyTask (prio 3) menunggu di belakang saveToNVS di task web —
  tidak ada starvation tanpa batas.
- Kontrak ISR: registry hanya boleh disentuh dari task context (mutex
  FreeRTOS); audit call-site mengonfirmasi seluruh pemanggil saat ini
  adalah loop task / handler web / callback MQTT (bukan ISR).

### 13.2 Fix sisi pembaca (p.469)

- **`Snapshot` + `snapshotInto()`** — satu akuisisi lock menyalin seluruh
  state (24 alarm + seluruh counter diagnostik). GET `/api/alarms` kini
  membangun JSON dari SATU snapshot ter-heap (3,3 KB — terlalu besar untuk
  stack task async-web); `overflowCount` diambil dari snapshot yang sama.
- **Envelope telemetry dari satu snapshot** — `highestSeverityIn()` +
  `copyActiveFrom()` (helper murni atas snapshot) menggantikan pasangan
  `highestActiveSeverity()` + `copyActiveAlarms()` dua-lock; baris severity
  dan daftar alarm kini menarasikan instan yang sama. Buffer statis
  lama digantikan Snapshot statis (delta RAM ≈ +14 byte).
- **`find()` copy-out** — `bool find(code, Alarm& out)` menggantikan
  `const Alarm*`; `getAlarm()` / `getActiveAlarms()` /
  `getActiveAlarmCount()` (aksesor pointer mentah) DIHAPUS total.
- **`clearIfActive()` atomik** — test-and-clear satu lock untuk loop
  evaluator; window TOCTOU find→check→clear (re-raise yang ketinggalan
  dibersihkan oleh keputusan basi "kondisi sudah reda") ditutup;
  `AnomalyDetector::_clearIfActive` kini mendelegasikannya.

### 13.3 Bukti — concurrency test NYATA (bukan mirror statis)

Auditor menolak mirror struktural sebagai bukti thread-safety. Harness baru
`scripts/native/verify_alarm_concurrency.cpp` menjalankan interleaving
sungguhan dengan `std::thread`: **2 writer** (30 kode sendiri + 4 shared,
melebihi 24 slot → saturasi + eviksi CLEARED + reject jujur; ~1.600
rejection per run), **1 operator** (acknowledgeAll + injeksi 1 kegagalan
NVS transien), **1 persistence task** (loop checkpoint `isDirty →
saveToNVS()` verbatim dari firmware), **2 reader** (validasi ~5.500
snapshot), **watchdog deadlock**. Mirror struktural 1:1 registry round-7;
shim (Log/NVS/RTC) disinkronkan internal seperti padanan firmware-nya
agar laporan TSAN selalu menunjuk registry, bukan artefak harness.

Hasil (reproduksibel via `bash scripts/native/run-native-tests.sh`):

| Build | Hasil |
|---|---|
| Treatment + **ThreadSanitizer** | **0 laporan race**, ~558.000 check invariant PASS |
| Treatment + ASAN/UBSAN | 0 error, ~580.000 check PASS |
| **Negative control** (round-6 unlocked, `-DREGISTRY_NO_LOCK`) + TSAN | **177–290 laporan race**, semuanya di `Services::alarms` — bukti harness TIDAK buta terhadap kelas race p.467/p.468/p.469 |

Invariant yang divalidasi reader per snapshot: count ≤ 24; kode
null-terminated & non-kosong; severity/lifecycle enum valid; TIDAK ADA
kode duplikat (manifestasi langsung race double-append p.467);
overflowCount/persistFailures/generation monoton per pembaca. Fase
deterministik tambahan: injeksi kegagalan NVS saat quiescent →
`AcceptedPersistFailed` jujur → `_dirty` bertahan → HANYA checkpoint
persistence task yang me-retry (kontrak recovery p.454/p.468).

Runner CI membangun ketiganya + **mensyaratkan negative control GAGAL**:
exit 0 pada mode unlocked = kegagalan suite ("harness blind") — bukti
sensitivitas jadi bagian dari gate permanen.

### 13.4 Gate statis round-7

`scripts/test_audit_round7_2026_09.py` — **29/29 PASS**: (A) mutex internal
dibuat pra-scheduler; (B) seluruh 7 mutator publik = wrapper locked →
Unlocked, dan TIDAK ada implementasi Unlocked yang mengambil lock
(deadlock-free by construction), immediate-save memakai bentuk Unlocked,
checkpoint persistenceTask lewat entry publik; (C) snapshot tunggal di
GET /api/alarms + envelope telemetry, aksesor pointer mentah hilang,
find copy-out; (D) clearIfActive atomik dipakai AnomalyDetector; (E)
harness konkurensi + negative control + syarat sensitivitas ada di CI;
(F) invariant round-6 (satu record NVS, eviksi CLEARED-only, reject jujur,
reaktivasi) utuh menembus refactor.

Dua asersi gate lama di-update dengan justifikasi terdokumentasi:
r5-G6/G7 dan r6-F5 mem-panggil `saveToNVS()` internal → kini
`_saveToNVSUnlocked()` (panggilan publik dari dalam lock akan double-lock
mutex non-rekursif). Rangkaian semantik yang di-pin tidak berubah.

### 13.5 Regresi

31/31 suite Python PASS (r5 74/74, r6 34/34, r7 29/29 baru, relay 39,
tx-durability 30, emg-modular 85, dsb.); seluruh harness native GREEN
(termasuk 3 harness round-6); `pio run -e development -e staging` SUCCESS
(RAM 37,8%, flash 93,0%/93,2% — kenaikan flash +2,3 KB dari kode lock +
snapshot). Tidak ada perubahan wire/API: field JSON dan envelope
identik; konsumen PWA tidak tersentuh.

### 13.6 Yang tetap menunggu operator (tidak berubah)

Rotasi token GitHub/Vercel yang pernah di-paste (hygiene, P0), Gate A/B
provisioning fisik, acceptance T9–T14, INA-001..004, keputusan produk
p.429, kontrak lintas-layer p.430/431/p.441.

## 14. Round 8 (2026-09-14): audit independen PR #38 + temuan baru p.470 (pointer-escape status snapshot)

Round ini dijalankan tanpa auditor eksternal (tidak tersedia): verifikasi
independen atas remediasi round-7 DILAKUKAN SENDIRI atas baseline firmware
`main` = `1b26847b` (PR #38), disusul perburuan defect baru dan remediasi.

### 14.1 Verifikasi PR #38 (p.467/p.468/p.469) — seluruhnya TERVERIFIKASI

- **Source**: disiplin kunci 1:1 terkonfirmasi — 7 mutator publik =
  `_lock()` → `*Unlocked()` → `_unlock()`; tidak ada implementasi Unlocked
  yang mengambil kunci; immediate-save memakai `_saveToNVSUnlocked()` di
  dalam kunci (satu transaksi RAM+NVS); `snapshotInto()` menyalin seluruh
  state dalam satu kunci; aksesor pointer mentah (`getAlarm`,
  `getActiveAlarms`, `getActiveAlarmCount`) tidak tersisa; `clearIfActive()`
  atomik; mutex dibuat di `begin()` (setup line 343, `xTaskCreatePinnedToCore`
  pertama line 508); statik rate-limit log saturasi hanya disentuh di bawah
  kunci.
- **Analisis deadlock (baru, eksplisit)**: peta kunci diverifikasi TANPA
  SIKLUS — registry→Wire-internal (rtc.getUnixTime() di dalam kunci),
  registry→LogService, emergency→registry; tidak ada tepi balik: kode
  aplikasi tidak pernah memanggil registry dari dalam critical section Wire
  (diperiksa Wire.cpp core 2.0.16: lock internal per-transaksi, jendela
  repeated-start di RtcDriver/Ina219Driver selalu selesai `requestFrom`
  alamat sama); LogService tidak pernah memanggil balik registry; GasOta
  spinlock tidak menyentuh registry; tidak ada ISR (tidak ada
  attachInterrupt pada registry; kontrak task-context terpenuhi).
- **Eksekusi independen**: 32/32 suite Python (r5 74/74, r6 34/34, r7
  29/29); native harness — TSAN treatment 551.820 check/0 failure, ASAN
  584.512/0, negative control 174 laporan race (semua di
  `Services::alarms`); `pio run -e development -e staging` SUCCESS.
- **CI diverifikasi dari log aktual** (menjawab keluhan r7 atas klaim tak
  terverifikasi): run `34807668402` (push main @ `1b26847b`) — job "Python
  unit + property tests" success, log memuat `AUDIT ROUND 7 GATE: ALL
  CHECKS PASSED`, TSAN treatment `checks=573472 failures=0`, `NEGATIVE
  CONTROL OK: 177 race report(s)`, `ALL NATIVE HARNESS GREEN`; job
  reproducible-build, staging+development, production, release-gate semua
  success.
- **Kesimpulan: p.467, p.468, p.469 CLOSED terverifikasi independen.**

### 14.2 Temuan baru — p.470 (P2, pre-existing; bukan regresi PR #38)

**Pointer-escape pada serialisasi status**: `Core::SystemStatus.activeAlarms`
adalah POINTER ke `s_activeAlarmsBuf` — buffer statis yang ditulis ulang
`publishTelemetry()` (telemetryTask) setiap siklus 5 detik. Dua konsumen
menyalin struct di bawah `telemetryMutex`, MELEPASKAN mutex, lalu
menserialisasi MELALUI pointer tersebut:

- `Web::handleStatus()` (GET /api/status, task web) — `snap = latestStatus`
  → give → `Web::serialize(snap)`.
- `AI::GasAdvisor` (gasEmergencyTask) — bentuk identik untuk payload GAS
  yang ditandatangani.

Jendela race: serialisasi JSON (orde ms) vs. penulisan ulang buffer
(~3,3 KB) oleh telemetryTask. Hasil yang mungkin: entri alarm TORN di
`/api/status` / payload GAS — kode dari daftar baru + message/severity dari
daftar lama dalam satu entri (analisis byte-level: string tetap
null-terminated di semua tahap; tidak ada memory-unsafety). Jalur MQTT
`publishTelemetry()` sendiri TIDAK terdampak (penulis tunggal membaca buffer
miliknya sendiri). Peluang rendah (poll web harus bertepatan dengan jendela
publish ~ms per 5 s) tetapi nyata; kelasnya identik dengan p.469, satu
lapis di atas registry. Pola ini ada SEBELUM PR #38 (buffer statis +
pointer sama-sama ada di round-6); scope r7 (registry) tidak mencakupnya.

**Bukti (harness native baru)** `verify_status_snapshot_detach.cpp`
(std::thread, mirror bentuk firmware; entri punya `tagA`/`tagB` yang selalu
ditulis bersama — pembacaan torn = tagA≠tagB dalam satu entri):

| Build | Hasil |
|---|---|
| Treatment (deep-copy di bawah mutex) + TSAN | 0 race; 65.172 pembacaan, **0 entri torn** |
| Treatment + ASAN/UBSAN | 0 error; 0 torn |
| **Negative control** (`-DNO_DETACH`, bentuk lama: pointer ter-alias pasca-rilis mutex) + TSAN | **race report TSAN + 5 entri torn teramati secara logis** dalam 3 detik — bukti race nyata, bukan teoretis |

### 14.3 Remediasi p.470

`Web::serializeLatestStatusLocked()` di BatteryStatusSerializer.h (header
bersama yang sudah di-include kedua konsumen): ambil `telemetryMutex` →
salin struct → **deep-copy daftar alarm ke heap** → re-point pointer →
rilis mutex → serialisasi salinan mandiri → free. OOM: daftar
dihilangkan jujur (count=0) — pusat alarm `/api/alarms` (snapshot registry)
tetap otoritatif. `publishTelemetry()` dipertahankan apa adanya dan
didokumentasikan sebagai pengecualian penulis-tunggal. Dua konsumen
(termasuk payload GAS yang ditandatangani) dimigrasikan; komentar kepemilikan
buffer ditambahkan di `.ino`.

Gate statis baru `test_audit_round8_2026_09.py` — **20/20 PASS**: (A) bentuk
helper (kunci → salin → deep-copy → rilis → serialisasi → free; OOM jujur;
copy terikat count yang disalin), (B) dua konsumen bermigrasi + 503 timeout
tetap, (C) satu-satunya pemanggil `Web::serialize(` yang tersisa adalah
`publishTelemetry` (pemilik buffer), (D) harness + negative control masuk
runner CI dengan syarat sensitivitas, (E) invariant round-7 registry utuh.

### 14.4 Observasi latency yang DITERIMA (P3, tanpa fix)

Kunci registry dipegang selama: `rtc.getUnixTime()` (jalur DS3231 = 7
transaksi I2C di bus bersama) + tulis NVS + `Log.append` (LittleFS +
rtc lagi). Hold time tipikal belasan–puluhan ms; worst-case terbatas
(~700 ms hanya bila bus I2C hang — setiap transaksi dibatasi timeout Wire
50 ms). Tidak ada deadlock (peta kunci asiklik, lihat 14.1); priority
inheritance mencegah starvation emergencyTask; aksi fisik relay darurat
mendahului `alarms.raise` (verifikasi urutan di EmergencySupervisor) —
keselamatan tidak pernah menunggu kunci registry. Diterima dengan
dokumentasi ini; optimasi (stempel waktu di luar kunci) ditunda sampai ada
bukti kebutuhan.

### 14.5 Catatan kecil (P4, tidak ditindak)

find()→acknowledge() dua kunci di pipeline ack (web/MQTT): TOCTOU benign —
acknowledge idempotent terhadap kode yang hilang; kasus ekstremnya adalah
"sukses" untuk alarm yang tereviksi di antara dua panggilan (sudah Cleared).
Mutex-create-failure fallback (registry tanpa kunci) hanya terjadi pada OOM
ekstrem saat boot — pola yang sama dengan AuthManager/LogService.
`generation` uint16 bisa wrap setelah 65.535 transaksi — diagnostik saja.
