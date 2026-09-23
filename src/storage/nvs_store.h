// nvs_store.h — thin wrapper around Preferences.h (NVS) for the small,
// OTA-survivable config values described in architecture plan §3.
//
// Namespaces/keys (Phase 1 scope):
//   "wifi"   -> ssid, password, hostname
//   "system" -> deviceName, naturalModeEnabled, ledEnabled,
//                otaNetworkEnabled
//                (naturalModeEnabled/ledEnabled are placeholders reserved
//                 for Phase 4/6, no behavior built around them yet;
//                 otaNetworkEnabled is read by OtaManager as of Phase 2)
//
// Phase 3 adds a third namespace:
//   "servocal" -> one blob (key "calTable") packing a 7-entry
//                 ServoCalibration array, indexed by ServoId (pin_map.h).
//                 A packed-blob array was chosen over 28 individual keys
//                 per plan §3's "small blob" NVS guidance — one
//                 get/putBytes call per read/write instead of 4 per
//                 servo.
//
// Phase 6 adds a fourth namespace:
//   "led" -> one blob (key "cfg") packing LedColorConfig
//            (brightness/colorL/colorR/effect), same "small blob" pattern
//            as servocal above. `enabled` deliberately stays in the
//            existing "system"/ledEnabled key (Phase 1 placeholder) rather
//            than being duplicated here.

#pragma once

#include <Arduino.h>

#include "pin_map.h" // ServoId

struct WifiCredentials {
  String ssid;
  String password;
  bool valid = false; // true if a non-empty ssid was found in NVS
};

// Per-servo pulse-width calibration. Defaults are a conservative,
// SG90-safe range (1000-2000us around a 1500us center) rather than the
// absolute extreme ESP32Servo/hobby-servo range (~500-2500us) — safe
// defaults matter here because these apply to *unconfigured* hardware
// (first boot, before the user has run calibration), and driving an
// un-calibrated SG90 to a hard mechanical stop can strain or strip its
// gears. Real calibration (POST /api/servos/config) is expected to widen
// this per-servo once the mechanism is assembled and tested.
//
// Field meaning per servo kind:
//   - minUs/maxUs: hard mechanical safety limits, every servo. The motion
//     engine never drives past them (calibration test pulses may, see
//     ServoHal::setRawPulseUs()).
//   - centerUs: pan/tilt "looking straight ahead" (0deg); aux neutral.
//     Unused by lids.
//   - closedUs/halfUs/openUs: lids only. closedUs is the pulse where
//     upper and lower lid *just touch* with the eyes looking straight
//     ahead (normalized lid 0.0), halfUs is half open (0.5) and openUs is
//     fully open (1.0). Lid linkages aren't linear and differ per eye, so
//     a straight line from closed to open put e.g. "85% open" at visibly
//     different heights left vs right; the middle point makes the mapping
//     two linear pieces that follow each linkage closely. The points
//     encode direction themselves (openUs may be below closedUs), so
//     `inverted` is ignored for lids.
//   - inverted: pan/tilt/aux only — flips which way positive degrees go.
struct ServoCalibration {
  uint16_t minUs = 1000;
  uint16_t centerUs = 1500;
  uint16_t maxUs = 2000;
  uint16_t closedUs = 1000;
  uint16_t openUs = 2000;
  uint16_t halfUs = 1500;
  bool inverted = false;
};

// True for the four eyelid servos (the ones using closedUs/halfUs/openUs).
inline bool isLidServo(ServoId id) {
  return id == ServoId::LidUpperL || id == ServoId::LidLowerL || id == ServoId::LidUpperR ||
         id == ServoId::LidLowerR;
}

namespace NvsStore {

// Must be called once from setup() before any other NvsStore call.
void begin();

// --- wifi namespace ------------------------------------------------------
WifiCredentials getWifiCredentials();
void saveWifiCredentials(const String &ssid, const String &password);
void clearWifiCredentials();
String getHostname();
void setHostname(const String &hostname);

// --- system namespace ------------------------------------------------------
String getDeviceName();
void setDeviceName(const String &name);

// Reserved since Phase 1 with sensible defaults; read by
// NaturalModeCoupler (Phase 4) and LedController (Phase 6) respectively.
bool getNaturalModeEnabled();
void setNaturalModeEnabled(bool enabled);
bool getLedEnabled();
void setLedEnabled(bool enabled);

// Read by OtaManager (Phase 2). Default true — ArduinoOTA is on by
// default once STA-connected; can be disabled for security-conscious
// users per architecture plan §8.
bool getOtaNetworkEnabled();
void setOtaNetworkEnabled(bool enabled);

// LD2420 UART baud rate (bring-up finding, 2026-09-22): the sensor's
// actual baud depends on its firmware version (115200 for fw >= v1.5.3,
// 256000 for older — see LD2420GeoGab's own doc comment), which isn't
// knowable in advance from the ESP32 side. Previously a compile-time
// GG_BAUDRATE build flag, requiring a full rebuild+reflash to try a
// different speed during bring-up; now a runtime NVS value so the Radar
// setup page can change it with just a device reboot. Default 115200
// matches the library's own compiled-in default. Read by
// Ld2420Sensor::begin() only (RADAR_LD2420 builds) — LD2450 uses a fixed
// protocol baud, not a user-adjustable one.
uint32_t getRadarBaudRate();
void setRadarBaudRate(uint32_t baudRate);

// --- servocal namespace (Phase 3) -----------------------------------------
// Returns defaults (see ServoCalibration above) if nothing has been saved
// yet. Tables saved by older firmware are migrated on read, reproducing
// the old lid mapping exactly: pre-closed/open tables (min/center/max/
// inverted only) derive closedUs/openUs from min/max and the old per-lid
// `inverted` flag; tables without halfUs get it halfway between closedUs
// and openUs. Any other size mismatch is treated as "nothing saved" rather
// than reinterpreting stale/foreign bytes.
ServoCalibration getServoCalibration(ServoId id);
// The defaults used when nothing is saved: ServoCalibration's uniform pulse
// range, with LidUpperL and LidLowerR's closed/open swapped (mechanism
// mounting reverses those two lids).
ServoCalibration getDefaultServoCalibration(ServoId id);
void setServoCalibration(ServoId id, const ServoCalibration &cal);

// --- led namespace (Phase 6) -----------------------------------------------
// Brightness/colorL/colorR/effect, packed as one blob (see LedColorConfig
// below) in a dedicated "led" namespace — mirrors servocal's "small blob"
// pattern above. `effect` is stored as a raw uint8_t (see
// hal/led_controller.h's LedEffect enum for the meaning of each value) so
// this header doesn't need to depend on hal/led_controller.h for an enum
// type. `enabled` is NOT part of this struct — it's the existing "system"
// namespace's ledEnabled key (getLedEnabled()/setLedEnabled() above),
// reused rather than duplicated, per the Phase 1 placeholder this phase
// was told to build on.
struct LedColorConfig {
  uint8_t brightness = 128;
  uint8_t colorL[3] = {0, 120, 255}; // R,G,B; default a cool blue-ish glow
  uint8_t colorR[3] = {0, 120, 255};
  uint8_t effect = 0; // LedEffect::Off
};

// Returns defaults (see LedColorConfig above) if nothing has been saved
// yet, or if the stored blob's size doesn't match the current
// LedColorConfig layout — same "size mismatch == nothing saved" treatment
// as getServoCalibration() above.
LedColorConfig getLedColorConfig();
void setLedColorConfig(const LedColorConfig &cfg);

} // namespace NvsStore
