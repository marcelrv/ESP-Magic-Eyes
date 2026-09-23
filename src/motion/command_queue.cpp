#include "motion/command_queue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace {

// Small fixed depth — see command_queue.h: this is a "latest intent" pipe
// drained every ~20ms by MotionTask, not a backlog that needs headroom.
constexpr UBaseType_t kQueueDepth = 8;

QueueHandle_t gQueue = nullptr;

} // namespace

namespace CommandQueue {

void begin() {
  if (gQueue != nullptr) {
    return; // already initialized
  }
  gQueue = xQueueCreate(kQueueDepth, sizeof(EyeCommand));
}

bool push(const EyeCommand &command) {
  if (gQueue == nullptr) {
    return false;
  }
  if (xQueueSend(gQueue, &command, 0) == pdTRUE) {
    return true;
  }

  // Full (only possible if something pushes much faster than 50Hz, e.g. a
  // buggy client hammering the API): drop the single oldest entry and
  // retry once so the newest command always wins — an intermediate
  // command that never got applied is harmless, only the latest target
  // per axis matters (see header comment).
  EyeCommand discarded;
  xQueueReceive(gQueue, &discarded, 0);
  return xQueueSend(gQueue, &command, 0) == pdTRUE;
}

bool pop(EyeCommand &outCommand) {
  if (gQueue == nullptr) {
    return false;
  }
  return xQueueReceive(gQueue, &outCommand, 0) == pdTRUE;
}

} // namespace CommandQueue
