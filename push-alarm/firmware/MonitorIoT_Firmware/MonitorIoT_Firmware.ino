/*
 * MonitorIoT_Firmware.ino - Firmware ESP32 untuk ekosistem MonitorIoT
 * =====================================================================
 * Peran dalam arsitektur (kontrak Tabel 11 - Ambang alarm FW-GAS):
 *
 *   FIRMWARE  = pemasok data: baca sensor, evaluasi ambang LOKAL,
 *               kirim laporan berkala (flag alarm level, bukan event)
 *               ke endpoint `ingest` Google Apps Script (GAS).
 *   GAS       = pemilik keputusan kirim: deteksi tepi naik (rising
 *               edge) per sensor -> push Web Push ke PWA SEKALI per
 *               kejadian alarm.
 *   PWA       = penerima alarm push + dashboard.
 *
 * PRINSIP PENGIRIM TUNGGAL (anti dobel-kirim):
 *   Firmware TIDAK PERNAH memanggil endpoint push (subscribe/testPush/
 *   sendAlarmToAll). Firmware hanya melaporkan STATUS level (alarm
 *   true/false) setiap siklus laporan. Ketika kondisi alarm bertahan
 *   (beberapa laporan berturut-turut alarm=true), GAS tidak mengirim
 *   push tambahan - hanya tepi naik false->true yang memicu push.
 *   Alarm lokal (LED + buzzer) tetap berbunyi kontinu sesuai kondisi,
 *   karena indikator lokal adalah kanal terpisah dari push.
 *
 * Skema laporan ingest (kontrak FW-GAS):
 *   POST {GAS_URL}
 *   Content-Type: application/json
 *   {
 *     "action": "ingest",
 *     "token": "<token perangkat>",
 *     "device": { "id": "...", "fw": "1.0.0", "uptimeMs": 123456 },
 *     "sensors": [
 *       { "name": "Suhu Greenhouse 1", "value": 41.2, "unit": "C",
 *         "alarm": true, "severity": "critical",
 *         "status": "suhu 41.2 C > ambang 40.0 C" },
 *       ...
 *     ],
 *     "reportedAt": 1724900000000
 *   }
 *
 *   Respons GAS: { "ok": true, "triggered": ["ALM-..."],
 *                 "resolved": ["ALM-..."], "pushes": {...} }
 *
 * Kebutuhan pustaka (Arduino Library Manager):
 *   - ArduinoJson        (v6 atau v7; jika v7, StaticJsonDocument masih
 *                         tersedia sebagai alias JsonDocument)
 *   - DHT sensor library (Adafruit) + Adafruit Unified Sensor
 *     (hanya bila SENSOR_MODE_DHT22 aktif)
 *
 * Kebutuhan inti papan (board core): arduino-esp32 >= 2.0.4
 *   (setCACert ganda untuk verifikasi rantai Google Trust Services;
 *   v1.1.0: verifikasi TLS aktif secara default).
 *
 * Riwayat versi:
 *   1.0.0  - rilis awal (audit silang).
 *   1.1.0  - keamanan: verifikasi sertifikat TLS root CA Google (GTS
 *            Root R1 + R4) aktif default; setInsecure kini opt-in
 *            lewat TLS_SKIP_CERT_VERIFY untuk jaringan ber-proxy.
 *
 * Mode uji tanpa perangkat keras:
 *   - Biarkan SENSOR_MODE_SIMULATED aktif: nilai sensor simulasi
 *     random-walk di sekitar SIM_*_CENTER sehingga seluruh rantai
 *     FW -> GAS -> push -> PWA dapat diuji dari meja tanpa ESP32
 *     terpasang di lapangan. Naikkan SIM_TEMP_CENTER di atas ambang
 *     untuk memicu alarm sungguhan dari sisi firmware.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

/* ======================= MODE SENSOR ======================= */
// Pilih SATU: (aktifkan salah satu, yang lain dikomentari)
#define SENSOR_MODE_SIMULATED 1
// #define SENSOR_MODE_DHT22 1

