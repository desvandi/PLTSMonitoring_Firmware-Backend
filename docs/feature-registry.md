# Feature Registry — PLTS Monitoring (Firmware + GAS Backend + PWA)

> **[PARITY-4 2026-09-06]** Registry lintas-lapisan ini adalah jawaban atas temuan
> audit paritas fitur: *"fitur yang terlihat sudah ada hanya karena sebuah
> TypeScript interface atau komponen sudah dibuat"* (feature ghosts). Setiap
> fitur produksi punya SATU baris di sini yang menyatakan lapisan mana yang
> memilikinya, jalur mutasinya, statusnya, dan syarat acceptance-nya.
> Versi machine-readable: `docs/feature-registry.json`.
>
> **Cara membaca status:**
> - `ALIGNED` — kontrak konsisten PWA ↔ GAS ↔ firmware, end-to-end.
> - `PARTIAL` — implementasi ada tapi belum lengkap end-to-end (alasan dicantumkan).
> - `OPTIONAL / NOT-ACTIVE` — sengaja tidak diaktifkan (butuh validasi fisik / hardware).
> - `RESERVED` — slot hardware/kontrak disiapkan, tidak ada sensor/perangkat.
> - `INTERNAL` — invarian firmware, BUKAN konfigurasi operator (dilarang diklaim sebagai config).
> - `ENGINEERING` — tool servis, bukan fitur operator produksi.
> - `BLOCKED-PHYSICAL` — jalur kode selesai; butuh perangkat fisik untuk evidence.
> - `REMOVED-GHOST` — pernah tampil di satu lapisan tanpa implementasi otoritatif; dihapus.

## Commissioning Authority (keputusan arsitektur — menutup ambiguitas P1)

Dua otoritas komisioning yang BERBEDA dan tidak saling menimpa (keputusan: **Option B**):

| Otoritas | Pemilik | Cakupan |
| --- | --- | --- |
| WiFi / jaringan / first-boot secrets | **Firmware AP provisioning portal** (`/`, `/api/provision` — AP setup mode saja) | SSID, password, mengisi credential GAS |
| Binding GAS + identitas dashboard | **PWA onboarding QR** (`/setup`) | `gas_webapp_url`, `auth_token`, `device_id`, preferensi dashboard |

PWA **tidak** mengendalikan `/api/provision` firmware — keduanya hidup berdampingan
dengan batas tanggung jawab eksplisit ini. Menggabungkannya ke satu UI = perubahan
arsitektur tersendiri, bukan bug.

## Registry

