#include "api/radar_routes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "api/json_helpers.h"
#include "radar/iradar_sensor.h"
#include "radar/radar_task.h"
#include "storage/nvs_store.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

// --- GET /api/radar/status --------------------------------------------
void handleStatus(AsyncWebServerRequest *request) {
  RadarTask::RadarStatus status = RadarTask::getStatus();
  JsonDocument doc;
  doc["sensorModel"] = status.sensorModel;
  doc["linkOk"] = status.linkOk;
  doc["lastUpdateMs"] = status.lastUpdateMs;
  // beginOk/rawBytesSeen/lastRawActivityMs: see radar_task.h's RadarStatus
  // doc comment — these separate "nothing is physically arriving on the
  // wire" (rawBytesSeen stays 0) from "bytes arrive but don't parse"
  // (rawBytesSeen grows, linkOk stays false), so a dead link can be
  // diagnosed as hardware vs. software without a serial monitor.
  doc["beginOk"] = status.beginOk;
  doc["rawBytesSeen"] = status.rawBytesSeen;
  doc["lastRawActivityMs"] = status.lastRawActivityMs;
  sendJson(request, doc);
}

// --- GET /api/radar/latest ----------------------------------------------
// Fields a sensor can't provide (e.g. LD2420 has no angle/x/y/speed) are
// serialized as JSON null, not omitted and not a magic-number placeholder
// — plan §6's "frontend + tracking play-mode both handle that gracefully"
// note, made explicit over the wire rather than left to "the key is just
// absent".
void handleLatest(AsyncWebServerRequest *request) {
  RadarState state = RadarTask::getState();
  JsonDocument doc;
  doc["presence"] = state.presence;
  doc["lastUpdateMs"] = state.lastUpdateMs;

  JsonArray targets = doc["targets"].to<JsonArray>();
  for (size_t i = 0; i < state.targetCount; ++i) {
    const RadarTarget &t = state.targets[i];
    JsonObject o = targets.add<JsonObject>();
    if (t.distanceMm) o["distanceMm"] = *t.distanceMm; else o["distanceMm"] = nullptr;
    if (t.angleDeg) o["angleDeg"] = *t.angleDeg; else o["angleDeg"] = nullptr;
    if (t.xMm) o["xMm"] = *t.xMm; else o["xMm"] = nullptr;
    if (t.yMm) o["yMm"] = *t.yMm; else o["yMm"] = nullptr;
    if (t.speedMmS) o["speedMmS"] = *t.speedMmS; else o["speedMmS"] = nullptr;
  }

  sendJson(request, doc);
}

// --- GET /api/radar/config -----------------------------------------------
void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["baudRate"] = NvsStore::getRadarBaudRate();
  sendJson(request, doc);
}

// --- POST /api/radar/config ------------------------------------------------
// Only saves + reboots — RadarTask::begin() only ever opens the UART once
// at boot (radar_task.cpp), so a newly-saved baud can't take effect without
// a fresh begin() call. Deferred-restart pattern matches
// ota_routes.cpp/rest_routes.cpp: respond first, let AsyncTCP flush that
// response, then ESP.restart().
constexpr uint32_t kMinBaud = 1200;
constexpr uint32_t kMaxBaud = 1000000;

bool gRestartPending = false;
uint32_t gRestartAtMs = 0;
constexpr uint32_t kRestartDelayMs = 500;

String gConfigBodyBuffer;

void handlePostConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    gConfigBodyBuffer = "";
    gConfigBodyBuffer.reserve(total);
  }
  gConfigBodyBuffer.concat(reinterpret_cast<const char *>(data), len);
  if (index + len != total) {
    return;  // wait for the remaining chunk(s)
  }

  JsonDocument reqDoc;
  DeserializationError parseErr = deserializeJson(reqDoc, gConfigBodyBuffer);
  if (parseErr) {
    sendJsonError(request, 400, "invalid_json");
    return;
  }

  if (!reqDoc["baudRate"].is<uint32_t>()) {
    sendJsonError(request, 400, "missing_baudRate");
    return;
  }
  uint32_t baudRate = reqDoc["baudRate"].as<uint32_t>();
  if (baudRate < kMinBaud || baudRate > kMaxBaud) {
    sendJsonError(request, 400, "baudRate_out_of_range");
    return;
  }

  NvsStore::setRadarBaudRate(baudRate);

  JsonDocument doc;
  doc["success"] = true;
  doc["baudRate"] = baudRate;
  doc["rebooting"] = true;
  sendJson(request, doc);

  gRestartPending = true;
  gRestartAtMs = millis() + kRestartDelayMs;
}

}  // namespace

namespace RadarRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/radar/status", HTTP_GET, handleStatus);
  server.on("/api/radar/latest", HTTP_GET, handleLatest);
  server.on("/api/radar/config", HTTP_GET, handleGetConfig);
  server.on("/api/radar/config", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostConfigBody);
}

void handle() {
  if (gRestartPending && millis() >= gRestartAtMs) {
    gRestartPending = false;
    Serial.println("[Radar] Restarting after POST /api/radar/config...");
    ESP.restart();
  }
}

}  // namespace RadarRoutes
