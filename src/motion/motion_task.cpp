#include "motion/motion_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>

#include "hal/servo_hal.h"
#include "motion/command_queue.h"
#include "motion/gesture_engine.h"
#include "motion/natural_mode.h"
#include "motion/playmode_manager.h"
#include "pin_map.h"

namespace {

// --- Task configuration ----------------------------------------------------
// Core 1 per architecture plan §2 ("Control Logic (Core 1)") — this keeps
// MotionTask off Core 0, where WiFi/LWIP/AsyncTCP's own internal tasks
// mostly live, so servo timing doesn't compete with network interrupt
// handling. Arduino's own loop() task also runs on Core 1 by default at
// priority 1; MotionTask's priority (3) sits above that so a busy loop()
// (OTA handling, button polling) can't delay a tick, but stays well below
// the WiFi/BT system tasks (typically priority ~18-23 on arduino-esp32)
// so it never starves anything safety/connectivity-critical. Stack is
// 4096 bytes — the tick body is a handful of float ops per axis plus a
// small JSON-free struct copy, no large local buffers, so this is a
// comfortable margin over what's actually used, not a tight fit.
constexpr BaseType_t kMotionTaskCore = 1;
constexpr UBaseType_t kMotionTaskPriority = 3;
constexpr uint32_t kMotionTaskStackBytes = 4096;
constexpr TickType_t kTickPeriod = pdMS_TO_TICKS(20); // ~50Hz

// --- Axis bookkeeping --------------------------------------------------
// One AxisState per EyePose field, indexed by the first 6 ServoId values
// (Pan..LidLowerR) — SERVO_AUX has no EyePose field and is intentionally
// excluded from interpolation; it's only reachable via ServoHal directly
// (calibration test route).
constexpr size_t kAxisCount = static_cast<size_t>(ServoId::LidLowerR) + 1;

struct AxisState {
  float startValue = 0.0f;
  float targetValue = 0.0f;
  uint32_t startTimeMs = 0;
  uint32_t durationMs = 0; // 0 == already at targetValue
  Easing easing = Easing::Linear;
  // Phase 4 addition: who owns this axis's in-flight interpolation. This
  // is the "ownership" tag plan §2 describes for natural-mode coupling —
  // deliberately no separate ownership bookkeeping beyond this plus the
  // existing timing fields: an axis is "free" again for
  // NaturalModeCoupler to touch once `now >= startTimeMs + durationMs`
  // (its last interpolation has finished), regardless of what source set
  // it, or immediately if `source == Natural` already (see
  // maybeApplyNatural() below).
  CommandSource source = CommandSource::Manual;
};

AxisState gAxis[kAxisCount];

TaskHandle_t gTaskHandle = nullptr;
SemaphoreHandle_t gPoseMutex = nullptr;
EyePose gCurrentPose; // only written by the motion task, under gPoseMutex

// See motion_task.h's doc comment for the full commandGeneration
// mechanism. Bumped from the AsyncTCP/HTTP task (API handlers) and
// PlayModeManager::activate(); read from the MotionTask task
// (GestureEngine/PlayModeManager's own coroutines).
std::atomic<uint32_t> gCommandGeneration{0};

// Calibration hold (see motion_task.h): millis() timestamp the hold
// expires at, 0 == not held. Written from the AsyncTCP/HTTP task
// (servo_routes.cpp), read every tick by the motion task.
std::atomic<uint32_t> gHoldUntilMs{0};

bool holdActive(uint32_t nowMs) {
  uint32_t until = gHoldUntilMs.load(std::memory_order_relaxed);
  return until != 0 && static_cast<int32_t>(until - nowMs) > 0;
}

// Pose MotionTask eases back to when a calibration hold ends: straight
// ahead, lids at natural mode's resting openness.
constexpr float kRestLidOpenness = 0.85f;
constexpr uint32_t kHoldExitEaseMs = 400;

size_t idx(ServoId id) { return static_cast<size_t>(id); }

// Value of `axis` at time `nowMs`, without mutating it. Pure function of
// (start, target, startTime, duration, easing) — this is what makes
// re-targeting cheap: "current value" is always derivable on demand, so
// interrupting an in-flight interpolation never needs a stored velocity.
float valueAt(const AxisState &axis, uint32_t nowMs) {
  if (axis.durationMs == 0) {
    return axis.targetValue;
  }
  uint32_t elapsed = nowMs - axis.startTimeMs; // wraps safely (unsigned)
  if (elapsed >= axis.durationMs) {
    return axis.targetValue;
  }
  float t = static_cast<float>(elapsed) / static_cast<float>(axis.durationMs);
  float eased = applyEasing(axis.easing, t);
  return axis.startValue + (axis.targetValue - axis.startValue) * eased;
}

// Re-targets `axis` to `newTarget`, starting from wherever it actually is
// *right now* (not its old target) — this is plan §2's interruptibility
// requirement: a new command always wins immediately, it never queues
// behind whatever motion was already in flight. `source` tags who now
// owns this axis (Phase 4 — see AxisState's comment above).
void retarget(AxisState &axis, float newTarget, uint32_t nowMs, uint32_t durationMs, Easing easing, CommandSource source) {
  float current = valueAt(axis, nowMs);
  axis.startValue = current;
  axis.targetValue = newTarget;
  axis.startTimeMs = nowMs;
  axis.durationMs = durationMs;
  axis.easing = easing;
  axis.source = source;
}

void applyCommand(const EyeCommand &cmd, uint32_t nowMs) {
  if (cmd.panDeg) retarget(gAxis[idx(ServoId::Pan)], *cmd.panDeg, nowMs, cmd.durationMs, cmd.easing, cmd.source);
  if (cmd.tiltDeg) retarget(gAxis[idx(ServoId::Tilt)], *cmd.tiltDeg, nowMs, cmd.durationMs, cmd.easing, cmd.source);
  if (cmd.lidUpperL) retarget(gAxis[idx(ServoId::LidUpperL)], *cmd.lidUpperL, nowMs, cmd.durationMs, cmd.easing, cmd.source);
  if (cmd.lidLowerL) retarget(gAxis[idx(ServoId::LidLowerL)], *cmd.lidLowerL, nowMs, cmd.durationMs, cmd.easing, cmd.source);
  if (cmd.lidUpperR) retarget(gAxis[idx(ServoId::LidUpperR)], *cmd.lidUpperR, nowMs, cmd.durationMs, cmd.easing, cmd.source);
  if (cmd.lidLowerR) retarget(gAxis[idx(ServoId::LidLowerR)], *cmd.lidLowerR, nowMs, cmd.durationMs, cmd.easing, cmd.source);
}

// --- Natural-mode eyelid coupling (Phase 4, plan §5) --------------------
// NOT pushed through CommandQueue — see natural_mode.h for why. Instead
// this directly retargets a lid AxisState, and only when it's "free": not
// currently owned by an in-flight Manual/Gesture/PlayMode interpolation
// (checked via AxisState.source + timing, no separate ownership
// bookkeeping — see the AxisState comment above), and only when the
// candidate target has actually moved enough to be worth a retarget (the
// >0.02 jitter guard from the task spec) — comparing directly against the
// axis's own current targetValue, so NaturalModeCoupler itself can stay a
// stateless pure function (see its header).
constexpr float kNaturalChangeThreshold = 0.02f;
constexpr uint32_t kNaturalSmoothingMs = 200; // plan §5's ~150-250ms smoothing window

void maybeApplyNatural(AxisState &axis, float candidateTarget, uint32_t nowMs) {
  bool free = (axis.source == CommandSource::Natural) || (nowMs >= axis.startTimeMs + axis.durationMs);
  if (!free) {
    return;
  }
  if (std::fabs(candidateTarget - axis.targetValue) < kNaturalChangeThreshold) {
    return;
  }
  retarget(axis, candidateTarget, nowMs, kNaturalSmoothingMs, Easing::EaseInOut, CommandSource::Natural);
}

// --- Degrees/normalized -> pulse-us mapping ---------------------------
// Pan/tilt assumption (documented per task spec, needs real-hardware
// tuning once the ε-SERIES mechanism is assembled): the working gaze
// range is treated as ±45°, linearly mapped to the servo's calibrated
// [minUs, maxUs], centered on centerUs at 0°, with the low/high halves
// scaled independently so an asymmetric calibration (centerUs not
// exactly halfway between minUs/maxUs) still lands exactly on centerUs
// at 0deg and exactly on minUs/maxUs at -45/+45.
constexpr float kGazeRangeDeg = 45.0f;

uint16_t degreesToPulseUs(ServoId id, float deg) {
  ServoCalibration cal = ServoHal::getCalibration(id);
  float clampedDeg = constrain(deg, -kGazeRangeDeg, kGazeRangeDeg);
  float t = clampedDeg / kGazeRangeDeg; // -1..1
  if (cal.inverted) {
    t = -t;
  }
  float us = (t >= 0.0f) ? static_cast<float>(cal.centerUs) + t * static_cast<float>(cal.maxUs - cal.centerUs)
                          : static_cast<float>(cal.centerUs) + t * static_cast<float>(cal.centerUs - cal.minUs);
  return static_cast<uint16_t>(us + 0.5f);
}

// Eyelid mapping, piecewise linear through the three calibrated points:
// 0.0 == closedUs (upper and lower lid just touching, eyes straight
// ahead), 0.5 == halfUs, 1.0 == openUs. The middle point is what keeps
// left and right lids at the same visual height in between (linkages are
// not linear and differ per eye). The points already encode which
// physical direction "open" is for mirror-mounted lids, so `inverted` is
// not used here. ServoHal::setPulseUs() still clamps to [minUs, maxUs].
uint16_t normalizedToPulseUs(ServoId id, float normalized) {
  ServoCalibration cal = ServoHal::getCalibration(id);
  float n = constrain(normalized, 0.0f, 1.0f);
  float closed = static_cast<float>(cal.closedUs);
  float half = static_cast<float>(cal.halfUs);
  float open = static_cast<float>(cal.openUs);
  float us = (n <= 0.5f) ? closed + (n / 0.5f) * (half - closed) : half + ((n - 0.5f) / 0.5f) * (open - half);
  return static_cast<uint16_t>(us + 0.5f);
}

// Called once on the tick a calibration hold ends: aborts any sequence
// that was mid-flight when calibration started and eases every axis back
// to the rest pose, so autonomous motion resumes from a known state.
void exitCalibrationHold(uint32_t nowMs) {
  MotionTask::bumpCommandGeneration();
  const float targets[kAxisCount] = {0.0f, 0.0f, kRestLidOpenness, kRestLidOpenness, kRestLidOpenness,
                                     kRestLidOpenness};
  for (size_t i = 0; i < kAxisCount; ++i) {
    retarget(gAxis[i], targets[i], nowMs, kHoldExitEaseMs, Easing::EaseInOut, CommandSource::Manual);
  }
}

void motionTaskFn(void * /*param*/) {
  TickType_t lastWakeTime = xTaskGetTickCount();
  bool wasHeld = false;

  for (;;) {
    // vTaskDelayUntil (not vTaskDelay) so the tick period doesn't drift
    // by however long the previous tick's own work took — required for
    // an accurate, steady 50Hz per the task spec.
    vTaskDelayUntil(&lastWakeTime, kTickPeriod);

    uint32_t now = millis();

    EyeCommand cmd;

    // Calibration hold: the calibration page owns the servos (raw pulses
    // via ServoHal::setRawPulseUs()), so write nothing, tick nothing, and
    // drop any commands that arrive meanwhile rather than replaying them
    // when the hold ends.
    if (holdActive(now)) {
      while (CommandQueue::pop(cmd)) {
      }
      if (!wasHeld) {
        Serial.println("[Motion] Calibration hold started.");
      }
      wasHeld = true;
      continue;
    }
    if (wasHeld) {
      // Deliberately does NOT clear gHoldUntilMs: an expired timestamp is
      // already inert, and clearing it here raced with the HTTP task — a
      // slider request re-setting the hold between holdActive() above and
      // the clear got wiped, easing the lids to rest mid-calibration.
      wasHeld = false;
      Serial.printf("[Motion] Calibration hold ended (%s), resuming motion.\n",
                    gHoldUntilMs.load(std::memory_order_relaxed) == 0 ? "released" : "expired");
      exitCalibrationHold(now);
    }

    while (CommandQueue::pop(cmd)) {
      applyCommand(cmd, now);
    }

    // GestureEngine/PlayModeManager are ticked from here — not separate
    // FreeRTOS tasks, per plan §2 — and may enqueue new EyeCommands for
    // THIS tick; drain the queue again right after so a same-tick push
    // lands this tick instead of waiting a full extra ~20ms cycle.
    GestureEngine::tick(now);
    PlayModeManager::tick(now);
    while (CommandQueue::pop(cmd)) {
      applyCommand(cmd, now);
    }

    // Natural-mode eyelid coupling (see maybeApplyNatural() above). Reads
    // the just-updated pan/tilt axes so the lid bias always reflects this
    // tick's gaze, including anything GestureEngine/PlayModeManager just
    // pushed above.
    if (NaturalModeCoupler::isEnabled() && !NaturalModeCoupler::isSuppressed()) {
      float panNow = valueAt(gAxis[idx(ServoId::Pan)], now);
      float tiltNow = valueAt(gAxis[idx(ServoId::Tilt)], now);
      NaturalModeCoupler::LidTargets t = NaturalModeCoupler::computeTargets(panNow, tiltNow);
      maybeApplyNatural(gAxis[idx(ServoId::LidUpperL)], t.upperL, now);
      maybeApplyNatural(gAxis[idx(ServoId::LidLowerL)], t.lowerL, now);
      maybeApplyNatural(gAxis[idx(ServoId::LidUpperR)], t.upperR, now);
      maybeApplyNatural(gAxis[idx(ServoId::LidLowerR)], t.lowerR, now);
    }

    EyePose pose;
    pose.panDeg = valueAt(gAxis[idx(ServoId::Pan)], now);
    pose.tiltDeg = valueAt(gAxis[idx(ServoId::Tilt)], now);
    pose.lidUpperL = valueAt(gAxis[idx(ServoId::LidUpperL)], now);
    pose.lidLowerL = valueAt(gAxis[idx(ServoId::LidLowerL)], now);
    pose.lidUpperR = valueAt(gAxis[idx(ServoId::LidUpperR)], now);
    pose.lidLowerR = valueAt(gAxis[idx(ServoId::LidLowerR)], now);

    ServoHal::setPulseUs(ServoId::Pan, degreesToPulseUs(ServoId::Pan, pose.panDeg));
    ServoHal::setPulseUs(ServoId::Tilt, degreesToPulseUs(ServoId::Tilt, pose.tiltDeg));
    ServoHal::setPulseUs(ServoId::LidUpperL, normalizedToPulseUs(ServoId::LidUpperL, pose.lidUpperL));
    ServoHal::setPulseUs(ServoId::LidLowerL, normalizedToPulseUs(ServoId::LidLowerL, pose.lidLowerL));
    ServoHal::setPulseUs(ServoId::LidUpperR, normalizedToPulseUs(ServoId::LidUpperR, pose.lidUpperR));
    ServoHal::setPulseUs(ServoId::LidLowerR, normalizedToPulseUs(ServoId::LidLowerR, pose.lidLowerR));

    // Publish under a short critical section so getCurrentPose() (called
    // from the AsyncTCP/HTTP task) never observes a torn struct.
    xSemaphoreTake(gPoseMutex, portMAX_DELAY);
    gCurrentPose = pose;
    xSemaphoreGive(gPoseMutex);
  }
}

void initAxis(AxisState &axis, float value, uint32_t nowMs) {
  axis.startValue = value;
  axis.targetValue = value;
  axis.startTimeMs = nowMs;
  axis.durationMs = 0; // already "arrived" — valueAt() returns targetValue
  axis.easing = Easing::Linear;
  axis.source = CommandSource::Manual; // arbitrary; durationMs==0 already makes it "free"
}

} // namespace

