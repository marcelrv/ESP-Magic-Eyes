// rest_routes.h — /api/system/* route registration.
//
// /api/ota/* lives in api/ota_routes.* (Phase 2) and /api/servos/* lives
// in api/servo_routes.* (Phase 3) instead of here — see those headers for
// why. /api/wifi/*, /api/eyes/*, /api/gestures/*, /api/playmodes/*,
// /api/radar/*, /api/led/* were added by later phases in their own route
// files, per architecture plan §4.
//
// PLAN-GAP FIX (Phase 8): plan §4 also lists POST /api/system/reboot, and
// data/www/js/api.js has exposed an `Api.reboot()` wrapper for it since
// Phase 7 — but no phase 1-7 ever actually registered the route (only
// /api/system/info, /api/system/status, /api/system/config existed). Same
// class of gap Phase 7 already found and fixed for /api/wifi/*; closed here
// the same way. See PROGRESS.md Phase 8 notes.
//   POST /api/system/reboot — responds immediately, then reboots after a
//                             short deferred delay so AsyncTCP has time to
//                             flush the HTTP response first — mirrors
//                             ota_routes.cpp's/wifi_routes.cpp's existing
//                             deferred-restart pattern.

#pragma once

class AsyncWebServer;

namespace RestRoutes {

// Registers all /api/system/* routes on the given server. Does not call
// server.begin() — that's WebServer::begin()'s job.
void registerRoutes(AsyncWebServer &server);

// Must be polled every loop() iteration (non-blocking). Performs the
// deferred ESP.restart() after POST /api/system/reboot, once enough time
// has passed for the HTTP response to actually flush to the client.
void handle();

} // namespace RestRoutes
