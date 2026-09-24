// servo_hal.h — ESP32Servo wrapper across all 7 servo channels
// (pin_map.h's ServoId / kServoPins), architecture plan §1, §10 Phase 3.
//
// This is the single low-level entry point that both MotionTask (eased,
// per-tick writes) and the raw calibration-test route
// (POST /api/servos/test) ultimately call — see setPulseUs() below.

#pragma once

#include <cstdint>

#include "pin_map.h"       // ServoId, kServoPins
#include "storage/nvs_store.h" // ServoCalibration

namespace ServoHal {

// Loads each servo's calibration from NvsStore and attach()es it with
// ESP32Servo's full absolute 500-2500us range (the calibrated clamp is
// applied by setPulseUs() itself, so setRawPulseUs() can reach beyond it
// during calibration), then writes its rest pulse (lids: openUs, others:
// centerUs). Call once from setup(), after NvsStore::begin()
// (calibration must be loadable before servos attach).
//
// ServoId::Aux (the spare/unconnected GPIO13 channel, see pin_map.h) is
// the one exception: its calibration is still loaded here, but it is
// deliberately NOT attach()ed at boot — real hardware testing found
// Servo::attach() on this specific channel hangs indefinitely as the 7th
// simultaneous attach in this loop, triggering an RTC-watchdog reset boot
// loop (see PROGRESS.md's "Hardware bring-up fixes (post-Phase 8)"
// section). Aux is instead attached lazily on first real use, from inside
// setPulseUs() — see attachIfNeeded() in servo_hal.cpp. Since nothing
// touches Aux during boot (MotionTask never owns it, see
// motion_task.cpp), this only defers the attach, it never skips it: the
// first POST /api/servos/test or POST /api/servos/config targeting Aux
// still attaches and drives it correctly.
//
// Note: the installed ESP32Servo (madhephaestus/ESP32Servo @ 3.2.1, see
// .pio/libdeps/<env>/ESP32Servo/src/ESP32Servo.h) internally enforces its
// own absolute MIN_PULSE_WIDTH=500 / MAX_PULSE_WIDTH=2500us regardless of
// what's passed to attach() — a calibration min/max outside that range is
// silently clamped tighter by the library itself, on top of this HAL's
// own clamp to the calibrated range in setPulseUs().
void begin();

// Direct, immediate pulse write, clamped to `id`'s calibrated
// [minUs, maxUs] (not the library's wider absolute bound). No easing, no
// interaction with CommandQueue/MotionTask — this is the raw primitive;
// MotionTask calls it every tick with an already-eased value, and
// POST /api/servos/test calls it directly for calibration (see
// api/servo_routes.cpp for the documented caveat about MotionTask
// potentially overwriting a test pulse on an axis it currently owns).
// Lazily attach()es `id` first if it hasn't been attached yet (currently
// only ever true for ServoId::Aux — see begin()'s doc comment).
void setPulseUs(ServoId id, uint16_t us);

// Calibration-tool write: clamps only to ESP32Servo's absolute 500-2500us
// range, not to the calibrated [minUs, maxUs], so the user can explore
// past the currently saved limits while calibrating. Used by
// POST /api/servos/test and /api/servos/pose, while MotionTask's
// calibration hold keeps the motion engine from overwriting it.
void setRawPulseUs(ServoId id, uint16_t us);

// Re-reads `id`'s calibration from NvsStore into the cache so a saved
// POST /api/servos/config change takes effect immediately without a
// reboot. Does not move the servo.
void reapplyCalibration(ServoId id);

// Pulse width most recently written to `id` (by either setter, or the rest
// pulse from begin()) — i.e. where the servo physically is. MotionTask
// uses it to ease out of a calibration hold from the servo's real
// position rather than its pre-hold pose.
uint16_t getLastPulseUs(ServoId id);

// Cheap accessor for the currently-applied (cached, not re-read from
// NVS/flash) calibration — used by MotionTask every ~20ms tick for the
// degrees/normalized -> pulse-us mapping, so the 50Hz loop never touches
// flash.
ServoCalibration getCalibration(ServoId id);

// Human-readable name for logging / JSON responses (GET /api/servos/config).
const char *servoIdName(ServoId id);

} // namespace ServoHal
