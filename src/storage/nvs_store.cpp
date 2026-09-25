#include "storage/nvs_store.h"

#include <Preferences.h>

#include <cctype>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr const char *kWifiNamespace = "wifi";
constexpr const char *kSystemNamespace = "system";
constexpr const char *kServoCalNamespace = "servocal";
constexpr const char *kLedNamespace = "led";
constexpr const char *kAuthNamespace = "auth";

constexpr const char *kKeySsid = "ssid";
constexpr const char *kKeyPassword = "password";
constexpr const char *kKeyHostname = "hostname";

constexpr const char *kKeyDeviceName = "devName";
constexpr const char *kKeyNaturalMode = "naturalMode";
constexpr const char *kKeyLedEnabled = "ledEnabled";
constexpr const char *kKeyOtaNetworkEnabled = "otaNetEn";
constexpr const char *kKeyRadarBaud = "radarBaud";
constexpr const char *kKeyRadarType = "radarType";
constexpr const char *kKeyServoCalTable = "calTable";
constexpr const char *kKeyLedConfig = "cfg";
constexpr const char *kKeyAuthRecord = "hashes";

constexpr const char *kDefaultHostname = "esp-magic-eyes";
constexpr const char *kDefaultDeviceName = "Magic Eyes";
constexpr uint32_t kDefaultRadarBaud = 115200; // LD2420GeoGab's own compiled-in default

constexpr size_t kServoCount = static_cast<size_t>(ServoId::Count);

Preferences wifiPrefs;
Preferences systemPrefs;
Preferences servoCalPrefs;
Preferences ledPrefs;
Preferences authPrefs;

// NvsStore is called from several tasks — loop() (WifiManager, serial
// console), the AsyncTCP task (every API route) and setup(). The shared
// Preferences instances above carry per-open state (handle, started flag),
// so two tasks interleaving begin()/get/end() on one of them corrupt each
// other, and setServoCalibration()'s load-modify-save of the table could
// lose a concurrent write. Every public function holds this for its whole
// body. Created on first use, which happens in setup() (NvsStore::begin())
// before any other task can call in.
SemaphoreHandle_t gMutex = nullptr;

