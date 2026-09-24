#include "api/led_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "api/json_helpers.h"
#include "hal/led_controller.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

void writeConfigJson(JsonDocument &doc, const LedController::LedConfig &cfg) {
  doc["enabled"] = cfg.enabled;
  doc["brightness"] = cfg.brightness;
  JsonObject cl = doc["colorL"].to<JsonObject>();
  cl["r"] = cfg.colorL.r;
  cl["g"] = cfg.colorL.g;
  cl["b"] = cfg.colorL.b;
  JsonObject cr = doc["colorR"].to<JsonObject>();
  cr["r"] = cfg.colorR.r;
  cr["g"] = cfg.colorR.g;
  cr["b"] = cfg.colorR.b;
  doc["effect"] = LedController::effectToName(cfg.effect);
}

// --- GET /api/led/config -------------------------------------------------

void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  writeConfigJson(doc, LedController::getConfig());
  sendJson(request, doc);
}

// --- POST /api/led/config -------------------------------------------------
// Partial update: the request may omit any field, in which case it keeps
// its current value — the working copy starts from
// LedController::getConfig(), not a fresh default-constructed LedConfig.

bool parseColorField(JsonVariantConst field, LedController::RgbColor &outColor, const char *&error) {
  if (!field["r"].is<int>() || !field["g"].is<int>() || !field["b"].is<int>()) {
    error = "invalid_color";
    return false;
  }
  int r = field["r"].as<int>();
  int g = field["g"].as<int>();
  int b = field["b"].as<int>();
  if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
    error = "color_out_of_range";
    return false;
  }
  outColor = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
  return true;
}

void handlePostConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  LedController::LedConfig cfg = LedController::getConfig(); // partial-update base

  if (reqDoc["enabled"].is<bool>()) {
    cfg.enabled = reqDoc["enabled"].as<bool>();
  }

  if (!reqDoc["brightness"].isNull()) {
    if (!reqDoc["brightness"].is<int>()) {
      sendJsonError(request, 400, "invalid_brightness");
      return;
    }
    int brightness = reqDoc["brightness"].as<int>();
    if (brightness < 0 || brightness > 255) {
      sendJsonError(request, 400, "brightness_out_of_range");
      return;
    }
    cfg.brightness = static_cast<uint8_t>(brightness);
  }

  if (!reqDoc["colorL"].isNull()) {
    const char *error = nullptr;
    if (!parseColorField(reqDoc["colorL"], cfg.colorL, error)) {
      sendJsonError(request, 400, error);
      return;
    }
  }

  if (!reqDoc["colorR"].isNull()) {
    const char *error = nullptr;
    if (!parseColorField(reqDoc["colorR"], cfg.colorR, error)) {
      sendJsonError(request, 400, error);
      return;
    }
  }

  if (!reqDoc["effect"].isNull()) {
    if (!reqDoc["effect"].is<const char *>()) {
      sendJsonError(request, 400, "invalid_effect");
      return;
    }
    String effectName = reqDoc["effect"].as<const char *>();
    LedController::LedEffect effect;
    if (!LedController::effectFromName(effectName, effect)) {
      sendJsonError(request, 400, "unknown_effect");
      return;
    }
    cfg.effect = effect;
  }

  LedController::setConfig(cfg);

  JsonDocument respDoc;
  respDoc["success"] = true;
  writeConfigJson(respDoc, LedController::getConfig());
  sendJson(request, respDoc);
}

} // namespace

namespace LedRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/led/config", HTTP_GET, handleGetConfig);
  server.on("/api/led/config", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostConfigBody);
}

} // namespace LedRoutes
