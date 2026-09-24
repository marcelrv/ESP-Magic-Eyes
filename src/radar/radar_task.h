// radar_task.h — RadarTask: dedicated FreeRTOS task owning UART2 (Serial2,
// pin_map.h's RADAR_RX_PIN/RADAR_TX_PIN) + the configured IRadarSensor
// (Ld2420Sensor or Ld2450Sensor, per NvsStore::getRadarType()),
// architecture plan §2 ("Sensor & IO (Core 0)"), §6, §10 Phase 5.
//
// Same "task owns private state + publishes a mutex-guarded snapshot" shape
// as MotionTask (motion/motion_task.h) — RadarTask::getState() is the
// thread-safe accessor other tasks (HTTP handlers via radar_routes.cpp,
// PlayModeManager's tracking mode via MotionTask's own tick) should call,
// never an IRadarSensor instance directly.

#pragma once

#include "radar/iradar_sensor.h"
#include "storage/nvs_store.h" // RadarType

namespace RadarTask {

// Creates the state mutex, instantiates the sensor selected by
// NvsStore::getRadarType() and starts the task. With RadarType::None no
// sensor or task is created at all; getState() then always returns an
// empty reading. Changing the type takes a reboot (POST /api/radar/config
// does that). Call once from setup(), after NvsStore::begin() — order
// relative to MotionTask::begin()
// doesn't matter for correctness (getState() is safe to call before begin()
// runs, returning a default/empty RadarState), but this project starts it
// before MotionTask so the tracking play-mode never sees a not-yet-created
// mutex even in principle.
void begin();

// Thread-safe snapshot of the most recent radar reading. Safe to call from
// any task. Returns a default-constructed (presence=false, no targets,
// lastUpdateMs=0) RadarState if called before begin() or if the mutex is
// briefly contended past its timeout — same "never blocks the caller
// meaningfully" contract as MotionTask::getCurrentPose().
RadarState getState();

// The radar type chosen at boot, and its display name ("LD2420", "LD2450"
// or "NONE").
RadarType getType();
const char *sensorModelName();

// GET /api/radar/status's shape.
//
// beginOk/rawBytesSeen/lastRawActivityMs (bring-up finding, 2026-09-22):
// added because linkOk/lastUpdateMs alone can't distinguish "no bytes are
// physically arriving on the UART" (wiring/power problem) from "bytes are
// arriving but never form a valid frame" (baud/protocol mismatch, a
// software-side problem). rawBytesSeen/lastRawActivityMs come from a
// non-destructive Serial2.available() peek taken every RadarTask poll
// cycle, upstream of and independent from IRadarSensor's own frame
// parser — so they stay meaningful even if the parsing library itself is
// completely wrong for what's on the wire.
struct RadarStatus {
  const char *sensorModel;  // sensorModelName(): "LD2420", "LD2450" or "NONE"
  bool linkOk;               // true if a frame was parsed within the last ~5s
  uint32_t lastUpdateMs;      // millis() of the last successfully parsed frame; 0 == never
  bool beginOk;               // IRadarSensor::begin() succeeded (at boot or a later retry)
  uint32_t rawBytesSeen;      // cumulative bytes ever observed on Serial2, regardless of parse success
  uint32_t lastRawActivityMs; // millis() of the last time any raw byte was observed; 0 == never
};

RadarStatus getStatus();

}  // namespace RadarTask
