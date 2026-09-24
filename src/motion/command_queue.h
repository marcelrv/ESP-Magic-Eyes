// command_queue.h — thin FreeRTOS-queue wrapper carrying EyeCommand from
// producers (HTTP handlers today; GestureEngine/PlayModeManager/
// NaturalModeCoupler in Phase 4) to MotionTask (architecture plan §2).
//
// This is a "latest intent" pipe, not a work backlog: small fixed depth,
// and if it's ever full, push() drops the OLDEST queued command in favor
// of the newest rather than blocking or rejecting the caller — an HTTP
// handler pushing a command must never block on this.

#pragma once

#include "motion/eye_pose.h"

namespace CommandQueue {

// Creates the underlying FreeRTOS queue. Idempotent. Called internally by
// MotionTask::begin() — most code should just call that and never touch
// CommandQueue::begin() directly.
void begin();

// Enqueues a command. Non-blocking (0 tick wait), safe to call from any
// FreeRTOS task, including the AsyncTCP task that runs HTTP handlers.
// Returns false only if the queue hasn't been created yet.
bool push(const EyeCommand &command);

// Non-blocking dequeue; returns false if the queue is empty. Intended to
// be drained to empty, once per tick, by MotionTask only.
bool pop(EyeCommand &outCommand);

} // namespace CommandQueue
