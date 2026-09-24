#include "api/ota_routes.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

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

bool gRestartPending = false;
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
  (void)request;
  (void)filename;

  if (index == 0) {
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

} // namespace

namespace OtaRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/ota/firmware", HTTP_POST, sendOtaResultResponse, handleFirmwareUpload);
  server.on("/api/ota/filesystem", HTTP_POST, sendOtaResultResponse, handleFilesystemUpload);
  server.on("/api/ota/status", HTTP_GET, handleOtaStatus);
}

void handle() {
  if (gRestartPending && millis() >= gRestartAtMs) {
    gRestartPending = false;
    Serial.println("[OTA] Restarting after successful web OTA update...");
    ESP.restart();
  }
}

} // namespace OtaRoutes
