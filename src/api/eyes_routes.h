// eyes_routes.h — /api/eyes/* route registration (Phase 4 scope,
// architecture plan §4).
//
// Routes registered:
//   POST /api/eyes/gaze     — {pan, tilt, durationMs?, easing?}, degrees,
//                              source=Manual, bumps commandGeneration
//   POST /api/eyes/eyelids  — {upperL?, lowerL?, upperR?, lowerR?,
//                              durationMs?}, normalized 0..1, partial
//                              updates allowed, source=Manual, bumps
//                              commandGeneration
//   GET  /api/eyes/pose     — thin wrapper around
//                              MotionTask::getCurrentPose()

#pragma once

class AsyncWebServer;

namespace EyesRoutes {

void registerRoutes(AsyncWebServer &server);

} // namespace EyesRoutes
