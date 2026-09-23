// gesture_routes.h — /api/gestures/* route registration (Phase 4 scope,
// architecture plan §4).
//
// Routes registered:
//   GET  /api/gestures             — list of {id, label}
//   POST /api/gestures/{id}/trigger — bumps commandGeneration, starts that
//                                      gesture's keyframe playback
//                                      (source=Gesture)

#pragma once

class AsyncWebServer;

namespace GestureRoutes {

void registerRoutes(AsyncWebServer &server);

} // namespace GestureRoutes
