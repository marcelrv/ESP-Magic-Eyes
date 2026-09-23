// motion_task.h — the real-time motion core (architecture plan §2, §10
// Phase 3): a dedicated FreeRTOS task that owns per-axis eased
// interpolation and drives ServoHal, plus a thread-safe pose snapshot for
// HTTP handlers to read.

#pragma once

#include "motion/eye_pose.h"

namespace MotionTask {

// Creates CommandQueue, the pose mutex, seeds axis state to the EyePose
// defaults (pan/tilt=0deg, lids=1.0 open), and starts the task. Call once
// from setup(), after ServoHal::begin() (this task writes to ServoHal on
// its very first tick).
void begin();

// Thread-safe snapshot of the current pose. Safe to call from any task,
// including the AsyncTCP task that runs HTTP handlers — internally takes
// a short-held mutex around a plain struct copy so callers never observe
// a torn/partially-updated EyePose.
EyePose getCurrentPose();

// Monotonically increasing counter (Phase 4, deferred from Phase 3 — see
// plan §2), bumped whenever a new EXTERNALLY-sourced command sequence
// begins: a Manual /api/eyes/* call, an API-triggered gesture (POST
// /api/gestures/{id}/trigger), or a play-mode switch (POST
// /api/playmodes/{id}/activate). GestureEngine's and PlayModeManager's own
// multi-keyframe playback coroutines capture this value when a sequence
// starts and compare it before enqueueing each subsequent keyframe; a
// mismatch means something else has taken over since, so the coroutine
// aborts its remaining keyframes instead of continuing to fight for
// control. Backed by std::atomic (relaxed ordering is enough — this is a
// simple "has anything changed" token, not used to order/synchronize any
// other memory access) since it's written from the AsyncTCP/HTTP task
// context (bumpCommandGeneration) and read from the MotionTask task
// context (getCommandGeneration, via GestureEngine/PlayModeManager).
//
// Autonomous, routine actions a play mode takes on its own (idle's
// periodic drift/blink, curious's saccades) deliberately do NOT bump this
// counter themselves — only a genuinely new EXTERNAL intent should, or the
// counter would increment continuously and lose its meaning as "something
// new took over".
uint32_t bumpCommandGeneration();
uint32_t getCommandGeneration();

// Calibration hold: while held, the motion task writes no servo pulses,
// ticks no gestures/play modes and discards queued commands, so raw
// calibration pulses (ServoHal::setRawPulseUs()) stay where the user put
// them. The hold expires on its own `durationMs` after the last call —
// the calibration page refreshes it on every slider move and on a
// keepalive timer, so a closed browser tab can never freeze the eyes for
// good. When it ends (expiry or release), in-flight sequences are aborted
// and every axis eases back to the rest pose (straight ahead, lids 0.85).
// Safe to call from any task.
void setCalibrationHold(uint32_t durationMs);
void releaseCalibrationHold();
// 0 when not held.
uint32_t calibrationHoldRemainingMs();

} // namespace MotionTask
