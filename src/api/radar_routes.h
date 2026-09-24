// radar_routes.h — /api/radar/* route registration (architecture plan §4,
// §10 Phase 5).
//
// Routes registered:
//   GET /api/radar/status — {sensorModel, linkOk, lastUpdateMs}
//   GET /api/radar/latest — the current RadarState as JSON (presence +
//                            target array; every target object always has
//                            all five keys present — a field a given sensor
//                            can't provide serializes as JSON `null`, not
//                            an omitted key or a magic-number placeholder,
//                            so callers can rely on the key always existing
//                            — see radar_routes.cpp's handleLatest())
//   GET  /api/radar/config — {baudRate} (LD2420 UART baud, NVS-backed —
//                            see storage/nvs_store.h's getRadarBaudRate()
//                            doc comment for why this became a runtime
//                            setting instead of a compile-time flag)
//   POST /api/radar/config — {baudRate} saves + deferred-reboots (RadarTask
//                            only opens the UART once at boot, so a new
//                            baud needs a fresh begin() to take effect)
//
// No WebSocket endpoint exists anywhere in this codebase yet (Phases 1-4
// didn't add one) — per the task spec, REST polling is sufficient for this
// phase; a `/ws` `radar` topic (plan §4) is deferred to whenever Phase 7's
// frontend actually needs a live-updating visualization page. See
// PROGRESS.md Phase 5 notes.

#pragma once

class AsyncWebServer;

namespace RadarRoutes {

void registerRoutes(AsyncWebServer &server);

// Fires the deferred restart scheduled by POST /api/radar/config, once its
// short delay has elapsed — mirrors ota_routes.cpp's/rest_routes.cpp's
// existing deferred-restart pattern (respond first, let AsyncTCP flush,
// then ESP.restart()). Poll from loop().
void handle();

}  // namespace RadarRoutes
