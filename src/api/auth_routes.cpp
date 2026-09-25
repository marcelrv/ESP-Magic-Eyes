#include "api/auth_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstring>

#include "api/json_helpers.h"
#include "net/auth.h"
#include "net/ota_manager.h"

namespace {

void fillStatus(JsonDocument &doc) {
  bool control = Auth::hasPassword(Auth::Level::Control);
  bool admin = Auth::hasPassword(Auth::Level::Admin);
  doc["controlPassword"] = control;
  doc["adminPassword"] = admin;
  doc["adminProtected"] = admin || control;
  doc["otaRestartRequired"] = OtaManager::restartRequiredForPassword();
}

void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  fillStatus(doc);
  JsonHelpers::sendJson(request, doc);
}

void handlePasswordBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, not authorized, or an error response was already sent
  }

  const char *levelName = reqDoc["level"] | "";
  Auth::Level level;
  if (strcmp(levelName, "control") == 0) {
    level = Auth::Level::Control;
  } else if (strcmp(levelName, "admin") == 0) {
    level = Auth::Level::Admin;
  } else {
    JsonHelpers::sendJsonError(request, 400, "invalid_level");
    return;
  }
  if (!reqDoc["password"].is<const char *>()) {
    JsonHelpers::sendJsonError(request, 400, "missing_password");
    return;
  }
  String password = reqDoc["password"].as<const char *>();
  if (password.length() > 0 &&
      (password.length() < Auth::kMinPasswordLength || password.length() > Auth::kMaxPasswordLength)) {
    JsonHelpers::sendJsonError(request, 400, "invalid_password_length");
    return;
  }
  if (!Auth::setPassword(level, password)) {
    JsonHelpers::sendJsonError(request, 500, "save_failed");
    return;
  }

  JsonDocument doc;
  doc["success"] = true;
  fillStatus(doc);
  JsonHelpers::sendJson(request, doc);
}

} // namespace

namespace AuthRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/auth/status", HTTP_GET, handleStatus);
  server.on("/api/auth/password", HTTP_POST, JsonHelpers::requireBody, nullptr, handlePasswordBody);
}

} // namespace AuthRoutes
