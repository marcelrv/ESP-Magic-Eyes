// playmode_manager.h — PlayModeManager: idle/curious/sleep/greeting/
// tracking/manual state machine, architecture plan §5, §10 Phase 4.
//
// Like GestureEngine, this is ticked from inside MotionTask's own tick —
// not a separate FreeRTOS task (plan §2) — and produces EyeCommands via
// the same CommandQueue path any API handler uses, plus GestureEngine
// triggers for its own gesture use (idle/curious's periodic blinks).

#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

namespace PlayModeManager {

// One row of the public listing (GET /api/playmodes).
struct PlayModeInfo {
  const char *id;
  const char *label;
  const char *description;
};

// Sets the active mode to "manual" (plan §5: the default/no-autonomous-
// behavior mode). Deliberately NOT restored from NVS — always boots to
// manual so a freshly-flashed or just-rebooted device never starts moving
// on its own before anything has configured/activated a mode (documented
// simplification, see PROGRESS.md). Call once from MotionTask::begin().
void begin();

// Advances the active mode's autonomous behavior for this tick: idle's
// drift/blinks, curious's bigger/faster saccades and blinks, sleep's
// one-shot ease-to-closed (plus a rare "still breathing" micro-drift),
// greeting's scripted keyframe playback (auto-settles to idle when the
// sequence finishes), and tracking's radar-driven gaze following / presence
// glance (idle behavior when there's no radar data). Manual does nothing. Called
// once per MotionTask tick, same as GestureEngine::tick().
void tick(uint32_t nowMs);

// Switches the active mode. Bumps MotionTask's commandGeneration counter
// (plan §2) first, so anything the PREVIOUS mode had in flight (a
// greeting sequence mid-keyframe-wait, an idle-triggered gesture) aborts
// cleanly on its own next check, then starts the new mode's behavior.
// Returns false if `id` isn't a known mode (no change made).
bool activate(const char *id);

// String id of the currently active mode, for GET /api/playmodes/active
// and GET /api/system/status's "playMode" field.
const char *getActiveModeId();

// Fills `outArray` (capacity `maxCount`) with the fixed mode listing for
// GET /api/playmodes. Returns the number of entries written.
size_t listPlayModes(PlayModeInfo *outArray, size_t maxCount);

} // namespace PlayModeManager
