// backup_routes.h — settings backup / restore (Firmware setup page).
//
// Every user setting lives in NVS (storage/nvs_store.h); LittleFS only
// holds the shipped web UI and greeting sequence. A backup is therefore a
// JSON rendering of the NVS settings, written field by field rather than
// as raw NVS blobs, so it restores onto any board and any later firmware
// regardless of struct layout (e.g. the servocal blob migrations).
//
// Routes registered (both admin-only, see kAdminPrefixes in net/auth.cpp):
//   GET  /api/system/backup[?secrets=1]
//        The saved settings as a downloadable JSON file. WiFi credentials
//        and the password hashes are included only with secrets=1: the
//        WiFi password is then in the file in plain text.
//   POST /api/system/restore   (a backup file as the JSON body)
//        Validates the whole file first and writes nothing if any part is
//        invalid. Every section is optional; a section that is present
//        replaces the stored values, anything absent keeps the target
//        device's current values. Then reboots (deferred, after the
//        response) so every module starts from the restored settings.
//
// File layout (formatVersion 1):
//   { "format": "esp-magic-eyes-settings", "formatVersion": 1,
//     "firmwareVersion": "...",                   informational only
//     "deviceName": "...", "hostname": "...",
//     "naturalMode": bool, "otaNetworkEnabled": bool,
//     "radar": {"type": "none"|"ld2420"|"ld2450", "baudRate": n},
//     "led": {"enabled", "brightness", "colorL": {r,g,b}, "colorR", "effect"},
//     "servos": [{"name", "servoId", "minUs", "centerUs", "maxUs",
//                 "closedUs", "halfUs", "openUs", "inverted"}, ... x7],
//     "wifi": {"ssid", "password"},                         secrets only
//     "passwords": {"adminHa1", "userHa1", "otaMd5"} }      secrets only
// Servos are matched by `name` (ServoHal::servoIdName()), falling back to
// `servoId`, so a renumbered ServoId enum can't put one servo's
// calibration onto another.

#pragma once

class AsyncWebServer;

namespace BackupRoutes {

// Registers the routes above. Does not call server.begin().
void registerRoutes(AsyncWebServer &server);

} // namespace BackupRoutes
