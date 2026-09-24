#include "api/json_helpers.h"

#include <cstdlib>
#include <cstring>

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

bool collectJsonBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total,
                     JsonDocument &doc) {
  if (request->getResponse() != nullptr) {
    return false; // already answered (e.g. rejected on an earlier chunk)
  }
  if (total == 0) {
    // Transfer-Encoding: chunked — no Content-Length, so the end of the
    // body can't be recognised from index/total. Every client of this API
    // sends small bodies with a Content-Length.
    sendJsonError(request, 411, "length_required");
    return false;
  }
  if (total > kMaxJsonBodyBytes) {
    sendJsonError(request, 413, "body_too_large");
    return false;
  }

  // Per-request buffer in _tempObject (freed by the library, with free(),
  // when the request is destroyed) — never shared between requests, so
  // concurrent clients can't interleave chunks into each other's body.
  char *buf = static_cast<char *>(request->_tempObject);
  if (index == 0 && buf == nullptr) {
    buf = static_cast<char *>(malloc(total + 1));
    if (buf == nullptr) {
      sendJsonError(request, 500, "out_of_memory");
      return false;
    }
    request->_tempObject = buf;
  }
  if (buf == nullptr || index + len > total) {
    sendJsonError(request, 400, "invalid_body");
    return false;
  }
  memcpy(buf + index, data, len);
  if (index + len != total) {
    return false; // wait for the remaining chunk(s)
  }
  buf[total] = '\0';

  if (deserializeJson(doc, buf, total)) {
    sendJsonError(request, 400, "invalid_json");
    return false;
  }
  return true;
}

void requireBody(AsyncWebServerRequest *request) {
  if (request->getResponse() == nullptr) {
    sendJsonError(request, 400, "missing_body");
  }
  // Else: the route's onBody handler (or collectJsonBody()) already set the
  // response — send() only stores it; the library transmits it once, after
  // this callback returns. Checking the response rather than
  // contentLength() also covers chunked uploads, where contentLength() is 0.
}

} // namespace JsonHelpers