#if defined(SENSOR_MODE_SIMULATED) && defined(SENSOR_MODE_DHT22)
#error "Pilih hanya SATU mode sensor."
#endif

/* ======================= KONFIGURASI ======================= */

// Versi firmware (dikirim sebagai device.fw ke GAS; naikkan setiap
// perubahan perilaku agar jejak laporan di GAS bisa dibedakan).
#define FW_VERSION "1.1.0"

// -- WiFi --
const char* WIFI_SSID = "GANTI_NAMA_WIFI";
const char* WIFI_PASS = "GANTI_PASSWORD_WIFI";

// -- Endpoint GAS (URL deployment Web App, sama dengan API_BASE PWA) --
const char* GAS_URL =
  "https://script.google.com/macros/s/AKfycbxGANTI_DENGAN_ID_DEPLOYMENT_ANDA/exec";

// -- Token perangkat: harus sama dengan Script Properties GAS
//    (FW_DEVICE_TOKEN, atau daftar FW_DEVICE_TOKENS untuk banyak perangkat).
//    JANGAN di-commit ke repositori publik.
const char* DEVICE_ID    = "esp32-greenhouse-01";
const char* DEVICE_TOKEN = "GANTI_TOKEN_PERANGKAT";

// -- Interval & batas waktu --
const uint32_t REPORT_INTERVAL_MS = 60000UL;  // laporan rutin tiap 60 detik
const uint32_t HTTP_TIMEOUT_MS    = 15000UL;  // batas waktu POST ke GAS
const uint32_t WIFI_TIMEOUT_MS    = 20000UL;  // batas waktu join WiFi
const uint32_t MAX_BACKOFF_MS     = 480000UL; // backoff maksimum 8 menit
const uint32_t NTP_TIMEOUT_MS     = 10000UL;

// -- Ambang alarm LOKAL (harus konsisten dengan THRESHOLDS di
//    PushService.gs agar evaluasi fallback GAS sejajar) --
const float TEMP_MAX   = 40.0;  // C  -> critical bila suhu > ambang
const float SOIL_MIN   = 30.0;  // %  -> warning  bila tanah < ambang
const float HUM_MIN    = 25.0;  // %  -> warning  bila udara < ambang

// -- Simulasi (mode SENSOR_MODE_SIMULATED) --
float SIM_TEMP_CENTER = 29.5;  // naikkan > 40 untuk memicu alarm suhu
float SIM_HUM_CENTER  = 71.0;
float SIM_SOIL_CENTER = 55.0;

// -- Pin (mode DHT22) --
#define DHT_PIN     4          // GPIO data DHT22
#define DHT_TYPE    DHT22
#define SOIL_PIN    34         // ADC capacitive soil moisture (0-3.3V)
#define BUZZER_PIN  -1         // GPIO buzzer/relay, -1 = nonaktif
// LED_BUILTIN umumnya GPIO2 pada modul ESP32 DevKit.

// -- Keamanan TLS --
// Default: verifikasi rantai sertifikat GAS terhadap root CA Google
// (GTS Root R1 + R4, di bawah). Aktifkan baris berikut HANYA bila
// jaringan Anda memasang proxy yang memutus TLS (hotspot captive,
// firewall sekolah/kantor) - koneksi menjadi rentan MITM:
// #define TLS_SKIP_CERT_VERIFY 1

/* ======================= STATUS INTERNAL ======================= */

// Catatan: float NAN dipakai sebagai penanda gagal baca sensor.
float lastTempC    = NAN;      // suhu terakhir (C)
float lastHumPct   = NAN;      // kelembapan udara (%)
float lastSoilPct  = NAN;      // kelembapan tanah (%)

bool  wifiOk        = false;
bool  ntpOk         = false;
uint32_t lastReportMs    = 0;     // waktu laporan terakhir (millis)
uint32_t nextReportAtMs  = 0;     // jadwal laporan berikutnya (millis)
uint8_t  httpFailCount   = 0;     // hitungan kegagalan beruntun
uint32_t bootMs = 0;

