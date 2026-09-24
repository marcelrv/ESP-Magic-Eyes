#include "motion/gesture_engine.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

#include "motion/command_queue.h"
#include "motion/motion_task.h"

namespace {

// --- Gesture keyframe tables --------------------------------------------
// Hand-authored C++ data (plan §5: gestures are fixed/small, so a data
// table beats the JSON-on-LittleFS approach used for play-mode *sequences*
// — see playmode_manager.cpp's greeting.json loader for that path). Each
// keyframe is just an EyeCommand: an unset (std::nullopt) field means
// "leave that axis alone", so most gestures below never touch
// panDeg/tiltDeg and the eyes keep doing whatever they were already doing
// (a play mode's drift, natural-mode coupling, ...) while only the lids
// animate.
//
// Baseline resting lid openness used to "reopen" after a gesture is 0.85,
// matching NaturalModeCoupler's own baseline (natural_mode.cpp) — so a
// gesture leaves the lids at the same resting look natural mode would,
// whether or not natural mode is actually on. NOTE: if natural mode *is*
// on, the lid axes go "free" again the instant a gesture's last keyframe
// finishes, and NaturalModeCoupler may then nudge them further based on
// current gaze tilt — expected/desired interaction, not a bug (this is
// exactly the "coupling resumes smoothly from wherever the lid ended up"
// behavior plan §5 asks for).
using KF = EyeCommand;

EyeCommand lidsCmd(float upperL, float lowerL, float upperR, float lowerR, uint32_t durationMs,
                    Easing easing = Easing::EaseInOut) {
  EyeCommand c;
  c.lidUpperL = upperL;
  c.lidLowerL = lowerL;
  c.lidUpperR = upperR;
  c.lidLowerR = lowerR;
  c.durationMs = durationMs;
  c.easing = easing;
  return c;
}

EyeCommand gazeCmd(float pan, float tilt, uint32_t durationMs, Easing easing = Easing::EaseInOut) {
  EyeCommand c;
  c.panDeg = pan;
  c.tiltDeg = tilt;
  c.durationMs = durationMs;
  c.easing = easing;
  return c;
}

EyeCommand gazeAndLidsCmd(float pan, float tilt, float upperL, float lowerL, float upperR, float lowerR,
                           uint32_t durationMs, Easing easing = Easing::EaseInOut) {
  EyeCommand c = lidsCmd(upperL, lowerL, upperR, lowerR, durationMs, easing);
  c.panDeg = pan;
  c.tiltDeg = tilt;
  return c;
}

constexpr float kBaseline = 0.85f;
constexpr float kFullOpen = 1.0f;
// 0.0 is the calibrated "lids just touching" point (ServoCalibration::closedUs).
constexpr float kClosed = 0.0f;

// Timing notes for every table below:
//   - Durations are multiples of MotionTask's 20ms tick so keyframes chain
//     without gaps (GestureEngine schedules the next one at now+duration).
//   - Lid moves are kept >= ~100ms: an SG90 needs roughly that long to
//     cover a lid's travel, so anything shorter never reaches its target.
//   - A keyframe that re-targets axes to where they already are is a pure
//     *hold*: nothing moves, but the axes stay owned by the gesture, so
//     NaturalModeCoupler can't pull the lids back to its baseline until
//     the hold ends. Every "held" look ends with an explicit relax
//     keyframe so it behaves the same with natural mode on or off.

// blink: close, touch briefly, reopen a little slower than it closed.
const KF kBlinkFrames[] = {
  lidsCmd(kClosed, kClosed, kClosed, kClosed, 100),
  lidsCmd(kClosed, kClosed, kClosed, kClosed, 40), // hold
  lidsCmd(kBaseline, kBaseline, kBaseline, kBaseline, 160),
};

// wink_left / wink_right: the winking eye closes and holds for a beat;
// the other eye narrows slightly (lower lid rises, like a cheek pushing
// up) instead of staying frozen, then both return to baseline.
EyeCommand winkCmd(bool leftEye, float winkLid, float otherUpper, float otherLower, uint32_t durationMs) {
  EyeCommand c;
  c.durationMs = durationMs;
  c.easing = Easing::EaseInOut;
  if (leftEye) {
    c.lidUpperL = winkLid;
    c.lidLowerL = winkLid;
    c.lidUpperR = otherUpper;
    c.lidLowerR = otherLower;
  } else {
    c.lidUpperR = winkLid;
    c.lidLowerR = winkLid;
    c.lidUpperL = otherUpper;
    c.lidLowerL = otherLower;
  }
  return c;
}

constexpr float kWinkOtherUpper = 0.75f;
constexpr float kWinkOtherLower = 0.60f;

const KF kWinkLeftFrames[] = {
  winkCmd(true, kClosed, kWinkOtherUpper, kWinkOtherLower, 160),
  winkCmd(true, kClosed, kWinkOtherUpper, kWinkOtherLower, 300), // hold
  winkCmd(true, kBaseline, kBaseline, kBaseline, 240),
};
const KF kWinkRightFrames[] = {
  winkCmd(false, kClosed, kWinkOtherUpper, kWinkOtherLower, 160),
  winkCmd(false, kClosed, kWinkOtherUpper, kWinkOtherLower, 300), // hold
  winkCmd(false, kBaseline, kBaseline, kBaseline, 240),
};

// surprise: lids snap wide with a small upward/forward gaze reset (plan
// §5), stay wide long enough to register, then settle back.
const KF kSurpriseFrames[] = {
  gazeAndLidsCmd(0.0f, 4.0f, kFullOpen, kFullOpen, kFullOpen, kFullOpen, 100, Easing::Linear),
  gazeAndLidsCmd(0.0f, 4.0f, kFullOpen, kFullOpen, kFullOpen, kFullOpen, 700), // hold
  gazeAndLidsCmd(0.0f, 0.0f, kBaseline, kBaseline, kBaseline, kBaseline, 400),
};

// sleepy: upper lids droop slowly (lower lids barely move, as in a real
// drowsy look), linger, droop a bit further as if nodding off, then open
// slowly back to baseline.
const KF kSleepyFrames[] = {
  lidsCmd(0.30f, 0.75f, 0.30f, 0.75f, 800),
  lidsCmd(0.30f, 0.75f, 0.30f, 0.75f, 1200), // hold
  lidsCmd(0.15f, 0.70f, 0.15f, 0.70f, 700),
  lidsCmd(0.15f, 0.70f, 0.15f, 0.70f, 600), // hold
  lidsCmd(kBaseline, kBaseline, kBaseline, kBaseline, 800),
};

// squint (alias: suspicious): lids narrow — lower lids rise more than the
// upper lids drop — with a slight downward tilt, held, then relax.
const KF kSquintFrames[] = {
  gazeAndLidsCmd(0.0f, -5.0f, 0.45f, 0.35f, 0.45f, 0.35f, 300),
  gazeAndLidsCmd(0.0f, -5.0f, 0.45f, 0.35f, 0.45f, 0.35f, 1500), // hold
  gazeAndLidsCmd(0.0f, 0.0f, kBaseline, kBaseline, kBaseline, kBaseline, 400),
};

// look_around_quick: glance to one side, linger, glance to the other,
// linger, back to center. A keyframe re-targeting the axes to where they
// already are is a pure hold (no motion for its durationMs). Durations are
// multiples of MotionTask's 20ms tick so keyframes chain without gaps.
const KF kLookAroundQuickFrames[] = {
  gazeCmd(25.0f, -6.0f, 360),
  gazeCmd(25.0f, -6.0f, 400), // hold
  gazeCmd(-22.0f, 6.0f, 500),
  gazeCmd(-22.0f, 6.0f, 400), // hold
  gazeCmd(0.0f, 0.0f, 360),
};

// double_blink: close/open/close/open.
const KF kDoubleBlinkFrames[] = {
  lidsCmd(kClosed, kClosed, kClosed, kClosed, 100),
  lidsCmd(kBaseline, kBaseline, kBaseline, kBaseline, 120),
  lidsCmd(kClosed, kClosed, kClosed, kClosed, 100),
  lidsCmd(kBaseline, kBaseline, kBaseline, kBaseline, 160),
};

// roll_eyes: circular gaze sweep; lids deliberately untouched so they
// track the sweep via NaturalModeCoupler if natural mode is on (plan §5:
// "circular gaze sweep with lids tracking via natural-mode coupling").
// Ease out to the left edge, then one full circle (up over the top, right,
// down, back to left) as 16 short *linear* segments — constant speed with
// no stop at any point, which is what makes it read as rolling rather than
// a diamond of four eased moves — then ease back to center. Points are
// pan = 20*cos(a), tilt = 16*sin(a) for a = 180deg - k*22.5deg.
constexpr uint32_t kRollSegmentMs = 100; // multiple of the 20ms tick; ~1.6s per circle

const KF kRollEyesFrames[] = {
  gazeCmd(-20.0f, 0.0f, 300),
  gazeCmd(-18.5f, 6.1f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-14.1f, 11.3f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-7.7f, 14.8f, kRollSegmentMs, Easing::Linear),
  gazeCmd(0.0f, 16.0f, kRollSegmentMs, Easing::Linear),
  gazeCmd(7.7f, 14.8f, kRollSegmentMs, Easing::Linear),
  gazeCmd(14.1f, 11.3f, kRollSegmentMs, Easing::Linear),
  gazeCmd(18.5f, 6.1f, kRollSegmentMs, Easing::Linear),
  gazeCmd(20.0f, 0.0f, kRollSegmentMs, Easing::Linear),
  gazeCmd(18.5f, -6.1f, kRollSegmentMs, Easing::Linear),
  gazeCmd(14.1f, -11.3f, kRollSegmentMs, Easing::Linear),
  gazeCmd(7.7f, -14.8f, kRollSegmentMs, Easing::Linear),
  gazeCmd(0.0f, -16.0f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-7.7f, -14.8f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-14.1f, -11.3f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-18.5f, -6.1f, kRollSegmentMs, Easing::Linear),
  gazeCmd(-20.0f, 0.0f, kRollSegmentMs, Easing::Linear),
  gazeCmd(0.0f, 0.0f, 360),
};

struct GestureDef {
  const char *id;
  const char *label;
  const KF *keyframes;
  size_t count;
};

template <size_t N>
constexpr size_t arrayCount(const KF (&)[N]) {
  return N;
}

const GestureDef kGestures[] = {
  {"blink", "Blink", kBlinkFrames, arrayCount(kBlinkFrames)},
  {"wink_left", "Wink (left)", kWinkLeftFrames, arrayCount(kWinkLeftFrames)},
  {"wink_right", "Wink (right)", kWinkRightFrames, arrayCount(kWinkRightFrames)},
  {"surprise", "Surprise", kSurpriseFrames, arrayCount(kSurpriseFrames)},
  {"sleepy", "Sleepy", kSleepyFrames, arrayCount(kSleepyFrames)},
  {"squint", "Squint / suspicious", kSquintFrames, arrayCount(kSquintFrames)},
  {"look_around_quick", "Look around", kLookAroundQuickFrames, arrayCount(kLookAroundQuickFrames)},
  {"double_blink", "Double blink", kDoubleBlinkFrames, arrayCount(kDoubleBlinkFrames)},
  {"roll_eyes", "Roll eyes", kRollEyesFrames, arrayCount(kRollEyesFrames)},
};
constexpr size_t kGestureCount = sizeof(kGestures) / sizeof(kGestures[0]);

const GestureDef *findGesture(const char *id) {
  // "suspicious" is an alias for the canonical "squint" id (plan §5 lists
  // it as "squint/suspicious" without picking one — squint chosen as
  // canonical since it's the shorter, more standard term).
  const char *lookupId = (strcmp(id, "suspicious") == 0) ? "squint" : id;
  for (size_t i = 0; i < kGestureCount; ++i) {
    if (strcmp(kGestures[i].id, lookupId) == 0) {
      return &kGestures[i];
    }
  }
  return nullptr;
}

// --- Playback state --------------------------------------------------------
const GestureDef *gActive = nullptr;
size_t gKeyframeIndex = 0;
uint32_t gNextDueMs = 0;
uint32_t gCapturedGeneration = 0;
CommandSource gActiveSource = CommandSource::Gesture;

// Integration-pass fix (Phase 8): tick() runs on MotionTask's own FreeRTOS
// task every ~20ms, but trigger() is also called directly from the
// AsyncTCP/HTTP task (gesture_routes.cpp's POST /api/gestures/{id}/trigger
// — the codebase's one place that calls into GestureEngine from outside
// MotionTask's own tick; PlayModeManager's idle/curious/tracking-triggered
// blink/surprise calls all happen from *inside* MotionTask's tick, so
// those were never actually racing tick()). Before this fix, gActive/
// gKeyframeIndex/gNextDueMs/gCapturedGeneration/gActiveSource were plain
// globals with no synchronization at all between those two tasks: an HTTP
// trigger() landing mid-tick() could overwrite gActive with a new (shorter)
// GestureDef between tick()'s two reads of it, so pushKeyframe() could
// index gActive->keyframes[gKeyframeIndex] with a stale index against the
// new array — an out-of-bounds read. A short-held mutex around each
// function's whole body (same "hold only for the critical section" idiom
// as motion_task.cpp's gPoseMutex / radar_task.cpp's gStateMutex) closes
// this without changing either function's external behavior.
SemaphoreHandle_t gMutex = nullptr;

void pushKeyframe(size_t index, uint32_t nowMs) {
  EyeCommand cmd = gActive->keyframes[index];
  cmd.source = gActiveSource;
  // Pinned (not stamped at push time) so pushAbortRestore()'s generation
  // match stays exact even if an HTTP bump lands mid-push.
  cmd.generation = gCapturedGeneration;
  CommandQueue::push(cmd);
  gNextDueMs = nowMs + cmd.durationMs;
}

// Called when playback is aborted mid-gesture (commandGeneration moved on).
// Without this, a blink aborted after its "close" keyframe would leave the
// lids shut. Pushes the gesture's final keyframe (always its relax/baseline
// frame, see the tables above), lids only, as a restore-only command
// pinned to the aborted playback's generation: MotionTask applies it only
// to lids nothing newer has claimed — a manual gaze command lets the lids
// reopen, while a manual eyelid command or a sleep-mode close keeps them.
void pushAbortRestore() {
  if (gKeyframeIndex + 1 >= gActive->count) {
    return; // final keyframe already pushed
  }
  EyeCommand cmd = gActive->keyframes[gActive->count - 1];
  if (!cmd.lidUpperL && !cmd.lidLowerL && !cmd.lidUpperR && !cmd.lidLowerR) {
    return; // gaze-only gesture, nothing to reopen
  }
  cmd.panDeg.reset();
  cmd.tiltDeg.reset();
  cmd.source = gActiveSource;
  cmd.generation = gCapturedGeneration;
  cmd.restoreOnly = true;
  CommandQueue::push(cmd);
}

} // namespace

