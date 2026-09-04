// ---------------------------------------------------------------------------
// GasRootCa.h — TLS trust anchor for Google Apps Script (script.google.com)
// [WAVE-4 / GAS-2-D] Replaces the unconditional setInsecure() in GasAdvisor.
//
// The pinned certificate is the SELF-SIGNED Google Trust Services root
// ("GTS Root R4", ECDSA P-384, CA:TRUE):
//   Subject/Issuer: C=US, O=Google Trust Services LLC, CN=GTS Root R4
//   Validity:       2026-06-22 backdated creation — valid through 2036-06-22
//   SHA-256 fp:     34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:
//                   4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D
//
// Verified cryptographically before embedding (openssl):
//   script.google.com serves  *.google.com <- WE2 <- GTS Root R4
//   openssl verify -partial_chain -trusted <this cert> WE2   → OK
//   openssl verify -trusted <this cert> -untrusted WE2 leaf → OK
//
// This is PUBLIC PKI material, not a secret. Rotation policy: Google rotates
// INTERMEDIATES freely (WE2 expires 2029) but roots last decades; if a future
// migration moves script.google.com off GTS Root R4, firmware GAS calls will
// FAIL CLOSED (TLS verify error) — the honest behavior. An operator can
// override the anchor at build time with -DGAS_ROOT_CA="<pem>" without
// touching this file.
//
// NOTE: certificate validation requires a SNTP-synced clock (mbedTLS checks
// notBefore/notAfter). The HMAC contract already requires it (±300 s replay
// window on auth.timestamp) — by the time GasAdvisor signs a request, time
// is valid, so pinning adds no new clock dependency.
// ---------------------------------------------------------------------------

#pragma once

namespace PLTS {

// NOLINTNEXTLINE(readability-identifier-naming)
static const char GAS_ROOT_CA_GTS_R4[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

}  // namespace PLTS
