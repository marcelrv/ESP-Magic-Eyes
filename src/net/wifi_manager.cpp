#include "net/wifi_manager.h"

#include <DNSServer.h>
#include <WiFi.h>

#include <atomic>

#include <esp_wifi.h>

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

// Credentials from POST /api/wifi/connect, held in RAM until the attempt
// actually succeeds (persisted in handle() on WL_CONNECTED, discarded on
// timeout). Saving them up front meant a mistyped password on an already-
// connected device overwrote the last *working* credentials, and the
// fallback retry then kept retrying the bad ones.
bool gHasPendingCreds = false;
String gPendingSsid;
String gPendingPassword;

// Set by the WiFi event task when the *current* connect attempt gets an IP;
// cleared at the start of every attempt (beginStaConnect()). handle() must
// not trust WiFi.status() alone: right after WiFi.begin() on an
// already-connected device, status() still reports the OLD connection as
// WL_CONNECTED for a few loop() passes (the disconnect is processed
// asynchronously). Trusting it saved a mistyped password to NVS as
// "working" and locked the device out of the network.
std::atomic<bool> gGotIpThisAttempt{false};

// A connect attempt only counts as successful once staLinkUp() has held
// continuously for this long. Even with gGotIpThisAttempt, real hardware
// showed a momentary "connected" right after switching to a wrong password
// (before the router's 4-way handshake rejected it), so a single sample is
// not trusted.
constexpr uint32_t kStaStableMs = 3000;
// Real millis() timestamps plus separate "timing" flags — not a 0 == idle
// sentinel: `millis() | 1` could land 1ms in the future, and a same-ms
// `millis() - since` then wrapped to ~4 billion, instantly satisfying the
// stability/loss checks.
bool gLinkUpTiming = false; // link continuously up since gLinkUpSinceMs
uint32_t gLinkUpSinceMs = 0;

// Once connected, how long the link may stay down (the WiFi driver's own
// auto-reconnect keeps trying meanwhile) before this is treated as a
// failure: fall back to the setup AP and retry the saved network from
// there every kStaRetryIntervalMs. Without it, a link that dropped for
// good (router gone, credentials changed) left the device "connected"
// but unreachable forever.
constexpr uint32_t kStaLostTimeoutMs = 30000;
bool gLinkDownTiming = false; // link continuously down since gLinkDownSinceMs
uint32_t gLinkDownSinceMs = 0;

// True only when the station is genuinely associated and has an IP for
// the current attempt: WiFi.status(), our per-attempt IP flag, AND the
// driver's own association state all agree.
bool staLinkUp() {
  if (!gGotIpThisAttempt || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] STA associated with AP.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gGotIpThisAttempt = true;
      Serial.printf("[WiFi] STA got IP %s.\n", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      gGotIpThisAttempt = false;
      auto reason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
      Serial.printf("[WiFi] STA disconnected, reason %u (%s)\n", static_cast<unsigned>(reason),
                    WiFi.disconnectReasonName(reason));
      break;
    }
    default:
      break;
  }
}

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

  Serial.print("[WiFi] Connecting to SSID '");
  Serial.print(ssid);
  Serial.println("'...");

  // Drop any existing connection explicitly first, then start counting
  // from a clean slate — see gGotIpThisAttempt.
  if (WiFi.isConnected()) {
    WiFi.disconnect(false);
  }
  gGotIpThisAttempt = false;
  gLinkUpTiming = false;

  // If AP is currently active, stay in WIFI_AP_STA so the captive portal
  // remains reachable while this attempt is in flight; otherwise plain
  // WIFI_STA is enough.
  WiFi.mode(gApActive ? WIFI_AP_STA : WIFI_STA);
  wl_status_t result = WiFi.begin(ssid.c_str(), password.c_str());
  if (result == WL_CONNECT_FAILED) {
    Serial.println("[WiFi] WiFi.begin() failed immediately.");
  }
}

} // namespace

namespace WifiManager {

void begin() {
  // Credentials live in our own NVS namespace (NvsStore); don't let the
  // WiFi driver keep a second copy in its own flash config, which also
  // recorded every attempted (possibly wrong) password.
  WiFi.persistent(false);
  WiFi.onEvent(onWifiEvent);

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
    bool stable = false;
    if (staLinkUp()) {
      if (!gLinkUpTiming) {
        gLinkUpTiming = true;
        gLinkUpSinceMs = millis();
      }
      stable = millis() - gLinkUpSinceMs >= kStaStableMs;
    } else {
      gLinkUpTiming = false;
    }
    if (stable) {
      gMode = WifiMode::STA_CONNECTED;
      gLinkDownTiming = false;
      if (gHasPendingCreds) {
        NvsStore::saveWifiCredentials(gPendingSsid, gPendingPassword);
        gHasPendingCreds = false;
        gPendingPassword = String();
      }
      if (gApActive) {
        stopApMode();
      }
      Serial.print("[WiFi] Connected, IP=");
      Serial.println(WiFi.localIP());
    } else if (millis() - gStaConnectStartMs > kStaConnectTimeoutMs) {
      Serial.println("[WiFi] STA connect timed out.");
      gMode = WifiMode::STA_FAILED;
      gStaFailedAtMs = millis();
      if (gHasPendingCreds) {
        // Discard: the fallback retry below uses the saved (last working)
        // network, not the one that just failed.
        gHasPendingCreds = false;
        gPendingPassword = String();
      }
      WiFi.disconnect(true);
    }
  } else if (gMode == WifiMode::STA_CONNECTED) {
    if (WiFi.isConnected()) {
      gLinkDownTiming = false;
    } else if (!gLinkDownTiming) {
      gLinkDownTiming = true;
      gLinkDownSinceMs = millis();
      Serial.println("[WiFi] Connection lost, waiting for auto-reconnect...");
    } else if (millis() - gLinkDownSinceMs > kStaLostTimeoutMs) {
      Serial.println("[WiFi] Connection lost for 30s, falling back to setup AP.");
      gLinkDownTiming = false;
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
        // A portal-initiated attempt (AP kept up, WIFI_AP_STA) failed: the
        // AP is still up, just go back to it (no re-scan — scanning with
        // the AP up is avoided, see runScanOnce()).
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
        // Nobody is using the portal: take the AP down and retry exactly
        // like a boot-time connect (plain WIFI_STA), rather than bolting
        // STA onto the running AP. On failure the STA_FAILED branch above
        // re-scans and brings the AP back up.
        Serial.println("[WiFi] Retrying saved network from fallback AP mode.");
        stopApMode();
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
  gPendingSsid = ssid;
  gPendingPassword = password;
  gHasPendingCreds = true;
  beginStaConnect(ssid, password);
}

void setCredentialsAndConnect(const String &ssid, const String &password) {
  // Saved immediately (unlike connectToNetwork()): this is the serial
  // console's recovery path, used with physical access when the device
  // can't reach its network — the network may not even be up yet, so
  // saving only on a successful connect could leave it stuck.
  NvsStore::saveWifiCredentials(ssid, password);
  gHasPendingCreds = false;
  gPendingPassword = String();
  gRetrySavedNetwork = true;
  if (gApActive && WiFi.softAPgetStationNum() == 0) {
    stopApMode();
  }
  beginStaConnect(ssid, password);
}

String getSavedSsid() { return NvsStore::getWifiCredentials().ssid; }

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
