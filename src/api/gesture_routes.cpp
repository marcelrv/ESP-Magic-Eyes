#include "api/gesture_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

#include "api/json_helpers.h"
#include "motion/eye_pose.h"
#include "motion/gesture_engine.h"
#include "motion/motion_task.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

void handleListGestures(AsyncWebServerRequest *request) {
  GestureEngine::GestureInfo infos[16];
  size_t count = GestureEngine::listGestures(infos, 16);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < count; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = infos[i].id;
    o["label"] = infos[i].label;
  }
  sendJson(request, doc);
}

// POST /api/gestures/{id}/trigger — ESPAsyncWebServer's non-regex path
// matchers (see AsyncURIMatcher in ESPAsyncWebServer.h) don't support
// {param} capture without the heavier ASYNCWEBSERVER_REGEX build flag
// (pulls in <regex>, notably flash-costly on an embedded target already
// being watched for flash headroom — see PROGRESS.md). Registered instead
// against AsyncURIMatcher::dir("/api/gestures") (matches any
// "/api/gestures/<...>" request) and the {id} segment is pulled out of
// request->url() by hand below, in the same spirit as this codebase's
// existing hand-rolled JSON body parsing.
constexpr const char *kTriggerPrefix = "/api/gestures/";
constexpr const char *kTriggerSuffix = "/trigger";

void handleTrigger(AsyncWebServerRequest *request) {
  String url = request->url();
  size_t prefixLen = strlen(kTriggerPrefix);
  size_t suffixLen = strlen(kTriggerSuffix);
  if (!url.startsWith(kTriggerPrefix) || !url.endsWith(kTriggerSuffix) || url.length() <= prefixLen + suffixLen) {
    sendJsonError(request, 404, "not_found");
    return;
  }
  String id = url.substring(prefixLen, url.length() - suffixLen);
  if (id.length() == 0 || id.indexOf('/') >= 0) {
    sendJsonError(request, 404, "not_found");
    return;
  }

  // API-triggered gesture: a new external intent (plan §2) — bump before
  // GestureEngine captures its playback-baseline generation.
  MotionTask::bumpCommandGeneration();
  bool ok = GestureEngine::trigger(id.c_str(), CommandSource::Gesture);
  if (!ok) {
    sendJsonError(request, 404, "unknown_gesture");
    return;
  }

  JsonDocument doc;
  doc["success"] = true;
  doc["id"] = id;
  sendJson(request, doc);
}

} // namespace

namespace GestureRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/gestures", HTTP_GET, handleListGestures);
  server.on(AsyncURIMatcher::dir("/api/gestures"), HTTP_POST, handleTrigger);
}

} // namespace GestureRoutes
