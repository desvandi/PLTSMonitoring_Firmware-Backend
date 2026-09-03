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
// [audit-2 R-2] handleAlarmAckGeneric removed — non-canonical route eliminated (P1-3)
void handleRs485Frames();                   // GET /api/rs485/frames (bench capture)

} // namespace ExtraHandlers
} // namespace Web

#endif // PLTS_WEB_EXTRA_HANDLERS_H
