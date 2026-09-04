// =============================================================================
// Web/RelayHandlers.h — REST endpoints for 8-channel relay control
// =============================================================================
#pragma once
#ifndef PLTS_WEB_RELAY_HANDLERS_H
#define PLTS_WEB_RELAY_HANDLERS_H

#if PLTS_ENABLE_RELAYS
namespace Web {
namespace RelayHandlers {
  void registerRoutes();
}}
#endif

#endif
