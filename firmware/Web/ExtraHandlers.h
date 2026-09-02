// =============================================================================
// Web/ExtraHandlers.h — PWA-contract route parity (FW-20)
// =============================================================================
#pragma once
#ifndef PLTS_WEB_EXTRA_HANDLERS_H
#define PLTS_WEB_EXTRA_HANDLERS_H

namespace Web {
namespace ExtraHandlers {

void registerRoutes();
void noteBootEvent();                       // seed /api/ota/history with the boot record
void noteOtaEvent(const char* version, const char* event);
void handleAlarmAckGeneric();               // POST /api/alarms/acknowledge {code}
void handleRs485Frames();                   // GET /api/rs485/frames (bench capture)

} // namespace ExtraHandlers
} // namespace Web

#endif // PLTS_WEB_EXTRA_HANDLERS_H
