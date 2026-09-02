// =============================================================================
// Services/WifiManager.h — STA primary + AP fallback (Config Portal)
// -----------------------------------------------------------------------------
// Generates per-device secrets at first boot via esp_random():
//   JWT secret, MQTT password, GAS HMAC secret, device PIN, AP fallback password
// NVS-persisted. PRODUCTION_BUILD hides secrets from Serial.
// =============================================================================
#pragma once
#ifndef PLTS_SERVICES_WIFI_MANAGER_H
#define PLTS_SERVICES_WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

namespace Services {

enum class WifiMode : uint8_t {
  Disconnected = 0,
  STA          = 1,
  AP           = 2,
};

class WifiManager {
public:
  void begin();
  void tick();
  WifiMode getMode() const { return _mode; }
  String   getLocalIp() const;
  String   getMacAddress() const;
  int8_t   getRssi() const { return _mode == WifiMode::STA ? WiFi.RSSI() : -127; }
  bool     isStaConnected() const { return _mode == WifiMode::STA && WiFi.isConnected(); }
  // AP fallback password (CSPRNG-generated)
  String   getApPassword() const;
  // Reboot into STA mode
  void     connectSta(const String& ssid, const String& pass);
  // Force AP fallback (config portal)
  void     startConfigPortal();

private:
  WifiMode _mode = WifiMode::Disconnected;
  unsigned long _lastReconnectMs = 0;
  uint8_t  _staRetries = 0;
  void _tryStaConnect();
  void _startAP();
};

extern WifiManager wifi;

} // namespace Services

#endif // PLTS_SERVICES_WIFI_MANAGER_H
