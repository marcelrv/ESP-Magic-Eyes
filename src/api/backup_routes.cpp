#include "api/backup_routes.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cctype>
#include <cstring>

#include "api/json_helpers.h"
#include "api/radar_routes.h"
#include "api/rest_routes.h"
#include "api/servo_routes.h"
#include "hal/led_controller.h"
#include "hal/servo_hal.h"
#include "storage/nvs_store.h"
#include "version.h"

namespace {

using JsonHelpers::sendJson;
using JsonHelpers::sendJsonError;

constexpr const char *kFormatId = "esp-magic-eyes-settings";
// The file layout's version, independent of FIRMWARE_VERSION (which the
// file carries for display only — a backup must restore onto newer
// firmware, so the firmware version is never enforced). Rules for changing
// the layout:
//   - Adding a field: no bump. Older firmware ignores unknown fields, and a
//     field missing from an older file keeps the device's current value.
//   - Changing what an existing field means (units, splitting a servo,
//     renaming a value): bump this, keep accepting every older version and
//     convert it in buildPlan() — like the servocal blob migrations in
//     nvs_store.cpp. Older firmware then refuses the new files
//     (backup_from_newer_firmware) instead of misreading them.
// Keep data/www/setup/ota.html's copy of this number in step.
constexpr int kFormatVersion = 1;
constexpr size_t kServoCount = static_cast<size_t>(ServoId::Count);

// --- GET /api/system/backup -------------------------------------------------

void addColor(JsonObject o, const uint8_t (&rgb)[3]) {
  o["r"] = rgb[0];
  o["g"] = rgb[1];
  o["b"] = rgb[2];
}

// Reads the *saved* values (NvsStore), not the modules' RAM state: the
// backup must hold what the next boot would use.
void handleGetBackup(AsyncWebServerRequest *request) {
  bool secrets = request->hasParam("secrets") && request->getParam("secrets")->value() == "1";

  JsonDocument doc;
  doc["format"] = kFormatId;
  doc["formatVersion"] = kFormatVersion;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  doc["deviceName"] = NvsStore::getDeviceName();
  doc["hostname"] = NvsStore::getHostname();
  doc["naturalMode"] = NvsStore::getNaturalModeEnabled();
  doc["otaNetworkEnabled"] = NvsStore::getOtaNetworkEnabled();

  JsonObject radar = doc["radar"].to<JsonObject>();
  radar["type"] = RadarRoutes::radarTypeId(NvsStore::getRadarType());
  radar["baudRate"] = NvsStore::getRadarBaudRate();

  NvsStore::LedColorConfig ledCfg = NvsStore::getLedColorConfig();
  JsonObject led = doc["led"].to<JsonObject>();
  led["enabled"] = NvsStore::getLedEnabled();
  led["brightness"] = ledCfg.brightness;
  addColor(led["colorL"].to<JsonObject>(), ledCfg.colorL);
  addColor(led["colorR"].to<JsonObject>(), ledCfg.colorR);
  led["effect"] = LedController::effectToName(static_cast<LedController::LedEffect>(ledCfg.effect));

  JsonArray servos = doc["servos"].to<JsonArray>();
  for (size_t i = 0; i < kServoCount; ++i) {
    ServoId id = static_cast<ServoId>(i);
    ServoCalibration cal = NvsStore::getServoCalibration(id);
    JsonObject o = servos.add<JsonObject>();
    o["name"] = ServoHal::servoIdName(id);
    o["servoId"] = i;
    o["minUs"] = cal.minUs;
    o["centerUs"] = cal.centerUs;
    o["maxUs"] = cal.maxUs;
    o["closedUs"] = cal.closedUs;
    o["halfUs"] = cal.halfUs;
    o["openUs"] = cal.openUs;
    o["inverted"] = cal.inverted;
  }

  if (secrets) {
    NvsStore::AuthHashes hashes;
    if (!NvsStore::getAuthHashes(hashes)) {
      // Unreachable in practice (Auth fails closed on the same error), but
      // never hand out a file that would restore "no passwords".
      sendJsonError(request, 500, "auth_storage_error");
      return;
    }
    WifiCredentials creds = NvsStore::getWifiCredentials();
    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = creds.ssid;
    wifi["password"] = creds.password;
    JsonObject pw = doc["passwords"].to<JsonObject>();
    pw["adminHa1"] = hashes.adminHa1;
    pw["userHa1"] = hashes.userHa1;
    pw["otaMd5"] = hashes.otaMd5;
  }

  // Pretty-printed: the file is meant to be readable (and hand-editable).
  // The page re-serializes it compactly on restore, so this doesn't eat
  // into collectJsonBody()'s 4 KB limit.
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  response->addHeader("Content-Disposition", "attachment; filename=\"magic-eyes-settings.json\"");
  response->addHeader("Cache-Control", "no-store");
  serializeJsonPretty(doc, *response);
  request->send(response);
}

// --- POST /api/system/restore -------------------------------------------------

// Everything a restore will write, filled and validated completely before
// the first NVS write, so a bad file changes nothing.
struct RestorePlan {
  bool hasDeviceName = false;
  String deviceName;
  bool hasHostname = false;
  String hostname;
  bool hasNaturalMode = false;
  bool naturalMode = true;
  bool hasOtaNetwork = false;
  bool otaNetwork = true;
  bool hasRadar = false;
  RadarType radarType = RadarType::LD2420;
  uint32_t radarBaud = 0;
  bool hasLed = false;
  bool ledEnabled = false;
  NvsStore::LedColorConfig led;
  bool hasServos = false;
  ServoCalibration servos[kServoCount];
  bool hasWifi = false;
  String ssid;
  String password;
  bool hasPasswords = false;
  NvsStore::AuthHashes hashes;
};

// RFC 1123 label: 1-63 of [A-Za-z0-9-], not starting or ending with '-'.
bool validHostname(const String &h) {
  if (h.length() == 0 || h.length() > 63 || h[0] == '-' || h[h.length() - 1] == '-') return false;
  for (size_t i = 0; i < h.length(); ++i) {
    char c = h[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
  }
  return true;
}

bool validDeviceName(const String &n) {
  if (n.length() == 0 || n.length() > 32) return false;
  for (size_t i = 0; i < n.length(); ++i) {
    if (iscntrl(static_cast<unsigned char>(n[i]))) return false;
  }
  return true;
}

bool isHashOrEmpty(JsonVariantConst v) {
  if (!v.is<const char *>()) return false;
  const char *s = v.as<const char *>();
  size_t len = strlen(s);
  if (len == 0) return true;
  if (len != 32) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

bool parseColor(JsonVariantConst v, uint8_t (&out)[3]) {
  const char *keys[3] = {"r", "g", "b"};
  for (int i = 0; i < 3; ++i) {
    if (!v[keys[i]].is<int>()) return false;
    int c = v[keys[i]].as<int>();
    if (c < 0 || c > 255) return false;
    out[i] = static_cast<uint8_t>(c);
  }
  return true;
}

// A pulse field must be an int in uint16_t range before it is narrowed;
// the actual limits are ServoRoutes::calibrationError()'s.
bool readPulse(JsonObjectConst o, const char *key, uint16_t &out) {
  if (!o[key].is<int>()) return false;
  int v = o[key].as<int>();
  if (v < 0 || v > 0xFFFF) return false;
  out = static_cast<uint16_t>(v);
  return true;
}

int findServo(JsonObjectConst o) {
  if (o["name"].is<const char *>()) {
    const char *name = o["name"].as<const char *>();
    for (size_t i = 0; i < kServoCount; ++i) {
      if (strcmp(name, ServoHal::servoIdName(static_cast<ServoId>(i))) == 0) return static_cast<int>(i);
    }
    return -1; // a name we don't know: don't guess from servoId
  }
  if (o["servoId"].is<int>()) {
    int i = o["servoId"].as<int>();
    if (i >= 0 && static_cast<size_t>(i) < kServoCount) return i;
  }
  return -1;
}

// Returns nullptr if `doc` is a valid backup (filling `plan`), else the
// API error code.
const char *buildPlan(JsonDocument &doc, RestorePlan &plan) {
  if (!doc["format"].is<const char *>() || strcmp(doc["format"].as<const char *>(), kFormatId) != 0) {
    return "not_a_settings_backup";
  }
  if (!doc["formatVersion"].is<int>() || doc["formatVersion"].as<int>() < 1) return "invalid_formatVersion";
  if (doc["formatVersion"].as<int>() > kFormatVersion) return "backup_from_newer_firmware";

  if (!doc["deviceName"].isNull()) {
    if (!doc["deviceName"].is<const char *>()) return "invalid_deviceName";
    plan.deviceName = doc["deviceName"].as<const char *>();
    if (!validDeviceName(plan.deviceName)) return "invalid_deviceName";
    plan.hasDeviceName = true;
  }
  if (!doc["hostname"].isNull()) {
    if (!doc["hostname"].is<const char *>()) return "invalid_hostname";
    plan.hostname = doc["hostname"].as<const char *>();
    if (!validHostname(plan.hostname)) return "invalid_hostname";
    plan.hasHostname = true;
  }
  if (!doc["naturalMode"].isNull()) {
    if (!doc["naturalMode"].is<bool>()) return "invalid_naturalMode";
    plan.naturalMode = doc["naturalMode"].as<bool>();
    plan.hasNaturalMode = true;
  }
  if (!doc["otaNetworkEnabled"].isNull()) {
    if (!doc["otaNetworkEnabled"].is<bool>()) return "invalid_otaNetworkEnabled";
    plan.otaNetwork = doc["otaNetworkEnabled"].as<bool>();
    plan.hasOtaNetwork = true;
  }

  JsonVariantConst radar = doc["radar"];
  if (!radar.isNull()) {
    if (!RadarRoutes::parseRadarType(radar["type"].as<const char *>(), &plan.radarType)) return "invalid_radar_type";
    if (!radar["baudRate"].is<uint32_t>()) return "invalid_radar_baudRate";
    plan.radarBaud = radar["baudRate"].as<uint32_t>();
    if (plan.radarBaud < RadarRoutes::kMinBaud || plan.radarBaud > RadarRoutes::kMaxBaud) {
      return "invalid_radar_baudRate";
    }
    plan.hasRadar = true;
  }

  JsonVariantConst led = doc["led"];
  if (!led.isNull()) {
    if (!led["enabled"].is<bool>()) return "invalid_led_enabled";
    plan.ledEnabled = led["enabled"].as<bool>();
    if (!led["brightness"].is<int>() || led["brightness"].as<int>() < 0 || led["brightness"].as<int>() > 255) {
      return "invalid_led_brightness";
    }
    plan.led.brightness = static_cast<uint8_t>(led["brightness"].as<int>());
    if (!parseColor(led["colorL"], plan.led.colorL) || !parseColor(led["colorR"], plan.led.colorR)) {
      return "invalid_led_color";
    }
    LedController::LedEffect effect;
    if (!led["effect"].is<const char *>() || !LedController::effectFromName(led["effect"].as<const char *>(), effect)) {
      return "invalid_led_effect";
    }
    plan.led.effect = static_cast<uint8_t>(effect);
    plan.hasLed = true;
  }

  JsonVariantConst servos = doc["servos"];
  if (!servos.isNull()) {
    if (!servos.is<JsonArrayConst>()) return "invalid_servos";
    // Start from what's stored, so a backup listing fewer servos (e.g. one
    // from firmware with fewer channels) leaves the rest untouched.
    for (size_t i = 0; i < kServoCount; ++i) {
      plan.servos[i] = NvsStore::getServoCalibration(static_cast<ServoId>(i));
    }
    bool seen[kServoCount] = {};
    for (JsonObjectConst o : servos.as<JsonArrayConst>()) {
      int i = findServo(o);
      if (i < 0) return "unknown_servo";
      if (seen[i]) return "duplicate_servo";
      seen[i] = true;
      ServoCalibration cal;
      if (!readPulse(o, "minUs", cal.minUs) || !readPulse(o, "centerUs", cal.centerUs) ||
          !readPulse(o, "maxUs", cal.maxUs) || !readPulse(o, "closedUs", cal.closedUs) ||
          !readPulse(o, "halfUs", cal.halfUs) || !readPulse(o, "openUs", cal.openUs) || !o["inverted"].is<bool>()) {
        return "invalid_servo_fields";
      }
      cal.inverted = o["inverted"].as<bool>();
      if (ServoRoutes::calibrationError(static_cast<ServoId>(i), cal) != nullptr) {
        return "invalid_servo_calibration";
      }
      plan.servos[i] = cal;
    }
    plan.hasServos = true;
  }

  JsonVariantConst wifi = doc["wifi"];
  if (!wifi.isNull()) {
    if (!wifi["ssid"].is<const char *>() || !wifi["password"].is<const char *>()) return "invalid_wifi";
    plan.ssid = wifi["ssid"].as<const char *>();
    plan.password = wifi["password"].as<const char *>();
    if (plan.ssid.length() > 32 || plan.password.length() > 64) return "invalid_wifi";
    // An empty SSID (backup taken while unconfigured) restores nothing: it
    // must not wipe the target's working WiFi.
    plan.hasWifi = plan.ssid.length() > 0;
  }

  JsonVariantConst pw = doc["passwords"];
  if (!pw.isNull()) {
    if (!isHashOrEmpty(pw["adminHa1"]) || !isHashOrEmpty(pw["userHa1"]) || !isHashOrEmpty(pw["otaMd5"])) {
      return "invalid_passwords";
    }
    plan.hashes.adminHa1 = pw["adminHa1"].as<const char *>();
    plan.hashes.userHa1 = pw["userHa1"].as<const char *>();
    plan.hashes.otaMd5 = pw["otaMd5"].as<const char *>();
    // Both derive from the admin password (nvs_store.h): set or empty together.
    if ((plan.hashes.adminHa1.length() == 0) != (plan.hashes.otaMd5.length() == 0)) return "invalid_passwords";
    plan.hasPasswords = true;
  }

  if (!plan.hasDeviceName && !plan.hasHostname && !plan.hasNaturalMode && !plan.hasOtaNetwork && !plan.hasRadar &&
      !plan.hasLed && !plan.hasServos && !plan.hasWifi && !plan.hasPasswords) {
    return "nothing_to_restore";
  }
  return nullptr;
}

void handleRestoreBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  JsonDocument reqDoc;
  if (!JsonHelpers::collectJsonBody(request, data, len, index, total, reqDoc)) {
    return; // more chunks pending, or an error response was already sent
  }

  RestorePlan plan;
  if (const char *error = buildPlan(reqDoc, plan)) {
    sendJsonError(request, 400, error);
    return;
  }

  // The two writes that can report failure go first, so a full/broken NVS
  // most likely fails before anything else has changed.
  JsonDocument doc;
  JsonArray restored = doc["restored"].to<JsonArray>();
  const char *error = nullptr;
  if (plan.hasServos) {
    if (NvsStore::setServoCalibrationTable(plan.servos)) {
      restored.add("servos");
    } else {
      error = "nvs_write_failed";
    }
  }
  if (!error && plan.hasPasswords) {
    if (NvsStore::setAuthHashes(plan.hashes)) {
      restored.add("passwords");
    } else {
      error = "nvs_write_failed";
    }
  }
  if (!error) {
    if (plan.hasDeviceName) NvsStore::setDeviceName(plan.deviceName);
    if (plan.hasHostname) NvsStore::setHostname(plan.hostname);
    if (plan.hasDeviceName || plan.hasHostname) restored.add("names");
    if (plan.hasNaturalMode) NvsStore::setNaturalModeEnabled(plan.naturalMode);
    if (plan.hasOtaNetwork) NvsStore::setOtaNetworkEnabled(plan.otaNetwork);
    if (plan.hasNaturalMode || plan.hasOtaNetwork) restored.add("system");
    if (plan.hasRadar) {
      NvsStore::setRadarType(plan.radarType);
      NvsStore::setRadarBaudRate(plan.radarBaud);
      restored.add("radar");
    }
    if (plan.hasLed) {
      NvsStore::setLedEnabled(plan.ledEnabled);
      NvsStore::setLedColorConfig(plan.led);
      restored.add("led");
    }
    if (plan.hasWifi) {
      // Saved directly, unlike POST /api/wifi/connect which only saves a
      // link that held: if these don't work at the new location, the
      // device falls back to the setup AP as on any failed connect.
      NvsStore::saveWifiCredentials(plan.ssid, plan.password);
      restored.add("wifi");
    }
  }

  // Restart whenever something was written: the modules cache these
  // settings in RAM (servo limits, LED, auth, radar, WiFi), and a reboot
  // is the one way to apply all of them consistently.
  bool reboot = restored.size() > 0;
  doc["success"] = error == nullptr;
  if (error) doc["error"] = error;
  doc["rebooting"] = reboot;
  if (error) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    response->setCode(500);
    serializeJson(doc, *response);
    request->send(response);
  } else {
    sendJson(request, doc);
  }
  if (reboot) {
    Serial.printf("[Backup] Settings restored (%u section(s)); rebooting.\n", static_cast<unsigned>(restored.size()));
    RestRoutes::scheduleReboot();
  }
}

} // namespace

namespace BackupRoutes {

void registerRoutes(AsyncWebServer &server) {
  server.on("/api/system/backup", HTTP_GET, handleGetBackup);
  server.on("/api/system/restore", HTTP_POST, JsonHelpers::requireBody, nullptr, handleRestoreBody);
}

} // namespace BackupRoutes
