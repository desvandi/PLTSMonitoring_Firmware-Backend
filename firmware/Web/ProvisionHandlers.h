// =============================================================================
// Web/ProvisionHandlers.h — First-boot WiFi provisioning (AP setup portal)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_PROVISION_HANDLERS_H
#define PLTS_WEB_PROVISION_HANDLERS_H

namespace Web {
namespace ProvisionHandlers {

// GET  /               — captive provisioning page (AP mode ONLY; STA → 404)
// GET  /api/provision  — status JSON (mode, deviceId — never secrets)
// POST /api/provision  — { ssid, pass } → NVS → ack → reboot into STA
void registerRoutes();

} // namespace ProvisionHandlers
} // namespace Web

#endif // PLTS_WEB_PROVISION_HANDLERS_H
