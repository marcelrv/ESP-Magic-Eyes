#include "radar/radar_task.h"

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if defined(RADAR_LD2420)
#include "radar/ld2420_sensor.h"
#elif defined(RADAR_LD2450)
#include "radar/ld2450_sensor.h"
#else
#error "RadarTask requires RADAR_LD2420 or RADAR_LD2450 (see platformio.ini build_flags)"
#endif

namespace {

// Core 0 per architecture plan §2 ("Sensor & IO (Core 0)") — MotionTask
// (the real-time servo control loop) owns Core 1, so radar UART parsing
// stays off it. Priority 1 (below MotionTask's 3, above Arduino's own
// loop() task at the default priority 1 — actually equal to it, which is
// fine: this task blocks in vTaskDelay() almost the entire time, so it
// never starves loop()'s OTA/button housekeeping). Not a real-time control
// loop (task spec) — no vTaskDelayUntil precision needed, a plain
// vTaskDelay between polls is enough; this task's natural cadence is
// however fast the sensor's own UART cadence is, not a fixed tick rate.
constexpr BaseType_t kRadarTaskCore = 0;
constexpr UBaseType_t kRadarTaskPriority = 1;
constexpr uint32_t kRadarTaskStackBytes = 4096;
constexpr TickType_t kPollDelay = pdMS_TO_TICKS(30);  // 20-50ms per task spec

// linkOk (GET /api/radar/status) is "was a frame parsed recently" — this is
// the "some recent window" the task spec asks for.
constexpr uint32_t kLinkOkWindowMs = 5000;

#if defined(RADAR_LD2420)
Ld2420Sensor gSensor;
constexpr const char *kSensorModel = "LD2420";
#elif defined(RADAR_LD2450)
Ld2450Sensor gSensor;
constexpr const char *kSensorModel = "LD2450";
#endif

SemaphoreHandle_t gStateMutex = nullptr;
RadarState gSharedState;  // only written by RadarTask, under gStateMutex
TaskHandle_t gTaskHandle = nullptr;

// Diagnostics (see radar_task.h's RadarStatus doc comment for why these
// exist). Single-writer (this task) / multi-reader (getStatus(), any
// task) — std::atomic with relaxed ordering, same justification as
// motion_task.cpp's commandGeneration: these are plain "how much/when"
// counters, never used to order any other memory access.
std::atomic<bool> gBeginOk{false};
std::atomic<uint32_t> gRawBytesSeen{0};
std::atomic<uint32_t> gLastRawActivityMs{0};

void radarTaskFn(void * /*param*/) {
  // Bounded blocking (see each sensor's begin() doc comment) happens here,
  // inside this task — never in setup()/loop() — so it can't delay boot,
  // the web server, or MotionTask, even if no physical sensor is wired up
  // (plan's explicit "may not be attached yet" caveat).
  bool ok = gSensor.begin();
  gBeginOk.store(ok, std::memory_order_relaxed);
  Serial.print("[RadarTask] sensor begin(): ");
  Serial.println(ok ? "OK" : "no response (will keep polling — sensor may not be wired up yet)");

  for (;;) {
    // Non-destructive peek at Serial2's hardware FIFO, taken *before*
    // gSensor.poll() drains it — this sees raw UART activity regardless of
    // whether IRadarSensor's own parser ever turns it into a valid frame,
    // so it stays a meaningful "is anything physically arriving on the
    // wire" signal even when begin()/frame-parsing are completely broken.
    int available = Serial2.available();
    if (available > 0) {
      gRawBytesSeen.fetch_add(static_cast<uint32_t>(available), std::memory_order_relaxed);
      gLastRawActivityMs.store(millis(), std::memory_order_relaxed);
    }

    gSensor.poll();  // non-blocking; see each sensor's poll() doc comment
    RadarState snapshot = gSensor.getState();

    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    gSharedState = snapshot;
    xSemaphoreGive(gStateMutex);

    vTaskDelay(kPollDelay);
  }
}

}  // namespace

namespace RadarTask {

void begin() {
  gStateMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(radarTaskFn, "RadarTask", kRadarTaskStackBytes, nullptr, kRadarTaskPriority, &gTaskHandle,
                           kRadarTaskCore);
}

RadarState getState() {
  RadarState snapshot;
  if (gStateMutex != nullptr && xSemaphoreTake(gStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    snapshot = gSharedState;
    xSemaphoreGive(gStateMutex);
  }
  return snapshot;
}

RadarStatus getStatus() {
  RadarState state = getState();
  RadarStatus status;
  status.sensorModel = kSensorModel;
  status.lastUpdateMs = state.lastUpdateMs;
  status.linkOk = (state.lastUpdateMs != 0) && (millis() - state.lastUpdateMs < kLinkOkWindowMs);
  status.beginOk = gBeginOk.load(std::memory_order_relaxed);
  status.rawBytesSeen = gRawBytesSeen.load(std::memory_order_relaxed);
  status.lastRawActivityMs = gLastRawActivityMs.load(std::memory_order_relaxed);
  return status;
}

}  // namespace RadarTask
