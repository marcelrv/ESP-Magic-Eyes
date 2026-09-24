#include "api/radar_routes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

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

// Radar type <-> API string ("none" | "ld2420" | "ld2450").
const char *radarTypeId(RadarType type) {
  switch (type) {
    case RadarType::LD2420:
      return "ld2420";
    case RadarType::LD2450:
      return "ld2450";
    case RadarType::None:
      break;
  }
  return "none";
}

bool parseRadarType(const char *id, RadarType *out) {
  if (id == nullptr) return false;
  if (strcmp(id, "none") == 0) { *out = RadarType::None; return true; }
  if (strcmp(id, "ld2420") == 0) { *out = RadarType::LD2420; return true; }
  if (strcmp(id, "ld2450") == 0) { *out = RadarType::LD2450; return true; }
  return false;
}

// --- GET /api/radar/config -----------------------------------------------
// `type` is the saved setting; `activeType` is what RadarTask started with
// at boot (they differ only between a save and the reboot it triggers).
void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["type"] = radarTypeId(NvsStore::getRadarType());
  doc["activeType"] = radarTypeId(RadarTask::getType());
  doc["baudRate"] = NvsStore::getRadarBaudRate();
  sendJson(request, doc);
}

// --- POST /api/radar/config ------------------------------------------------
// {"type": "none"|"ld2420"|"ld2450", "baudRate": n} — either field may be
// omitted, but not both. Only saves + reboots — RadarTask::begin() picks
// the sensor and opens the UART once at boot (radar_task.cpp), so a new
// type or baud can't take effect without a fresh begin() call.
// Deferred-restart pattern matches
// ota_routes.cpp/rest_routes.cpp: respond first, let AsyncTCP flush that
// response, then ESP.restart().
constexpr uint32_t kMinBaud = 1200;
constexpr uint32_t kMaxBaud = 1000000;

bool gRestartPending = false;
uint32_t gRestartAtMs = 0;
constexpr uint32_t kRestartDelayMs = 500;


void handlePostConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  bool hasType = !reqDoc["type"].isNull();
  bool hasBaud = !reqDoc["baudRate"].isNull();
  if (!hasType && !hasBaud) {
    sendJsonError(request, 400, "missing_type_or_baudRate");
    return;
  }
  RadarType type = NvsStore::getRadarType();
  if (hasType && !parseRadarType(reqDoc["type"].as<const char *>(), &type)) {
    sendJsonError(request, 400, "invalid_type");
    return;
  }
  uint32_t baudRate = NvsStore::getRadarBaudRate();
  if (hasBaud) {
    if (!reqDoc["baudRate"].is<uint32_t>()) {
      sendJsonError(request, 400, "invalid_baudRate");
      return;
    }
    baudRate = reqDoc["baudRate"].as<uint32_t>();
    if (baudRate < kMinBaud || baudRate > kMaxBaud) {
      sendJsonError(request, 400, "baudRate_out_of_range");
      return;
    }
  }

  // Validated in full before anything is saved.
  if (hasType) NvsStore::setRadarType(type);
  if (hasBaud) NvsStore::setRadarBaudRate(baudRate);

  JsonDocument doc;
  doc["success"] = true;
  doc["type"] = radarTypeId(type);
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
