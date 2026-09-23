// eye_pose.h — core motion-engine value types (architecture plan §2, §10
// Phase 3).
//
// EyePose is a fully-specified snapshot of every interpolated axis.
// EyeCommand is a *partial* target: only the axes it actually sets are
// re-targeted by MotionTask, everything else keeps doing whatever it was
// already doing — this is the "interruptibility" primitive the whole
// concurrency model (plan §2) is built on.

#pragma once

#include <cstdint>
#include <optional>

// A fully-specified pose snapshot, in "logical" units:
//   - panDeg/tiltDeg: degrees, centered at 0 (shared-yoke gaze, plan §1)
//   - lid*: normalized 0.0 (closed) .. 1.0 (open)
// This is what MotionTask publishes via getCurrentPose() and what
// GET /api/system/status (Phase 3) / GET /api/eyes/pose (Phase 4) report.
struct EyePose {
  float panDeg = 0.0f;
  float tiltDeg = 0.0f;
  float lidUpperL = 1.0f;
  float lidLowerL = 1.0f;
  float lidUpperR = 1.0f;
  float lidLowerR = 1.0f;
};

// Interpolation curve applied over an axis's [start, target] transition.
// EaseInOut is implemented as a smoothstep (3t^2 - 2t^3) blend — see
// eye_pose.cpp.
enum class Easing : uint8_t {
  Linear,
  EaseInOut,
};

// Returns the eased progress (0..1) for a linear progress t (0..1).
// Values of t outside [0, 1] are clamped.
float applyEasing(Easing easing, float t);

// What produced an EyeCommand. Only Manual (generic API-driven target —
// Phase 4's /api/eyes/* will be the first real producer) and Calibration
// are meaningful in Phase 3 (and Calibration's own test path actually
// bypasses EyeCommand/CommandQueue entirely — see servo_routes.cpp — this
// value is reserved for symmetry / future use, e.g. a calibration-mode
// UI that *does* want eased motion). Gesture/PlayMode/Natural are
// reserved now so Phase 4 (GestureEngine, PlayModeManager,
// NaturalModeCoupler) doesn't need to touch this enum.
enum class CommandSource : uint8_t {
  Manual,
  Calibration,
  Gesture,
  PlayMode,
  Natural,
};

// A partial motion target: only fields with a value are applied. Pushed
// onto CommandQueue by API handlers (HTTP/AsyncTCP task context) and, in
// later phases, by GestureEngine/PlayModeManager/NaturalModeCoupler.
struct EyeCommand {
  std::optional<float> panDeg;
  std::optional<float> tiltDeg;
  std::optional<float> lidUpperL;
  std::optional<float> lidLowerL;
  std::optional<float> lidUpperR;
  std::optional<float> lidLowerR;

  uint32_t durationMs = 200;
  Easing easing = Easing::EaseInOut;
  CommandSource source = CommandSource::Manual;
};
