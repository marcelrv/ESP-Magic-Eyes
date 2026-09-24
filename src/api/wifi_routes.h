// wifi_routes.h — /api/wifi/* route registration.
//
// PLAN-GAP FIX (Phase 7): the architecture plan's §4 API spec lists
// GET /api/wifi/scan, POST /api/wifi/connect, and POST /api/wifi/forget,
// but no phase 0-6 was ever actually assigned to wire these HTTP routes up
// — WifiManager::connectToNetwork()/forgetNetwork()/getCachedScanResults()
// have existed since Phase 1 (see net/wifi_manager.h) but had no route in
// front of them. Phase 7's setup/wifi.html fundamentally needs them, so
// this module closes that gap now. See PROGRESS.md Phase 7 notes for the
// full writeup — this is a plan omission being closed, not a deviation.
//
// Routes registered:
//   GET  /api/wifi/scan    — cached scan results
//                             (WifiManager::getCachedScanResults()). The
//                             scan itself only ever runs once, at boot,
//                             before AP mode starts (see wifi_manager.cpp)
//                             — this route does not trigger a fresh scan
//                             and may return an empty/stale list; the
//                             setup UI must offer a manual SSID entry
//                             fallback.
//   POST /api/wifi/connect {ssid, password} — saves credentials to NVS and
//                             starts a non-blocking STA connect attempt
//                             (WifiManager::connectToNetwork()). Responds
//                             immediately with {success, status:
//                             "connecting"} — STA connection isn't
//                             instant, so the actual outcome (connected/
//                             failed) must be polled via
//                             GET /api/system/status's wifiMode field
//                             afterward (STA_CONNECTING -> STA_CONNECTED
//                             or STA_FAILED, bounded ~15s timeout).
//   POST /api/wifi/forget  — clears saved credentials and reboots into AP
//                             setup mode
//                             (WifiManager::forgetNetwork(), which itself
//                             does delay(200)+ESP.restart()). Responds to
//                             the request FIRST, then defers the actual
//                             forgetNetwork() call briefly via handle()
//                             (polled from loop()) so AsyncTCP has time to
//                             flush the HTTP response before the reboot —
//                             mirrors ota_routes.cpp's deferred-restart
//                             pattern for the same reason.

#pragma once

class AsyncWebServer;

namespace WifiRoutes {

// Registers the routes above. Does not call server.begin() — that's
// WebServer::begin()'s job.
void registerRoutes(AsyncWebServer &server);

// Must be polled every loop() iteration (non-blocking). Performs the
// deferred WifiManager::forgetNetwork() call after POST /api/wifi/forget,
// once enough time has passed for the HTTP response to flush.
void handle();

} // namespace WifiRoutes
