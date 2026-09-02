// =============================================================================
// Services/WifiManager.cpp
// =============================================================================
#include "WifiManager.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Storage/ConfigStore.h"
#include "HealthSupervisor.h"
#include "LogService.h"
#include "AlarmRegistry.h"
#include <Preferences.h>
#include <DNSServer.h>
#include <esp_task_wdt.h>
#include <cstring>

namespace Services {

WifiManager wifi;

static DNSServer* g_dns = nullptr;

void WifiManager::begin() {
  _mode = WifiMode::Disconnected;
  WiFi.mode(WIFI_OFF);
  delay(100);

  Preferences p;
  p.begin(Core::NVS_NAMESPACE, true);
  String ssid = p.getString(Core::NVS_KEY_WIFI_SSID, "");
  String pass = p.getString("wifi_pass", "");
  p.end();

  if (ssid.length() > 0) {
    connectSta(ssid, pass);
  } else {
    startConfigPortal();
  }
}

void WifiManager::connectSta(const String& ssid, const String& pass) {
  Serial.printf("[WIFI] STA connecting to '%s'...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower((wifi_power_t)Core::WIFI_TX_POWER_DBM);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < Core::WIFI_STA_TIMEOUT_MS) {
    // [FW-33 REMEDIATION 2026-08] This loop previously blocked up to 15 s with
    // no watchdog reset while the task WDT is configured for 10 s — an
    // unreachable AP guaranteed a WDT reset loop at boot. Feed the watchdog
    // while waiting (connection failure is a RECOVERABLE condition, not a
    // hang — the AP fallback below handles it).
    esp_task_wdt_reset();
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    _mode = WifiMode::STA;
    _staRetries = 0;
    Serial.printf("[WIFI] STA connected, IP: %s, RSSI: %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    alarms.clear("NETWORK_DEGRADED");
    Log.append(Core::LogType::WifiConnected,
               String("STA connected: ") + WiFi.localIP().toString(), 0);
  } else {
    Serial.println(F("[WIFI] STA connect failed — starting AP fallback"));
    if (++_staRetries >= Core::WIFI_STA_MAX_RETRIES) {
      startConfigPortal();
    }
  }
}

void WifiManager::_tryStaConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - _lastReconnectMs < 5000) return;
  _lastReconnectMs = millis();
  Serial.println(F("[WIFI] STA reconnect attempt"));
  WiFi.reconnect();
  Services::health.recordWifiReconnect();
}

void WifiManager::_startAP() {
  WiFi.mode(WIFI_AP);
  // [AUDIT 2026-08-28 F-FW3] Same SSID pattern as firmware-generic + every
  // doc/PWA surface: PLTS-Monitor-Setup-XXXX (was "PLTS-Setup<XXXX>" — an
  // operator following the README looked for a network name that never
  // appeared). 4 hex tail from the eFuse MAC keeps devices distinguishable.
  String macTail = String((uint32_t)(ESP.getEfuseMac() & 0xFFFF), HEX);
  macTail.toUpperCase();
  while (macTail.length() < 4) macTail = "0" + macTail;
  String ssid = "PLTS-Monitor-Setup-" + macTail.substring(macTail.length() - 4);
  String pass = getApPassword();
  WiFi.softAP(ssid.c_str(), pass.c_str(), Core::WIFI_CHANNEL, false, 4);
  _mode = WifiMode::AP;
  // [AUDIT 2026-08-28 F-G15] PRODUCTION_BUILD meredaksi password AP di sini:
  // pengungkapan resmi HANYA sekali saat generation (ConfigStore) — model
  // "log boot awal / sheet provisioning" yang didokumentasikan README. Build
  // dev/staging tetap mencetak untuk kenyamanan bench.
#ifndef PRODUCTION_BUILD
  Serial.printf("[WIFI] AP fallback: SSID=%s pass=%s\n", ssid.c_str(), pass.c_str());
#else
  Serial.printf("[WIFI] AP fallback: SSID=%s pass=******** "
                "(PRODUCTION_BUILD: password AP hanya dicetak saat pertama "
                "di-generate; bila hilang → factory reset untuk regenerasi)\n",
                ssid.c_str());
#endif
  Serial.printf("[WIFI] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  if (!g_dns) {
    g_dns = new DNSServer();
    g_dns->start(53, "*", WiFi.softAPIP());
  }
}

void WifiManager::startConfigPortal() {
  _startAP();
  Log.append(Core::LogType::WifiDisconnected, "Config portal started (AP fallback)", 0);
}

void WifiManager::tick() {
  if (_mode == WifiMode::AP) {
    // [AUDIT 2026-08-28 F-FW2] The captive DNS wildcard is STARTED in
    // _startAP() but was previously only processed inside the STA branch —
    // a dead branch (g_dns is null in pure-STA). In AP mode DNS requests
    // were never answered, so the captive redirect never fired. Process
    // them HERE, where the portal actually runs.
    if (g_dns) g_dns->processNextRequest();
    return;
  }
  if (_mode == WifiMode::STA) {
    if (WiFi.status() != WL_CONNECTED) {
      _tryStaConnect();
      if (WiFi.status() != WL_CONNECTED) {
        alarms.raise("NETWORK_DEGRADED", Core::AlarmSeverity::Warning,
                     "WiFi STA disconnected");
      }
    }
  }
}

String WifiManager::getLocalIp() const {
  if (_mode == WifiMode::STA) return WiFi.localIP().toString();
  if (_mode == WifiMode::AP)  return WiFi.softAPIP().toString();
  return "0.0.0.0";
}

String WifiManager::getMacAddress() const {
  uint64_t mac = ESP.getEfuseMac();
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
  return String(buf);
}

String WifiManager::getApPassword() const {
  return String(Core::apPassword);
}

} // namespace Services