// Buzzer non-blocking
bool     buzzerOn      = false;
uint32_t buzzerNextToggleMs = 0;
uint32_t buzzerPeriodMs     = 0;

#ifdef SENSOR_MODE_DHT22
#include <DHT.h>
DHT dht(DHT_PIN, DHT_TYPE);
#endif

/* ======================= ROOT CA GOOGLE (TLS) ======================= */
/* Dihasilkan otomatis oleh scripts/gen-tls-ca-embed.js dari
 * https://pki.goog/roots.pem (Google Trust Services). Jangan edit manual. */
const char* GTS_ROOT_R1 =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
  "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
  "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
  "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
  "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
  "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
  "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
  "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
  "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
  "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
  "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
  "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
  "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
  "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
  "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
  "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
  "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
  "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
  "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
  "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
  "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
  "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
  "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
  "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
  "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
  "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
  "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
  "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
  "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
  "-----END CERTIFICATE-----";

const char* GTS_ROOT_R4 =
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
  "-----END CERTIFICATE-----";

/* ======================= SETUP ======================= */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=====================================================");
  Serial.println(" MonitorIoT Firmware v" FW_VERSION " (ESP32)");
  Serial.println(" Peran: pemasok data + alarm lokal. Push = GAS.");
#ifdef TLS_SKIP_CERT_VERIFY
  Serial.println(" PERINGATAN: verifikasi TLS DIMATIKAN (mode uji).");
#endif
  Serial.println("=====================================================");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  if (BUZZER_PIN >= 0) pinMode(BUZZER_PIN, OUTPUT);

#ifdef SENSOR_MODE_DHT22
  dht.begin();
  Serial.println("[SENSOR] Mode DHT22 + soil capacitive aktif.");
#elif defined(SENSOR_MODE_SIMULATED)
  randomSeed(esp_random());
  Serial.println("[SENSOR] Mode SIMULASI aktif (tanpa perangkat keras).");
#endif

  wifiConnectBlocking();      // pertama kali: bloking agar cepat jelas
  syncTime();

  bootMs = millis();
  // Laporan pertama langsung dikirim, tidak menunggu interval.
  nextReportAtMs = millis();
}

/* ======================= LOOP UTAMA ======================= */

void loop() {
  ensureWifi();

  if (millis() >= nextReportAtMs) {
    reportCycle();
  }

  updateIndicators();   // LED + buzzer mengikuti kondisi alarm lokal
  delay(50);            // jeda kecil, hemat daya
}

/* ======================= SIKLUS LAPORAN ======================= */

/**
 * Satu siklus: baca sensor -> evaluasi ambang lokal -> kirim ke GAS.
 * Kegagalan kirim menaikkan backoff; data TIDAK dibuffer berlama-lama
 * (hanya dicoba ulang pada siklus berikutnya dengan bacaan segar,
 * karena data sensor basi berbahaya untuk keputusan alarm).
 */
void reportCycle() {
  readSensors();
  bool anyAlarm = evaluateLocalAlarms();  // isi flag tiap sensor
  logSensorState();

  if (sendReport()) {
    httpFailCount = 0;
    nextReportAtMs = millis() + REPORT_INTERVAL_MS;
  } else {
    httpFailCount++;
    // Backoff eksponensial: 2x, 4x, 8x ... maksimum MAX_BACKOFF_MS.
    uint32_t backoff = REPORT_INTERVAL_MS;
    for (uint8_t i = 0; i < httpFailCount && backoff < MAX_BACKOFF_MS; i++) {
      backoff *= 2;
    }
    if (backoff > MAX_BACKOFF_MS) backoff = MAX_BACKOFF_MS;
    nextReportAtMs = millis() + backoff;
    Serial.printf("[KIRIM] Gagal (%u kali beruntun). Coba lagi dalam %u detik.\n",
                  httpFailCount, (uint32_t)(backoff / 1000));
  }

  (void)anyAlarm; // alarm lokal dipakai updateIndicators via flag sensor
}

/* ======================= SENSOR ======================= */

