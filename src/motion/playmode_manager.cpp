#include "motion/playmode_manager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>

#include "motion/command_queue.h"
#include "motion/eye_pose.h"
#include "motion/gesture_engine.h"
#include "motion/motion_task.h"
#include "motion/natural_mode.h"
#include "radar/iradar_sensor.h"
#include "radar/radar_task.h"

namespace {

enum class Mode : uint8_t { Manual, Idle, Curious, Sleep, Greeting, Tracking };

struct ModeDef {
  Mode mode;
  const char *id;
  const char *label;
  const char *description;
};

// Order here is also GET /api/playmodes' listing order.
const ModeDef kModes[] = {
  {Mode::Idle, "idle", "Idle", "Low-key resting micro-motion: gentle drift plus occasional natural blinks."},
  {Mode::Curious, "curious", "Curious", "Larger randomized saccades and more frequent blinks, as if exploring the room."},
  {Mode::Sleep, "sleep", "Sleep", "Eyelids ease closed (calibrated just-touching point) and gaze motion stops."},
  {Mode::Greeting, "greeting", "Greeting", "One-shot scripted wake-up/greeting sequence, then settles into idle."},
  {Mode::Tracking, "tracking", "Tracking",
   "Follows detected radar targets. LD2450 builds get real directional gaze-following; LD2420 builds "
   "(distance/presence only, no angle) degrade to a presence-triggered alert glance."},
  {Mode::Manual, "manual", "Manual", "No autonomous behavior; direct /api/eyes and /api/gestures calls flow through unopposed."},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

// Atomic: written from the AsyncTCP/HTTP task (activate()) and from
// MotionTask (greeting's settle-to-idle), read every MotionTask tick.
std::atomic<Mode> gActiveMode{Mode::Manual};

const ModeDef *findModeDef(Mode m) {
  for (const auto &d : kModes) {
    if (d.mode == m) return &d;
  }
  return nullptr;
}

const ModeDef *findModeDef(const char *id) {
  for (const auto &d : kModes) {
    if (strcmp(d.id, id) == 0) return &d;
  }
  return nullptr;
}

void pushPlayModeCommand(const EyeCommand &cmdIn) {
  EyeCommand cmd = cmdIn;
  cmd.source = CommandSource::PlayMode;
  CommandQueue::push(cmd);
}

// --- Idle / Curious ---------------------------------------------------
// Shared timer state — safe to reuse across modes since only one mode is
// ever active at a time; each mode's startX() reseeds these on entry.
uint32_t gDriftNextMs = 0;
uint32_t gBlinkNextMs = 0;

void startIdle(uint32_t nowMs) {
  gDriftNextMs = nowMs + random(1500, 3000);
  gBlinkNextMs = nowMs + random(3000, 7000);
}

void idleTick(uint32_t nowMs) {
  // Hold off while a gesture plays (e.g. tracking's presence "surprise"):
  // a drift would override its gaze and a blink would replace it outright.
  // Overdue timers simply fire once it finishes.
  if (GestureEngine::isPlaying()) {
    return;
  }
  if (deadlineReached(nowMs, gDriftNextMs)) {
    EyeCommand cmd;
    cmd.panDeg = static_cast<float>(random(-800, 801)) / 100.0f;  // ±8.00°
    cmd.tiltDeg = static_cast<float>(random(-500, 501)) / 100.0f; // ±5.00°
    cmd.durationMs = random(900, 1800);
    cmd.easing = Easing::EaseInOut;
    pushPlayModeCommand(cmd);
    gDriftNextMs = nowMs + random(2500, 5500);
  }
  if (deadlineReached(nowMs, gBlinkNextMs)) {
    GestureEngine::trigger("blink", CommandSource::PlayMode);
    gBlinkNextMs = nowMs + random(4000, 9000);
  }
}

void startCurious(uint32_t nowMs) {
  gDriftNextMs = nowMs + random(800, 2000);
  gBlinkNextMs = nowMs + random(1800, 4200);
}

void curiousTick(uint32_t nowMs) {
  // Fully code-parameterized (amplitude/frequency only, no JSON) — see
  // PROGRESS.md Phase 4 notes for why a full JSON-driven "curious"
  // behavior was judged out of scope for this phase.
  if (GestureEngine::isPlaying()) {
    return; // same as idleTick(): don't step on a playing gesture
  }
  if (deadlineReached(nowMs, gDriftNextMs)) {
    EyeCommand cmd;
    cmd.panDeg = static_cast<float>(random(-3500, 3501)) / 100.0f;  // ±35°
    cmd.tiltDeg = static_cast<float>(random(-2000, 2001)) / 100.0f; // ±20°
    cmd.durationMs = random(300, 700);                              // snappier than idle's drift
    cmd.easing = Easing::EaseInOut;
    pushPlayModeCommand(cmd);
    gDriftNextMs = nowMs + random(1200, 3000);
  }
  if (deadlineReached(nowMs, gBlinkNextMs)) {
    GestureEngine::trigger("blink", CommandSource::PlayMode);
    gBlinkNextMs = nowMs + random(1800, 4200);
  }
  // NOTE (scope simplification, see PROGRESS.md): plan §5 also mentions
  // "occasional head-tilt-like asymmetric lid narrowing" for curious mode.
  // Not implemented — a cosmetic flourish on top of already-functional,
  // fully interruptible curious behavior; adding it would mean a second,
  // separate lid-owning state machine competing with NaturalModeCoupler
  // for the same axes, for a detail the plan itself flags as descriptive
  // color rather than a core requirement.
}

// --- Sleep ---------------------------------------------------------------
void startSleep(uint32_t nowMs) {
  EyeCommand ease;
  // 0.0 == calibrated "lids just touching" point (ServoCalibration::closedUs).
  ease.lidUpperL = 0.0f;
  ease.lidLowerL = 0.0f;
  ease.lidUpperR = 0.0f;
  ease.lidLowerR = 0.0f;
  ease.panDeg = 0.0f;
  ease.tiltDeg = 0.0f;
  ease.durationMs = 900;
  ease.easing = Easing::EaseInOut;
  pushPlayModeCommand(ease);
  // NaturalModeCoupler would otherwise reclaim the lid axes the instant
  // they go "free" (900ms from now) and pull them back toward its ~0.85
  // open baseline — directly fighting sleep mode's whole point. Suppressed
  // for the duration of sleep mode; see natural_mode.h.
  NaturalModeCoupler::setSuppressed(true);
  gDriftNextMs = nowMs + random(20000, 35000); // rare "still breathing" micro-drift
}

void sleepTick(uint32_t nowMs) {
  if (deadlineReached(nowMs, gDriftNextMs)) {
    EyeCommand cmd;
    cmd.panDeg = static_cast<float>(random(-200, 201)) / 100.0f;
    cmd.tiltDeg = static_cast<float>(random(-150, 151)) / 100.0f;
    cmd.durationMs = random(1800, 3000);
    cmd.easing = Easing::EaseInOut;
    pushPlayModeCommand(cmd);
    gDriftNextMs = nowMs + random(20000, 35000);
  }
}

// --- Greeting (JSON-driven one-shot sequence) -----------------------------
// Schema (see data/sequences/greeting.json): {"keyframes": [ {panDeg?,
// tiltDeg?, lidUpperL?, lidLowerL?, lidUpperR?, lidLowerR?, durationMs,
// easing?}, ... ]} — each keyframe is a partial EyeCommand, same
// optional-fields shape used everywhere else in the motion engine; an
// absent field means "leave that axis alone". `easing` is "linear" or
// "easeInOut" (default if absent/unrecognized).
constexpr size_t kMaxSequenceKeyframes = 16;
// FS path, NOT "/data/sequences/...": PlatformIO's LittleFS image build
// uploads the `data/` directory's *contents* as the filesystem root, so
// `data/sequences/greeting.json` on disk lands at FS "/sequences/
// greeting.json" — confirmed directly against this project's own
// `pio run -t buildfs` output. The previous "/data/sequences/..." path
// never matched any file on the real filesystem, so loadGreetingSequence()
// always failed and "greeting" mode silently fell back to idle every time
// it was activated (integration-pass fix, Phase 8 — see PROGRESS.md;
// the same FS-root mislabeling also affected web_server.cpp's static
// file serving, fixed alongside this).
constexpr const char *kGreetingPath = "/sequences/greeting.json";

EyeCommand gGreetingKeyframes[kMaxSequenceKeyframes];
size_t gGreetingKeyframeCount = 0;
size_t gGreetingIndex = 0;
uint32_t gGreetingNextDueMs = 0;
uint32_t gGreetingGeneration = 0;
// True from a successful startGreeting() load until the sequence ends or is
// superseded. Guarded by gGreetingMutex.
bool gGreetingRunning = false;

// Integration-pass fix (Phase 8): startGreeting()/loadGreetingSequence()
// runs on the AsyncTCP/HTTP task (POST /api/playmodes/greeting/activate),
// while greetingTick() reads the same gGreetingKeyframes[]/
// gGreetingKeyframeCount/gGreetingIndex every ~20ms from MotionTask's own
// task. Before this fix nothing synchronized the two: re-activating
// "greeting" while a previous greeting sequence was still mid-playback
// could rewrite gGreetingKeyframes[] (loadGreetingSequence() resets the
// count to 0 and repopulates it element-by-element) at the same instant
// greetingTick() was reading gGreetingKeyframes[gGreetingIndex] against
// the OLD count/index — an out-of-bounds/torn read. Guarded the same way
// motion_task.cpp's gPoseMutex / radar_task.cpp's gStateMutex guard their
// own cross-task publish. greetingTick()'s take uses a short bounded
// timeout (not portMAX_DELAY) so a slow LittleFS read on the HTTP-task
// side can never stall MotionTask's real-time tick — it just skips this
// one tick's greeting-sequence work and retries ~20ms later, harmless for
// a one-shot scripted sequence.
SemaphoreHandle_t gGreetingMutex = nullptr;
constexpr TickType_t kGreetingTickMutexWaitTicks = pdMS_TO_TICKS(5);

Easing parseEasingString(const char *s) {
  if (s != nullptr && strcmp(s, "linear") == 0) return Easing::Linear;
  return Easing::EaseInOut; // default, also covers "easeInOut" and unknown/absent values
}

bool loadGreetingSequence() {
  gGreetingKeyframeCount = 0;

  File f = LittleFS.open(kGreetingPath, "r");
  if (!f) {
    Serial.print("[PlayModeManager] ");
    Serial.print(kGreetingPath);
    Serial.println(" not found on LittleFS");
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.print("[PlayModeManager] greeting.json parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray arr = doc["keyframes"].as<JsonArray>();
  for (JsonObject kf : arr) {
    if (gGreetingKeyframeCount >= kMaxSequenceKeyframes) break;
    EyeCommand cmd;
    if (kf["panDeg"].is<float>()) cmd.panDeg = kf["panDeg"].as<float>();
    if (kf["tiltDeg"].is<float>()) cmd.tiltDeg = kf["tiltDeg"].as<float>();
    if (kf["lidUpperL"].is<float>()) cmd.lidUpperL = kf["lidUpperL"].as<float>();
    if (kf["lidLowerL"].is<float>()) cmd.lidLowerL = kf["lidLowerL"].as<float>();
    if (kf["lidUpperR"].is<float>()) cmd.lidUpperR = kf["lidUpperR"].as<float>();
    if (kf["lidLowerR"].is<float>()) cmd.lidLowerR = kf["lidLowerR"].as<float>();
    cmd.durationMs = kf["durationMs"] | 200;
    cmd.easing = parseEasingString(kf["easing"] | "easeInOut");
    gGreetingKeyframes[gGreetingKeyframeCount++] = cmd;
  }

  return gGreetingKeyframeCount > 0;
}

void pushGreetingKeyframe(size_t index, uint32_t nowMs) {
  EyeCommand cmd = gGreetingKeyframes[index];
  cmd.source = CommandSource::PlayMode;
  cmd.generation = gGreetingGeneration;
  CommandQueue::push(cmd);
  gGreetingNextDueMs = nowMs + cmd.durationMs;
}

void startGreeting(uint32_t nowMs); // fwd decl, activate() defined later in this file

void greetingTick(uint32_t nowMs) {
  if (!deadlineReached(nowMs, gGreetingNextDueMs)) return;
  if (xSemaphoreTake(gGreetingMutex, kGreetingTickMutexWaitTicks) != pdTRUE) {
    return; // startGreeting() is mid-(re)load on the HTTP task — try again next tick
  }
  if (!gGreetingRunning) {
    // Not loaded yet: activate() cleared the flag and startGreeting() hasn't
    // captured the new generation yet — a generation mismatch seen in this
    // window is not a supersede.
    xSemaphoreGive(gGreetingMutex);
    return;
  }
  if (MotionTask::getCommandGeneration() != gGreetingGeneration) {
    // Superseded (a Manual command, API gesture, or calibration exit
    // mid-sequence): stop pushing keyframes and settle into idle, as the
    // sequence's normal end does. Deliberately not via activate(), whose
    // generation bump would cancel whatever just took over.
    gGreetingRunning = false;
    gActiveMode = Mode::Idle;
    startIdle(nowMs);
    xSemaphoreGive(gGreetingMutex);
    return;
  }
  size_t next = gGreetingIndex + 1;
  if (next >= gGreetingKeyframeCount) {
    gGreetingRunning = false;
    xSemaphoreGive(gGreetingMutex);
    PlayModeManager::activate("idle"); // one-shot sequence finished — settle to idle (plan §5)
    return;
  }
  gGreetingIndex = next;
  pushGreetingKeyframe(gGreetingIndex, nowMs);
  xSemaphoreGive(gGreetingMutex);
}

void startGreeting(uint32_t nowMs) {
  if (gGreetingMutex == nullptr) {
    gGreetingMutex = xSemaphoreCreateMutex();
  }
  xSemaphoreTake(gGreetingMutex, portMAX_DELAY);
  bool loaded = loadGreetingSequence();
  if (loaded) {
    gGreetingIndex = 0;
    gGreetingGeneration = MotionTask::getCommandGeneration();
    gGreetingRunning = true;
    pushGreetingKeyframe(0, nowMs);
  }
  xSemaphoreGive(gGreetingMutex);

  if (!loaded) {
    Serial.println("[PlayModeManager] greeting sequence unavailable, falling back to idle");
    gActiveMode = Mode::Idle;
    startIdle(nowMs);
  }
}

// --- Tracking (Phase 5, radar-driven) -------------------------------------
// Reads RadarTask::getState() (mutex-guarded, safe to call from here — this
// runs inside MotionTask's own tick, a different task than RadarTask, same
// cross-task read pattern as MotionTask::getCurrentPose() elsewhere in the
// codebase) once per PlayModeManager tick.
//
// Two behaviors, chosen per-tick by whether any current target actually has
// angle data — NOT by which radar type is configured, so this logic
// stays correct regardless of which sensor is compiled in (and doesn't need
// duplicating if a future sensor sits somewhere in between):
//   - A target with angleDeg available (LD2450) -> real proportional
//     gaze-following toward the closest such target (plan §5/§6), damped to
//     at most one retarget every kTrackingMinRetargetMs so a fast radar
//     update rate doesn't produce a nervous/jittery gaze.
//   - No target has angle data at all (LD2420: presence/distance only, or
//     simply no target currently detected) -> degrades to a
//     presence-triggered "alert" glance: on the rising edge of presence
//     only, trigger the existing "surprise" gesture (lids snap wide + gaze
//     reset, see gesture_engine.cpp) rather than continuously chasing a
//     direction the sensor can't actually provide (plan §5's explicit
//     LD2420 degradation note). Falls back to idle's baseline drift/blink
//     the rest of the time so tracking mode never looks visibly "dead"
//     between presence events.
bool gTrackingWasPresent = false;
uint32_t gTrackingNextRetargetMs = 0;
// Damping window (task spec: 200-400ms) so continuous fast radar updates
// (LD2450) don't retarget the gaze every single tick.
constexpr uint32_t kTrackingMinRetargetMs = 300;
// Matches MotionTask's own pan/tilt working range (motion_task.cpp's
// kGazeRangeDeg, +/-45 deg) — duplicated here since that constant is
// file-local to motion_task.cpp and not exported; keep in sync if that
// mapping ever changes.
constexpr float kTrackingGazeRangeDeg = 45.0f;

void startTracking(uint32_t nowMs) {
  gTrackingWasPresent = false;
  gTrackingNextRetargetMs = nowMs; // due immediately (a 0 sentinel would not be wrap-safe)
  // Reuses idle's own drift/blink timers/behavior for the "no directional
  // target right now" baseline (LD2420, or LD2450 between detections).
  startIdle(nowMs);
}

void trackingTick(uint32_t nowMs) {
  RadarState state = RadarTask::getState();

  // Primary target = closest target that actually has angle data (my
  // choice, documented per the task spec's "your call, document it" —
  // closest is preferred over "first in list" so a nearer person takes
  // priority over a farther one the sensor happens to report first).
  const RadarTarget *primary = nullptr;
  for (size_t i = 0; i < state.targetCount; ++i) {
    const RadarTarget &t = state.targets[i];
    if (!t.angleDeg.has_value()) continue;
    if (primary == nullptr) {
      primary = &t;
      continue;
    }
    if (t.distanceMm.has_value() && primary->distanceMm.has_value() && *t.distanceMm < *primary->distanceMm) {
      primary = &t;
    }
  }

  if (primary != nullptr) {
    // LD2450-style real directional tracking.
    if (deadlineReached(nowMs, gTrackingNextRetargetMs)) {
      float clampedAngle = *primary->angleDeg;
      if (clampedAngle > kTrackingGazeRangeDeg) clampedAngle = kTrackingGazeRangeDeg;
      if (clampedAngle < -kTrackingGazeRangeDeg) clampedAngle = -kTrackingGazeRangeDeg;

      EyeCommand cmd;
      cmd.panDeg = clampedAngle;
      cmd.durationMs = 280;
      cmd.easing = Easing::EaseInOut;
      pushPlayModeCommand(cmd);
      gTrackingNextRetargetMs = nowMs + kTrackingMinRetargetMs;
    }
    gTrackingWasPresent = true;
    return;
  }

  // No target with usable angle data this tick — LD2420 degradation path
  // (plan §5/§6): presence-triggered alert glance on the rising edge only,
  // not a continuous behavior, since there's no direction to follow.
  if (state.presence && !gTrackingWasPresent) {
    GestureEngine::trigger("surprise", CommandSource::PlayMode);
  }
  gTrackingWasPresent = state.presence;
  idleTick(nowMs);
}

void enterMode(Mode mode, uint32_t nowMs) {
  // Only "sleep" suppresses natural-mode lid coupling; every other mode
  // must make sure it's not left suppressed from a previous sleep.
  if (mode != Mode::Sleep) {
    NaturalModeCoupler::setSuppressed(false);
  }
  switch (mode) {
    case Mode::Idle:
      startIdle(nowMs);
      break;
    case Mode::Curious:
      startCurious(nowMs);
      break;
    case Mode::Sleep:
      startSleep(nowMs);
      break;
    case Mode::Greeting:
      startGreeting(nowMs);
      break;
    case Mode::Tracking:
      // Phase 5: real radar-driven tracking — see trackingTick()/
      // startTracking() above for the full LD2450 (directional
      // proportional following) vs. LD2420 (presence-triggered "alert"
      // glance) split.
      startTracking(nowMs);
      break;
    case Mode::Manual:
    default:
      break;
  }
}

} // namespace

namespace PlayModeManager {

void begin() {
  gActiveMode = Mode::Manual;
  if (gGreetingMutex == nullptr) {
    gGreetingMutex = xSemaphoreCreateMutex();
  }
}

void tick(uint32_t nowMs) {
  switch (gActiveMode.load()) {
    case Mode::Idle:
      idleTick(nowMs);
      break;
    case Mode::Tracking:
      trackingTick(nowMs);
      break;
    case Mode::Curious:
      curiousTick(nowMs);
      break;
    case Mode::Sleep:
      sleepTick(nowMs);
      break;
    case Mode::Greeting:
      greetingTick(nowMs);
      break;
    case Mode::Manual:
    default:
      break;
  }
}

bool activate(const char *id) {
  const ModeDef *def = findModeDef(id);
  if (def == nullptr) {
    return false;
  }
  // Stop whatever the previous mode had in flight (plan §2/§5: switching
  // play mode cancels the sequencer's further enqueues) before starting
  // the new one. The greeting flag is cleared first, under the greeting
  // mutex, so a MotionTask tick landing between the bump below and a new
  // startGreeting() capturing the new generation can't mistake that
  // mismatch for a supersede (see greetingTick()).
  xSemaphoreTake(gGreetingMutex, portMAX_DELAY);
  gGreetingRunning = false;
  xSemaphoreGive(gGreetingMutex);
  MotionTask::bumpCommandGeneration();
  gActiveMode = def->mode;
  enterMode(def->mode, millis());
  return true;
}

const char *getActiveModeId() {
  const ModeDef *def = findModeDef(gActiveMode.load());
  return def != nullptr ? def->id : "manual";
}

size_t listPlayModes(PlayModeInfo *outArray, size_t maxCount) {
  size_t n = kModeCount < maxCount ? kModeCount : maxCount;
  for (size_t i = 0; i < n; ++i) {
    outArray[i].id = kModes[i].id;
    outArray[i].label = kModes[i].label;
    outArray[i].description = kModes[i].description;
  }
  return n;
}

} // namespace PlayModeManager
