#include "net/wifi_manager.h"

#include <DNSServer.h>
#include <WiFi.h>

#include "storage/nvs_store.h"

namespace {

constexpr uint32_t kStaConnectTimeoutMs = 15000;
constexpr uint8_t kDnsPort = 53;
constexpr size_t kMaxScanResults = 20;

// Fixed AP setup password. Documented here and in PROGRESS.md: the AP is
// WPA2-protected (not open) so casual neighbors don't land on the setup
// portal, but the password is a fixed, publicly-documented default since
// there's no per-device secret to seed it from yet. SSID includes a
// chip-ID suffix so multiple units on the same site are distinguishable.
constexpr const char *kApPasswordDefault = "eyes-setup";

WifiMode gMode = WifiMode::AP_SETUP;
DNSServer gDnsServer;
IPAddress gApIp(192, 168, 4, 1);
String gApSsid;
uint32_t gStaConnectStartMs = 0;
uint32_t gStaFailedAtMs = 0;
bool gApActive = false;

// How long STA_FAILED is left in place (observable via GET
// /api/system/status's wifiMode) before falling back to AP mode.
// Integration-pass fix (Phase 8): handle() used to call startApMode()
// (which immediately overwrites gMode back to AP_SETUP) in the very same
// call that set gMode = STA_FAILED, so STA_FAILED was never actually
// observable between two loop() iterations — data/www/setup/wifi.html's
// "Could not connect... falling back to setup AP" branch (which polls
// GET /api/system/status every 1s specifically watching for
// wifiMode === 'STA_FAILED') was consequently dead code; the connecting
// user only ever saw the generic "still waiting" timeout message. Holding
// STA_FAILED for a couple of poll cycles before transitioning gives that
// UI branch an actual chance to observe it.
constexpr uint32_t kStaFailedHoldMs = 2500;

// Fallback-AP retry: when *saved* credentials failed (router slow to come
// back after a power blip, device restarted by OTA/brownout while the AP
// was busy, ...), the device used to sit in AP setup mode forever — off
// the home network, not even pingable, until someone power-cycled it.
// Now it retries the saved network this often while in fallback AP mode.
// A retry is skipped while a client is connected to the setup AP, since
// an AP_STA connect attempt can hop the AP's channel and disrupt the
// captive portal the user is actively using.
constexpr uint32_t kStaRetryIntervalMs = 60000;
bool gRetrySavedNetwork = false;
uint32_t gNextStaRetryMs = 0;

WifiScanResult gScanResults[kMaxScanResults];
size_t gScanCount = 0;

String buildApSsid() {
  uint64_t chipId = ESP.getEfuseMac();
  char suffix[5];
  // Last two bytes of the efuse MAC, uppercase hex — enough to
  // disambiguate units on the same site without being unwieldy.
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(chipId & 0xFFFF));
  String ssid = "MagicEyes-Setup-";
  ssid += suffix;
  return ssid;
}

