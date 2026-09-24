// playmode_routes.h — /api/playmodes/* route registration (Phase 4 scope,
// architecture plan §4).
//
// Routes registered:
//   GET  /api/playmodes            — list of {id, label, description}
//   POST /api/playmodes/{id}/activate — switches the active mode
//   GET  /api/playmodes/active     — current mode id

#pragma once

class AsyncWebServer;

namespace PlaymodeRoutes {

void registerRoutes(AsyncWebServer &server);

} // namespace PlaymodeRoutes
