// natural_mode.h — NaturalModeCoupler: derives eyelid-openness targets from
// the current gaze (pan/tilt), architecture plan §5, §10 Phase 4.
//
// Deliberately NOT a FreeRTOS task and does NOT push through CommandQueue —
// per plan §2, this is a plain function called directly from inside
// MotionTask's own tick. MotionTask itself retargets the lid AxisStates
// using the values this module computes, and only for axes that are
// currently "free" (not owned by an in-flight Manual/Gesture/PlayMode
// interpolation — see motion_task.cpp's maybeApplyNatural()). Keeping this
// module a pure function of (panDeg, tiltDeg) with no internal mutable
// bias state means motion_task.cpp can compare its output directly against
// each AxisState's own targetValue to decide both "is this a big enough
// change to be worth a retarget" (the >0.02 jitter guard from the task
// spec) and "is this axis free" — no separate bookkeeping needed here.

#pragma once

namespace NaturalModeCoupler {

// Loads the `naturalMode` NVS flag (storage/nvs_store.h, Phase 1 key) into
// a RAM cache — same pattern as ServoHal's calibration cache (see
// hal/servo_hal.h) — because MotionTask checks isEnabled() every ~20ms
// tick, too hot a path to hit NVS/flash. Call once, from
// MotionTask::begin().
void begin();

bool isEnabled();

// Updates both the RAM cache and NVS. Called by POST /api/system/config's
// {"naturalMode": bool} handling (api/rest_routes.cpp).
void setEnabled(bool enabled);

// PlayModeManager calls this to temporarily suppress lid coupling while a
// play mode owns the lids persistently. Currently only "sleep" mode uses
// this — its eased-nearly-closed lids would otherwise get pulled straight
// back toward the open baseline the moment the lid axes go "free" again
// (durationMs elapses), which would fight sleep mode's entire point within
// a quarter of a second of entering it. RAM-only, not persisted — always
// false after boot/at natural-mode toggle.
void setSuppressed(bool suppressed);
bool isSuppressed();

// Absolute target lid-openness values (0..1, already clamped) derived from
// the current interpolated pan/tilt. See eye_pose.h for the value scale.
struct LidTargets {
  float upperL;
  float lowerL;
  float upperR;
  float lowerR;
};

// Coupling formula (plan §5): baseline resting openness ~=0.85 (not fully
// open, for a more organic look) plus `tiltNormalized * coupling`, with
// separate (larger) coefficient for the upper lid than the lower lid so
// looking down drops the upper lid noticeably more than the lower one —
// mimicking real eyelid occlusion — rather than both lids moving by an
// identical amount. L/R get identical targets: gaze is shared-yoke (plan
// §1, one pan + one tilt servo for both eyes), so there's no per-eye
// difference derivable from pan/tilt alone.
//
// TODO(stretch, plan §5): the plan also mentions an optional smaller
// pan-based asymmetric term ("slight asymmetric lid tightening on the
// trailing side during a fast saccade"). Not implemented this phase —
// explicitly called out as optional/stretch in the task spec, and it would
// need saccade-velocity tracking this module doesn't otherwise need. See
// PROGRESS.md Phase 4 notes.
LidTargets computeTargets(float panDeg, float tiltDeg);

} // namespace NaturalModeCoupler
