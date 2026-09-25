// auth.h — two-level password protection for the web UI, the REST API and
// OTA.
//
// Levels:
//   Control — the web UI, manual control, gestures, play modes and status
//             reads. Username "user".
//   Admin   — every /setup/* page and the API behind it (WiFi, servos and
//             calibration, radar and LED config, web OTA, reboot, system
//             config, password changes) plus ArduinoOTA. Username "admin".
//
// Each level is open until its own password is set. Admin credentials are
// accepted wherever Control is required, and an Admin route with no admin
// password falls back to the control password, so admin is never weaker
// than control.
//
// HTTP Digest (the browser's built-in login prompt): the password itself
// never crosses the network, and NVS holds only the HA1 hash (NvsStore
// "auth" namespace). Known limitation: ESPAsyncWebServer doesn't track the
// nonces it issues, so a captured request can be replayed to the same
// method + URI. Plain HTTP has no better option without TLS.
//
// Why there are two enforcement points: ESPAsyncWebServer runs middleware
// only at the *end* of a request, after every onBody/onUpload callback has
// already run. Every POST handler here acts inside onBody, and web OTA
// writes flash inside onUpload — so middleware alone would move servos,
// save settings and flash firmware before answering 401. Hence:
//   - middleware() — registered on the server, answers the 401 challenge
//     (or 429), covering GETs, static files and body-less POSTs;
//   - allowed() — called by JsonHelpers::collectJsonBody() and the OTA
//     upload handler on every chunk; on failure they drop the data without
//     responding, and middleware() then sends the single 401.
//
// Threading: middleware()/allowed()/setPassword() run on the AsyncTCP task,
// clearAll() on loop() (serial console, BOOT button). The hash cache is
// mutex-guarded and handed out as copies.

#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace Auth {

enum class Level : uint8_t {
  Public,  // never needs a password (GET /api/auth/status)
  Control, // "user" (or "admin")
  Admin,   // "admin" (or "user" while no admin password is set)
};

constexpr const char *kRealm = "ESP Magic Eyes";
constexpr const char *kAdminUser = "admin";
constexpr const char *kControlUser = "user";

// Minimum/maximum password length accepted by setPassword().
constexpr size_t kMinPasswordLength = 4;
constexpr size_t kMaxPasswordLength = 64;

// Loads the stored hashes. Call from setup() after NvsStore::begin().
void begin();

// The level a request needs, from its URL alone (see auth.cpp for the map).
Level requiredLevel(AsyncWebServerRequest *request);

// True if the request's credentials satisfy `level` (always true for a
// level with no password set). Doesn't send anything or count failures, so
// it's safe to call once per body/upload chunk.
bool allowed(AsyncWebServerRequest *request, Level level);
inline bool allowed(AsyncWebServerRequest *request) { return allowed(request, requiredLevel(request)); }

// Server-wide middleware (AsyncWebServer::addMiddleware()).
void middleware(AsyncWebServerRequest *request, ArMiddlewareNext next);

// Sets (or, with an empty string, clears) the password for Control or
// Admin. Returns false for Public, an invalid length, or an NVS write
// failure — the previous password then stays in effect.
bool setPassword(Level level, const String &password);

// Clears both passwords (BOOT-button hold, serial "auth reset"). Returns
// false if NVS couldn't be written; the passwords then stay in effect.
bool clearAll();

// True if this level's own password is set.
bool hasPassword(Level level);

// MD5 of the admin password for ArduinoOTA.setPasswordHash(), or "" when
// no admin password is set.
String otaPasswordMd5();

// False while the stored hashes couldn't be loaded: every protected
// request is refused and ArduinoOTA must not start (see auth.cpp).
bool storageOk();

// Bumped on every password change, so OtaManager can notice a new admin
// password without copying the hash on every loop() pass.
uint32_t generation();

} // namespace Auth
