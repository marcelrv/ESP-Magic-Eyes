// iradar_sensor.h — radar sensor abstraction (architecture plan §6, §10
// Phase 5).
//
// RadarState is the common, sensor-agnostic reading shape both HLK-LD2420
// (distance/presence only) and HLK-LD2450 (position/angle/speed, up to 3
// targets) can populate. A field a given sensor can't provide is left
// `std::nullopt` (same "partial data" convention EyeCommand already uses in
// motion/eye_pose.h) rather than a magic-number placeholder — callers
// (radar_routes.cpp, PlayModeManager's tracking mode) must check `has_value()`
// before reading a target field.
//
// IRadarSensor itself is a small interface implemented by exactly one of
// Ld2420Sensor / Ld2450Sensor per build (selected by the existing
// RADAR_LD2420 / RADAR_LD2450 build-time flag — see radar_task.cpp). It is
// NOT thread-safe on its own: only RadarTask ever calls begin()/poll()/
// getState() on a sensor instance, all from within RadarTask's own FreeRTOS
// task. RadarTask.cpp then copies that into a mutex-guarded shared RadarState
// for other tasks to read — same split as MotionTask's internal AxisState[]
// (task-private, unsynchronized) vs. its published gCurrentPose (mutex-
// guarded, see motion/motion_task.cpp) — so callers should reach for
// RadarTask::getState()/getStatus() (radar_task.h), not a sensor object
// directly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// A single detected target. LD2420 (distance/presence only) only ever fills
// `distanceMm`; LD2450 (position/angle/speed) fills all five fields.
struct RadarTarget {
  std::optional<float> distanceMm;  // millimeters
  std::optional<float> angleDeg;    // degrees, 0 = straight ahead, +/- per sensor's lateral convention
  std::optional<float> xMm;         // millimeters, lateral (LD2450 only)
  std::optional<float> yMm;         // millimeters, forward (LD2450 only)
  std::optional<float> speedMmS;    // millimeters/second (LD2450 only)
};

// LD2450 reports up to 3 simultaneous targets; LD2420 reports at most one
// (it has no per-target discrimination, just an overall presence/distance
// reading), so this ceiling comfortably covers both sensors.
constexpr size_t kRadarMaxTargets = 3;

// A full sensor reading snapshot, as published by RadarTask.
struct RadarState {
  bool presence = false;
  RadarTarget targets[kRadarMaxTargets];
  size_t targetCount = 0;
  uint32_t lastUpdateMs = 0;  // millis() of the last successfully parsed frame; 0 == never
};

// Implemented by Ld2420Sensor (RADAR_LD2420 builds) and Ld2450Sensor
// (RADAR_LD2450 builds) — see radar/ld2420_sensor.h / radar/ld2450_sensor.h.
class IRadarSensor {
 public:
  virtual ~IRadarSensor() = default;

  // One-time setup: opens the UART and (sensor-dependent) performs an
  // initial communication check. May block briefly (bounded — see each
  // implementation's own header) but must never hang forever, since a
  // physical sensor may not actually be wired up. Called once from
  // RadarTask's own dedicated task, never from setup()/loop(), so this
  // bounded blocking never delays boot or any other subsystem. Returns
  // false if the sensor didn't respond — callers should keep polling
  // afterward anyway (a sensor plugged in later, or a link that recovers,
  // should still start reporting data), not treat false as fatal.
  virtual bool begin() = 0;

  // Non-blocking: drains whatever UART bytes are currently available,
  // parses at most what's needed, and updates the internal state returned
  // by getState(). Must return promptly even if zero bytes are available
  // (no physical sensor wired up) — called repeatedly from RadarTask's
  // poll loop (radar_task.cpp), every ~20-50ms.
  virtual void poll() = 0;

  // Returns the sensor's current reading. NOT thread-safe by itself (see
  // header note above) — only ever called from RadarTask's own task,
  // immediately after poll().
  virtual RadarState getState() = 0;
};
