// wifi_manager.h — STA connect with bounded timeout, AP fallback +
// captive-portal DNS, per architecture plan §2 (Sensor & IO layer) and §6
// (research: AP+scan conflicts, non-blocking connect).
//
// Non-blocking: begin() kicks off a connect attempt (or goes straight to
// AP mode) and returns immediately; handle() must be polled from loop()
// every iteration to drive the STA-connect timeout state machine and the
// captive-portal DNS server. Nothing in this module calls delay().

#pragma once

#include <Arduino.h>

#include <vector>

enum class WifiMode {
  AP_SETUP,       // Serving a local AP + captive portal, no STA creds or STA failed
  STA_CONNECTING, // Attempting STA connect, within the timeout window
  STA_CONNECTED,  // STA connected, has an IP
  STA_FAILED,     // STA connect attempt timed out/failed (transient, en route to AP_SETUP)
};

struct WifiScanResult {
  String ssid;
  int32_t rssiDbm = 0;
  bool secure = false;
};

namespace WifiManager {

// Loads credentials from NvsStore and starts the connect/AP state machine.
// Call once from setup(), after NvsStore::begin().
void begin();

// Must be polled every loop() iteration. Drives the STA-connect timeout
// and services the captive-portal DNS server while in AP mode. Never
// blocks.
void handle();

WifiMode getMode();
const char *getModeName(WifiMode mode);

// Valid once getMode() == STA_CONNECTED (STA IP) or AP_SETUP (AP IP,
// normally 192.168.4.1). Empty string otherwise.
String getIpAddress();

// True once mDNS (<hostname>.local) runs. Started on the first home-network
// connection and then left running (it follows reconnects by itself), so
// other code, e.g. OtaManager, may add or remove services but never end it.
// loop() only.
bool mdnsRunning();

// AP SSID in use while in AP_SETUP mode, e.g. "MagicEyes-Setup-A1B2".
String getApSsid();

// (Re)attempts a connection with these credentials, which are persisted to
// NVS only once the attempt succeeds — a failed attempt leaves the
// previously saved (last working) network in place for the fallback retry.
// If currently in AP_SETUP, the AP is left running (WIFI_AP_STA) so the
// captive portal stays reachable while the new connection is attempted; on
// success the AP is torn down. Used by POST /api/wifi/connect. Returns
// immediately (non-blocking); poll getMode() for result.
void connectToNetwork(const String &ssid, const String &password);

// Saves these credentials to NVS immediately, then connects with them.
// Unlike connectToNetwork(), the save does not wait for success: this is
// the serial console's recovery path (physical access, network possibly
// unreachable right now). If the attempt fails, the fallback AP retries
// these saved credentials every minute.
void setCredentialsAndConnect(const String &ssid, const String &password);

// SSID currently saved in NVS (empty if none).
String getSavedSsid();

// Clears saved credentials and reboots into AP setup mode. Intended for
// the future POST /api/wifi/forget handler and the factory-reset button.
void forgetNetwork();

// Optional scan-for-SSID-list support (plan §4 GET /api/wifi/scan). A
// single scan is run once at boot, before AP is brought up (per research
// finding: avoid scanning while AP is already active), and cached here.
// Returns a copy of the cached results (safe to call from any task); may be
// empty if no scan has run yet.
std::vector<WifiScanResult> getCachedScanResults();

} // namespace WifiManager
