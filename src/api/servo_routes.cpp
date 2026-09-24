#include "api/servo_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "api/json_helpers.h"
#include "hal/servo_hal.h"
#include "motion/motion_task.h"
#include "pin_map.h"
#include "storage/nvs_store.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

constexpr size_t kServoCount = static_cast<size_t>(ServoId::Count);

// Sane absolute pulse-width bound for POST /api/servos/config and
// POST /api/servos/test, per the task spec (400-2600us). Note the
// installed ESP32Servo (see hal/servo_hal.h) internally clamps to its own
// tighter 500-2500us absolute range regardless of what's saved/sent here
// — a value accepted by this validation but outside 500-2500 will still
// end up clamped by the library at attach()/writeMicroseconds() time.
// Documented deviation, not treated as a bug: this API-level bound is
// deliberately the spec'd one, ESP32Servo's is a second, tighter floor
// underneath it.
constexpr int kAbsMinUs = 400;
constexpr int kAbsMaxUs = 2600;

// How long one calibration hold request keeps MotionTask off the servos
// (see MotionTask::setCalibrationHold()). The calibration page refreshes
// it every ~10s and on every raw pulse it sends.
constexpr uint32_t kCalibrationHoldMs = 30000;

const char *servoKind(ServoId id) {
  if (isLidServo(id)) return "lid";
  if (id == ServoId::Pan || id == ServoId::Tilt) return "gaze";
  return "aux";
}

void addCalibrationFields(JsonObject o, const ServoCalibration &cal) {
  o["minUs"] = cal.minUs;
  o["centerUs"] = cal.centerUs;
  o["maxUs"] = cal.maxUs;
  o["closedUs"] = cal.closedUs;
  o["openUs"] = cal.openUs;
  o["halfUs"] = cal.halfUs;
  o["inverted"] = cal.inverted;
}

// --- GET /api/servos/config ------------------------------------------------

void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (size_t i = 0; i < kServoCount; ++i) {
    ServoId id = static_cast<ServoId>(i);
    ServoCalibration cal = NvsStore::getServoCalibration(id);
    JsonObject o = arr.add<JsonObject>();
    o["servoId"] = i;
    o["name"] = ServoHal::servoIdName(id);
    o["kind"] = servoKind(id);
    addCalibrationFields(o, cal);
  }
  sendJson(request, doc);
}

// --- POST /api/servos/config -------------------------------------------
void handlePostConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  if (!reqDoc["servoId"].is<int>()) {
    sendJsonError(request, 400, "missing_servoId");
    return;
  }
  int servoIdInt = reqDoc["servoId"].as<int>();
  if (servoIdInt < 0 || static_cast<size_t>(servoIdInt) >= kServoCount) {
    sendJsonError(request, 400, "servoId_out_of_range");
    return;
  }

  ServoId id = static_cast<ServoId>(servoIdInt);
  bool lid = isLidServo(id);

  // Lids need min/max/closed/open (halfUs optional, defaults to halfway;
  // center is unused and derived); every other servo needs min/center/max.
  if (!reqDoc["minUs"].is<int>() || !reqDoc["maxUs"].is<int>()) {
    sendJsonError(request, 400, "missing_pulse_fields");
    return;
  }
  if (lid ? (!reqDoc["closedUs"].is<int>() || !reqDoc["openUs"].is<int>()) : !reqDoc["centerUs"].is<int>()) {
    sendJsonError(request, 400, "missing_pulse_fields");
    return;
  }
  int minUs = reqDoc["minUs"].as<int>();
  int maxUs = reqDoc["maxUs"].as<int>();
  if (minUs < kAbsMinUs || maxUs > kAbsMaxUs) {
    sendJsonError(request, 400, "pulse_out_of_absolute_bounds");
    return;
  }

  ServoCalibration cal = NvsStore::getServoCalibration(id); // keeps fields this kind doesn't use
  cal.minUs = static_cast<uint16_t>(minUs);
  cal.maxUs = static_cast<uint16_t>(maxUs);

  if (lid) {
    int closedUs = reqDoc["closedUs"].as<int>();
    int openUs = reqDoc["openUs"].as<int>();
    if (!(minUs < maxUs) || closedUs < minUs || closedUs > maxUs || openUs < minUs || openUs > maxUs) {
      sendJsonError(request, 400, "invalid_range_ordering"); // requires minUs <= closedUs, openUs <= maxUs
      return;
    }
    if (closedUs == openUs) {
      sendJsonError(request, 400, "closed_equals_open");
      return;
    }
    int halfUs = reqDoc["halfUs"].is<int>() ? reqDoc["halfUs"].as<int>() : (closedUs + openUs) / 2;
    // Must lie strictly between closed and open, or the mapping would
    // reverse direction partway through.
    if (!((closedUs < halfUs && halfUs < openUs) || (openUs < halfUs && halfUs < closedUs))) {
      sendJsonError(request, 400, "half_not_between_closed_and_open");
      return;
    }
    cal.closedUs = static_cast<uint16_t>(closedUs);
    cal.openUs = static_cast<uint16_t>(openUs);
    cal.halfUs = static_cast<uint16_t>(halfUs);
    cal.centerUs = static_cast<uint16_t>((closedUs + openUs) / 2);
  } else {
    int centerUs = reqDoc["centerUs"].as<int>();
    if (!(minUs < centerUs && centerUs < maxUs)) {
      sendJsonError(request, 400, "invalid_range_ordering"); // requires minUs < centerUs < maxUs
      return;
    }
    cal.centerUs = static_cast<uint16_t>(centerUs);
    // Omitted "inverted" keeps the currently saved value.
    cal.inverted = reqDoc["inverted"] | cal.inverted;
  }

  if (!NvsStore::setServoCalibration(id, cal)) {
    sendJsonError(request, 500, "nvs_write_failed");
    return;
  }
  ServoHal::reapplyCalibration(id); // takes effect immediately, no reboot

  JsonDocument doc;
  doc["success"] = true;
  doc["servoId"] = servoIdInt;
  addCalibrationFields(doc.as<JsonObject>(), cal);
  sendJson(request, doc);
}

