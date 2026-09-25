// ota_manager.h — network (IDE/ArduinoOTA) OTA, per architecture plan §4/§8.
//
// Wraps the arduino-esp32-core-provided ArduinoOTA library (no extra
// lib_deps needed). Only meaningful once WiFi is in STA_CONNECTED mode —
// there's no point advertising ArduinoOTA while only the AP setup portal
// is up, so begin() itself does nothing observable and the real
// ArduinoOTA.begin() call happens lazily from handle() on the first tick
// where WifiManager::getMode() == STA_CONNECTED (and stays running from
// then on, unless disabled).
//
// Toggleable via the "system" NVS namespace key "otaNetworkEnabled"
// (default true, see storage/nvs_store.h). The enabled flag is cached in
// RAM at begin() (and whenever setEnabled() is called) rather than read
// from NVS every loop() tick.
//
// Password: while an admin password is set (net/auth.h), ArduinoOTA
// requires it — `pio run -t upload --upload-port <ip>` then needs
// `upload_flags = --auth=<admin password>`. ArduinoOTA keeps the first
// password it's given until reboot (its setPasswordHash() is a no-op once
// one is set, and end() doesn't reset it), so after the admin password is
// changed or cleared network OTA stays stopped until the next restart —
// failing closed rather than accepting the old password.

#pragma once

namespace OtaManager {

// Call once from setup(), after NvsStore::begin(). Cheap — just caches the
// enabled flag from NVS; does not touch WiFi/ArduinoOTA yet.
void begin();

// Must be polled every loop() iteration (non-blocking). Starts ArduinoOTA
// the first time WiFi reaches STA_CONNECTED while enabled, stops it if
// disabled or WiFi drops out of STA_CONNECTED, and otherwise pumps
// ArduinoOTA.handle().
void handle();

// Updates the cached enabled flag and persists it to NVS. Intended for a
// future/optional POST /api/system/config handler. Takes effect on the
// next handle() tick (starts or stops ArduinoOTA as appropriate).
void setEnabled(bool enabled);

bool isEnabled();

// True while a changed/cleared admin password can't be applied to
// ArduinoOTA until the device restarts (see header note). Safe to call
// from any task.
bool restartRequiredForPassword();

} // namespace OtaManager
