#include "net/update_manager.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <time.h>

#include "esp_partition.h"
#include "api/ota_routes.h"
#include "motion/eye_pose.h" // deadlineReached()
#include "net/wifi_manager.h"
#include "version.h"

// mbedTLS root certificate bundle, embedded in the prebuilt Arduino core
// libraries (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). arduino-esp32 2.0.x does not
// attach it on its own: WiFiClientSecure::setCACertBundle() needs the pointer.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

namespace {

// Same host the web flasher serves its manifests from. Plain GETs on
// github.io: no redirects, no API rate limit, and (unlike release assets)
// no hop to a second host. Each channel directory holds manifest.json plus
// the images it names (see webflash/assemble.sh).
constexpr const char *kBaseUrl = "https://marcelrv.github.io/ESP-Magic-Eyes/";
constexpr const char *kFirmwareFile = "firmware.bin";
constexpr const char *kFilesystemFile = "littlefs.bin";

constexpr uint32_t kRestartDelayMs = 2500;       // lets the UI poll read "success" first
constexpr uint32_t kStallMs = 20000;             // no bytes for this long = give up
constexpr uint32_t kNtpWaitMs = 10000;
constexpr time_t kMinValidEpoch = 1700000000;    // 2023-11; anything below = clock not set yet
constexpr uint32_t kMinFreeHeap = 55000;         // a TLS session needs ~40-50 KB
constexpr uint32_t kTaskStackBytes = 16384;      // TLS handshake is stack hungry
constexpr size_t kMaxManifestBytes = 4096;

std::mutex gMutex; // guards gCheck, gInstall, gRestartAtMs
UpdateManager::CheckResult gCheck;
UpdateManager::InstallStatus gInstall;
std::atomic<bool> gBusy{false};       // a worker task exists
std::atomic<bool> gInstalling{false}; // ...and it is an install (holds Update.h)
std::atomic<bool> gRestartPending{false};
uint32_t gRestartAtMs = 0;

// Worker parameters, written before the task starts (gBusy serialises use).
bool gTaskInstall = false;
UpdateManager::Channel gTaskChannel = UpdateManager::Channel::STABLE;

const char *channelDir(UpdateManager::Channel c) {
  return c == UpdateManager::Channel::STABLE ? "stable" : "latest";
}

// ---- version comparison ----------------------------------------------------

struct SemVer {
  int major = 0, minor = 0, patch = 0;
  bool preRelease = false;
  bool valid = false;
};

// Accepts "v0.1.0", "0.2.0-dev", "0.2.0-dev+a38f6eb". The +sha part is build
// metadata and ignored here (the caller splits it off separately).
SemVer parseVersion(String v) {
  SemVer out;
  v.trim();
  if (v.startsWith("v") || v.startsWith("V")) v.remove(0, 1);
  int plus = v.indexOf('+');
  if (plus >= 0) v.remove(plus);
  int dash = v.indexOf('-');
  if (dash >= 0) {
    out.preRelease = true;
    v.remove(dash);
  }
  out.valid = sscanf(v.c_str(), "%d.%d.%d", &out.major, &out.minor, &out.patch) == 3;
  return out;
}

// <0, 0, >0 like strcmp. A pre-release ("-dev") sorts before the same
// number's release: 0.2.0-dev < 0.2.0.
int compareVersions(const SemVer &a, const SemVer &b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  if (a.preRelease != b.preRelease) return a.preRelease ? -1 : 1;
  return 0;
}

// Prefix match either way round, so an abbreviation of a different length on
// the build machine and in CI doesn't read as "different commit".
bool sameSha(const String &a, const String &b) {
  if (a.isEmpty() || b.isEmpty()) return false;
  return a.startsWith(b) || b.startsWith(a);
}

// ---- state helpers ---------------------------------------------------------

void setInstall(UpdateManager::InstallStatus::Result r, const char *phase, const String &error = "") {
  std::lock_guard<std::mutex> lock(gMutex);
  gInstall.result = r;
  gInstall.phase = phase;
  gInstall.error = error;
  gInstall.bytesWritten = 0;
  gInstall.totalBytes = 0;
}

void setPhase(const char *phase, size_t total) {
  std::lock_guard<std::mutex> lock(gMutex);
  gInstall.phase = phase;
  gInstall.bytesWritten = 0;
  gInstall.totalBytes = total;
}

void setProgress(size_t written) {
  std::lock_guard<std::mutex> lock(gMutex);
  gInstall.bytesWritten = written;
}

void failInstall(const String &error) {
  Serial.print("[UPDATE] Install failed: ");
  Serial.println(error);
  std::lock_guard<std::mutex> lock(gMutex);
  gInstall.result = UpdateManager::InstallStatus::Result::FAILURE;
  gInstall.error = error;
}

// ---- network helpers -------------------------------------------------------

// TLS certificate validation checks validity dates, and the board has no RTC,
// so the clock must be set before the first request.
bool ensureClock() {
  if (time(nullptr) > kMinValidEpoch) return true;
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  uint32_t due = millis() + kNtpWaitMs;
  while (time(nullptr) <= kMinValidEpoch) {
    if (deadlineReached(millis(), due)) return false;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return true;
}

void configureHttp(HTTPClient &http) {
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(String("ESP-Magic-Eyes/") + FIRMWARE_VERSION);
}

String httpError(int code) {
  return code < 0 ? String("Connection failed: ") + HTTPClient::errorToString(code) : String("Server answered HTTP ") + code;
}

struct Manifest {
  String version;
  String firmwareUrl;
  String filesystemUrl;
};

// A manifest part path is only ever a file next to the manifest.
bool plainFileName(const char *path) {
  return path && *path && !strchr(path, '/') && !strchr(path, ':') && !strstr(path, "..");
}

String fetchManifest(WiFiClientSecure &client, UpdateManager::Channel channel, Manifest &out) {
  String dirUrl = String(kBaseUrl) + channelDir(channel) + "/";
  HTTPClient http;
  configureHttp(http);
  if (!http.begin(client, dirUrl + "manifest.json")) return "Could not open the manifest URL";
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return httpError(code);
  }
  int size = http.getSize();
  if (size > (int)kMaxManifestBytes) {
    http.end();
    return "Manifest is unexpectedly large";
  }
  String body = http.getString();
  http.end();
  client.stop();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return "Manifest is not valid JSON";
  out.version = doc["version"] | "";
  if (out.version.isEmpty()) return "Manifest has no version";
  for (JsonObject part : doc["builds"][0]["parts"].as<JsonArray>()) {
    const char *path = part["path"] | "";
    if (!plainFileName(path)) continue;
    if (!strcmp(path, kFirmwareFile)) out.firmwareUrl = dirUrl + path;
    if (!strcmp(path, kFilesystemFile)) out.filesystemUrl = dirUrl + path;
  }
  if (out.firmwareUrl.isEmpty() || out.filesystemUrl.isEmpty()) return "Manifest lacks firmware or filesystem image";
  return "";
}

// Content-Length of a file via HEAD, or -1 if the server doesn't say. Error
// text in `error` (empty on success).
int headSize(WiFiClientSecure &client, const String &url, String &error) {
  HTTPClient http;
  configureHttp(http);
  if (!http.begin(client, url)) {
    error = "Could not open the image URL";
    return -1;
  }
  int code = http.sendRequest("HEAD");
  int size = http.getSize();
  http.end();
  client.stop();
  if (code != HTTP_CODE_OK) {
    error = httpError(code);
    return -1;
  }
  return size;
}

// Streams one image into Update.h. Returns "" on success, else the error
// (Update is aborted by then, so the running image/partition is untouched
// unless Update.end() itself got as far as switching the boot slot).
String streamToUpdate(WiFiClientSecure &client, const String &url, int command, const char *phase) {
  HTTPClient http;
  configureHttp(http);
  if (!http.begin(client, url)) return "Could not open the image URL";
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return httpError(code);
  }
  // -1 when the server doesn't send Content-Length; Update then finds the end
  // itself. With a length, Update.begin() rejects an image that can't fit
  // before anything is erased.
  int len = http.getSize();
  setPhase(phase, len > 0 ? (size_t)len : 0);

