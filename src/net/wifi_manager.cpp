#include "net/wifi_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
bool gMdnsStarted = false;

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
// The retry runs as WIFI_AP_STA with the setup AP kept up, so the AP never
// disappears from the air (tearing it down for the ~20s connect+fallback
// cycle hid it a third of the time and dropped phones mid-join). A retry
// is still skipped while a client is connected to the setup AP, since the
// STA's channel search can hop the AP's channel and disrupt the captive
// portal the user is actively using.
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
// Guards gScanResults/gScanCount: written from loop() (storeScanResults()),
// read from the AsyncTCP task (GET /api/wifi/scan). The entries hold
// Strings, so an unguarded read during a rewrite could touch freed heap.
SemaphoreHandle_t gScanMutex = nullptr;

// True while the fallback path's asynchronous scan (see the STA_FAILED
// branch of handle()) is in flight.
bool gFallbackScanRunning = false;

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

// Copies a finished scan (`found` = scanNetworks()/scanComplete() result)
// into gScanResults and frees the driver's copy.
void storeScanResults(int found) {
  size_t n = found > 0 ? static_cast<size_t>(found) : 0;
  if (n > kMaxScanResults) {
    n = kMaxScanResults;
  }
  xSemaphoreTake(gScanMutex, portMAX_DELAY);
  for (size_t i = 0; i < n; ++i) {
    gScanResults[i].ssid = WiFi.SSID(i);
    gScanResults[i].rssiDbm = WiFi.RSSI(i);
    gScanResults[i].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  gScanCount = n;
  xSemaphoreGive(gScanMutex);
  WiFi.scanDelete();
}

void runScanOnce() {
  // Per research finding (plan §6): avoid scanning while the AP is
  // already up. This is called once at boot, before softAP() starts, so
  // it's safe to run synchronously here (one-time startup cost, not a
  // loop()-repeated operation). The runtime fallback path scans
  // asynchronously instead — see the STA_FAILED branch of handle().
  //
  // Note: on a freshly-erased board (no cached WiFi/PHY calibration data)
  // this first WiFi.scanNetworks() call takes a few seconds longer than
  // on later boots since it also has to run a full RF calibration pass —
  // confirmed via real-hardware timestamped logging to take ~3.1s, which
  // is not the cause of the RTCWDT_RTC_RESET boot loop previously seen
  // here (that turned out to be ServoHal::begin()'s attach() of the Aux
  // servo channel — see PROGRESS.md's "Hardware bring-up fixes
  // (post-Phase 8)" section for the full root-cause story).
  storeScanResults(WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false));
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
  if (gFallbackScanRunning) {
    // e.g. serial-console credentials arriving mid-fallback-scan. Stop the
    // scan itself (scanDelete() only frees cached results): the driver
    // rejects a connect while a scan is running.
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    gFallbackScanRunning = false;
  }

  // If AP is currently active, stay in WIFI_AP_STA so the captive portal
  // remains reachable while this attempt is in flight; otherwise plain
  // WIFI_STA is enough.
  WiFi.mode(gApActive ? WIFI_AP_STA : WIFI_STA);
  wl_status_t result = WiFi.begin(ssid.c_str(), password.c_str());
  if (result == WL_CONNECT_FAILED) {
    Serial.println("[WiFi] WiFi.begin() failed immediately.");
  }
}

// mDNS makes the device reachable as http://<hostname>.local (default
// esp-magic-eyes.local), the address the README tells users to open.
// Owned here, not by ArduinoOTA: ArduinoOTA.end() calls MDNS.end(), so
// the name used to vanish whenever network OTA stopped (admin password
// changed, OTA disabled, auth storage error). Started once and never
// ended: the IDF mDNS responder re-announces on its own after reconnects.
void startMdnsOnce() {
  if (gMdnsStarted) {
    return;
  }
  String hostname = NvsStore::getHostname();
  if (!MDNS.begin(hostname)) {
    Serial.println("[WiFi] mDNS failed to start.");
    return;
  }
  MDNS.setInstanceName(NvsStore::getDeviceName());
  MDNS.addService("http", "tcp", 80);
  gMdnsStarted = true;
  Serial.print("[WiFi] mDNS: http://");
  Serial.print(hostname);
  Serial.println(".local/");
}

} // namespace

namespace WifiManager {

void begin() {
  // Credentials live in our own NVS namespace (NvsStore); don't let the
  // WiFi driver keep a second copy in its own flash config, which also
  // recorded every attempted (possibly wrong) password.
  WiFi.persistent(false);
  // Name the STA interface before any WiFi.mode() so DHCP reports it and
  // the router's device list shows "esp-magic-eyes" rather than the
  // framework's "esp32-XXXXXX". The framework copies the string.
  WiFi.setHostname(NvsStore::getHostname().c_str());
  WiFi.onEvent(onWifiEvent);
  gScanMutex = xSemaphoreCreateMutex();

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
      startMdnsOnce();
    } else if (!gLinkUpTiming && millis() - gStaConnectStartMs > kStaConnectTimeoutMs) {
      // Not while the link is up and being verified (gLinkUpTiming): a slow
      // router that hands out an IP near the deadline still gets its
      // kStaStableMs check. A link that drops again clears gLinkUpTiming,
      // so the attempt is bounded by timeout + kStaStableMs.
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
      bool apReady = true;
      if (gApActive) {
        // A portal-initiated attempt or background retry (AP kept up,
        // WIFI_AP_STA) failed: the AP is still up, just go back to it (no
        // re-scan — scanning with the AP up is avoided, see runScanOnce()).
        gMode = WifiMode::AP_SETUP;
        WiFi.mode(WIFI_AP);
      } else if (!gFallbackScanRunning) {
        // Scan before the AP comes up, but asynchronously: a blocking scan
        // stalls loop() (buttons, LEDs, OTA, serial console) for 3-6s.
        WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
        gFallbackScanRunning = true;
        apReady = false;
      } else {
        int found = WiFi.scanComplete();
        if (found == WIFI_SCAN_RUNNING) {
          apReady = false;
        } else {
          gFallbackScanRunning = false;
          storeScanResults(found);
          startApMode();
        }
      }
      if (apReady) {
        gRetrySavedNetwork = NvsStore::getWifiCredentials().valid;
        gNextStaRetryMs = millis() + kStaRetryIntervalMs;
      }
    }
  } else if (gMode == WifiMode::AP_SETUP && gRetrySavedNetwork &&
             static_cast<int32_t>(millis() - gNextStaRetryMs) >= 0) {
    if (WiFi.softAPgetStationNum() > 0) {
      gNextStaRetryMs = millis() + kStaRetryIntervalMs; // portal in use, try later
    } else {
      WifiCredentials creds = NvsStore::getWifiCredentials();
      if (creds.valid) {
        // Nobody is using the portal: retry as WIFI_AP_STA with the AP kept
        // up (see kStaRetryIntervalMs). On success the STA_CONNECTING
        // branch takes the AP down; on failure STA_FAILED returns to it.
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

bool mdnsRunning() { return gMdnsStarted; }

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

std::vector<WifiScanResult> getCachedScanResults() {
  std::vector<WifiScanResult> copy;
  if (gScanMutex == nullptr) {
    return copy;
  }
  xSemaphoreTake(gScanMutex, portMAX_DELAY);
  copy.assign(gScanResults, gScanResults + gScanCount);
  xSemaphoreGive(gScanMutex);
  return copy;
}

} // namespace WifiManager
