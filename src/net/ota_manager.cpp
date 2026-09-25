#include "net/ota_manager.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>

#include <atomic>

#include "net/auth.h"
#include "net/wifi_manager.h"
#include "storage/nvs_store.h"

namespace {

// ArduinoOTA's ESP32 default, set explicitly because the mDNS advert
// below has to name the same port.
constexpr uint16_t kOtaPort = 3232;

bool gEnabledCached = true;
bool gStarted = false;
// Whether the "_arduino" mDNS advert is up. Tracked on its own because mDNS
// may only come up after OTA started (WifiManager retries a failed mDNS
// start); OTA itself deliberately doesn't wait for it, since uploads by IP
// work without mDNS.
bool gAdvertised = false;

// Password bookkeeping, loop() only except gPasswordLocked (read by the
// /api/auth/status handler). gAppliedMd5 is the hash handed to ArduinoOTA,
// which it keeps until reboot; gWantedMd5 is the current admin password's.
bool gAuthLoaded = false;
uint32_t gAuthGeneration = 0;
String gAppliedMd5;
String gWantedMd5;
std::atomic<bool> gPasswordLocked{false};

void refreshPassword() {
  uint32_t generation = Auth::generation();
  if (gAuthLoaded && generation == gAuthGeneration) {
    return;
  }
  gAuthLoaded = true;
  gAuthGeneration = generation;
  gWantedMd5 = Auth::otaPasswordMd5();
  // A stored password can only be replaced by a restart; an empty one can
  // still be set now (setPasswordHash() works while ArduinoOTA has none).
  bool locked = gAppliedMd5.length() > 0 && gAppliedMd5 != gWantedMd5;
  if (locked && !gPasswordLocked) {
    Serial.println("[OTA] Admin password changed — network OTA paused until restart.");
  }
  gPasswordLocked = locked;
}

void startOta() {
  String hostname = NvsStore::getHostname();
  ArduinoOTA.setHostname(hostname.c_str());
  if (gWantedMd5.length() > 0) {
    ArduinoOTA.setPasswordHash(gWantedMd5.c_str());
    gAppliedMd5 = gWantedMd5;
  }

  ArduinoOTA.onStart([]() {
    const char *type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.print("[OTA] Network OTA starting, type=");
    Serial.println(type);
  });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Network OTA complete, rebooting..."); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.print("[OTA] Network OTA error: ");
    Serial.println(static_cast<int>(error));
  });

  // WifiManager owns mDNS (see startMdnsOnce()): with ArduinoOTA's own mDNS
  // on, its end() would take <hostname>.local down with it. handle() only
  // adds or removes the "_arduino" service that IDE network-port discovery
  // uses.
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setPort(kOtaPort);
  ArduinoOTA.begin();
  gStarted = true;
  Serial.print("[OTA] ArduinoOTA started, hostname=");
  Serial.println(hostname);
}

void stopOta() {
  ArduinoOTA.end();
  if (gAdvertised) {
    MDNS.disableArduino();
    gAdvertised = false;
  }
  gStarted = false;
  Serial.println("[OTA] ArduinoOTA stopped.");
}

} // namespace

namespace OtaManager {

void begin() { gEnabledCached = NvsStore::getOtaNetworkEnabled(); }

void handle() {
  bool connected = WifiManager::getMode() == WifiMode::STA_CONNECTED;
  refreshPassword();
  // Running with a password other than the admin one (just set, changed or
  // cleared): stop, and restart below with the new one if ArduinoOTA allows.
  bool passwordStale = gStarted && gAppliedMd5 != gWantedMd5;
  // Auth::storageOk(): with unreadable stored passwords the admin hash is
  // unknown, so starting would mean running ArduinoOTA without a password.
  bool canRun = gEnabledCached && connected && !gPasswordLocked && Auth::storageOk();

  if (canRun && !gStarted) {
    startOta();
  } else if ((!canRun || passwordStale) && gStarted) {
    stopOta();
  }

  if (gStarted && !gAdvertised && WifiManager::mdnsRunning()) {
    MDNS.enableArduino(kOtaPort, gAppliedMd5.length() > 0);
    gAdvertised = true;
  }

  if (gStarted) {
    ArduinoOTA.handle();
  }
}

void setEnabled(bool enabled) {
  gEnabledCached = enabled;
  NvsStore::setOtaNetworkEnabled(enabled);
}

bool isEnabled() { return gEnabledCached; }

bool restartRequiredForPassword() { return gPasswordLocked; }

} // namespace OtaManager