struct SensorReading {
  const char* name;
  float value;
  const char* unit;
  bool  alarm;
  const char* severity; // "critical" | "warning" | "info"
  char  status[96];
};

SensorReading readings[3]; // diisi evaluateLocalAlarms()

void readSensors() {
#ifdef SENSOR_MODE_DHT22
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t)) Serial.println("[SENSOR] DHT22: gagal baca suhu");
  if (isnan(h)) Serial.println("[SENSOR] DHT22: gagal baca kelembapan");
  lastTempC  = t;
  lastHumPct = h;

  // Capacitive soil moisture: ADC mentah (kering ~3200, basah ~1300 pada 3.3V).
  int raw = analogRead(SOIL_PIN);
  const int RAW_DRY = 3200;
  const int RAW_WET = 1300;
  float soil = (float)(RAW_DRY - raw) / (float)(RAW_DRY - RAW_WET) * 100.0f;
  if (soil < 0)   soil = 0;
  if (soil > 100) soil = 100;
  lastSoilPct = soil;

#elif defined(SENSOR_MODE_SIMULATED)
  // Random-walk di sekitar nilai pusat (kisaran per siklus).
  if (isnan(lastTempC))   lastTempC  = SIM_TEMP_CENTER;
  if (isnan(lastHumPct))  lastHumPct = SIM_HUM_CENTER;
  if (isnan(lastSoilPct)) lastSoilPct = SIM_SOIL_CENTER;
  lastTempC   += ((float)(random(-80, 81)) / 100.0f);
  lastHumPct  += ((float)(random(-150, 151)) / 100.0f);
  lastSoilPct += ((float)(random(-200, 201)) / 100.0f);
#endif
}

/**
 * Evaluasi ambang LOKAL - hasilnya flag level per sensor.
 * Sengaja berbentuk LEVEL (bukan event) agar deduplikasi kejadian
 * sepenuhnya milik GAS (lihat header file: prinsip pengirim tunggal).
 * Aturan yang sama tercermin di THRESHOLDS PushService.gs sebagai
 * jaring pengaman bila firmware lama tidak mengirim flag.
 */
bool evaluateLocalAlarms() {
  bool any = false;

  // 1) Suhu
  readings[0].name = "Suhu Greenhouse 1";
  readings[0].unit = "C";
  readings[0].value = lastTempC;
  readings[0].alarm = false;
  readings[0].severity = "info";
  snprintf(readings[0].status, sizeof(readings[0].status), "-");
  if (!isnan(lastTempC)) {
    if (lastTempC > TEMP_MAX) {
      readings[0].alarm = true;
      readings[0].severity = "critical";
      snprintf(readings[0].status, sizeof(readings[0].status),
               "suhu %.1f C melebihi ambang %.1f C", lastTempC, TEMP_MAX);
    } else {
      snprintf(readings[0].status, sizeof(readings[0].status),
               "suhu %.1f C (ambang maks %.1f C)", lastTempC, TEMP_MAX);
    }
  } else {
    snprintf(readings[0].status, sizeof(readings[0].status), "sensor suhu gagal dibaca");
  }

  // 2) Kelembapan udara
  readings[1].name = "Kelembapan Udara";
  readings[1].unit = "%";
  readings[1].value = lastHumPct;
  readings[1].alarm = false;
  readings[1].severity = "info";
  if (!isnan(lastHumPct)) {
    if (lastHumPct < HUM_MIN) {
      readings[1].alarm = true;
      readings[1].severity = "warning";
      snprintf(readings[1].status, sizeof(readings[1].status),
               "kelembapan %.0f%% di bawah ambang %.0f%%", lastHumPct, HUM_MIN);
    } else {
      snprintf(readings[1].status, sizeof(readings[1].status), "-");
    }
  } else {
    snprintf(readings[1].status, sizeof(readings[1].status), "sensor kelembapan gagal dibaca");
  }

  // 3) Kelembapan tanah
  readings[2].name = "Kelembapan Tanah";
  readings[2].unit = "%";
  readings[2].value = lastSoilPct;
  readings[2].alarm = false;
  readings[2].severity = "info";
  if (!isnan(lastSoilPct)) {
    if (lastSoilPct < SOIL_MIN) {
      readings[2].alarm = true;
      readings[2].severity = "warning";
      snprintf(readings[2].status, sizeof(readings[2].status),
               "tanah %.0f%% di bawah ambang %.0f%% (perlu siram)", lastSoilPct, SOIL_MIN);
    } else {
      snprintf(readings[2].status, sizeof(readings[2].status), "-");
    }
  } else {
    snprintf(readings[2].status, sizeof(readings[2].status), "sensor tanah gagal dibaca");
  }

  for (uint8_t i = 0; i < 3; i++) {
    if (readings[i].alarm) any = true;
  }
  return any;
}

