# FINAL ACCEPTANCE GATES — v1.9.3 (Struktur A–F)

> **Ditetapkan oleh:** Independent Auditor (2026-09-06, tahap final acceptance).
> **Verdict saat ditetapkan:** 🔴 **NOT PRODUCTION GRADE — RELEASE BLOCKED** —
> nol temuan software baru, nol temuan CLOSED yang dibuka ulang; seluruh blocker
> yang tersisa bersifat fisik, kredensial, atau otoritas rilis.
>
> **Freeze source:** firmware SRC `e84798a7243bfb3e11b2398484ae66defdf9c292`
> (binary; repo `main` hanya boleh menerima perubahan docs di atasnya) ·
> PWA `9e47b4f89f08db23a6eeafe56604255322f357af`.

Dokumen ini mengarsipkan struktur gate final audit A–F beserta aturan main
eksekusinya, agar rantai rilis v1.9.3 di repo ini self-contained. Pendamping
operasional: `docs/PANDUAN_PENGISIAN_v1.9.3.md` (cara mengisi evidence + perintah
verifier) dan `docs/PRODUKSI_ENV_EVIDENCE_v1.9.3.md` (referensi Gate A). Latar
belakang audit software-side: `docs/FINAL_SOFTWARE_AUDIT_v1.9.3.md`; runbook
eksekusi rilis: `docs/RELEASE_EXECUTION_v1.9.3.md`.

## 1. Matriks gate

| Gate | Isi | Artefak / perintah | Pemilik |
|---|---|---|---|
| **A** — Topologi PWA produksi | Set env transport nilai riil di Vercel → redeploy → probe `/api/health` (env transport kosong saat audit = release blocker operasional, P1 #5) | Matriks env + prosedur + perintah probe: `docs/PRODUKSI_ENV_EVIDENCE_v1.9.3.md` §3–4 | Pemilik project (akses Vercel) |
| **B** — Hardware acceptance | Flash binary byte-exact (Actions run #100) → 12 kriteria bench → isi `docs/hardware-acceptance/v1.9.3.json` | Buku Lapangan 12 blok + template pre-fill (paket bench pemilik project) | Pemilik project (bench) |
| **C** — Verifier (fail-closed) | `scripts/verify_ina219_hardware_acceptance.py` dan `scripts/verify_ota_evidence.py` (flag `--source-commit e84798a…`): PASS → evidence eligible; FAIL → tetap blocked | Perintah lengkap: `docs/PANDUAN_PENGISIAN_v1.9.3.md` §4 | Engineer (on-demand) |
| **D** — Release authority | Commit evidence ter-pin `@1788673260` di `release/v1.9.3` → `git tag -s v1.9.3` (GPG pemilik project) → push → rantai CI → publish GitHub Release → provenance binding | Langkah 3–4 PANDUAN; `canonicalReleaseGitCommit` diisi setelah tag | Pemilik project (GPG) + Engineer (provenance on-demand) |
| **E** — Physical OTA | Terhadap release yang benar-benar published: Phase B 16 kriteria + rollback/recovery | Buku Lapangan 16 blok + template pre-fill OTA (paket bench) | Pemilik project (bench) |
| **F** — Final audit | Verdict Independent Auditor atas seluruh evidence | — | Independent Auditor |

## 2. Urutan eksekusi (jangan terlewat)

```
flash → Gate B (12 kriteria HW) → Phase A (PWA menolak firmware pra-publish)
→ commit evidence ter-pin → Gate D (tag -s → CI chain → publish)
→ Phase B (16 kriteria OTA + rollback) → Gate C ke-2 (verifier JSON OTA)
→ provenance final → Gate F (verdict auditor)
```

- **Phase A dijalankan SEBELUM Gate D** (langkah `P0-02a` buku lapangan,
  setelah Gate B selesai): PWA produksi harus membuktikan menolak firmware
  yang belum dipublish (fail-closed). Jika Phase A menawarkan v1.8.0 untuk
  di-push → BERHENTI — kebijakan fail-closed rusak.
- **Gate C dijalankan DUA kali**: JSON hardware acceptance sebelum tag;
  JSON OTA physical test setelah Phase B.
- Gate E hanya menyisakan Phase B karena Phase A sudah dikerjakan pra-publish.

## 3. Aturan main (freeze + larangan defect loop)

1. **Freeze source**: tidak ada perubahan source firmware/PWA selama tahap
   acceptance. Repo `main` firmware boleh bergerak hanya untuk docs.
2. **Empat larangan defect loop** (berlaku jika ditemukan defect saat
   acceptance):
   - Tidak ada patch di tempat (no in-place patch);
   - Tidak ada reuse artifact lama;
   - Tidak ada perubahan hasil acceptance yang sudah dicatat;
   - Defect → branch fix terpisah → CI ulang → artifact baru → re-flash →
     acceptance diulang dari awal.
3. **Anti bukti sintetis**: semua nilai `observed` harus berasal dari alat ukur
   nyata di bench. Template/pre-fill JSON bukan evidence sampai terisi dan
   di-commit oleh pemilik project pada branch `release/v1.9.3` dengan tanggal
   ter-pin.
4. **Tiga identitas terikat**: firmware (SRC + SHA), hardware (boardRevision
   `bench-prototype` + serial ESP32 nyata), release (tag `v1.9.3` bertanda
   tangan GPG pemilik project).

## 4. Kunci rantai rilis (RE-CUT 2026-09-06)

| Item | Nilai |
|---|---|
| SRC (source commit yang diuji) | `e84798a7243bfb3e11b2398484ae66defdf9c292` |
| SRC_TS (committer timestamp, untuk commit ter-pin) | `1788673260` |
| firmwareSha256 (artifact otoritatif) | `24e8ae225f53ad48c3cb2e9a0a0ebfc71cff4bbd6f7127a1d74e9425a7be424e` |
| Artifact CI | Actions run #100 (id 34014592009, push main `e84798a`, `gitDirty=false`) |
| Branch rilis | `release/v1.9.3` = `e84798a` + docs re-cut `df2a5e5` (`diff firmware/` = kosong) |
| PWA produksi | Vercel, live dari main `9e47b4f` (PARITY-4) |

Penutupan seluruh pekerjaan diakhiri rotasi token yang pernah terpapar
(GitHub + Vercel) oleh pemilik project.
