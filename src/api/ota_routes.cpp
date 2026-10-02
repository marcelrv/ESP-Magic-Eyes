#include "api/ota_routes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <atomic>
#include <Update.h>

#include "api/json_helpers.h"
#include "net/auth.h"
#include "net/update_manager.h"
#include "version.h"

namespace {

enum class OtaResult {
  NONE,        // no OTA attempted yet since boot
  IN_PROGRESS, // upload/write in flight
  SUCCESS,
  FAILURE,
};

struct OtaStatusState {
  OtaResult result = OtaResult::NONE;
  String type; // "firmware" | "filesystem"
  String errorString;
  size_t bytesWritten = 0;
};

OtaStatusState gStatus;

// Atomic: set on the AsyncTCP task, read by loop() and the update worker.
std::atomic<bool> gRestartPending{false};
uint32_t gRestartAtMs = 0;
// Gives AsyncTCP time to actually flush the HTTP response to the client
// before the reboot tears the connection down.
constexpr uint32_t kRestartDelayMs = 1500;

const char *resultToString(OtaResult result) {
  switch (result) {
    case OtaResult::NONE:
      return "none";
    case OtaResult::IN_PROGRESS:
      return "in_progress";
    case OtaResult::SUCCESS:
      return "success";
    case OtaResult::FAILURE:
      return "failure";
  }
  return "unknown";
}

// Shared multipart upload handler body for both /api/ota/firmware and
// /api/ota/filesystem — only the Update.h command constant + status
// "type" label differ between the two routes.
void handleOtaUploadChunk(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len,
                           bool final, int updateCommand, const char *typeName) {
  (void)filename;

  // Checked on every chunk, before anything touches Update or gStatus: the
  // auth middleware only runs once the whole upload has been received (see
  // net/auth.h), by which point the image would already be flashed. A
  // rejected upload is just drained; the middleware then answers 401.
  if (!Auth::allowed(request, Auth::Level::Admin)) {
    return;
  }

  if (index == 0) {
    // A WiFi update (net/update_manager) holds Update.h; the "abort stale
    // update" step below would kill its download mid-write.
    if (UpdateManager::installing()) {
      gStatus.result = OtaResult::FAILURE;
      gStatus.type = typeName;
      gStatus.errorString = "A WiFi update is in progress";
      gStatus.bytesWritten = 0;
      return;
    }
    UpdateManager::clearInstallStatus();
    gStatus.result = OtaResult::IN_PROGRESS;
    gStatus.type = typeName;
    gStatus.errorString = "";
    gStatus.bytesWritten = 0;

    Serial.print("[OTA] Web upload starting, type=");
    Serial.print(typeName);
    Serial.print(" filename=");
    Serial.println(filename);

    // Multipart body content-length includes headers/boundaries, not just
    // the file payload, so it isn't a reliable size hint here — always let
    // Update.h discover the end via end()/final instead.
    //
    // A previous upload whose client disconnected mid-transfer never got a
    // `final` chunk, so its Update is still "running" — and Update.begin()
    // refuses to start while one is, which would fail every later web OTA
    // until a reboot. Abort that stale one first.
    if (Update.isRunning()) {
      Serial.println("[OTA] Aborting stale, unfinished update.");
      Update.abort();
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateCommand)) {
      gStatus.result = OtaResult::FAILURE;
      gStatus.errorString = Update.errorString();
      Serial.print("[OTA] Update.begin() failed: ");
      Serial.println(gStatus.errorString);
      return;
    }
  }

  // Once a chunk has failed, ignore the remaining chunks for this upload
  // (Update state is already aborted) — just let the request drain.
  if (gStatus.result == OtaResult::FAILURE) {
    return;
  }

  if (len > 0) {
    if (Update.write(data, len) != len) {
      gStatus.result = OtaResult::FAILURE;
      gStatus.errorString = Update.errorString();
      Serial.print("[OTA] Update.write() failed: ");
      Serial.println(gStatus.errorString);
      return;
    }
    gStatus.bytesWritten += len;
  }

  if (final) {
    if (Update.end(true)) {
      gStatus.result = OtaResult::SUCCESS;
      Serial.print("[OTA] Web upload complete, type=");
      Serial.print(typeName);
      Serial.print(" bytes=");
      Serial.println(gStatus.bytesWritten);
      gRestartPending = true;
      gRestartAtMs = millis() + kRestartDelayMs;
    } else {
      gStatus.result = OtaResult::FAILURE;
      gStatus.errorString = Update.errorString();
      Serial.print("[OTA] Update.end() failed: ");
      Serial.println(gStatus.errorString);
    }
  }
}

void handleFirmwareUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len,
                           bool final) {
  handleOtaUploadChunk(request, filename, index, data, len, final, U_FLASH, "firmware");
}

void handleFilesystemUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len,
                             bool final) {
  handleOtaUploadChunk(request, filename, index, data, len, final, U_SPIFFS, "filesystem");
}

