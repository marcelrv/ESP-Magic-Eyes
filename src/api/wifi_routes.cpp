#include "api/wifi_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

#include "api/json_helpers.h"
#include "net/wifi_manager.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

// --- GET /api/wifi/scan ----------------------------------------------------

void handleScan(AsyncWebServerRequest *request) {
  size_t count = 0;
  const WifiScanResult *results = WifiManager::getCachedScanResults(count);
  JsonDocument doc;
  JsonArray arr = doc["results"].to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = results[i].ssid;
    o["rssiDbm"] = results[i].rssiDbm;
    o["secure"] = results[i].secure;
  }
  sendJson(request, doc);
}

// --- POST /api/wifi/connect {ssid, password} --------------------------------
// Body-buffer pattern matches every other POST route in this codebase
// (servo_routes.cpp/eyes_routes.cpp/etc): ArBodyHandlerFunction may deliver
// the body in more than one chunk, so it's accumulated here and only parsed
// once the final chunk arrives.
String gConnectBodyBuffer;

void handleConnectBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    gConnectBodyBuffer = "";
    gConnectBodyBuffer.reserve(total);
  }
  gConnectBodyBuffer.concat(reinterpret_cast<const char *>(data), len);
  if (index + len != total) {
    return; // wait for the remaining chunk(s)
  }

  JsonDocument reqDoc;
  DeserializationError parseErr = deserializeJson(reqDoc, gConnectBodyBuffer);
  if (parseErr) {
    sendJsonError(request, 400, "invalid_json");
    return;
  }

  if (!reqDoc["ssid"].is<const char *>() || strlen(reqDoc["ssid"].as<const char *>()) == 0) {
    sendJsonError(request, 400, "missing_ssid");
    return;
  }
  String ssid = reqDoc["ssid"].as<const char *>();
  String password = reqDoc["password"] | "";

  // Non-blocking: WifiManager::connectToNetwork() saves creds to NVS,
  // calls WiFi.begin(), and returns immediately — it does not wait for the
  // connection outcome. The caller must poll GET /api/system/status's
  // wifiMode field to see STA_CONNECTING resolve to STA_CONNECTED or
  // STA_FAILED (bounded ~15s timeout, see wifi_manager.cpp).
  WifiManager::connectToNetwork(ssid, password);

  JsonDocument doc;
  doc["success"] = true;
  doc["status"] = "connecting";
  doc["ssid"] = ssid;
  sendJson(request, doc);
}

// --- POST /api/wifi/forget --------------------------------------------------
bool gForgetPending = false;
uint32_t gForgetAtMs = 0;
// Mirrors ota_routes.cpp's deferred-restart delay: gives AsyncTCP time to
// actually flush the HTTP response to the client before
// WifiManager::forgetNetwork()'s own delay(200)+ESP.restart() tears the
// TCP connection down.
constexpr uint32_t kForgetDelayMs = 800;

void handleForget(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["success"] = true;
  doc["message"] = "WiFi credentials cleared. Device is rebooting into AP setup mode.";
  sendJson(request, doc);

  gForgetPending = true;
  gForgetAtMs = millis() + kForgetDelayMs;
}

} // namespace

namespace WifiRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/wifi/scan", HTTP_GET, handleScan);
  server.on("/api/wifi/connect", HTTP_POST, JsonHelpers::requireBody, nullptr, handleConnectBody);
  server.on("/api/wifi/forget", HTTP_POST, handleForget);
}

void handle() {
  if (gForgetPending && millis() >= gForgetAtMs) {
    gForgetPending = false;
    WifiManager::forgetNetwork(); // clears NVS creds + ESP.restart()
  }
}

} // namespace WifiRoutes