namespace GestureEngine {

void begin() {
  gActive = nullptr;
  if (gMutex == nullptr) {
    gMutex = xSemaphoreCreateMutex();
  }
}

void tick(uint32_t nowMs) {
  xSemaphoreTake(gMutex, portMAX_DELAY);

  if (gActive == nullptr) {
    xSemaphoreGive(gMutex);
    return;
  }
  // Something else (a Manual command, a new gesture trigger, or a
  // play-mode switch) has taken over since this playback started — stop
  // enqueueing the remaining keyframes rather than fighting for control
  // (plan §2's commandGeneration mechanism). Checked every tick, before the
  // keyframe deadline: checking only at the deadline left the lids where
  // the gesture had them for up to the rest of the current keyframe (e.g.
  // sleepy's 1.2s hold) before the abort-restore reopened them.
  if (MotionTask::getCommandGeneration() != gCapturedGeneration) {
    pushAbortRestore();
    gActive = nullptr;
    xSemaphoreGive(gMutex);
    return;
  }
  if (!deadlineReached(nowMs, gNextDueMs)) {
    xSemaphoreGive(gMutex);
    return;
  }

  size_t nextIndex = gKeyframeIndex + 1;
  if (nextIndex >= gActive->count) {
    gActive = nullptr; // playback complete
    xSemaphoreGive(gMutex);
    return;
  }
  gKeyframeIndex = nextIndex;
  pushKeyframe(gKeyframeIndex, nowMs);
  xSemaphoreGive(gMutex);
}

bool trigger(const char *id, CommandSource source) {
  const GestureDef *def = findGesture(id);
  if (def == nullptr || def->count == 0) {
    return false;
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  if (gActive != nullptr) {
    pushAbortRestore(); // the gesture being replaced may have left lids closed
  }
  gActive = def;
  gActiveSource = source;
  gKeyframeIndex = 0;
  gCapturedGeneration = MotionTask::getCommandGeneration();
  pushKeyframe(0, millis());
  xSemaphoreGive(gMutex);
  return true;
}

bool isPlaying() { return gActive != nullptr; }

size_t listGestures(GestureInfo *outArray, size_t maxCount) {
  size_t n = kGestureCount < maxCount ? kGestureCount : maxCount;
  for (size_t i = 0; i < n; ++i) {
    outArray[i].id = kGestures[i].id;
    outArray[i].label = kGestures[i].label;
  }
  return n;
}

} // namespace GestureEngine