// Common "request complete" handler for both upload routes — the actual
// HTTP response has to come from here (not the upload callback), since
// ESPAsyncWebServer only sends what onRequest returns.
void sendOtaResultResponse(AsyncWebServerRequest *request) {
  JsonDocument doc;
  bool ok = gStatus.result == OtaResult::SUCCESS;
  doc["success"] = ok;
  doc["type"] = gStatus.type;
  doc["bytesWritten"] = gStatus.bytesWritten;
  if (!ok) {
    doc["updateError"] = Update.getError();
    doc["error"] = gStatus.errorString;
  } else {
    doc["rebooting"] = true;
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->setCode(ok ? 200 : 400);
  serializeJson(doc, *response);
  request->send(response);
}

void handleOtaStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  // A WiFi update (check-and-install) reports through the same shape, so the
  // page's one status poll covers both it and the manual uploads.
  UpdateManager::InstallStatus wifi = UpdateManager::getInstall();
  if (wifi.result != UpdateManager::InstallStatus::Result::NONE) {
    using R = UpdateManager::InstallStatus::Result;
    doc["result"] = wifi.result == R::IN_PROGRESS ? "in_progress" : wifi.result == R::SUCCESS ? "success" : "failure";
    doc["type"] = wifi.phase;
    doc["source"] = "wifi";
    doc["bytesWritten"] = wifi.bytesWritten;
    doc["totalBytes"] = wifi.totalBytes;
    if (wifi.result == R::FAILURE) {
      doc["error"] = wifi.error;
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
    return;
  }
  doc["result"] = resultToString(gStatus.result);
  doc["type"] = gStatus.type;
  doc["bytesWritten"] = gStatus.bytesWritten;
  if (gStatus.result == OtaResult::FAILURE) {
    doc["error"] = gStatus.errorString;
  }

  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

void putChannel(JsonObject out, const UpdateManager::ChannelInfo &info) {
  out["available"] = info.available;
  out["version"] = info.version;
  out["newer"] = info.newer;
  if (!info.sha.isEmpty()) out["sha"] = info.sha;
  if (!info.available && !info.error.isEmpty()) out["error"] = info.error;
}

// POST /api/ota/check — starts the background check. No body, so the auth
// middleware (admin, /api/ota/ prefix) is the only gate, as for /reboot.
void handleCheckStart(AsyncWebServerRequest *request) {
  String error;
  if (!UpdateManager::startCheck(error)) {
    JsonHelpers::sendJsonError(request, 409, error.c_str());
    return;
  }
  JsonDocument doc;
  doc["success"] = true;
  JsonHelpers::sendJson(request, doc);
}

// GET /api/ota/check — state of the last/ongoing check, plus what is installed.
void handleCheckResult(AsyncWebServerRequest *request) {
  using State = UpdateManager::CheckResult::State;
  UpdateManager::CheckResult r = UpdateManager::getCheck();
  JsonDocument doc;
  doc["state"] = r.state == State::CHECKING ? "checking" : r.state == State::DONE ? "done" : r.state == State::ERROR ? "error" : "idle";
  if (r.state == State::ERROR) doc["error"] = r.error;
  JsonObject installed = doc["installed"].to<JsonObject>();
  installed["version"] = FIRMWARE_VERSION;
  installed["sha"] = FIRMWARE_GIT_SHA;
  putChannel(doc["stable"].to<JsonObject>(), r.stable);
  putChannel(doc["latest"].to<JsonObject>(), r.latest);
  JsonHelpers::sendJson(request, doc);
}

// POST /api/ota/install {"channel": "stable" | "latest"}
void handleInstallBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return;
  }
  UpdateManager::Channel channel;
  if (!UpdateManager::parseChannel(reqDoc["channel"] | "", channel)) {
    JsonHelpers::sendJsonError(request, 400, "channel must be \"stable\" or \"latest\"");
    return;
  }
  String error;
  if (!UpdateManager::startInstall(channel, error)) {
    JsonHelpers::sendJsonError(request, 409, error.c_str());
    return;
  }
  JsonDocument doc;
  doc["success"] = true;
  JsonHelpers::sendJson(request, doc);
}

} // namespace

namespace OtaRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/ota/check", HTTP_POST, handleCheckStart);
  server.on("/api/ota/check", HTTP_GET, handleCheckResult);
  server.on("/api/ota/install", HTTP_POST, JsonHelpers::requireBody, nullptr, handleInstallBody);
  server.on("/api/ota/firmware", HTTP_POST, sendOtaResultResponse, handleFirmwareUpload);
  server.on("/api/ota/filesystem", HTTP_POST, sendOtaResultResponse, handleFilesystemUpload);
  server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
}

bool restartPending() { return gRestartPending; }

void handle() {
  if (gRestartPending && millis() >= gRestartAtMs) {
    gRestartPending = false;
    Serial.println("[OTA] Restarting after successful web OTA update...");
    ESP.restart();
  }
}

} // namespace OtaRoutes
