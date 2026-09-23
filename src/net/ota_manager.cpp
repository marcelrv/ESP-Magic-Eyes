#include "net/ota_manager.h"

#include <ArduinoOTA.h>

#include "net/wifi_manager.h"
#include "storage/nvs_store.h"

namespace {

bool gEnabledCached = true;
bool gStarted = false;

void startOta() {
  String hostname = NvsStore::getHostname();
  ArduinoOTA.setHostname(hostname.c_str());

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

  ArduinoOTA.begin();
  gStarted = true;
  Serial.print("[OTA] ArduinoOTA started, hostname=");
  Serial.println(hostname);
}

void stopOta() {
  ArduinoOTA.end();
  gStarted = false;
  Serial.println("[OTA] ArduinoOTA stopped.");
}

} // namespace

namespace OtaManager {

void begin() { gEnabledCached = NvsStore::getOtaNetworkEnabled(); }

void handle() {
  bool connected = WifiManager::getMode() == WifiMode::STA_CONNECTED;

  if (gEnabledCached && connected && !gStarted) {
    startOta();
  } else if ((!gEnabledCached || !connected) && gStarted) {
    stopOta();
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

} // namespace OtaManager
