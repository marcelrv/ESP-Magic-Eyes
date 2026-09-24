// ota_routes.h — /api/ota/* route registration (Phase 2 scope).
//
// Kept separate from rest_routes.* (which owns /api/system/*) because the
// multipart upload handlers + deferred-restart state machine are a
// distinct, slightly larger concern — matches Phase 1's split of
// web_server.* (raw server bootstrap) vs rest_routes.* (route logic).
//
// Routes registered:
//   POST /api/ota/firmware   — multipart upload, Update.h U_FLASH
//   POST /api/ota/filesystem — multipart upload, Update.h U_SPIFFS (this
//                               is the correct Update.h mode constant for
//                               a LittleFS-backed OTA partition too, not a
//                               typo — see architecture plan §8)
//   GET  /api/ota/status     — progress/result of the last OTA attempt

#pragma once

class AsyncWebServer;

namespace OtaRoutes {

// Registers the routes above on the given server. Does not call
// server.begin() — that's WebServer::begin()'s job.
void registerRoutes(AsyncWebServer &server);

// Must be polled every loop() iteration (non-blocking). Performs the
// deferred ESP.restart() after a successful firmware/filesystem update,
// once enough time has passed for the HTTP response to actually flush to
// the client.
void handle();

} // namespace OtaRoutes