class NvsLock {
public:
  NvsLock() {
    if (gMutex == nullptr) {
      gMutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(gMutex, portMAX_DELAY);
  }
  ~NvsLock() { xSemaphoreGive(gMutex); }
  NvsLock(const NvsLock &) = delete;
  NvsLock &operator=(const NvsLock &) = delete;
};

// ServoCalibration layout saved by firmware before closedUs/openUs
// existed. Lids then mapped normalized 0..1 across minUs..maxUs, reversed
// when `inverted` was set.
struct LegacyServoCalibrationV1 {
  uint16_t minUs;
  uint16_t centerUs;
  uint16_t maxUs;
  bool inverted;
};
// Layout saved after closedUs/openUs were added, before halfUs existed.
struct LegacyServoCalibrationV2 {
  uint16_t minUs;
  uint16_t centerUs;
  uint16_t maxUs;
  uint16_t closedUs;
  uint16_t openUs;
  bool inverted;
};
static_assert(sizeof(LegacyServoCalibrationV1) != sizeof(ServoCalibration) &&
                  sizeof(LegacyServoCalibrationV2) != sizeof(ServoCalibration) &&
                  sizeof(LegacyServoCalibrationV1) != sizeof(LegacyServoCalibrationV2),
              "calibration layouts must be distinguishable by size");

uint16_t halfway(uint16_t a, uint16_t b) { return static_cast<uint16_t>((a + b) / 2); }

ServoCalibration migrateLegacy(const LegacyServoCalibrationV1 &old) {
  ServoCalibration cal;
  cal.minUs = old.minUs;
  cal.centerUs = old.centerUs;
  cal.maxUs = old.maxUs;
  cal.inverted = old.inverted;
  cal.closedUs = old.inverted ? old.maxUs : old.minUs;
  cal.openUs = old.inverted ? old.minUs : old.maxUs;
  cal.halfUs = halfway(cal.closedUs, cal.openUs);
  return cal;
}

ServoCalibration migrateLegacy(const LegacyServoCalibrationV2 &old) {
  ServoCalibration cal;
  cal.minUs = old.minUs;
  cal.centerUs = old.centerUs;
  cal.maxUs = old.maxUs;
  cal.inverted = old.inverted;
  cal.closedUs = old.closedUs;
  cal.openUs = old.openUs;
  cal.halfUs = halfway(old.closedUs, old.openUs);
  return cal;
}

// Reads and migrates a table stored in layout `Legacy`, if that's the
// layout the stored size says it is.
template <typename Legacy, size_t N>
bool loadLegacyTable(Preferences &prefs, const char *key, size_t storedBytes, ServoCalibration (&table)[N]) {
  if (storedBytes != sizeof(Legacy) * N) {
    return false;
  }
  Legacy legacy[N];
  if (prefs.getBytes(key, legacy, storedBytes) != storedBytes) {
    return true; // layout recognized but unreadable — keep `table`'s defaults
  }
  for (size_t i = 0; i < N; ++i) {
    table[i] = migrateLegacy(legacy[i]);
  }
  return true;
}

// Loads the full 7-entry calibration table into `table`. It is first
// filled with per-servo defaults (NvsStore::getDefaultServoCalibration());
// on first boot, or on an unrecognized stored size, `table` is simply left
// at those defaults. A legacy (V1/V2) table is migrated in RAM; it is
// rewritten in the new layout on the next save.
void loadServoCalTable(ServoCalibration (&table)[kServoCount]) {
  for (size_t i = 0; i < kServoCount; ++i) {
    table[i] = NvsStore::getDefaultServoCalibration(static_cast<ServoId>(i));
  }
  servoCalPrefs.begin(kServoCalNamespace, true);
  size_t expectedBytes = sizeof(ServoCalibration) * kServoCount;
  size_t storedBytes = servoCalPrefs.getBytesLength(kKeyServoCalTable);
  if (storedBytes == expectedBytes) {
    servoCalPrefs.getBytes(kKeyServoCalTable, table, expectedBytes);
  } else if (!loadLegacyTable<LegacyServoCalibrationV2>(servoCalPrefs, kKeyServoCalTable, storedBytes, table)) {
    loadLegacyTable<LegacyServoCalibrationV1>(servoCalPrefs, kKeyServoCalTable, storedBytes, table);
  }
  servoCalPrefs.end();
}

bool saveServoCalTable(const ServoCalibration (&table)[kServoCount]) {
  constexpr size_t kTableBytes = sizeof(ServoCalibration) * kServoCount;
  if (!servoCalPrefs.begin(kServoCalNamespace, false)) {
    return false;
  }
  size_t written = servoCalPrefs.putBytes(kKeyServoCalTable, table, kTableBytes);
  servoCalPrefs.end();
  return written == kTableBytes;
}


// The auth namespace's single blob: three MD5 hex strings (32 chars + NUL),
// empty = that password isn't set. Stored as one entry so updates are
// atomic (see setAuthHashes()).
constexpr size_t kHashChars = 32;
struct AuthRecord {
  char adminHa1[kHashChars + 1] = {};
  char userHa1[kHashChars + 1] = {};
  char otaMd5[kHashChars + 1] = {};
};

bool isHashOrEmpty(const char *s, size_t len) {
  if (len == 0) return true;
  if (len != kHashChars) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

// A field that isn't empty or 32 hex digits means a corrupt record.
bool readHashField(const char (&field)[kHashChars + 1], String &out) {
  size_t len = strnlen(field, sizeof(field));
  if (len == sizeof(field) || !isHashOrEmpty(field, len)) {
    return false;
  }
  out = String(field);
  return true;
}

bool writeHashField(const String &value, char (&field)[kHashChars + 1]) {
  if (!isHashOrEmpty(value.c_str(), value.length())) {
    return false;
  }
  memcpy(field, value.c_str(), value.length() + 1);
  return true;
}

} // namespace

namespace NvsStore {

void begin() {
  NvsLock lock;
  // Namespaces are opened/closed per-call below (Preferences is cheap to
  // open/close and this avoids holding two NVS handles open for the whole
  // app lifetime). begin() here just verifies NVS is reachable early and
  // seeds defaults on first boot.
  if (wifiPrefs.begin(kWifiNamespace, false)) {
    wifiPrefs.end();
  }
  if (systemPrefs.begin(kSystemNamespace, false)) {
    systemPrefs.end();
  }
}

WifiCredentials getWifiCredentials() {
  NvsLock lock;
  WifiCredentials creds;
  wifiPrefs.begin(kWifiNamespace, true);
  creds.ssid = wifiPrefs.getString(kKeySsid, "");
  creds.password = wifiPrefs.getString(kKeyPassword, "");
  wifiPrefs.end();
  creds.valid = creds.ssid.length() > 0;
  return creds;
}

void saveWifiCredentials(const String &ssid, const String &password) {
  NvsLock lock;
  wifiPrefs.begin(kWifiNamespace, false);
  wifiPrefs.putString(kKeySsid, ssid);
  wifiPrefs.putString(kKeyPassword, password);
  wifiPrefs.end();
}

void clearWifiCredentials() {
  NvsLock lock;
  wifiPrefs.begin(kWifiNamespace, false);
  wifiPrefs.remove(kKeySsid);
  wifiPrefs.remove(kKeyPassword);
  wifiPrefs.end();
}

String getHostname() {
  NvsLock lock;
  wifiPrefs.begin(kWifiNamespace, true);
  String hostname = wifiPrefs.getString(kKeyHostname, kDefaultHostname);
  wifiPrefs.end();
  return hostname;
}

void setHostname(const String &hostname) {
  NvsLock lock;
  wifiPrefs.begin(kWifiNamespace, false);
  wifiPrefs.putString(kKeyHostname, hostname);
  wifiPrefs.end();
}

String getDeviceName() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  String name = systemPrefs.getString(kKeyDeviceName, kDefaultDeviceName);
  systemPrefs.end();
  return name;
}

void setDeviceName(const String &name) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putString(kKeyDeviceName, name);
  systemPrefs.end();
}

