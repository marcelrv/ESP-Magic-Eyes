#include "net/serial_console.h"

#include <Arduino.h>
#include <WiFi.h>

#include "net/auth.h"
#include "net/wifi_manager.h"
#include "version.h"

namespace {

constexpr size_t kMaxLineLength = 160;
constexpr size_t kMaxArgs = 4;

String gLine;
bool gLineOverflow = false;

// Splits `line` into whitespace-separated arguments; "double quotes" group
// words containing spaces (SSIDs and passwords often do). Returns the
// argument count (at most kMaxArgs; extra arguments are ignored).
size_t tokenize(const String &line, String (&args)[kMaxArgs]) {
  size_t count = 0;
  size_t i = 0;
  const size_t n = line.length();
  while (i < n && count < kMaxArgs) {
    while (i < n && isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= n) break;
    String arg;
    if (line[i] == '"') {
      ++i;
      while (i < n && line[i] != '"') arg += line[i++];
      ++i; // closing quote (or end of line)
    } else {
      while (i < n && !isspace(static_cast<unsigned char>(line[i]))) arg += line[i++];
    }
    args[count++] = arg;
  }
  return count;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help                          this list");
  Serial.println("  status                        firmware, uptime, WiFi state");
  Serial.println("  wifi [status]                 WiFi mode, SSID, IP, signal");
  Serial.println("  wifi set <ssid> [password]    save credentials and connect now");
  Serial.println("                                (use \"double quotes\" for spaces)");
  Serial.println("  wifi forget                   clear credentials, reboot to setup AP");
  Serial.println("  auth [status]                 which passwords are set");
  Serial.println("  auth reset                    clear both web/OTA passwords");
  Serial.println("  reboot                        restart the device");
}

void printAuthStatus() {
  Serial.printf("Control password: %s\n", Auth::hasPassword(Auth::Level::Control) ? "set" : "not set");
  Serial.printf("Admin password  : %s\n", Auth::hasPassword(Auth::Level::Admin) ? "set" : "not set");
}

void printWifiStatus() {
  WifiMode mode = WifiManager::getMode();
  Serial.printf("WiFi mode : %s\n", WifiManager::getModeName(mode));
  String saved = WifiManager::getSavedSsid();
  Serial.printf("Saved SSID: %s\n", saved.length() ? saved.c_str() : "(none)");
  if (mode == WifiMode::STA_CONNECTED) {
    Serial.printf("Connected : %s, IP %s, RSSI %d dBm\n", WiFi.SSID().c_str(),
                  WifiManager::getIpAddress().c_str(), WiFi.RSSI());
  } else if (mode == WifiMode::AP_SETUP) {
    Serial.printf("Setup AP  : %s (password eyes-setup), IP %s\n", WifiManager::getApSsid().c_str(),
                  WifiManager::getIpAddress().c_str());
  }
}

void runCommand(const String &line) {
  String args[kMaxArgs];
  size_t argc = tokenize(line, args);
  if (argc == 0) return;
  String cmd = args[0];
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "status") {
    Serial.printf("ESP Magic Eyes v%s, uptime %lus, free heap %u\n", FIRMWARE_VERSION,
                  static_cast<unsigned long>(millis() / 1000), ESP.getFreeHeap());
    printWifiStatus();
  } else if (cmd == "wifi") {
    String sub = argc > 1 ? args[1] : String("status");
    sub.toLowerCase();
    if (sub == "status") {
      printWifiStatus();
    } else if (sub == "set") {
      if (argc < 3 || args[2].length() == 0) {
        Serial.println("Usage: wifi set <ssid> [password]");
        return;
      }
      if (args[2].length() > 32 || (argc > 3 && args[3].length() > 64)) {
        Serial.println("SSID max 32 characters, password max 64.");
        return;
      }
      String password = argc > 3 ? args[3] : String();
      Serial.printf("Saving WiFi credentials for '%s' and connecting...\n", args[2].c_str());
      WifiManager::setCredentialsAndConnect(args[2], password);
    } else if (sub == "forget") {
      WifiManager::forgetNetwork(); // reboots
    } else {
      Serial.println("Unknown wifi command. Try: wifi status | wifi set <ssid> [password] | wifi forget");
    }
  } else if (cmd == "auth") {
    // Password recovery needs a USB cable, i.e. physical access.
    String sub = argc > 1 ? args[1] : String("status");
    sub.toLowerCase();
    if (sub == "status") {
      printAuthStatus();
    } else if (sub == "reset") {
      Auth::clearAll();
      Serial.println("Both passwords cleared. Network OTA picks this up after a reboot.");
    } else {
      Serial.println("Unknown auth command. Try: auth status | auth reset");
    }
  } else if (cmd == "reboot" || cmd == "restart") {
    Serial.println("Rebooting...");
    delay(100);
    ESP.restart();
  } else {
    Serial.printf("Unknown command '%s'. Type 'help'.\n", args[0].c_str());
  }
}

} // namespace

namespace SerialConsole {

void begin() {
  gLine.reserve(kMaxLineLength);
  Serial.println("Serial console ready — type 'help' for commands.");
}

void handle() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (gLineOverflow) {
        Serial.println("Line too long, ignored.");
      } else if (gLine.length() > 0) {
        String line = gLine;
        gLine = "";
        runCommand(line);
      }
      gLine = "";
      gLineOverflow = false;
    } else if (c == '\b' || c == 0x7f) {
      if (gLine.length() > 0) gLine.remove(gLine.length() - 1);
    } else if (gLine.length() < kMaxLineLength) {
      gLine += c;
    } else {
      gLineOverflow = true;
    }
  }
}

} // namespace SerialConsole