/* ======================= KIRIM LAPORAN ======================= */

/**
 * Bangun & kirim JSON ingest ke GAS.
 * true = HTTP 200 dan respons ok:true dari GAS.
 */
bool sendReport() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[KIRIM] WiFi terputus - laporan ditunda.");
    return false;
  }

  WiFiClientSecure client;
#ifdef TLS_SKIP_CERT_VERIFY
  // Mode uji / jaringan ber-intersepsi TLS. JANGAN dipakai di produksi:
  // identitas server GAS tidak diverifikasi (rentan MITM).
  client.setInsecure();
#else
  // Produksi: verifikasi rantai sertifikat GAS (Google Trust Services).
  // Google melayani beberapa varian rantai (teramati langsung: GTS Root
  // R1 dan GTS Root R4), keduanya dipercaya. Membutuhkan arduino-esp32
  // core >= 2.0.4 (setCACert dapat dipanggil lebih dari sekali).
  client.setCACert(GTS_ROOT_R1);
  client.setCACert(GTS_ROOT_R4);
#endif
  HTTPClient http;
  if (!http.begin(client, GAS_URL)) {
    Serial.println("[KIRIM] URL tidak valid.");
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  // GAS Web App menjawab POST dengan 302 -> script.googleusercontent.com;
  // tanpa mengikuti redirect, kode respons terbaca 302 dan body hilang
  // (temuan X-4 audit silang).
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<1024> doc; // ArduinoJson v6; v7: JsonDocument
  doc["action"] = "ingest";
  doc["token"]  = DEVICE_TOKEN;
  JsonObject dev = doc.createNestedObject("device");
  dev["id"]       = DEVICE_ID;
  dev["fw"]       = FW_VERSION;
  dev["uptimeMs"] = (uint32_t)(millis() - bootMs);

  JsonArray arr = doc.createNestedArray("sensors");
  for (uint8_t i = 0; i < 3; i++) {
    JsonObject s = arr.createNestedObject();
    s["name"]     = readings[i].name;
    if (!isnan(readings[i].value)) {
      // Dibulatkan 1 desimal agar payload ringkas.
      s["value"]   = roundf(readings[i].value * 10.0f) / 10.0f;
    } else {
      s["value"]   = nullptr; // null -> PWA menampilkan "--"
    }
    s["unit"]     = readings[i].unit;
    s["alarm"]    = readings[i].alarm;      // FLAG LEVEL (kontrak FW-GAS)
    s["severity"] = readings[i].severity;
    s["status"]   = readings[i].status;
  }
  doc["reportedAt"] = nowEpochMs();

  String body;
  if (serializeJson(doc, body) == 0) {
    Serial.println("[KIRIM] Gagal membangun JSON.");
    http.end();
    return false;
  }

  Serial.printf("[KIRIM] POST %s (%u byte)...\n", GAS_URL, (unsigned)body.length());
  int code = http.POST(body);

  bool ok = false;
  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<512> ans;
    if (deserializeJson(ans, resp) == DeserializationError::Ok) {
      bool respOk = ans["ok"] | false;
      int nTrigger  = ans["triggered"].size();
      int nResolved = ans["resolved"].size();
      Serial.printf("[KIRIM] GAS ok=%d | alarm dipicu=%d, pulih=%d\n",
                    respOk ? 1 : 0, nTrigger, nResolved);
      ok = respOk;
    } else {
      Serial.println("[KIRIM] Respons GAS bukan JSON valid.");
    }
  } else {
    Serial.printf("[KIRIM] HTTP %d\n", code);
  }

  http.end();
  return ok;
}

