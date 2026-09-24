// servo_routes.h — /api/servos/* route registration (Phase 3 scope,
// architecture plan §4). Split out from rest_routes.* for the same reason
// Phase 2 split out ota_routes.*: validated multi-field POST bodies +
// direct hardware interaction (ServoHal) are a distinct, sizeable enough
// concern to warrant their own module.
//
// Routes registered:
//   GET  /api/servos/config — calibration table for all 7 servos
//   POST /api/servos/config — save + live-reapply one servo's calibration
//   POST /api/servos/test   — raw, uneased single-servo pulse write for
//                              calibration tooling; bypasses
//                              CommandQueue/MotionTask and (re)starts the
//                              calibration hold so MotionTask can't
//                              overwrite it; not clamped to the saved
//                              min/max
//   GET/POST /api/servos/hold — query / start / release the calibration
//                              hold ({"enabled": bool})
//   POST /api/servos/pose   — several raw pulse writes at once
//                              ({"pulses": [{servoId, pulseUs}, ...]}),
//                              for reference poses like "straight ahead +
//                              lids just touching"
//
// NOT in scope here: /api/eyes/* (gaze/eyelid commands) — that's Phase 4,
// per the plan's phase breakdown. This phase deliberately exposes no
// public way to command eye movement via HTTP; only raw calibration.

#pragma once

class AsyncWebServer;

namespace ServoRoutes {

// Registers the routes above. Does not call server.begin() — that's
// WebServer::begin()'s job.
void registerRoutes(AsyncWebServer &server);

} // namespace ServoRoutes