| ID | Fitur | PWA | GAS | FW | REST | MQTT | Persist | Readback | Status | Catatan / Acceptance |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| F-TEL-001 | Telemetri inti baterai (V/I/P/SOC/EFC/Ah/Wh/peak) | ✅ | ✅ | ✅ | /api/status | TELEMETRY | GAS sheet | ✅ | ALIGNED | truth-semantics per kanal |
| F-TEL-002 | Semantik kejujuran telemetri (VALID/STALE/INVALID/NOT_AVAILABLE/ESTIMATED/DERIVED) | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ALIGNED | truth-semantics.test.ts |
| F-TEL-003 | Lingkungan SHT31 + titik embun | ✅ | ✅ | ✅ | ✅ | ✅ | GAS | ✅ | ALIGNED | dew point derived, bukan diklaim terukur |
| F-TEL-004 | Kesehatan sensor + diagnostik | ✅ | ✅ | ✅ | /api/diagnostics | ✅ | — | ✅ | ALIGNED | |
| F-TEL-005 | Event + log | ✅ | ✅ | ✅ | /api/events, /api/log | ✅ | GAS+FS | ✅ | ALIGNED | |
| F-INA-001 | INA219 + mode PGA dinamis | ✅ | ✅ | ✅ | ✅ | ✅ | — | pga_mode | ALIGNED | |
| F-FLT-001 | Fleet view (GAS LATEST nested envelope) | ✅ | ✅ | — (sumber) | — | — | GAS | ✅ | ALIGNED | gasEnvelope.ts parser |
| F-TRH-001 | Riwayat telemetri (HISTORY) | ✅ | ✅ | — | — | — | GAS | ✅ | ALIGNED | |
| F-BMS-001 | BMS multi-protokol (Pylontech CAN / Modbus RTU / TCP) + mismatch + SOC otoritatif | ✅ | ✅ | ✅ | /api/bms | — | — | ✅ | ALIGNED (source) | penerimaan protokol fisik = domain terpisah |
| F-CAL-001 | Kalibrasi (3-titik tegangan, ACS712 zero, offset SHT31) | ✅ wizard | ✅ CALIBRATION_* | ✅ live-apply | /api/calibration* | ✅ | CRC32 | ✅ | ALIGNED | |
| F-EMG-001 | Emergency supervisor + kanal GAS (ARM/DISARM/CONFIG) | ✅ | ✅ | ✅ | — | ✅ | NVS plts_emg | ✅ | ALIGNED | |
| F-RLY-001 | Relay 8-kanal + transaction journal | ✅ | — | ✅ | /api/relays* | ✅ | journal | ✅ | ALIGNED | guard command-layer + UI |
| F-ALM-001 | Registry alarm (raise/clear/ACK≠CLEAR, max 24, persist) | ✅ | ✅ (events) | ✅ | /api/alarms* | ✅ | NVS plts_alarm(reg) | ✅ | ALIGNED | |
| **F-ALM-002** | **Threshold alarm two-tier (operator config)** | ✅ editable | — | ✅ | /api/config (flat + nested `alarmThresholds`) | config.update | NVS plts_alarm | ✅ flat + nested | **ALIGNED (PARITY-4)** | 11 field, nama = schema PWA; evaluasi tunggal di AnomalyDetector + hysteresis; validasi range + urutan tier di REST/MQTT/load |
| F-ALM-003 | Deteksi anomali rate-based (dV/dt, dI/dt, dT/dt, dSOC/dt, stuck) | — (events) | ✅ | ✅ | — | — | — | events | ALIGNED | konstanta engineering, bukan config operator |
| F-REP-001 | Laporan energi harian (DAILY) | ✅ GAS-first + badge sumber | ✅ agregasi honest | — (501 by design) | Next /api/reports: 503 jujur di non-demo | — | GAS | ✅ | ALIGNED (PARITY-3) | netWh dihitung klien; alarmCount/availability jujur kosong; energyQuality diteruskan |
| F-INS-001 | AI insights (advisory, Gemini) | ✅ | ✅ fail-closed | ✅ proxy HMAC | /api/insights | — | cache GAS | ✅ | ALIGNED (PARITY-3) | tanpa GEMINI_API_KEY → 503 jujur |
| F-OTA-001 | OTA bertanda tangan + kebijakan rilis + jembatan OTA_STATUS | ✅ | ✅ | ✅ | /api/ota* | ✅ | NVS plts_ota | ✅ | ALIGNED (source) | release authority → lihat F-REL-001 |
| F-CFG-001 | Konfigurasi identitas perangkat (nama/site/timezone) | ✅ | — | ✅ | /api/config/device | — | NVS | ✅ | ALIGNED | requestId + journal (PARITY-4 memastikan PWA selalu mengirim requestId) |
| F-CFG-002 | Konfigurasi baterai (kapasitas/fullV/lowV/idle/…) | ✅ | — | ✅ | /api/config | ✅ | NVS plts_batt | ✅ | ALIGNED | validasi range + journal |
| **F-CFG-003** | **Backup/restore konfigurasi (export/import)** | ✅ | — | ✅ | /api/config/export, /import | — | CRC32 | ✅ | **ALIGNED (PARITY-4)** | import kini ber-transaction identity (header X-Request-Id + journal sha256 body); alarmConfig ikut backup |
| **F-CFG-004** | **Konfigurasi BMS comm (protokol/poll/slave/host/port)** | ✅ | — | ✅ | /api/config | ✅ | NVS plts_batt | ✅ | **ALIGNED (PARITY-4)** | bug laten ditutup: whitelist canonicalizer kini menerima field BMS (sebelumnya REST/MQTT selalu ditolak "unknown field") |
| F-CFG-005 | Ganti kata sandi operator | ✅ | — | ✅ | /api/config/password | — | NVS | ✅ | ALIGNED (PARITY-4) | kini jalur transaksi canonical (requestId + journal) |
| F-AUT-001 | Autentikasi (PBKDF2, JWT, CSRF, factory-reset token) | ✅ | ✅ (token) | ✅ | /api/login, /api/session, … | — | NVS | ✅ | ALIGNED | |
| F-PUSH-001 | Push alarm (WebPush + PWA service worker) | ✅ | ✅ | — | — | — | GAS | ✅ | ALIGNED | |
| F-SYS-001 | Waktu (NTP/RTC) + timeQuality | ✅ | ✅ | ✅ | — | — | NVS | ✅ | ALIGNED | |
| F-PRV-001 | WiFi provisioning (AP portal, first-boot) | — (bukan otoritas) | — | ✅ | `/`, /api/provision | — | NVS | ✅ | ALIGNED (otoritas = firmware portal) | lihat tabel Commissioning Authority di atas |
| F-PZEM-001 | Meter AC nyata PZEM-004T v3 | ✅ model | ✅ ingest | ✅ driver (flag OFF) | — | — | — | meter.* | **OPTIONAL / NOT-ACTIVE** | `PLTS_ENABLE_PZEM_AC=0`. Aktivasi HANYA setelah: validasi fisik meter PASS → flag ON di platformio.ini → build baru → verifikasi ingest GAS → verifikasi readback PWA. Jangan sebut "production implemented" sebelum itu |
| F-GEN-001 | Kanal genset (ACS712 ke-2, i_ac_gen) | ✅ (NOT_AVAILABLE) | ✅ kolom | slot GPIO32 RESERVED | — | — | — | null = jujur | **RESERVED / FUTURE** | tanpa sensor, PWA wajib menampilkan NOT_AVAILABLE (bukan 0 A); threshold darurat `iAcGenOverA` = kanal RESERVED |
| F-RS485-001 | Konsol RS485 (low-level) | — (sengaja) | — | ✅ | — | — | — | — | **ENGINEERING / SERVICE** | tool servis internal; BUKAN fitur operator produksi — jangan dihitung sebagai gap UI |
| F-SOC-002 | "socParams" (syncOnFullCharge / syncOnVoltage / hysteresis / aging) | ❌ dihapus | — | invarian internal | — | — | — | — | **REMOVED-GHOST (PARITY-4)** | TIDAK ada konsumen di firmware: sinkronisasi full-charge = invarian mesin SOC (knob riil: `fullChargeCurrentThreshold` + `fullChargePersistenceSec`), OCV-at-rest = invarian boot. Ghost type+mock dihapus dari PWA |
| F-CAL-002 | sht31HeaterEnabled / autoZeroAcs712OnBoot | ❌ dihapus | — | ❌ tidak ada dukungan driver | — | — | — | — | **REMOVED-GHOST (PARITY-4)** | driver SHT31 tidak punya kontrol heater; tidak ada mekanisme auto-zero-on-boot. Ghost type+mock dihapus dari PWA. Realisasikan hanya bila driver ditulis dulu |
| F-MOCK-001 | Route Next.js `/api/*` berbasis mockStore | demo saja | — | — | PWA origin | — | .data/ | — | **DEMO NAMESPACE (fail-closed)** | produksi TIDAK PERNAH bisa menyajikan data mock: demo-auth fail-closed + guard 503 per route + contract test scan. Route device otoritatif hidup di origin `NEXT_PUBLIC_API_BASE_URL`, bukan origin PWA |
| F-REL-001 | Tag + GitHub Release v1.9.3 (signed, immutable) | policy pin | — | SRC e84798a (re-cut 2026-09-06) | — | — | — | — | **BLOCKED-PHYSICAL** | **RE-CUT 2026-09-06**: SRC di-pin ulang ke main `e84798a` (pasca PARITY-3/4). Rantai lama (`96cb34b`+`c2a2c1ca`) mendahului PARITY-3/4 — merilisnya akan mengirim firmware tanpa alarm config otoritatif, whitelist canonicalizer BMS/alarm, dan transaction identity yang sudah dieksekusi PWA produksi (main), membuka-ulang temuan CLOSED F-ALM-002/F-CFG-003/F-CFG-004 saat runtime. Tree firmware `e84798a` berbeda dari rantai lama HANYA oleh commit parity-3/4 (kerja PCB S12/S10 hanya menyentuh `pcb/`, nol source firmware). Artifact CI run #100 (push main): firmwareSha256 `24e8ae22…424e`. Tag HARUS menunjuk commit evidence DI ATAS SRC — bukan main HEAD yang bergerak |
| F-HWA-001 | Hardware acceptance v1.9.3 (12 kriteria) | — | — | ✅ template+verifier | — | — | — | — | **BLOCKED-PHYSICAL** | `docs/hardware-acceptance/v1.9.3.json` harus dari perangkat NYATA (template sintetis dilarang). Alat bantu: buku lapangan + pre-fill JSON di sesi bench |
| F-OTAT-001 | Uji OTA fisik v1.9.3 (16 kriteria, 2 fase) | — | — | ✅ template+verifier | — | — | — | — | **BLOCKED-PHYSICAL** | `docs/ota-physical-test/v1.9.3.json` dari perangkat NYATA |

