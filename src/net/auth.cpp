#include "net/auth.h"

#include <MD5Builder.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "motion/eye_pose.h" // deadlineReached()
#include "storage/nvs_store.h"

namespace {

// Brute-force brake: after kMaxFailures wrong credentials within
// kFailureWindowMs, every protected request gets 429 for kLockoutMs —
// correct credentials included, or the lockout wouldn't slow a guesser
// down. A request without an Authorization header (a browser's first try,
// before it has prompted) is not a failure. The flip side is that someone
// on the LAN can keep the UI locked out; on a hobby device that's a better
// trade than unlimited guessing.
constexpr uint32_t kMaxFailures = 10;
constexpr uint32_t kFailureWindowMs = 60000;
constexpr uint32_t kLockoutMs = 30000;

SemaphoreHandle_t gMutex = nullptr;
NvsStore::AuthHashes gHashes; // guarded by gMutex
// True when the stored hashes couldn't be loaded (NVS error or corrupt
// record). Every protected request is then refused rather than treated as
// "no password" — failing open would unprotect the device. Only a
// successful clearAll() (physical access) or setPassword() recovers.
std::atomic<bool> gStorageFailed{false};
std::atomic<uint32_t> gGeneration{0};

// Failure/lockout state. Written only by middleware() (AsyncTCP task);
// atomics because allowed() reads it and clearAll() resets it from loop().
std::atomic<uint32_t> gFailures{0};
std::atomic<uint32_t> gWindowStartMs{0};
std::atomic<uint32_t> gLockoutUntilMs{0};
std::atomic<bool> gLockedOut{false};

class AuthLock {
public:
  AuthLock() { xSemaphoreTake(gMutex, portMAX_DELAY); }
  ~AuthLock() { xSemaphoreGive(gMutex); }
  AuthLock(const AuthLock &) = delete;
  AuthLock &operator=(const AuthLock &) = delete;
};

NvsStore::AuthHashes snapshot() {
  AuthLock lock;
  return gHashes;
}

String md5Hex(const String &input) {
  MD5Builder md5;
  md5.begin();
  md5.add(input);
  md5.calculate();
  return md5.toString(); // 32 lowercase hex, matching the library's digest HA1 format
}

String digestHa1(const char *user, const String &password) {
  return md5Hex(String(user) + ":" + Auth::kRealm + ":" + password);
}

bool lockedOut() {
  if (!gLockedOut.load()) {
    return false;
  }
  if (deadlineReached(millis(), gLockoutUntilMs.load())) {
    gLockedOut = false;
    gFailures = 0;
    return false;
  }
  return true;
}

void recordFailure() {
  uint32_t now = millis();
  if (gFailures.load() == 0 || deadlineReached(now, gWindowStartMs.load() + kFailureWindowMs)) {
    gWindowStartMs = now;
    gFailures = 0;
  }
  if (++gFailures >= kMaxFailures) {
    gLockoutUntilMs = now + kLockoutMs;
    gLockedOut = true;
    Serial.println("[Auth] Too many failed logins — refusing protected requests for 30 s.");
  }
}

// The digest response hashes the `uri="..."` field of the Authorization
// header, but request->authenticate() never compares that field with the
// URL actually requested — so a header sniffed from one request (say GET
// /api/eyes/pose) would authenticate any other URL with the same method.
// Require them to match: the header's uri minus its query string, decoded
// the same way the library decodes the request line, must equal url().
bool digestUriMatchesRequest(AsyncWebServerRequest *request) {
  const AsyncWebHeader *header = request->getHeader("Authorization");
  if (header == nullptr) {
    return false;
  }
  const String &value = header->value();
  int start = -1;
  for (int i = value.indexOf("uri=\""); i > 0; i = value.indexOf("uri=\"", i + 1)) {
    char before = value[i - 1];
    if (before == ' ' || before == ',') { // not the tail of another field name
      start = i + 5;
      break;
    }
  }
  if (start < 0) {
    return false;
  }
  int end = value.indexOf('"', start);
  if (end < 0) {
    return false;
  }
  String uri = value.substring(start, end);
  int query = uri.indexOf('?');
  if (query >= 0) {
    uri = uri.substring(0, query);
  }
  return request->urlDecode(uri) == request->url();
}

// Digest only: with passwordIsHash=true, the library's Basic branch would
// compare the raw header against the stored HA1, turning the hash itself
// into a usable password.
bool digestMatches(AsyncWebServerRequest *request, const char *user, const String &ha1) {
  return ha1.length() > 0 && request->authType() == AsyncAuthType::AUTH_DIGEST &&
         request->authenticate(user, ha1.c_str(), Auth::kRealm, true) && digestUriMatchesRequest(request);
}

// Whether a request at `level` needs any credentials at all right now.
bool needsPassword(const NvsStore::AuthHashes &h, Auth::Level level) {
  switch (level) {
    case Auth::Level::Public: return false;
    case Auth::Level::Control: return h.userHa1.length() > 0;
    case Auth::Level::Admin: return h.adminHa1.length() > 0 || h.userHa1.length() > 0;
  }
  return true;
}

// Bodies of the 401/429 answers to page (non-/api/) requests. A browser
// shows a 401's body once the user cancels its login prompt; without one
// that's a blank page. Styles are inline because css/app.css is itself
// password-protected. Colors follow app.css. API requests get no body:
// js/api.js turns the status code into a message.
#define AUTH_PAGE_HEAD                                                                                     \
  "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"                                        \
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Magic Eyes</title><style>" \
  ":root{--bg:#f4f5f7;--card:#fff;--text:#1b1f24;--muted:#5b6472;--accent:#2f6fed}"                        \
  "@media(prefers-color-scheme:dark){:root{--bg:#14161a;--card:#1e2126;--text:#eef0f2;--muted:#a2aab5}}"   \
  "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"                 \
  "background:var(--bg);color:var(--text);font:16px/1.5 system-ui,sans-serif}"                             \
  "main{background:var(--card);border-radius:10px;padding:1.5rem;margin:1rem;max-width:26rem;"             \
  "box-shadow:0 1px 3px rgba(0,0,0,.15)}h1{font-size:1.3rem;margin:0 0 .5rem}p{margin:.5rem 0}"            \
  ".m{color:var(--muted);font-size:.9rem}code{font-size:.95em}"                                            \
  "button{margin-top:.75rem;width:100%;padding:.7rem;border:0;border-radius:8px;"                          \
  "background:var(--accent);color:#fff;font-size:1rem}</style></head><body><main>"
#define AUTH_PAGE_TAIL "<button onclick=\"location.reload()\">Try again</button></main></body></html>"

const char kLoginRequiredPage[] = AUTH_PAGE_HEAD
    "<h1>&#128065; Login required</h1>"
    "<p>This Magic Eyes device is password protected.</p>"
    "<p class=\"m\">Log in with user name <code>user</code> for the controls, or <code>admin</code> for the "
    "setup pages (the admin password also works for the controls).</p>"
    "<p class=\"m\">Forgot the password? Hold the device's BOOT button for 5 seconds (this also resets its "
    "WiFi), or type <code>auth reset</code> in its USB serial console.</p>" AUTH_PAGE_TAIL;

const char kLockedOutPage[] = AUTH_PAGE_HEAD
    "<h1>&#128065; Too many attempts</h1>"
    "<p>There were too many wrong passwords. Wait 30 seconds, then try again.</p>" AUTH_PAGE_TAIL;

#undef AUTH_PAGE_HEAD
#undef AUTH_PAGE_TAIL

bool isApiRequest(AsyncWebServerRequest *request) { return request->url().startsWith("/api/"); }

// URL prefixes that need Admin. Anything else not Public is Control.
const char *const kAdminPrefixes[] = {
    "/setup",            "/api/wifi/",          "/api/servos/",        "/api/radar/config",
    "/api/led/",         "/api/ota/",           "/api/system/reboot",  "/api/system/config",
    "/api/auth/",
};

} // namespace

namespace Auth {

void begin() {
  gMutex = xSemaphoreCreateMutex();
  NvsStore::AuthHashes stored;
  if (!NvsStore::getAuthHashes(stored)) {
    gStorageFailed = true;
    Serial.println("[Auth] Could not read stored passwords — refusing all protected requests. "
                   "Use serial 'auth reset' or the BOOT-button hold to clear them.");
    return;
  }
  {
    AuthLock lock;
    gHashes = stored;
  }
  Serial.printf("[Auth] Control password %s, admin password %s.\n", stored.userHa1.length() ? "set" : "not set",
                stored.adminHa1.length() ? "set" : "not set");
}

Level requiredLevel(AsyncWebServerRequest *request) {
  const String &url = request->url();
  if (url == "/api/auth/status") {
    return Level::Public;
  }
  // The static handler hands the path to LittleFS, which resolves "." and
  // ".." segments: "/./setup/wifi.html" or "/control/../setup/wifi.html"
  // must not slip past the /setup prefix. "/." catches every dot segment
  // (and hidden files); nothing legitimate uses these, "//" or backslashes,
  // so treat them as the strictest level.
  if (url.indexOf("/.") >= 0 || url.indexOf("..") >= 0 || url.indexOf("//") >= 0 || url.indexOf('\\') >= 0) {
    return Level::Admin;
  }
  for (const char *prefix : kAdminPrefixes) {
    if (url.startsWith(prefix)) {
      return Level::Admin;
    }
  }
  return Level::Control;
}

bool allowed(AsyncWebServerRequest *request, Level level) {
  if (level == Level::Public) {
    return true;
  }
  if (gStorageFailed) {
    return false;
  }
  NvsStore::AuthHashes h = snapshot();
  if (!needsPassword(h, level)) {
    return true;
  }
  if (lockedOut()) {
    return false;
  }
  if (digestMatches(request, kAdminUser, h.adminHa1)) {
    return true; // admin satisfies every level
  }
  // "user" satisfies Control, and Admin while no admin password exists.
  bool userAccepted = level == Level::Control || h.adminHa1.length() == 0;
  return userAccepted && digestMatches(request, kControlUser, h.userHa1);
}

void middleware(AsyncWebServerRequest *request, ArMiddlewareNext next) {
  // A response set before the middleware runs came from a body handler,
  // which only answers after collectJsonBody() passed allowed() — its
  // check comes before any response it sends. Don't re-check: the handler
  // may have just changed the password (setting the first admin password
  // would otherwise turn its own success into a 401). next() reaches
  // requireBody(), which leaves the existing response alone.
  if (request->getResponse() != nullptr) {
    next();
    return;
  }
  Level level = requiredLevel(request);
  if (allowed(request, level)) {
    next();
    return;
  }
  bool api = isApiRequest(request);
  if (lockedOut()) {
    if (api) {
      request->send(429, "application/json", "{\"success\":false,\"error\":\"too_many_attempts\"}");
    } else {
      request->send(429, "text/html", kLockedOutPage);
    }
    return;
  }
  if (request->authType() != AsyncAuthType::AUTH_NONE) {
    recordFailure(); // wrong credentials, not just a first unauthenticated try
  }
  request->requestAuthentication(AsyncAuthType::AUTH_DIGEST, kRealm, api ? nullptr : kLoginRequiredPage);
}

bool setPassword(Level level, const String &password) {
  if (level == Level::Public) {
    return false;
  }
  if (password.length() > 0 &&
      (password.length() < kMinPasswordLength || password.length() > kMaxPasswordLength)) {
    return false;
  }
  AuthLock lock;
  NvsStore::AuthHashes updated = gHashes;
  if (level == Level::Admin) {
    updated.adminHa1 = password.length() ? digestHa1(kAdminUser, password) : String();
    updated.otaMd5 = password.length() ? md5Hex(password) : String();
  } else {
    updated.userHa1 = password.length() ? digestHa1(kControlUser, password) : String();
  }
  if (!NvsStore::setAuthHashes(updated)) {
    return false;
  }
  gHashes = updated;
  gStorageFailed = false; // NVS is writable again and gHashes matches it
  ++gGeneration;
  Serial.printf("[Auth] %s password %s.\n", level == Level::Admin ? "Admin" : "Control",
                password.length() ? "set" : "cleared");
  return true;
}

bool clearAll() {
  {
    AuthLock lock; // held across the NVS write so a concurrent setPassword() can't interleave
    if (!NvsStore::clearAuth()) {
      // Keep the live hashes: clearing only RAM would report success while
      // the old passwords come back on the next boot.
      Serial.println("[Auth] Clearing passwords FAILED (NVS write error) — passwords unchanged.");
      return false;
    }
    gHashes = NvsStore::AuthHashes{};
  }
  gStorageFailed = false;
  ++gGeneration;
  gLockedOut = false;
  gFailures = 0;
  Serial.println("[Auth] All passwords cleared.");
  return true;
}

bool hasPassword(Level level) {
  NvsStore::AuthHashes h = snapshot();
  switch (level) {
    case Level::Control: return h.userHa1.length() > 0;
    case Level::Admin: return h.adminHa1.length() > 0;
    default: return false;
  }
}

String otaPasswordMd5() { return snapshot().otaMd5; }

bool storageOk() { return !gStorageFailed; }

uint32_t generation() { return gGeneration.load(); }

} // namespace Auth
