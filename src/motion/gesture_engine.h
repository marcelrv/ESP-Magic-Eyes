// gesture_engine.h — keyframe-sequence gesture player, architecture plan
// §5, §10 Phase 4.
//
// A gesture is a short (2-4 keyframe, ~150-400ms total per plan §5)
// hand-authored table of partial EyeCommand-shaped keyframes. Playback is
// a small state machine ticked from inside MotionTask's own tick (see
// motion_task.cpp) — NOT a separate FreeRTOS task, per plan §2's "since
// they only ever produce EyeCommands" reasoning. Each due keyframe is
// pushed onto CommandQueue exactly like an API handler would (chosen over
// directly poking MotionTask's internal AxisState — see motion_task.cpp's
// AxisState comment — so there's a single application path, applyCommand,
// for every command source; NaturalModeCoupler is the one exception that
// bypasses the queue, per its own header's explanation of why).

#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#include "motion/eye_pose.h"

namespace GestureEngine {

// One row of the public listing (GET /api/gestures).
struct GestureInfo {
  const char *id;
  const char *label;
};

// Resets playback state. Call once from MotionTask::begin().
void begin();

// Advances any in-flight gesture playback — pushes the next due keyframe
// onto CommandQueue when its predecessor's duration has elapsed. Called
// once per MotionTask tick; motion_task.cpp drains CommandQueue again
// right after calling this so a same-tick push still lands this tick
// instead of waiting a full extra ~20ms cycle.
void tick(uint32_t nowMs);

// Starts gesture `id` (canonical ids below; "suspicious" is accepted as an
// alias for "squint" per plan §5's "squint/suspicious" naming — see
// gesture_engine.cpp). GestureEngine only ever plays one gesture at a
// time, so calling trigger() while another gesture is mid-playback simply
// overwrites the active-playback state — a self-interruption, the same
// spirit as MotionTask's own per-axis retarget().
//
// `source` distinguishes an externally-triggered gesture (API call,
// CommandSource::Gesture — gesture_routes.cpp bumps MotionTask's
// commandGeneration counter *before* calling this, so the captured
// baseline below is already the new value) from an autonomously-triggered
// one (PlayModeManager's idle/curious periodic blinks,
// CommandSource::PlayMode — NOT generation-bumped by the caller, so an
// in-flight autonomous gesture cleanly aborts its remaining keyframes if
// something external takes over mid-playback; see motion_task.h's
// commandGeneration doc comment for the full mechanism).
//
// Returns false if `id` isn't a known gesture (no state change made).
bool trigger(const char *id, CommandSource source = CommandSource::Gesture);

// True while a gesture's keyframes are still being played back.
bool isPlaying();

// Fills `outArray` (capacity `maxCount`) with the fixed gesture listing for
// GET /api/gestures. Returns the number of entries written.
size_t listGestures(GestureInfo *outArray, size_t maxCount);

} // namespace GestureEngine