bool getNaturalModeEnabled() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  bool enabled = systemPrefs.getBool(kKeyNaturalMode, true);
  systemPrefs.end();
  return enabled;
}

void setNaturalModeEnabled(bool enabled) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putBool(kKeyNaturalMode, enabled);
  systemPrefs.end();
}

bool getLedEnabled() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  bool enabled = systemPrefs.getBool(kKeyLedEnabled, false);
  systemPrefs.end();
  return enabled;
}

void setLedEnabled(bool enabled) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putBool(kKeyLedEnabled, enabled);
  systemPrefs.end();
}

bool getOtaNetworkEnabled() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  bool enabled = systemPrefs.getBool(kKeyOtaNetworkEnabled, true);
  systemPrefs.end();
  return enabled;
}

void setOtaNetworkEnabled(bool enabled) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putBool(kKeyOtaNetworkEnabled, enabled);
  systemPrefs.end();
}

uint32_t getRadarBaudRate() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  uint32_t baud = systemPrefs.getUInt(kKeyRadarBaud, kDefaultRadarBaud);
  systemPrefs.end();
  return baud;
}

void setRadarBaudRate(uint32_t baudRate) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putUInt(kKeyRadarBaud, baudRate);
  systemPrefs.end();
}

RadarType getRadarType() {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, true);
  uint8_t raw = systemPrefs.getUChar(kKeyRadarType, static_cast<uint8_t>(RadarType::LD2420));
  systemPrefs.end();
  if (raw > static_cast<uint8_t>(RadarType::LD2450)) {
    return RadarType::LD2420; // unknown/corrupt value — fall back to the default
  }
  return static_cast<RadarType>(raw);
}

