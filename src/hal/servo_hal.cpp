#include "hal/servo_hal.h"

#include <ESP32Servo.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr size_t kServoCount = static_cast<size_t>(ServoId::Count);

Servo gServos[kServoCount];
ServoCalibration gCalibration[kServoCount]; // cache, see getCalibration()

// Root-cause fix (post-Phase 8 hardware bring-up): tracks which channels
// have actually had Servo::attach() called on them. All 6 real
// eye-mechanism servos (Pan/Tilt/lid x4) are attached eagerly in begin() as
// before; ServoId::Aux (GPIO13, pin_map.h's spare/"reserved for future
// use" channel — nothing physically connected) is deliberately left
// unattached at boot and only attached lazily on first real use (see
// attachIfNeeded(), called from setPulseUs()). Real-hardware serial
// capture (5 identical repeated boot cycles) showed Servo::attach(13) as
// the 7th and last of 7 sequential attach() calls in this loop never
// returning, hanging the board until the RTC watchdog reset it
// (RTCWDT_RTC_RESET) and it boot-looped forever — see PROGRESS.md's
// "Hardware bring-up fixes (post-Phase 8)" section for the full
// investigation and the evidence behind this fix. Deferring Aux's attach
// is a permanent design choice (it's genuinely unused hardware), not a
// stopgap: it means boot never touches the one channel that hangs, while
// still fully supporting Aux on demand via POST /api/servos/test or
// POST /api/servos/config.
bool gAttached[kServoCount] = {}; // all false until begin()/attachIfNeeded() set them

// Last pulse written per channel (see ServoHal::getLastPulseUs()), guarded
// by gMutex like the writes themselves.
uint16_t gLastPulseUs[kServoCount] = {};

// Integration-pass fix (Phase 8): setPulseUs()/getCalibration() are called
// every ~20ms from MotionTask's own task (motion_task.cpp's tick, once per
// interpolated axis), while reapplyCalibration() is called from the
// AsyncTCP/HTTP task (servo_routes.cpp's POST /api/servos/config, "takes
// effect immediately, no reboot"). Before this fix nothing serialized the
// two: reapplyCalibration() does gCalibration[i]=...; detach(); attach();
// writeMicroseconds(...) with no synchronization, so MotionTask's
// setPulseUs() could land between detach() and attach() for that same
// channel (silently dropping that tick's pulse write into a detached
// Servo instance), or getCalibration() could observe a torn
// ServoCalibration struct (a mix of the old and new minUs/maxUs/centerUs)
// mid-assignment. A short-held mutex around every function's body (each
// one is a handful of instructions/one hardware register write — normal
// case adds negligible overhead to the 50Hz hot path) closes this.
SemaphoreHandle_t gMutex = nullptr;

size_t idx(ServoId id) { return static_cast<size_t>(id); }

// Every channel is attach()ed with ESP32Servo's full absolute range, NOT
// the calibrated min/max: the library clamps writeMicroseconds() to the
// attach() bounds, which made it impossible to test a pulse outside the
// currently saved limits while calibrating. The calibrated clamp is done
// here instead (clampToCalibration()), for motion-engine writes only.
constexpr uint16_t kAbsMinUs = 500;
constexpr uint16_t kAbsMaxUs = 2500;

uint16_t clampToAbsolute(uint16_t us) {
  if (us < kAbsMinUs) return kAbsMinUs;
  if (us > kAbsMaxUs) return kAbsMaxUs;
  return us;
}

// Pulse a servo sits at when nothing has moved it yet: lids at their
// calibrated open point (MotionTask's default pose is lids 1.0), every
// other servo at centerUs (straight ahead / neutral).
uint16_t restPulseUs(ServoId id, const ServoCalibration &cal) {
  return isLidServo(id) ? cal.openUs : cal.centerUs;
}

uint16_t clampToCalibration(ServoId id, uint16_t us) {
  const ServoCalibration &cal = gCalibration[idx(id)];
  if (us < cal.minUs) return cal.minUs;
  if (us > cal.maxUs) return cal.maxUs;
  return us;
}

// Attaches channel `id` if it hasn't been already. No-op for the 6 real
// servos (already attached eagerly in begin()); this is what actually
// attaches ServoId::Aux the first time anything asks it to move. Caller
// must hold gMutex.
void attachIfNeeded(ServoId id) {
  size_t i = idx(id);
  if (gAttached[i]) {
    return;
  }
  gServos[i].attach(kServoPins[i], kAbsMinUs, kAbsMaxUs);
  gAttached[i] = true;
}

const char *kServoNames[kServoCount] = {
  "pan", "tilt", "lidUpperL", "lidLowerL", "lidUpperR", "lidLowerR", "aux",
};

} // namespace

namespace ServoHal {

void begin() {
  if (gMutex == nullptr) {
    gMutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < kServoCount; ++i) {
    ServoId id = static_cast<ServoId>(i);
    gCalibration[i] = NvsStore::getServoCalibration(id);

    if (id == ServoId::Aux) {
      // Root-cause fix: don't attach the unused spare channel at boot —
      // see gAttached's doc comment above. Its calibration is still
      // cached (above) so getCalibration() works immediately; attach()
      // happens lazily via attachIfNeeded() on first real use.
      continue;
    }

    gServos[i].attach(kServoPins[i], kAbsMinUs, kAbsMaxUs);
    gLastPulseUs[i] = clampToCalibration(id, restPulseUs(id, gCalibration[i]));
    gServos[i].writeMicroseconds(gLastPulseUs[i]);
    gAttached[i] = true;
  }
}

void setPulseUs(ServoId id, uint16_t us) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return;
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  attachIfNeeded(id);
  gLastPulseUs[i] = clampToCalibration(id, us);
  gServos[i].writeMicroseconds(gLastPulseUs[i]);
  xSemaphoreGive(gMutex);
}

void setRawPulseUs(ServoId id, uint16_t us) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return;
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  attachIfNeeded(id);
  gLastPulseUs[i] = clampToAbsolute(us);
  gServos[i].writeMicroseconds(gLastPulseUs[i]);
  xSemaphoreGive(gMutex);
}

void reapplyCalibration(ServoId id) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return;
  }
  ServoCalibration cal = NvsStore::getServoCalibration(id); // flash read outside the mutex
  xSemaphoreTake(gMutex, portMAX_DELAY);
  // Attach bounds are the fixed absolute range (see kAbsMinUs), so a new
  // calibration only needs swapping in — no detach/re-attach glitch, and
  // the servo is left wherever calibration just positioned it.
  gCalibration[i] = cal;
  attachIfNeeded(id); // POST /api/servos/config attaches Aux on demand too
  xSemaphoreGive(gMutex);
}

uint16_t getLastPulseUs(ServoId id) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return 0;
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  uint16_t us = gLastPulseUs[i];
  xSemaphoreGive(gMutex);
  return us;
}

ServoCalibration getCalibration(ServoId id) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return ServoCalibration{};
  }
  xSemaphoreTake(gMutex, portMAX_DELAY);
  ServoCalibration cal = gCalibration[i];
  xSemaphoreGive(gMutex);
  return cal;
}

const char *servoIdName(ServoId id) {
  size_t i = idx(id);
  if (i >= kServoCount) {
    return "unknown";
  }
  return kServoNames[i];
}

} // namespace ServoHal
