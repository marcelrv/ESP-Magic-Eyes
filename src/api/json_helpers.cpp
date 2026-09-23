#include "api/json_helpers.h"

namespace JsonHelpers {

void sendJson(AsyncWebServerRequest *request, const JsonDocument &doc) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  serializeJson(doc, *response);
  request->send(response);
}

void sendJsonError(AsyncWebServerRequest *request, int code, const char *error) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = error;
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->setCode(code);
  serializeJson(doc, *response);
  request->send(response);
}

void requireBody(AsyncWebServerRequest *request) {
  if (request->contentLength() == 0) {
    sendJsonError(request, 400, "missing_body");
  }
  // Else: a body was sent, so this request's onBody handler already parsed
  // it and sent the real response (see header note) — nothing to do here.
}

} // namespace JsonHelpers