namespace MotionTask {

void begin() {
  CommandQueue::begin();
  gPoseMutex = xSemaphoreCreateMutex();

  uint32_t now = millis();
  EyePose defaults; // EyePose's own default member initializers
  initAxis(gAxis[idx(ServoId::Pan)], defaults.panDeg, now);
  initAxis(gAxis[idx(ServoId::Tilt)], defaults.tiltDeg, now);
  initAxis(gAxis[idx(ServoId::LidUpperL)], defaults.lidUpperL, now);
  initAxis(gAxis[idx(ServoId::LidLowerL)], defaults.lidLowerL, now);
  initAxis(gAxis[idx(ServoId::LidUpperR)], defaults.lidUpperR, now);
  initAxis(gAxis[idx(ServoId::LidLowerR)], defaults.lidLowerR, now);
  gCurrentPose = defaults;

  // Phase 4 subsystems: not separate FreeRTOS tasks (plan §2), just RAM-
  // cached state + tables that get ticked from inside this task's own
  // loop below — must be initialized before the task starts ticking.
  NaturalModeCoupler::begin();
  GestureEngine::begin();
  PlayModeManager::begin();

  xTaskCreatePinnedToCore(motionTaskFn, "MotionTask", kMotionTaskStackBytes, nullptr, kMotionTaskPriority,
                           &gTaskHandle, kMotionTaskCore);
}

EyePose getCurrentPose() {
  EyePose snapshot;
  if (gPoseMutex != nullptr && xSemaphoreTake(gPoseMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snapshot = gCurrentPose;
    xSemaphoreGive(gPoseMutex);
  }
  return snapshot;
}

uint32_t bumpCommandGeneration() {
  return gCommandGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t getCommandGeneration() {
  return gCommandGeneration.load(std::memory_order_relaxed);
}

void setCalibrationHold(uint32_t durationMs) {
  uint32_t until = millis() + durationMs;
  if (until == 0) until = 1; // 0 is the "not held" sentinel
  gHoldUntilMs.store(until, std::memory_order_relaxed);
}

void releaseCalibrationHold() {
  gHoldUntilMs.store(0, std::memory_order_relaxed);
}

uint32_t calibrationHoldRemainingMs() {
  uint32_t now = millis();
  if (!holdActive(now)) return 0;
  return gHoldUntilMs.load(std::memory_order_relaxed) - now;
}

} // namespace MotionTask
