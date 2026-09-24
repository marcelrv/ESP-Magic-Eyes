#include "api/playmode_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

#include "api/json_helpers.h"
#include "motion/playmode_manager.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

void handleListPlayModes(AsyncWebServerRequest *request) {
  PlayModeManager::PlayModeInfo infos[8];
  size_t count = PlayModeManager::listPlayModes(infos, 8);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = infos[i].id;
    o["label"] = infos[i].label;
    o["description"] = infos[i].description;
  }
  sendJson(request, doc);
}

void handleActiveMode(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["playMode"] = PlayModeManager::getActiveModeId();
  sendJson(request, doc);
}

// POST /api/playmodes/{id}/activate — same hand-rolled path-param
// extraction as gesture_routes.cpp's POST /api/gestures/{id}/trigger; see
// that file's comment for why (no ASYNCWEBSERVER_REGEX).
constexpr const char *kActivatePrefix = "/api/playmodes/";
constexpr const char *kActivateSuffix = "/activate";

void handleActivate(AsyncWebServerRequest *request) {
  String url = request->url();
  size_t prefixLen = strlen(kActivatePrefix);
  size_t suffixLen = strlen(kActivateSuffix);
  if (!url.startsWith(kActivatePrefix) || !url.endsWith(kActivateSuffix) || url.length() <= prefixLen + suffixLen) {
    sendJsonError(request, 404, "not_found");
    return;
  }
  String id = url.substring(prefixLen, url.length() - suffixLen);
  if (id.length() == 0 || id.indexOf('/') >= 0) {
    sendJsonError(request, 404, "not_found");
    return;
  }

  bool ok = PlayModeManager::activate(id.c_str());
  if (!ok) {
    sendJsonError(request, 404, "unknown_play_mode");
    return;
  }

  JsonDocument doc;
  doc["success"] = true;
  doc["playMode"] = PlayModeManager::getActiveModeId();
  sendJson(request, doc);
}

} // namespace

namespace PlaymodeRoutes {

void registerRoutes(AsyncWebServer &server) {
  // Registered before the general "/api/playmodes" GET handler below:
  // ESPAsyncWebServer's plain-string server.on() overload matches
  // "BackwardCompatible" style (^{uri}(/.*)?$ — see ESPAsyncWebServer's
  // WebServer.cpp), so an unqualified GET /api/playmodes handler would
  // also match /api/playmodes/active if it were checked first. Handlers
  // are tried in registration order (first match wins), so the more
  // specific exact route goes first.
  server.on("/api/playmodes/active", HTTP_GET, handleActiveMode);
  server.on("/api/playmodes", HTTP_GET, handleListPlayModes);
  server.on(AsyncURIMatcher::dir("/api/playmodes"), HTTP_POST, handleActivate);
}

} // namespace PlaymodeRoutes
