#include "api/rest_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>

#include "api/json_helpers.h"
#include "hal/led_controller.h"
#include "motion/motion_task.h"
#include "motion/natural_mode.h"
#include "motion/playmode_manager.h"
#include "net/ota_manager.h"
#include "net/wifi_manager.h"
#include "radar/radar_task.h"
#include "storage/nvs_store.h"
#include "version.h"

namespace {

String chipIdHex() {
  uint64_t chipId = ESP.getEfuseMac();
  char buf[17];
  uint32_t high = static_cast<uint32_t>(chipId >> 32);
  uint32_t low = static_cast<uint32_t>(chipId & 0xFFFFFFFF);
  snprintf(buf, sizeof(buf), "%08lX%08lX", static_cast<unsigned long>(high), static_cast<unsigned long>(low));
  return String(buf);
}

// Why the chip last (re)started — lets a brownout (servo current spikes on
// a weak supply) or crash be diagnosed remotely, without a serial cable.
const char *resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_EXT: return "external_pin";
    default: return "unknown";
  }
}

void handleSystemInfo(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["buildDate"] = __DATE__ " " __TIME__;
  doc["chipId"] = chipIdHex();
  doc["uptimeMs"] = millis();
  doc["resetReason"] = resetReasonName();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["radarVariant"] = RadarTask::sensorModelName();
  // Phase 2: surfaced here so the future OTA setup page (Phase 7) can
  // display/toggle it. Read from OtaManager's cached flag (kept in sync
  // with NVS) rather than NvsStore directly, so this always reflects
  // whether ArduinoOTA is actually (going to be) running.
  doc["otaNetworkEnabled"] = OtaManager::isEnabled();
  // Phase 4: naturalMode has existed as an NVS flag since Phase 1 (see
  // storage/nvs_store.h) but wasn't surfaced over HTTP until now. Read
  // from NaturalModeCoupler's RAM cache, same reasoning as
  // otaNetworkEnabled above — always reflects what's actually applied,
  // not just the raw NVS value.
  doc["naturalMode"] = NaturalModeCoupler::isEnabled();
  // Phase 6: ledEnabled has existed as an NVS flag since Phase 1 (see
  // storage/nvs_store.h) but wasn't surfaced over HTTP until now. Read from
  // LedController's RAM-cached config, same reasoning as
  // otaNetworkEnabled/naturalMode above — always reflects what's actually
  // applied, not just the raw NVS value.
  doc["ledEnabled"] = LedController::getConfig().enabled;

  JsonHelpers::sendJson(request, doc);
}

void handleSystemStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  WifiMode mode = WifiManager::getMode();
  doc["wifiMode"] = WifiManager::getModeName(mode);
  doc["ipAddress"] = WifiManager::getIpAddress();
  // Phase 4: real play mode from PlayModeManager (was hardcoded "manual"
  // through Phase 1-3, before PlayModeManager existed).
  doc["playMode"] = PlayModeManager::getActiveModeId();
  doc["naturalMode"] = NaturalModeCoupler::isEnabled();
  // Phase 3: real pose snapshot from MotionTask, thread-safe (mutex-
  // guarded copy) read across into the AsyncTCP/HTTP task context.
  EyePose pose = MotionTask::getCurrentPose();
  JsonObject poseObj = doc["pose"].to<JsonObject>();
  poseObj["panDeg"] = pose.panDeg;
  poseObj["tiltDeg"] = pose.tiltDeg;
  poseObj["lidUpperL"] = pose.lidUpperL;
  poseObj["lidLowerL"] = pose.lidLowerL;
  poseObj["lidUpperR"] = pose.lidUpperR;
  poseObj["lidLowerR"] = pose.lidLowerR;

  JsonHelpers::sendJson(request, doc);
}

// POST /api/system/config — low-effort partial config endpoint for
// { "otaNetworkEnabled": bool, "naturalMode": bool }. Admin-only (net/auth.cpp):
// the manual page's natural-mode toggle uses control-level
// POST /api/eyes/natural instead; naturalMode is still accepted here so
// older callers keep working.

void handleSystemConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  if (reqDoc["otaNetworkEnabled"].is<bool>()) {
    OtaManager::setEnabled(reqDoc["otaNetworkEnabled"].as<bool>());
  }
  // Phase 4: extended to also accept {"naturalMode": bool} (live toggle of
  // the Phase 1 NVS flag — see NaturalModeCoupler::setEnabled()).
  if (reqDoc["naturalMode"].is<bool>()) {
    NaturalModeCoupler::setEnabled(reqDoc["naturalMode"].as<bool>());
  }

  JsonDocument doc;
  doc["success"] = true;
  doc["otaNetworkEnabled"] = OtaManager::isEnabled();
  doc["naturalMode"] = NaturalModeCoupler::isEnabled();
  JsonHelpers::sendJson(request, doc);
}

// POST /api/system/reboot — plan §4 gap closed in Phase 8 (see
// rest_routes.h). Mirrors ota_routes.cpp's/wifi_routes.cpp's existing
// deferred-restart pattern: respond first, then let AsyncTCP flush that
// response before ESP.restart() tears the connection down.
bool gRebootPending = false;
uint32_t gRebootAtMs = 0;
constexpr uint32_t kRebootDelayMs = 500;

void handleReboot(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["success"] = true;
  doc["message"] = "Rebooting.";
  JsonHelpers::sendJson(request, doc);

  gRebootPending = true;
  gRebootAtMs = millis() + kRebootDelayMs;
}

} // namespace

namespace RestRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/system/info", HTTP_GET, handleSystemInfo);
  server.on("/api/system/status", HTTP_GET, handleSystemStatus);
  server.on("/api/system/config", HTTP_POST, JsonHelpers::requireBody, nullptr, handleSystemConfigBody);
  server.on("/api/system/reboot", HTTP_POST, handleReboot);
}

void handle() {
  if (gRebootPending && millis() >= gRebootAtMs) {
    gRebootPending = false;
    Serial.println("[System] Rebooting via POST /api/system/reboot...");
    ESP.restart();
  }
}

} // namespace RestRoutes