  if (Update.isRunning()) Update.abort();
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN, command)) {
    http.end();
    return Update.errorString();
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[2048];
  size_t written = 0;
  uint32_t lastDataMs = millis();
  String error;
  while (len <= 0 || written < (size_t)len) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected()) break; // server closed: done (or truncated, checked below)
      if (deadlineReached(millis(), lastDataMs + kStallMs)) {
        error = "Download stalled";
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t n = stream->readBytes(buf, min((size_t)avail, sizeof(buf)));
    if (n == 0) continue;
    if (Update.write(buf, n) != n) {
      error = Update.errorString();
      break;
    }
    written += n;
    lastDataMs = millis();
    setProgress(written);
  }
  http.end();
  client.stop();

  if (error.isEmpty() && len > 0 && written != (size_t)len) error = "Download incomplete";
  if (error.isEmpty() && written == 0) error = "Download was empty";
  if (!error.isEmpty()) {
    Update.abort();
    return error;
  }
  if (!Update.end(true)) return Update.errorString();
  return "";
}

// Common worker prologue: memory, clock, client. Returns "" if ready.
String prepareClient(std::unique_ptr<WiFiClientSecure> &client) {
  if (WifiManager::getMode() != WifiMode::STA_CONNECTED) return "Not connected to a WiFi network";
  if (ESP.getFreeHeap() < kMinFreeHeap) return String("Not enough free memory for a secure connection (") + ESP.getFreeHeap() + " bytes)";
  if (!ensureClock()) return "Could not get the current time (is NTP blocked?); needed to verify the server certificate";
  client.reset(new (std::nothrow) WiFiClientSecure());
  if (!client) return "Out of memory";
  client->setCACertBundle(x509_crt_bundle_start);
  client->setTimeout(15);
  return "";
}

