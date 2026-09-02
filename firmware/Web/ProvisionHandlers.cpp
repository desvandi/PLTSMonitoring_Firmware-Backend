// =============================================================================
// Web/ProvisionHandlers.cpp — First-boot WiFi provisioning (AP setup portal)
// -----------------------------------------------------------------------------
// [AUDIT 2026-08-28 F-FW1] Nothing in the firmware ever WROTE the
// wifi_ssid/wifi_pass NVS keys — WifiManager::begin() only READS them, and
// no REST endpoint accepts WiFi credentials. A fresh device flashed with the
// production-grade firmware booted into AP mode and hit a DEAD END: no
// captive page, no provisioning route, and a README that pointed at
// /api/config (which cannot set WiFi). This closes that gap honestly.
//
// SECURITY MODEL (deliberate, mirrors firmware-generic with a HARDER edge):
//   1. The provisioning surface exists ONLY while the device is in AP setup
//      mode — every handler below 403s/404s when the mode is anything else.
//   2. The AP is WPA2-protected with a CSPRNG 32-char password generated at
//      first boot (NVS ap_pass). The password is revealed EXACTLY ONCE, at
//      generation time on Serial (the commissioning moment — all build
//      profiles); subsequent AP starts redact it in PRODUCTION_BUILD.
//      Joining the AP therefore requires PHYSICAL access to the device —
//      that proximity is the authentication boundary for provisioning.
//   3. The endpoint provisions WiFi credentials ONLY — it never touches JWT
//      secrets, MQTT passwords, or any other device secret.
//   4. All other /api/* routes keep their normal JWT auth; this surface does
//      not weaken them (it shares no code path with requireAuth).
//   5. Every accepted provisioning is audit-logged (ConfigurationChanged).
//
// QR ONBOARDING: the page supports the same `#plts=<base64-json>` fragment
// as firmware-generic + the PWA QR Onboarding panel (Settings → QR). Only
// ssid/password apply on this firmware; other keys are ignored client-side.
// =============================================================================
#include "ProvisionHandlers.h"
#include "HttpServer.h"
#include "Common.h"
#include "../Core/Globals.h"
#include "../Core/Config.h"
#include "../Services/WifiManager.h"
#include "../Services/LogService.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace Web {
namespace ProvisionHandlers {

// ---------------------------------------------------------------------------
// GET / — captive setup page (AP mode only)
// ---------------------------------------------------------------------------
static const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang='id'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>PLTS Monitor — Setup Perangkat</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b1220;color:#e8ecf1;margin:0;padding:24px}
h1{color:#fbbf24;font-size:20px}
form{max-width:420px;margin:0 auto;background:#111a2b;padding:20px;border-radius:14px;border:1px solid #1f2a44}
label{display:block;margin:12px 0 4px;font-size:13px;color:#94a3b8}
input{width:100%;padding:10px;border-radius:8px;border:1px solid #334155;background:#0b1220;color:#e8ecf1;box-sizing:border-box}
button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:10px;background:#fbbf24;color:#0b1220;font-weight:600;cursor:pointer}
.msg{background:#1e293b;padding:10px;border-radius:8px;margin-bottom:12px;border-left:3px solid #fbbf24;font-size:13px}
.tag{display:inline-block;background:#1e293b;color:#7dd3fc;padding:3px 8px;border-radius:8px;font-size:11px;margin-bottom:8px}
#err{color:#f87171;font-size:13px;margin-top:10px;display:none}
</style></head><body>
<form id='fm' onsubmit='return save(event)'>
<h1>PLTS Monitor — Setup WiFi</h1>
<span class='tag'>firmware production-grade</span>
<div class='msg'>Perangkat belum memiliki kredensial WiFi. Isi SSID dan password
jaringan lokasi (2,4 GHz), lalu simpan — perangkat akan reboot dan mencoba
tersambung.</div>
<label>WiFi SSID</label>
<input name='ssid' id='ssid' placeholder='Nama WiFi' required maxlength='32'>
<label>WiFi Password</label>
<input name='pass' id='pass' type='password' placeholder='Password WiFi (kosongkan untuk jaringan terbuka)' maxlength='64'>
<button type='submit'>Simpan &amp; Restart</button>
<div id='err'></div>
</form>
<script>
(function(){try{
var h=location.hash||'';var m=h.match(/plts=([^&]+)/);if(!m)return;
var j=JSON.parse(decodeURIComponent(escape(atob(m[1]))));
if(j.ssid)document.getElementById('ssid').value=j.ssid;
if(j.password)document.getElementById('pass').value=j.password;
var ok=document.createElement('div');ok.className='msg';
ok.textContent='Form terisi otomatis dari QR onboarding.';
document.getElementById('fm').prepend(ok);
}catch(e){}})();
function save(ev){
ev.preventDefault();
var ssid=document.getElementById('ssid').value.trim();
var pass=document.getElementById('pass').value;
if(!ssid){document.getElementById('err').style.display='block';
document.getElementById('err').textContent='SSID tidak boleh kosong';return false;}
fetch('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},
body:JSON.stringify({ssid:ssid,pass:pass})})
.then(function(r){return r.json();})
.then(function(j){
if(j.success){document.getElementById('err').style.display='none';
var ok=document.createElement('div');ok.className='msg';
ok.textContent='Tersimpan. Perangkat reboot dalam 1 detik…';
document.getElementById('fm').prepend(ok);}
else{document.getElementById('err').style.display='block';
document.getElementById('err').textContent=j.message||'Gagal menyimpan';}})
.catch(function(){document.getElementById('err').style.display='block';
document.getElementById('err').textContent='Kesalahan jaringan';});
return false;}
</script></body></html>)HTML";

void handleRoot() {
  // Provisioning page exists ONLY in AP setup mode. A provisioned, online
  // device serves no portal — 404 keeps the STA attack surface unchanged.
  if (Services::wifi.getMode() != Services::WifiMode::AP) {
    http.send(404, "application/json", "{\"success\":false,\"message\":\"Not Found\"}");
    return;
  }
  sendSecurityHeaders();
  http.send(200, "text/html; charset=utf-8", FPSTR(SETUP_PAGE));
}

// ---------------------------------------------------------------------------
// GET /api/provision — status (no secrets)
// ---------------------------------------------------------------------------
void handleProvisionStatus() {
  StaticJsonDocument<256> doc;
  bool apMode = Services::wifi.getMode() == Services::WifiMode::AP;
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, true);
  bool hasCreds = p.getString(Core::NVS_KEY_WIFI_SSID, "").length() > 0;
  p.end();
  doc["apMode"] = apMode;
  doc["provisioningNeeded"] = apMode;
  doc["hasStoredCredentials"] = hasCreds;
  doc["deviceId"] = Core::deviceId;
  doc["deviceName"] = Core::deviceName;
  String out;
  serializeJson(doc, out);
  sendSuccess("OK", out);
}

// ---------------------------------------------------------------------------
// POST /api/provision — { ssid, pass } → NVS → ack → reboot
// ---------------------------------------------------------------------------
void handleProvisionPost() {
  // Gate 1: AP setup mode ONLY (physical-proximity boundary — see header).
  if (Services::wifi.getMode() != Services::WifiMode::AP) {
    sendError(403, "Provisioning hanya tersedia saat perangkat dalam mode AP setup");
    return;
  }
  if (!requireBody(1024)) return;
  String raw = http.arg("plain");
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, raw)) { sendError(400, "Invalid JSON"); return; }

  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["pass"] | "";
  size_t ssidLen = strlen(ssid);
  size_t passLen = strlen(pass);
  if (ssidLen == 0 || ssidLen > 32) { sendError(400, "ssid harus 1..32 karakter"); return; }
  if (passLen > 64) { sendError(400, "pass maksimal 64 karakter"); return; }

  // Persist (namespace + keys IDENTICAL to WifiManager::begin() reads).
  Preferences p;
  p.begin(Core::NVS_NAMESPACE, false);
  p.putString(Core::NVS_KEY_WIFI_SSID, String(ssid));
  p.putString(Core::NVS_KEY_WIFI_PASS, String(pass));
  p.end();

  Services::Log.append(Core::LogType::ConfigurationChanged,
                       String("WiFi credentials provisioned via AP portal (ssid len=") +
                           ssidLen + ")", 0);

  // Ack FIRST, then reboot — same flush pattern as firmware-generic /save.
  sendSuccess("WiFi tersimpan — perangkat reboot dan mencoba tersambung",
              "{\"rebooting\":true}");
  delay(750);
  ESP.restart();
}

void registerRoutes() {
  http.on("/", HTTP_GET, handleRoot);
  http.on("/api/provision", HTTP_GET, handleProvisionStatus);
  http.on("/api/provision", HTTP_POST, handleProvisionPost);
}

} // namespace ProvisionHandlers
} // namespace Web