void runScanOnce() {
  // Per research finding (plan §6): avoid scanning while the AP is
  // already up. This is called once at boot, before softAP() starts, so
  // it's safe to run synchronously here (one-time startup cost, not a
  // loop()-repeated operation).
  //
  // Note: on a freshly-erased board (no cached WiFi/PHY calibration data)
  // this first WiFi.scanNetworks() call takes a few seconds longer than
  // on later boots since it also has to run a full RF calibration pass —
  // confirmed via real-hardware timestamped logging to take ~3.1s, which
  // is not the cause of the RTCWDT_RTC_RESET boot loop previously seen
  // here (that turned out to be ServoHal::begin()'s attach() of the Aux
  // servo channel — see PROGRESS.md's "Hardware bring-up fixes
  // (post-Phase 8)" section for the full root-cause story).
  int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  gScanCount = 0;
  if (found <= 0) {
    return;
  }
  size_t n = static_cast<size_t>(found);
  if (n > kMaxScanResults) {
    n = kMaxScanResults;
  }
  for (size_t i = 0; i < n; ++i) {
    gScanResults[i].ssid = WiFi.SSID(i);
    gScanResults[i].rssiDbm = WiFi.RSSI(i);
    gScanResults[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  gScanCount = n;
  WiFi.scanDelete();
}

void startApMode() {
  gApActive = true;
  gMode = WifiMode::AP_SETUP;
  gApSsid = buildApSsid();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(gApIp, gApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(gApSsid.c_str(), kApPasswordDefault);

  gDnsServer.start(kDnsPort, "*", gApIp);

  Serial.print("[WiFi] AP setup mode: SSID=");
  Serial.print(gApSsid);
  Serial.print(" password=");
  Serial.print(kApPasswordDefault);
  Serial.print(" ip=");
  Serial.println(WiFi.softAPIP());
}

void stopApMode() {
  if (!gApActive) {
    return;
  }
  gDnsServer.stop();
  WiFi.softAPdisconnect(true);
  gApActive = false;
}

void beginStaConnect(const String &ssid, const String &password) {
  gMode = WifiMode::STA_CONNECTING;
  gStaConnectStartMs = millis();

  // If AP is currently active, stay in WIFI_AP_STA so the captive portal
  // remains reachable while this attempt is in flight; otherwise plain
  // WIFI_STA is enough.
  WiFi.mode(gApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.print("[WiFi] Connecting to SSID '");
  Serial.print(ssid);
  Serial.println("'...");
}

} // namespace

namespace WifiManager {

void begin() {
  WifiCredentials creds = NvsStore::getWifiCredentials();

  if (!creds.valid) {
    Serial.println("[WiFi] No saved credentials — starting AP setup mode.");
    runScanOnce();
    startApMode();
    return;
  }

  beginStaConnect(creds.ssid, creds.password);
}

void handle() {
  if (gApActive) {
    gDnsServer.processNextRequest();
  }

  if (gMode == WifiMode::STA_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      gMode = WifiMode::STA_CONNECTED;
      if (gApActive) {
        stopApMode();
      }
      Serial.print("[WiFi] Connected, IP=");
      Serial.println(WiFi.localIP());
    } else if (millis() - gStaConnectStartMs > kStaConnectTimeoutMs) {
      Serial.println("[WiFi] STA connect timed out.");
      gMode = WifiMode::STA_FAILED;
      gStaFailedAtMs = millis();
      WiFi.disconnect(true);
    }
  } else if (gMode == WifiMode::STA_FAILED) {
    // Held for kStaFailedHoldMs (see its doc comment above) before actually
    // falling back to AP mode, so GET /api/system/status's wifiMode has a
    // real chance to be observed as STA_FAILED by a polling client first.
    if (millis() - gStaFailedAtMs > kStaFailedHoldMs) {
      if (gApActive) {
        // A retry from fallback AP mode failed: the AP is still up, just
        // go back to it (no re-scan — scanning with the AP up is avoided,
        // see runScanOnce()).
        gMode = WifiMode::AP_SETUP;
        WiFi.mode(WIFI_AP);
      } else {
        runScanOnce();
        startApMode();
      }
      gRetrySavedNetwork = NvsStore::getWifiCredentials().valid;
      gNextStaRetryMs = millis() + kStaRetryIntervalMs;
    }
  } else if (gMode == WifiMode::AP_SETUP && gRetrySavedNetwork &&
             static_cast<int32_t>(millis() - gNextStaRetryMs) >= 0) {
    if (WiFi.softAPgetStationNum() > 0) {
      gNextStaRetryMs = millis() + kStaRetryIntervalMs; // portal in use, try later
    } else {
      WifiCredentials creds = NvsStore::getWifiCredentials();
      if (creds.valid) {
        Serial.println("[WiFi] Retrying saved network from fallback AP mode.");
        beginStaConnect(creds.ssid, creds.password);
      } else {
        gRetrySavedNetwork = false;
      }
    }
  }
}

WifiMode getMode() { return gMode; }

const char *getModeName(WifiMode mode) {
  switch (mode) {
    case WifiMode::AP_SETUP:
      return "AP_SETUP";
    case WifiMode::STA_CONNECTING:
      return "STA_CONNECTING";
    case WifiMode::STA_CONNECTED:
      return "STA_CONNECTED";
    case WifiMode::STA_FAILED:
      return "STA_FAILED";
  }
  return "UNKNOWN";
}

String getIpAddress() {
  if (gMode == WifiMode::STA_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (gApActive) {
    return WiFi.softAPIP().toString();
  }
  return String();
}

String getApSsid() { return gApSsid; }

void connectToNetwork(const String &ssid, const String &password) {
  NvsStore::saveWifiCredentials(ssid, password);
  beginStaConnect(ssid, password);
}

void forgetNetwork() {
  NvsStore::clearWifiCredentials();
  Serial.println("[WiFi] Credentials cleared, rebooting into AP setup mode...");
  delay(200); // let the serial message flush before reset
  ESP.restart();
}

const WifiScanResult *getCachedScanResults(size_t &countOut) {
  countOut = gScanCount;
  return gScanResults;
}

} // namespace WifiManager