/* ======================= JARINGAN & WAKTU ======================= */

void wifiConnectBlocking() {
  Serial.printf("[WIFI] Menghubungkan ke \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); // kedip = mencari
  }
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) {
    Serial.printf("[WIFI] Terhubung. IP: %s\n",
                  WiFi.localIP().toString().c_str());
    digitalWrite(LED_BUILTIN, LOW);
  } else {
    Serial.println("[WIFI] Gagal - akan dicoba ulang non-blokir.");
  }
}

void ensureWifi() {
  static uint32_t lastTryMs = 0;
  if (WiFi.status() == WL_CONNECTED) { wifiOk = true; return; }
  wifiOk = false;
  if (millis() - lastTryMs > 30000UL) {   // coba ulang tiap 30 detik
    lastTryMs = millis();
    Serial.println("[WIFI] Sambungan hilang - menghubungkan ulang...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void syncTime() {
  // NTP untuk reportedAt yang absah (zona Asia/Jakarta, UTC+7).
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[NTP] Menunggu waktu");
  uint32_t t0 = millis();
  struct tm ti;
  while (millis() - t0 < NTP_TIMEOUT_MS) {
    if (getLocalTime(&ti, 500)) break;
    Serial.print(".");
  }
  ntpOk = (time(nullptr) > 1600000000);
  Serial.println(ntpOk ? " siap." : " gagal (pakai millis sebagai fallback).");
}

uint64_t nowEpochMs() {
  time_t now = time(nullptr);
  if (ntpOk && now > 1600000000) {
    return (uint64_t)now * 1000ULL;
  }
  // Fallback: millis sejak boot (relatif, hanya untuk diagnostik).
  return (uint64_t)millis();
}

/* ======================= INDIKATOR LOKAL ======================= */

/**
 * LED & buzzer = alarm LOKAL (kanal terpisah dari push; tidak
 * menimpa keputusan GAS). Kritikal: kedip/bunyi 300/300 ms.
 * Warning: kedip/bunyi 500/500 ms. Normal: mati.
 */
void updateIndicators() {
  bool crit = false, warn = false;
  for (uint8_t i = 0; i < 3; i++) {
    if (!readings[i].alarm) continue;
    if (strcmp(readings[i].severity, "critical") == 0) crit = true;
    else warn = true;
  }

  uint32_t period = crit ? 600 : (warn ? 1000 : 0);
  bool ledState = false;
  if (period > 0) {
    ledState = ((millis() / (period / 2)) % 2) == 0;
  }
  digitalWrite(LED_BUILTIN, ledState);

  if (BUZZER_PIN < 0) return;
  if (period != buzzerPeriodMs) {   // pola berubah -> reset siklus
    buzzerPeriodMs = period;
    buzzerNextToggleMs = millis();
    buzzerOn = true;
  }
  if (period == 0) { digitalWrite(BUZZER_PIN, LOW); buzzerOn = false; return; }
  if (millis() >= buzzerNextToggleMs) {
    buzzerOn = !buzzerOn;
    buzzerNextToggleMs = millis() + period / 2;
    digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
  }
}

void logSensorState() {
  Serial.println("---- Laporan sensor ----");
  for (uint8_t i = 0; i < 3; i++) {
    char val[16];
    if (!isnan(readings[i].value)) dtostrf(readings[i].value, 4, 1, val);
    else                           snprintf(val, sizeof(val), "ERR");
    Serial.printf("  %-20s %6s %s  alarm=%d (%s)\n",
                  readings[i].name, val, readings[i].unit,
                  readings[i].alarm ? 1 : 0, readings[i].severity);
  }
}
