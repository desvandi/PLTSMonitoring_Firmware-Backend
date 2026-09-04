// =============================================================================
// Web/RelayHandlers.h — REST endpoints for 8-channel relay control
// =============================================================================
#pragma once
#ifndef PLTS_WEB_RELAY_HANDLERS_H
#define PLTS_WEB_RELAY_HANDLERS_H

#include "../Core/Config.h"   // [CI fix] MUST be before #if PLTS_ENABLE_RELAYS
#if PLTS_ENABLE_RELAYS
namespace Web {
namespace RelayHandlers {
  void registerRoutes();
}}
#endif // PLTS_ENABLE_RELAYS

#endif // PLTS_WEB_RELAY_HANDLERS_H
