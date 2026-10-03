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
//   POST /api/eyes/natural  — {enabled}, live natural-mode toggle
//                              (NaturalModeCoupler::setEnabled())
//   GET  /api/eyes/pose     — MotionTask::getCurrentPose() (six values)
//                              plus `held` and `seg`, the per-axis
//                              interpolation segments from
//                              MotionTask::getPoseSegments()

#pragma once

class AsyncWebServer;

namespace EyesRoutes {

void registerRoutes(AsyncWebServer &server);

} // namespace EyesRoutes