// --- POST /api/servos/test ----------------------------------------------

void handlePostTestBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  if (!reqDoc["servoId"].is<int>() || !reqDoc["pulseUs"].is<int>()) {
    sendJsonError(request, 400, "missing_fields");
    return;
  }
  int servoIdInt = reqDoc["servoId"].as<int>();
  int pulseUs = reqDoc["pulseUs"].as<int>();
  if (servoIdInt < 0 || static_cast<size_t>(servoIdInt) >= kServoCount) {
    sendJsonError(request, 400, "servoId_out_of_range");
    return;
  }
  if (pulseUs < kAbsMinUs || pulseUs > kAbsMaxUs) {
    sendJsonError(request, 400, "pulse_out_of_absolute_bounds");
    return;
  }

  // Raw and uneased, bypassing CommandQueue/MotionTask. Also (re)starts
  // the calibration hold so MotionTask's 50Hz tick doesn't overwrite this
  // pulse, and writes past the saved min/max are allowed (only the
  // library's absolute range applies) so limits can be explored.
  MotionTask::setCalibrationHold(kCalibrationHoldMs);
  ServoHal::setRawPulseUs(static_cast<ServoId>(servoIdInt), static_cast<uint16_t>(pulseUs));

  JsonDocument doc;
  doc["success"] = true;
  doc["servoId"] = servoIdInt;
  doc["pulseUs"] = pulseUs;
  sendJson(request, doc);
}

// --- GET/POST /api/servos/hold -------------------------------------------
// {"enabled": true} starts/refreshes the calibration hold, {"enabled":
// false} releases it (motion eases back to the rest pose).

void sendHoldState(AsyncWebServerRequest *request) {
  JsonDocument doc;
  uint32_t remaining = MotionTask::calibrationHoldRemainingMs();
  doc["success"] = true;
  doc["held"] = remaining > 0;
  doc["expiresInMs"] = remaining;
  sendJson(request, doc);
}

void handlePostHoldBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }
  if (!reqDoc["enabled"].is<bool>()) {
    sendJsonError(request, 400, "missing_enabled");
    return;
  }
  if (reqDoc["enabled"].as<bool>()) {
    MotionTask::setCalibrationHold(kCalibrationHoldMs);
  } else {
    MotionTask::releaseCalibrationHold();
  }
  sendHoldState(request);
}

// --- POST /api/servos/pose -------------------------------------------------
// {"pulses": [{"servoId": n, "pulseUs": us}, ...]} — several raw writes in
// one request, so a reference pose (e.g. straight ahead + lids closed)
// lands on all servos together. Validated fully before anything moves.

void handlePostPoseBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }
  JsonArrayConst pulses = reqDoc["pulses"].as<JsonArrayConst>();
  if (pulses.isNull() || pulses.size() == 0) {
    sendJsonError(request, 400, "missing_pulses");
    return;
  }
  for (JsonObjectConst p : pulses) {
    if (!p["servoId"].is<int>() || !p["pulseUs"].is<int>()) {
      sendJsonError(request, 400, "missing_fields");
      return;
    }
    int servoIdInt = p["servoId"].as<int>();
    int pulseUs = p["pulseUs"].as<int>();
    if (servoIdInt < 0 || static_cast<size_t>(servoIdInt) >= kServoCount) {
      sendJsonError(request, 400, "servoId_out_of_range");
      return;
    }
    if (pulseUs < kAbsMinUs || pulseUs > kAbsMaxUs) {
      sendJsonError(request, 400, "pulse_out_of_absolute_bounds");
      return;
    }
  }

  MotionTask::setCalibrationHold(kCalibrationHoldMs);
  for (JsonObjectConst p : pulses) {
    ServoHal::setRawPulseUs(static_cast<ServoId>(p["servoId"].as<int>()),
                            static_cast<uint16_t>(p["pulseUs"].as<int>()));
  }

  JsonDocument doc;
  doc["success"] = true;
  doc["count"] = pulses.size();
  sendJson(request, doc);
}

} // namespace

namespace ServoRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/servos/config", HTTP_GET, handleGetConfig);
  server.on("/api/servos/config", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostConfigBody);
  server.on("/api/servos/test", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostTestBody);
  server.on("/api/servos/hold", HTTP_GET, sendHoldState);
  server.on("/api/servos/hold", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostHoldBody);
  server.on("/api/servos/pose", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePostPoseBody);
}

} // namespace ServoRoutes