## Urutan prioritas yang tersisa (melanjutkan fase audit)

1. ✅ Phase 1 — registry ini.
2. ✅ Phase 2 — kontrak P0 lintas lapisan (alarm config F-ALM-002; reports F-REP-001 sudah ditutup parity-3).
3. ✅ Phase 3 — mock API terisolasi perilaku (F-MOCK-001: guard fail-closed + contract test; namespace fisik `/api/demo/*` = hardening opsional berikutnya).
4. ✅ Phase 4 — paritas transaksi canonical (F-CFG-001/003/004/005).
5. ✅ Phase 5 — klasifikasi fitur opsional (F-PZEM-001, F-GEN-001, F-RS485-001, F-SOC-002, F-CAL-002).
6. ⏳ Phase 6 — hardware acceptance v1.9.3 (F-HWA-001) — **menunggu sesi bench fisik Anda**.
7. ⏳ Phase 7 — uji OTA fisik v1.9.3 (F-OTAT-001) — **menunggu perangkat**.
8. ⏳ Phase 8 — tag signed + release immutable (F-REL-001) — otomatis terbuka setelah 6–7.
9. ⏳ Phase 9 — audit lintas-lapisan final.

> **Aturan emas:** baris registry hanya boleh berstatus `ALIGNED` bila SELURUH rantai
> berlaku: tipe PWA → mutasi → canonical command → GAS/MQTT/REST → penyimpanan
> otoritatif firmware → runtime consumer → readback. Parameter yang hanya hidup di
> satu lapisan wajib diklasifikasi (INTERNAL / ENGINEERING / REMOVED-GHOST), dilarang
> menyamar sebagai konfigurasi perangkat.
