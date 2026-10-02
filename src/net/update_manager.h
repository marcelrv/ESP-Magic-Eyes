// update_manager.h — "check for updates / install latest" over the internet,
// the WiFi counterpart of the web flasher (webflash/index.html).
//
// The device fetches the same ESP Web Tools manifests the web flasher uses
// (https://marcelrv.github.io/ESP-Magic-Eyes/{stable,latest}/manifest.json),
// compares the version it advertises with FIRMWARE_VERSION / GIT_SHA, and on
// request streams firmware.bin and littlefs.bin straight into Update.h. Both
// images are written because the web UI lives in LittleFS: a firmware-only
// update would leave a UI that talks to the old API.
//
// Threading: HTTPS (TLS handshake, blocking reads) must never run on the
// AsyncTCP task or in loop(), so each check/install runs in its own
// short-lived worker task. The API handlers only start it and read copies of
// its state (mutex-guarded); loop() calls handle() for the deferred restart.
//
// TLS is verified against the mbedTLS root bundle, which needs a correct
// wall clock: the worker syncs SNTP first. A LAN without internet access
// (or with NTP/HTTPS blocked) reports a plain error instead of updating.

#pragma once

#include <Arduino.h>

namespace UpdateManager {

enum class Channel { STABLE, LATEST };

// Parses "stable" / "latest"; false for anything else.
bool parseChannel(const char *name, Channel &out);

struct ChannelInfo {
  bool available = false; // manifest fetched and parsed
  String version;         // manifest "version", e.g. "v0.1.0" or "0.2.0-dev+a38f6eb"
  String sha;             // text after '+' in the version, empty if none
  bool newer = false;     // better than what is running (see compare rules in the .cpp)
  String error;           // why !available
};

struct CheckResult {
  enum class State { IDLE, CHECKING, DONE, ERROR };
  State state = State::IDLE;
  String error; // set when state == ERROR (every channel failed)
  ChannelInfo stable;
  ChannelInfo latest;
};

struct InstallStatus {
  enum class Result { NONE, IN_PROGRESS, SUCCESS, FAILURE };
  Result result = Result::NONE;
  String phase; // "preparing" | "firmware" | "filesystem"
  String error;
  size_t bytesWritten = 0; // of the current phase
  size_t totalBytes = 0;   // of the current phase, 0 if unknown
};

// Start the worker. Return false (with a human-readable `error`) when the
// request can't run now: not on a home network, another check/install or a
// manual upload is running.
bool startCheck(String &error);
bool startInstall(Channel channel, String &error);

// True while an install holds Update.h, so the manual upload routes can
// refuse to start (their "abort stale update" step would kill ours).
bool installing();

// Copies — safe to call from any task.
CheckResult getCheck();
InstallStatus getInstall();

// Forgets a finished install's status (no-op while one runs), so a stale
// WiFi-update failure doesn't shadow the next manual upload's status.
void clearInstallStatus();

// Poll from loop(): fires the deferred ESP.restart() after a successful
// install, once the UI has had time to read the "success" status.
void handle();

} // namespace UpdateManager
