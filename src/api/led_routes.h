// led_routes.h — /api/led/* route registration (architecture plan §4,
// §10 Phase 6). Split into its own file following the established
// one-file-per-feature-area convention (servo_routes.*, radar_routes.*,
// etc).
//
// Routes registered:
//   GET  /api/led/config — {enabled, brightness, colorL:{r,g,b},
//                            colorR:{r,g,b}, effect}
//   POST /api/led/config — partial or full update of the same shape;
//                           validates ranges (brightness/RGB channels
//                           0-255, effect one of "off"/"solid"/"breathe"),
//                           applies via LedController::setConfig(),
//                           responds with the resulting full config.
//
// Lightweight cosmetic "mood glow" feature, safe no-op if the LED isn't
// enabled/wired — see hal/led_controller.h.

#pragma once

class AsyncWebServer;

namespace LedRoutes {

// Registers the routes above. Does not call server.begin() — that's
// WebServer::begin()'s job.
void registerRoutes(AsyncWebServer &server);

} // namespace LedRoutes
