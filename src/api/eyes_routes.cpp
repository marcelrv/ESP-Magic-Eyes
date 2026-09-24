#include "api/eyes_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

#include "api/json_helpers.h"
#include "motion/command_queue.h"
#include "motion/eye_pose.h"
#include "motion/motion_task.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

Easing parseEasing(const char *s) {
  if (s != nullptr && strcmp(s, "linear") == 0) return Easing::Linear;
  return Easing::EaseInOut; // default, also covers "easeInOut" and unknown/absent values
}

// Sane bound for a client-supplied durationMs (integration-pass fix, Phase
// 8): `reqDoc["durationMs"] | 200` deserializes as a signed int, and was
// being assigned straight into EyeCommand::durationMs (uint32_t,
// eye_pose.h) with no validation. A negative value (e.g. -1) wraps to
// ~4.29 billion ms (~49.7 days) on that unsigned assignment — motion_task.
// cpp's valueAt() then never sees `elapsed >= durationMs` become true, so
// the axis silently freezes at its start value instead of ever reaching
// the commanded target. Bounded here to the same spirit as this codebase's
// other numeric API validation (servo pulse-width bounds in
// servo_routes.cpp, LED brightness/color bounds in led_routes.cpp) rather
// than left unchecked. 60s is far beyond any real gesture/gaze/eyelid
// command this API is meant for, but generous enough not to constrain any
// legitimate use.
constexpr long kMaxDurationMs = 60000;

// Returns false (and leaves *out unchanged) if `durationMs` is present but
// out of range; otherwise writes the validated value (or `def` if absent)
// to *out and returns true.
bool parseDurationMs(JsonVariantConst reqDoc, uint32_t def, uint32_t *out) {
  long v = reqDoc["durationMs"] | static_cast<long>(def);
  if (v < 0 || v > kMaxDurationMs) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

// --- POST /api/eyes/gaze --------------------------------------------------
void handleGazeBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  if (!reqDoc["pan"].is<float>() && !reqDoc["tilt"].is<float>()) {
    sendJsonError(request, 400, "missing_pan_or_tilt");
    return;
  }

  EyeCommand cmd;
  if (reqDoc["pan"].is<float>()) cmd.panDeg = reqDoc["pan"].as<float>();
  if (reqDoc["tilt"].is<float>()) cmd.tiltDeg = reqDoc["tilt"].as<float>();
  if (!parseDurationMs(reqDoc, 200, &cmd.durationMs)) {
    sendJsonError(request, 400, "durationMs_out_of_range");
    return;
  }
  cmd.easing = parseEasing(reqDoc["easing"] | "easeInOut");
  cmd.source = CommandSource::Manual;

  // Manual command: a new external intent, so any in-flight
  // Gesture/PlayMode keyframe playback should back off (plan §2).
  MotionTask::bumpCommandGeneration();
  CommandQueue::push(cmd);

  JsonDocument doc;
  doc["success"] = true;
  if (cmd.panDeg) doc["pan"] = *cmd.panDeg;
  if (cmd.tiltDeg) doc["tilt"] = *cmd.tiltDeg;
  doc["durationMs"] = cmd.durationMs;
  sendJson(request, doc);
}

// --- POST /api/eyes/eyelids -----------------------------------------------
void handleEyelidsBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  EyeCommand cmd;
  bool any = false;
  if (reqDoc["upperL"].is<float>()) { cmd.lidUpperL = reqDoc["upperL"].as<float>(); any = true; }
  if (reqDoc["lowerL"].is<float>()) { cmd.lidLowerL = reqDoc["lowerL"].as<float>(); any = true; }
  if (reqDoc["upperR"].is<float>()) { cmd.lidUpperR = reqDoc["upperR"].as<float>(); any = true; }
  if (reqDoc["lowerR"].is<float>()) { cmd.lidLowerR = reqDoc["lowerR"].as<float>(); any = true; }
  if (!any) {
    sendJsonError(request, 400, "no_eyelid_fields");
    return;
  }

  if (!parseDurationMs(reqDoc, 150, &cmd.durationMs)) {
    sendJsonError(request, 400, "durationMs_out_of_range");
    return;
  }
  cmd.easing = Easing::EaseInOut;
  cmd.source = CommandSource::Manual;

  MotionTask::bumpCommandGeneration();
  CommandQueue::push(cmd);

  JsonDocument doc;
  doc["success"] = true;
  doc["durationMs"] = cmd.durationMs;
  sendJson(request, doc);
}

// --- GET /api/eyes/pose ----------------------------------------------------
void handleGetPose(AsyncWebServerRequest *request) {
  EyePose pose = MotionTask::getCurrentPose();
  JsonDocument doc;
  doc["panDeg"] = pose.panDeg;
  doc["tiltDeg"] = pose.tiltDeg;
  doc["lidUpperL"] = pose.lidUpperL;
  doc["lidLowerL"] = pose.lidLowerL;
  doc["lidUpperR"] = pose.lidUpperR;
  doc["lidLowerR"] = pose.lidLowerR;
  sendJson(request, doc);
}

} // namespace

namespace EyesRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/eyes/pose", HTTP_GET, handleGetPose);
  server.on("/api/eyes/gaze", HTTP_POST, JsonHelpers::requireBody, nullptr, handleGazeBody);
  server.on("/api/eyes/eyelids", HTTP_POST, JsonHelpers::requireBody, nullptr, handleEyelidsBody);
}

} // namespace EyesRoutes