// ---- worker bodies ---------------------------------------------------------

void fillChannel(UpdateManager::ChannelInfo &info, const Manifest &m, bool newer) {
  info.available = true;
  info.version = m.version;
  int plus = m.version.indexOf('+');
  info.sha = plus >= 0 ? m.version.substring(plus + 1) : String();
  info.newer = newer;
  info.error = "";
}

void runCheck() {
  UpdateManager::CheckResult result;
  result.state = UpdateManager::CheckResult::State::CHECKING;

  std::unique_ptr<WiFiClientSecure> client;
  String err = prepareClient(client);
  if (!err.isEmpty()) {
    result.state = UpdateManager::CheckResult::State::ERROR;
    result.error = err;
  } else {
    SemVer running = parseVersion(FIRMWARE_VERSION);
    String runningSha = FIRMWARE_GIT_SHA;
    for (UpdateManager::Channel ch : {UpdateManager::Channel::STABLE, UpdateManager::Channel::LATEST}) {
      UpdateManager::ChannelInfo &info = ch == UpdateManager::Channel::STABLE ? result.stable : result.latest;
      Manifest m;
      String e = fetchManifest(*client, ch, m);
      if (!e.isEmpty()) {
        info.error = e;
        continue;
      }
      bool newer;
      if (ch == UpdateManager::Channel::STABLE) {
        SemVer remote = parseVersion(m.version);
        newer = remote.valid && compareVersions(remote, running) > 0;
      } else {
        // "latest" is the head of main: same commit = up to date, anything
        // else (including an unknown local commit) is worth offering.
        int plus = m.version.indexOf('+');
        String remoteSha = plus >= 0 ? m.version.substring(plus + 1) : String();
        newer = !sameSha(remoteSha, runningSha);
      }
      fillChannel(info, m, newer);
    }
    bool any = result.stable.available || result.latest.available;
    result.state = any ? UpdateManager::CheckResult::State::DONE : UpdateManager::CheckResult::State::ERROR;
    if (!any) result.error = result.stable.error;
  }

  std::lock_guard<std::mutex> lock(gMutex);
  gCheck = result;
}

