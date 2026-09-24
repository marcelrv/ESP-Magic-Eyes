#include "net/web_server.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "api/eyes_routes.h"
#include "api/gesture_routes.h"
#include "api/led_routes.h"
#include "api/ota_routes.h"
#include "api/playmode_routes.h"
#include "api/radar_routes.h"
#include "api/rest_routes.h"
#include "api/servo_routes.h"
#include "api/wifi_routes.h"
#include "net/wifi_manager.h"

namespace {

AsyncWebServer gServer(80);

void handleNotFound(AsyncWebServerRequest *request) {
  if (WifiManager::getMode() == WifiMode::AP_SETUP) {
    // Captive portal trigger: send every unmatched request back to "/"
    // so phones/laptops pop their "sign in to network" prompt (plan §7).
    request->redirect("/");
    return;
  }

  JsonDocument doc;
  doc["error"] = "not_found";
  doc["path"] = request->url();
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->setCode(404);
  serializeJson(doc, *response);
  request->send(response);
}

} // namespace

namespace WebServer {

AsyncWebServer &getServer() { return gServer; }

void begin() {
  // The `data/` directory's contents are uploaded to LittleFS preserving
  // their relative paths (PlatformIO's standard uploadfs/buildfs behavior:
  // data/www/index.html -> FS "/www/index.html", data/sequences/*.json ->
  // FS "/sequences/*.json" — confirmed directly against this project's own
  // `pio run -t buildfs` output, which lists exactly those paths). The
  // frontend's HTML (data/www/**) uses root-relative links/asset paths
  // (e.g. "css/app.css", "control/manual.html") that assume the *www*
  // subtree is served at the URL root — so this serves FS "/www/" at URL
  // "/", not the raw FS root. (Integration-pass fix, Phase 8: earlier
  // phases' frontend work was verified by serving `data/www/` directly
  // with a throwaway local HTTP server, which masked this FS-root vs.
  // "/www/"-subtree mismatch — every page and the captive-portal redirect
  // target ["/", resolving to the default file] 404'd against the real
  // device's actual LittleFS layout. See PROGRESS.md Phase 8 notes.)
  // data/sequences/*.json is intentionally NOT part of this served subtree
  // — it's read directly off LittleFS by firmware (PlayModeManager), not
  // served over HTTP.
  RestRoutes::registerRoutes(gServer);
  OtaRoutes::registerRoutes(gServer);
  ServoRoutes::registerRoutes(gServer);
  EyesRoutes::registerRoutes(gServer);
  GestureRoutes::registerRoutes(gServer);
  PlaymodeRoutes::registerRoutes(gServer);
  RadarRoutes::registerRoutes(gServer);
  LedRoutes::registerRoutes(gServer);
  WifiRoutes::registerRoutes(gServer);

  // Registered AFTER every API route, and filtered off /api/: handlers are
  // tried in registration order, and the static handler's canHandle()
  // probes LittleFS for the path (plus .gz / index.html variants) before
  // declining. Registered first, it did that for every GET /api/* request
  // — ~4 failed flash opens (logged as vfs_api "does not exist" errors)
  // and noticeable latency per status/pose poll.
  gServer.serveStatic("/", LittleFS, "/www/")
      .setDefaultFile("index.html")
      .setFilter([](AsyncWebServerRequest *request) { return !request->url().startsWith("/api/"); });
  gServer.onNotFound(handleNotFound);

  gServer.begin();
  Serial.println("[WebServer] Listening on port 80.");
}

} // namespace WebServer
