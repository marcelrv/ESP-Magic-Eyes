// api/json_helpers.h — small shared JSON response helpers (integration-pass
// consolidation, Phase 8).
//
// sendJson()/sendJsonError() had been independently hand-duplicated
// (near-verbatim) in eyes_routes.cpp, servo_routes.cpp, gesture_routes.cpp,
// playmode_routes.cpp, led_routes.cpp, wifi_routes.cpp, and rest_routes.cpp
// — the architecture plan's §9 file layout always named a single
// api/json_helpers.* for this, but no phase 1-7 actually created it.
// Consolidated here rather than left duplicated seven times over, per
// Phase 8's cross-phase-consistency review (see PROGRESS.md).
//
// requireBody() closes a real bug found in the integration pass: every
// route previously registered as
//   server.on(path, method, [](AsyncWebServerRequest *r){ (void)r; },
//             nullptr, bodyHandler)
// never responds at all if the client POSTs with no body / Content-Length:
// 0 — ESPAsyncWebServer's request parser skips straight from
// PARSE_REQ_HEADERS to PARSE_REQ_END without ever invoking the onBody
// callback in that case (confirmed directly against
// .pio/libdeps/<env>/ESPAsyncWebServer/src/WebRequest.cpp's header-parse
// state machine: `_contentLength || _chunkedParseState != CHUNK_NONE` picks
// PARSE_REQ_BODY vs. going straight to PARSE_REQ_END and invoking
// onRequest immediately). onRequest is *always* the last callback invoked
// for a given request — after every onBody call, if any — so it's the
// correct, single place to catch "onBody never ran" and respond instead of
// leaving the request hanging until the client's own timeout. This
// project's own frontend (data/www/js/api.js) never triggers this in
// practice (`Api.post()` always sends at least "{}"), but a bare
// `curl -X POST <url>` or any third-party API/voice/AI integration client
// easily could — see PROGRESS.md Phase 8 notes for the full writeup.

#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

namespace JsonHelpers {

// Sends `doc` as a 200 application/json response.
void sendJson(AsyncWebServerRequest *request, const JsonDocument &doc);

// Sends {"success": false, "error": error} with the given HTTP status as an
// application/json response.
void sendJsonError(AsyncWebServerRequest *request, int code, const char *error);

// Use as the `onRequest` callback for a body-driven POST route, in place of
// a no-op lambda. onRequest always fires after every onBody call for the
// request has already completed (see header note above): if a body WAS
// sent, the route's onBody handler has already sent the real response by
// the time this runs, so this is a no-op in that case (checked via
// `request->contentLength() == 0`, which is 0 only when no body was ever
// declared/parsed for the request). If no body was sent at all, onBody
// never ran and nothing has responded yet, so this sends a 400
// "missing_body" response instead of leaving the request hanging forever.
void requireBody(AsyncWebServerRequest *request);

} // namespace JsonHelpers