void setRadarType(RadarType type) {
  NvsLock lock;
  systemPrefs.begin(kSystemNamespace, false);
  systemPrefs.putUChar(kKeyRadarType, static_cast<uint8_t>(type));
  systemPrefs.end();
}

ServoCalibration getDefaultServoCalibration(ServoId id) {
  ServoCalibration cal; // uniform pulse-range defaults (see nvs_store.h)
  // The eye mechanism's mounting reverses the direction of these two lids.
  if (id == ServoId::LidUpperL || id == ServoId::LidLowerR) {
    cal.closedUs = cal.maxUs;
    cal.openUs = cal.minUs;
  }
  return cal;
}

ServoCalibration getServoCalibration(ServoId id) {
  NvsLock lock;
  size_t i = static_cast<size_t>(id);
  if (i >= kServoCount) {
    return ServoCalibration{};
  }
  ServoCalibration table[kServoCount];
  loadServoCalTable(table);
  return table[i];
}

bool setServoCalibration(ServoId id, const ServoCalibration &cal) {
  NvsLock lock;
  size_t i = static_cast<size_t>(id);
  if (i >= kServoCount) {
    return false;
  }
  ServoCalibration table[kServoCount];
  loadServoCalTable(table);
  table[i] = cal;
  return saveServoCalTable(table);
}

LedColorConfig getLedColorConfig() {
  NvsLock lock;
  LedColorConfig cfg; // defaults (see nvs_store.h)
  ledPrefs.begin(kLedNamespace, true);
  size_t storedBytes = ledPrefs.getBytesLength(kKeyLedConfig);
  if (storedBytes == sizeof(LedColorConfig)) {
    ledPrefs.getBytes(kKeyLedConfig, &cfg, sizeof(LedColorConfig));
  }
  ledPrefs.end();
  return cfg;
}

void setLedColorConfig(const LedColorConfig &cfg) {
  NvsLock lock;
  ledPrefs.begin(kLedNamespace, false);
  ledPrefs.putBytes(kKeyLedConfig, &cfg, sizeof(LedColorConfig));
  ledPrefs.end();
}

bool getAuthHashes(AuthHashes &out) {
  NvsLock lock;
  out = AuthHashes{};
  // Read-write open: it creates the namespace on first boot, so a failure
  // here is a real NVS error rather than "never written". A read-only open
  // can't tell the two apart, and treating an error as "no passwords"
  // would silently unprotect the device.
  if (!authPrefs.begin(kAuthNamespace, false)) {
    return false;
  }
  size_t storedBytes = authPrefs.getBytesLength(kKeyAuthRecord);
  bool ok = true;
  if (storedBytes != 0) { // 0 = never set: no passwords
    AuthRecord record;
    ok = storedBytes == sizeof(record) && authPrefs.getBytes(kKeyAuthRecord, &record, sizeof(record)) == sizeof(record) &&
         readHashField(record.adminHa1, out.adminHa1) && readHashField(record.userHa1, out.userHa1) &&
         readHashField(record.otaMd5, out.otaMd5);
  }
  authPrefs.end();
  if (!ok) {
    out = AuthHashes{};
  }
  return ok;
}

bool setAuthHashes(const AuthHashes &hashes) {
  NvsLock lock;
  AuthRecord record;
  if (!writeHashField(hashes.adminHa1, record.adminHa1) || !writeHashField(hashes.userHa1, record.userHa1) ||
      !writeHashField(hashes.otaMd5, record.otaMd5)) {
    return false;
  }
  if (!authPrefs.begin(kAuthNamespace, false)) {
    return false;
  }
  // One blob, one NVS entry: it's either fully written or the old record
  // stays — never a new admin hash next to a stale OTA hash.
  size_t written = authPrefs.putBytes(kKeyAuthRecord, &record, sizeof(record));
  authPrefs.end();
  return written == sizeof(record);
}

bool clearAuth() { return setAuthHashes(AuthHashes{}); }

} // namespace NvsStore