void runInstall(UpdateManager::Channel channel) {
  using Result = UpdateManager::InstallStatus::Result;
  setInstall(Result::IN_PROGRESS, "preparing");
  Serial.print("[UPDATE] Installing channel ");
  Serial.println(channelDir(channel));

  std::unique_ptr<WiFiClientSecure> client;
  String err = prepareClient(client);
  if (!err.isEmpty()) return failInstall(err);

  Manifest m;
  err = fetchManifest(*client, channel, m);
  if (!err.isEmpty()) return failInstall(err);

  // Everything that can be checked without writing flash is checked first, so
  // the likeliest failures (missing file, image too big) leave the device as
  // it was. The filesystem image is the one that matters: it is written
  // second, and a failure after the firmware is already switched would leave
  // old UI + new firmware.
  int fsSize = headSize(*client, m.filesystemUrl, err);
  if (!err.isEmpty()) return failInstall("Filesystem image: " + err);
  const esp_partition_t *fsPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!fsPart) return failInstall("No filesystem partition found");
  // No Content-Length (-1) would make Update.begin() assume "fills the
  // partition", so an oversize image would only fail after the firmware is
  // already switched. Pages always sends the length; refuse if that changes.
  if (fsSize <= 0) return failInstall("Filesystem image: server did not report its size");
  if ((uint32_t)fsSize > fsPart->size) return failInstall("Filesystem image does not fit the partition");

  err = streamToUpdate(*client, m.firmwareUrl, U_FLASH, "firmware");
  if (!err.isEmpty()) return failInstall("Firmware: " + err);

  err = streamToUpdate(*client, m.filesystemUrl, U_SPIFFS, "filesystem");
  if (!err.isEmpty()) {
    // The new firmware is already the boot image; don't reboot into it with a
    // half-written UI behind the user's back. They can retry from the page.
    return failInstall("Firmware installed, but the web interface update failed (" + err + "). Retry the update before restarting.");
  }

  Serial.println("[UPDATE] Install complete, restarting.");
  {
    std::lock_guard<std::mutex> lock(gMutex);
    gInstall.result = Result::SUCCESS;
    gInstall.phase = "done";
    gRestartAtMs = millis() + kRestartDelayMs;
  }
  gRestartPending = true;
}

void workerTask(void *) {
  if (gTaskInstall) {
    runInstall(gTaskChannel);
  } else {
    runCheck();
  }
  gInstalling = false;
  gBusy = false;
  vTaskDelete(nullptr);
}

// Shared start-up gate for both entry points.
bool startWorker(bool install, UpdateManager::Channel channel, String &error) {
  if (WifiManager::getMode() != WifiMode::STA_CONNECTED) {
    error = "Updating over WiFi needs a connection to your home network";
    return false;
  }
  if (gRestartPending || OtaRoutes::restartPending()) {
    error = "An update was just installed; the device is restarting";
    return false;
  }
  bool expected = false;
  if (!gBusy.compare_exchange_strong(expected, true)) {
    error = "A check or update is already running";
    return false;
  }
  // A manual upload in flight owns Update.h. Not racy in practice: its first
  // chunk refuses to start while installing() (see ota_routes.cpp).
  if (Update.isRunning()) {
    gBusy = false;
    error = "A firmware upload is in progress";
    return false;
  }
  gTaskInstall = install;
  gTaskChannel = channel;
  gInstalling = install;
  if (install) {
    setInstall(UpdateManager::InstallStatus::Result::IN_PROGRESS, "preparing");
  } else {
    std::lock_guard<std::mutex> lock(gMutex);
    gCheck = UpdateManager::CheckResult();
    gCheck.state = UpdateManager::CheckResult::State::CHECKING;
  }
  // Core 0 next to the WiFi stack, low priority: neither MotionTask (core 1,
  // prio 3) nor loop() should feel a download.
  if (xTaskCreatePinnedToCore(workerTask, "update", kTaskStackBytes, nullptr, 1, nullptr, 0) != pdPASS) {
    gInstalling = false;
    gBusy = false;
    error = "Out of memory";
    return false;
  }
  return true;
}

} // namespace

namespace UpdateManager {

bool parseChannel(const char *name, Channel &out) {
  if (!name) return false;
  if (!strcmp(name, "stable")) {
    out = Channel::STABLE;
    return true;
  }
  if (!strcmp(name, "latest")) {
    out = Channel::LATEST;
    return true;
  }
  return false;
}

bool startCheck(String &error) { return startWorker(false, Channel::STABLE, error); }

bool startInstall(Channel channel, String &error) { return startWorker(true, channel, error); }

// Stays true through the deferred restart too: the worker has finished (so
// Update.h is idle) but the reboot is still due, and a manual upload started
// in that gap would be cut off mid-write.
bool installing() { return gInstalling || gRestartPending; }

CheckResult getCheck() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gCheck;
}

InstallStatus getInstall() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gInstall;
}

void clearInstallStatus() {
  if (gBusy) return;
  std::lock_guard<std::mutex> lock(gMutex);
  if (gInstall.result == InstallStatus::Result::FAILURE) gInstall = InstallStatus();
}

void handle() {
  if (!gRestartPending) return;
  uint32_t due;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    due = gRestartAtMs;
  }
  if (deadlineReached(millis(), due)) {
    gRestartPending = false;
    Serial.println("[UPDATE] Restarting after WiFi update...");
    ESP.restart();
  }
}

} // namespace UpdateManager
