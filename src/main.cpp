// main.cpp — ESP Magic Eyes firmware entry point
//
// Phase 1 (core infra): mounts LittleFS, brings up NVS-backed config
// storage, WiFi (STA with AP/captive-portal fallback), the factory-reset
// button, and a REST API + static file server.
// Phase 2 (OTA): adds network (ArduinoOTA) + web (/api/ota/*) OTA.
// Phase 3 (motion engine core): brings up ServoHal (attaches all 7 servos
// using saved calibration) and MotionTask (dedicated FreeRTOS task, ~50Hz
// eased interpolation) + /api/servos/*. Radar/LED are not wired up yet —
// later phases. loop() itself is unchanged by Phase 3: MotionTask is
// self-scheduling in its own task, nothing to poll from here.

#include <Arduino.h>
#include <LittleFS.h>

#include "api/ota_routes.h"
#include "api/radar_routes.h"
#include "api/rest_routes.h"
#include "api/wifi_routes.h"
#include "hal/buttons.h"
#include "hal/led_controller.h"
#include "hal/servo_hal.h"
#include "motion/motion_task.h"
#include "net/ota_manager.h"
#include "net/serial_console.h"
#include "net/web_server.h"
#include "net/wifi_manager.h"
#include "radar/radar_task.h"
#include "storage/nvs_store.h"
#include "version.h"

static constexpr const char *kFirmwareName = "ESP Magic Eyes";

void setup() {
  Serial.begin(115200);
  delay(200); // let USB-serial settle before the first print

  Serial.println();
  Serial.println("========================================");
  Serial.print(kFirmwareName);
  Serial.print(" v");
  Serial.print(FIRMWARE_VERSION);
  Serial.println(" — booting");

#if defined(RADAR_LD2420)
  Serial.println("Radar variant: HLK-LD2420 (RADAR_LD2420)");
#elif defined(RADAR_LD2450)
  Serial.println("Radar variant: HLK-LD2450 (RADAR_LD2450)");
#else
  Serial.println("Radar variant: NONE (no RADAR_LD2420/RADAR_LD2450 build flag set)");
#endif

  // Hardware bring-up fix (post-Phase 8): LittleFS::begin()'s default
  // partitionLabel is the literal string "spiffs" (see
  // LittleFS.h: begin(bool, const char*, uint8_t, const char* partitionLabel
  // = "spiffs")), regardless of the partition's SubType. Our partitions.csv
  // names the data partition "littlefs" (Name column — matches
  // board_build.filesystem = littlefs in platformio.ini), so the wrapper's
  // default label lookup found no partition literally named "spiffs" and
  // mounting failed on real hardware every time ("partition \"spiffs\"
  // could not be found") even though the correct partition was right there
  // under its actual name. Pass the real partition label explicitly.
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    // true = format on first-mount failure.
    Serial.println("[FS] LittleFS mount FAILED even after format attempt!");
  } else {
    Serial.println("[FS] LittleFS mounted.");
  }

  NvsStore::begin();

  WifiManager::begin();

  OtaManager::begin();

  Buttons::begin();

  // LedController is a lightweight, non-FreeRTOS-task module (polled from
  // loop() like Buttons/OtaManager) — a low-rate cosmetic effect, not
  // real-time control. Defaults to inert (system/ledEnabled == false)
  // until explicitly enabled via POST /api/led/config.
  LedController::begin();

  // Servo calibration (NvsStore) must be loadable before ServoHal attaches,
  // and ServoHal must be attached before MotionTask's first tick writes to
  // it — see hal/servo_hal.h and motion/motion_task.h.
  ServoHal::begin();

  // RadarTask before MotionTask: PlayModeManager's tracking mode (ticked
  // from inside MotionTask) reads RadarTask::getState() — starting it
  // first means that mutex always exists by the time any tick could touch
  // it, even though "tracking" is never the active mode this early (see
  // PlayModeManager::begin() — always boots to "manual").
  RadarTask::begin();

  MotionTask::begin();

  WebServer::begin();

  Serial.print("WiFi mode: ");
  Serial.println(WifiManager::getModeName(WifiManager::getMode()));
  Serial.println("Phase 1 core infra + Phase 2 OTA + Phase 3 motion engine online.");
  Serial.println("========================================");

  SerialConsole::begin();
}

void loop() {
  WifiManager::handle();
  SerialConsole::handle(); // "wifi set ..." etc. — WiFi recovery with physical access
  OtaManager::handle();  // starts/stops + pumps ArduinoOTA, non-blocking
  OtaRoutes::handle();   // fires the deferred restart after a web OTA
  WifiRoutes::handle();  // fires the deferred restart after POST /api/wifi/forget
  RestRoutes::handle();  // fires the deferred restart after POST /api/system/reboot
  RadarRoutes::handle(); // fires the deferred restart after POST /api/radar/config
  Buttons::handle();
  LedController::handle(); // drives Breathe's time-based animation, non-blocking
  // Nothing else blocking here — later phases move real work onto
  // FreeRTOS tasks (MotionTask, RadarTask) and event-driven handlers,
  // per the architecture plan. loop() stays essentially just OTA
  // handling + light housekeeping, per plan §2.
}
