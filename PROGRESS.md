# ESP Magic Eyes — Build Progress

Each phase must pass `pio run -e ld2420` and `pio run -e ld2450` (zero errors) before being marked done.

- [x] Phase 0 — Scaffold (platformio.ini, partitions.csv, pin map header, empty main.cpp, .vscode config)
- [x] Phase 1 — Core infra (NVS store, WiFi manager + captive portal, AsyncWebServer + placeholder page, /api/system/*)
- [x] Phase 2 — OTA (ArduinoOTA, web OTA firmware+filesystem upload, /api/ota/*)
- [x] Phase 3 — Motion engine core (ServoHAL, EyePose/EyeCommand, CommandQueue, MotionTask, /api/servos/*)
- [x] Phase 4 — Natural mode, gestures, play modes (/api/eyes/*, /api/gestures/*, /api/playmodes/*)
- [x] Phase 5 — Radar (IRadarSensor, Ld2420Sensor, Ld2450Sensor, RadarTask, /api/radar/*, tracking mode)
- [x] Phase 6 — LED (LedController/NeoPixel, /api/led/*, feature-flagged safe no-op)
- [x] Phase 7 — Frontend (setup/* and control/* pages, js/api.js, captive portal redirect, calibration + radar viz UI)
- [x] Phase 8 — Integration pass (code review, fixes, final compile of both envs, README.md)

## Notes / deviations from plan

- **Radar library swap (Phase 0, platformio.ini):** the plan's §9 skeleton
  named `bolukan/ld2420` as the LD2420 driver. Verified against the actual
  `github.com/Bolukan/ld2420` repo: it has no tags/releases and its
  `library.properties`/source is actually a fork of an **LD2410** library
  (`name=ld2410`, `includes=ld2410.h`) — not LD2420, and not published to
  the PlatformIO registry, so it can't be pinned to a version. Swapped for
  `gsieben/LD2420GeoGab @ 1.0.0`, a PlatformIO-registry-published driver
  actually targeting the HLK-LD2420 sensor. This is unused by any code in
  Phase 0 (main.cpp has no radar logic yet); Phase 5 should re-validate
  this library choice against the physical module before relying on it,
  and consider `cyrixninja/LD2420` or `mikerussellnz/LD2420` as
  alternatives if `LD2420GeoGab` doesn't fit.
- **partitions.csv SubType (Phase 0):** plan §8 specifies SubType
  `littlefs` for the data partition. The pinned toolchain's esptoolpy
  (4.5.1, `gen_esp32part.py`) rejects `littlefs` as an unknown SubType
  keyword (added in a later esptool/IDF release than what
  espressif32/framework-arduinoespressif32 currently pins). Changed
  SubType to `spiffs` (the standard tool-compatible label for a generic
  data/filesystem partition) while keeping the partition **name**
  `littlefs` and `board_build.filesystem = littlefs` in platformio.ini —
  the actual mounted filesystem is still LittleFS; only the partition
  table's SubType tag differs from the plan's literal text. See comment
  in partitions.csv.
- **Library version pins (Phase 0):** all six `lib_deps` entries pinned
  to specific current released versions (verified via `pio pkg search`
  against the live PlatformIO registry on 2026-09-22) rather than left
  floating, including ArduinoJson (pinned to `7.4.3` instead of the
  plan's floating `^7`):
  - `esp32async/AsyncTCP @ 3.5.0`
  - `esp32async/ESPAsyncWebServer @ 3.12.1`
  - `madhephaestus/ESP32Servo @ 3.2.1`
  - `adafruit/Adafruit NeoPixel @ 1.15.5`
  - `bblanchon/ArduinoJson @ 7.4.3`
  - `gsieben/LD2420GeoGab @ 1.0.0` (see radar library swap note above)

- **Phase 1 files added:**
  - `include/version.h` — `FIRMWARE_VERSION = "0.1.0-dev"`; build date/time
    is pulled from `__DATE__ __TIME__` directly at the call site
    (`src/api/rest_routes.cpp`) rather than stored as a second constant.
  - `src/storage/nvs_store.h/.cpp` — `Preferences.h` wrapper. Two
    namespaces, opened/closed per call (cheap, avoids holding NVS handles
    open for the app's whole lifetime):
    - `wifi`: keys `ssid`, `password`, `hostname` (hostname default
      `"esp-magic-eyes"`).
    - `system`: keys `devName` (default `"Magic Eyes"`), `naturalMode`
      (bool, default `true`), `ledEnabled` (bool, default `false`) — the
      latter two are Phase 4/6 placeholders per the task spec; nothing
      reads/branches on them yet.
    - Servo calibration (`servocal` namespace) is intentionally NOT part
      of this wrapper — that's Phase 3 scope.
  - `src/net/wifi_manager.h/.cpp` — `WifiMode` enum
    (`AP_SETUP`/`STA_CONNECTING`/`STA_CONNECTED`/`STA_FAILED`), fully
    non-blocking (`begin()` kicks off, `handle()` polled from `loop()`
    drives the timeout state machine + `DNSServer::processNextRequest()`,
    no `delay()` anywhere in the loop path). Bounded STA connect timeout:
    15s. On no-creds or timeout, falls back to AP mode:
    - **AP SSID**: `MagicEyes-Setup-XXXX`, where `XXXX` is the last 4 hex
      digits of `ESP.getEfuseMac()` (uppercase), e.g.
      `MagicEyes-Setup-A1B2`.
    - **AP password**: fixed, WPA2, `eyes-setup` (10 chars, meets the
      8-char WPA2 minimum). Chosen over an open network so casual
      neighbors don't land on the setup portal; documented here since
      there's no per-device secret to seed it from yet. Revisit in
      Phase 7/8 if a per-device or QR-code-provisioned password is
      wanted.
    - AP IP is the LittleFS/ESP32 default `192.168.4.1`; `DNSServer`
      wildcard-resolves all queries to it for the captive portal.
    - `connectToNetwork()`/`forgetNetwork()` implemented and exposed for
      the future `/api/wifi/connect` and `/api/wifi/forget` handlers
      (not wired to routes yet — Phase 1 only adds `/api/system/*` per
      the task spec). `connectToNetwork()` keeps the AP alive
      (`WIFI_AP_STA`) while attempting the new STA connection, and tears
      the AP down only on success.
    - Optional scan-for-SSID-list support **was implemented** (not
      skipped): a single synchronous `WiFi.scanNetworks()` runs once at
      boot, before `softAP()` starts (per the plan's research note on
      avoiding AP+scan conflicts), and results are cached in a small
      fixed array exposed via `getCachedScanResults()` for the future
      `GET /api/wifi/scan` handler.
  - `src/hal/buttons.h/.cpp` — non-blocking long-press detection on
    `FACTORY_RESET_BUTTON_PIN` (GPIO0, `INPUT_PULLUP`, active-low),
    `millis()`-based, 5s hold threshold, calls
    `WifiManager::forgetNetwork()` once per hold (guarded so it doesn't
    re-fire while still held post-trigger). Other HAL modules
    (`servo_hal`, `led_controller`) remain unimplemented — Phase 3/6.
  - `src/net/web_server.h/.cpp` — owns the single global
    `AsyncWebServer server(80)` instance, `serveStatic("/", LittleFS,
    "/").setDefaultFile("index.html")`, and `onNotFound`: redirects to
    `/` when `WifiManager::getMode() == AP_SETUP` (captive portal
    trigger), otherwise returns a JSON 404. Calls
    `RestRoutes::registerRoutes()` before `server.begin()`.
  - `src/api/rest_routes.h/.cpp` — registers:
    - `GET /api/system/info` → `firmwareVersion`, `buildDate`, `chipId`
      (hex string of `ESP.getEfuseMac()`), `uptimeMs`, `freeHeap`,
      `radarVariant` (`"LD2420"`/`"LD2450"`/`"NONE"`).
    - `GET /api/system/status` → `wifiMode` (string), `ipAddress`,
      `playMode` (hardcoded `"manual"`, real in Phase 4), `pose` (`null`,
      real in Phase 3).
    - Uses ArduinoJson v7 `JsonDocument` (not the removed
      `StaticJsonDocument`/`DynamicJsonDocument`) and
      `request->beginResponseStream("application/json")` +
      `serializeJson()` + `request->send(response)`, matching the
      ESP32Async fork's actual `AsyncWebServerRequest` API surface
      (verified directly against
      `.pio/libdeps/ld2420/ESPAsyncWebServer/src/ESPAsyncWebServer.h`
      rather than assumed from memory — signatures matched what was
      expected, no fork-specific workarounds were needed this phase).
  - `data/www/index.html` — extended with a small inline `<script>` that
    `fetch()`s `/api/system/info` and `/api/system/status` and dumps the
    JSON into `<pre>` blocks, for a manual smoke test once flashed.
  - `src/main.cpp` — now calls `LittleFS.begin(true)` (auto-format on
    first-mount failure), `NvsStore::begin()`, `WifiManager::begin()`,
    `Buttons::begin()`, `WebServer::begin()` from `setup()`;
    `loop()` is just `WifiManager::handle()` + `Buttons::handle()`, no
    blocking calls. Boot banner extended to print the resolved WiFi mode.

- **Build verification (Phase 1):** `pio run -e ld2420`, `pio run -e
  ld2450`, `pio run -t buildfs -e ld2420`, and `pio run -t buildfs -e
  ld2450` all succeeded with zero errors/warnings-as-errors. Flash usage
  ~55% (859929/1572864 bytes), RAM ~14% (45920/327680 bytes), identical
  between both radar-variant environments as expected (no radar code
  exists yet). `buildfs` packed `data/www/index.html` and
  `data/sequences/README.md` into `littlefs.bin` for both environments
  with no errors.

- **Phase 2 files added:**
  - `src/net/ota_manager.h/.cpp` — wraps the core-provided `ArduinoOTA`
    library (no new `lib_deps`). `OtaManager::begin()` (called from
    `setup()`, after `NvsStore::begin()`) just caches the
    `otaNetworkEnabled` NVS flag into RAM — it deliberately does **not**
    read NVS on every `loop()` tick. `OtaManager::handle()` (polled from
    `loop()`) lazily calls `ArduinoOTA.begin()` the first time
    `WifiManager::getMode() == WifiMode::STA_CONNECTED` while enabled
    (network OTA isn't meaningful while only the AP setup portal is up),
    calls `ArduinoOTA.end()` if later disabled or WiFi drops out of
    `STA_CONNECTED`, and pumps `ArduinoOTA.handle()` whenever running.
    Hostname comes from `NvsStore::getHostname()` (Phase 1's existing
    `wifi/hostname` key — reused as-is, no new hostname key added).
    `OtaManager::setEnabled(bool)` updates both the RAM cache and NVS, for
    the new `POST /api/system/config` handler.
  - `src/api/ota_routes.h/.cpp` — **new file, not added into
    `rest_routes.cpp`**: kept OTA's multipart upload handlers + the
    deferred-restart mechanism in their own module, mirroring how Phase 1
    already separated `web_server.*` (raw server bootstrap) from
    `rest_routes.*` (route logic) — OTA is a big enough, distinct enough
    concern to warrant the same split. Routes:
    - `POST /api/ota/firmware` — multipart upload (`AsyncWebServer`'s
      `server.on(path, HTTP_POST, onRequest, onUpload)` pattern,
      `ArUploadHandlerFunction` signature confirmed against the actual
      `.pio/libdeps/.../ESPAsyncWebServer.h` in this repo:
      `(request, filename, index, data, len, final)`), streams into
      `Update.write()` with `U_FLASH`. `Update.begin(UPDATE_SIZE_UNKNOWN,
      U_FLASH)` on the first chunk (`index == 0`) — deliberately **not**
      using `request->contentLength()` as a size hint, since for a
      multipart body that length includes headers/boundaries, not just
      the file payload, so it would be a wrong/misleading size for
      `Update.begin()`. `Update.end(true)` on `final`.
    - `POST /api/ota/filesystem` — identical pattern, `U_SPIFFS` (per
      plan §8: this is intentionally the correct `Update.h` mode constant
      for the LittleFS-backed OTA partition too, not a typo).
    - `GET /api/ota/status` — JSON `{result, type, bytesWritten,
      error?}`; `result` is one of `none | in_progress | success |
      failure`, tracked in a file-local `OtaStatusState` struct updated
      by the upload handlers (not persisted across reboot — resets to
      `none` on boot, which is fine since a successful OTA reboots
      anyway).
    - On success, the actual JSON response (`{success, type,
      bytesWritten, rebooting:true}` or, on failure, `{success:false,
      type, bytesWritten, updateError, error}`) is sent from the
      **request-complete** callback (`sendOtaResultResponse`), not from
      inside the upload chunk callback — `ESPAsyncWebServer` only sends
      what the `onRequest` callback produces, the `onUpload` callback
      can't itself write the HTTP response. The reboot itself is
      deferred: a success sets a `gRestartPending` flag + a `millis() +
      1500` deadline, and `OtaRoutes::handle()` (polled from `main.cpp`
      `loop()`, alongside `OtaManager::handle()`) calls `ESP.restart()`
      once that deadline passes — this gives AsyncTCP time to actually
      flush the response before the reboot tears the TCP connection down,
      per the task's explicit "don't restart synchronously inside the
      handler" requirement.
  - `src/api/rest_routes.cpp` — `GET /api/system/info` gained an
    `otaNetworkEnabled` field (read via `OtaManager::isEnabled()`, i.e.
    the RAM-cached flag, so it always reflects whether ArduinoOTA is
    actually running/would run, not just the raw NVS value). Also added
    `POST /api/system/config` (low-effort, in scope per the task's "use
    your judgement" note) — currently handles only `{"otaNetworkEnabled":
    bool}`, via `ArBodyHandlerFunction` (accumulates the body across
    possibly-chunked delivery into a static `String`, parses once
    `index + len == total`), calls `OtaManager::setEnabled()`, responds
    `{success, otaNetworkEnabled}`.
  - `src/storage/nvs_store.h/.cpp` — added `system` namespace key
    `otaNetworkEnabled` (NVS key name `otaNetEn`, ≤15 chars per NVS key
    length limit), bool, default `true`, alongside the existing
    `naturalMode`/`ledEnabled` placeholders — `getOtaNetworkEnabled()` /
    `setOtaNetworkEnabled()`, same open-close-per-call `Preferences`
    pattern as every other key in this file.
  - `src/net/web_server.cpp` — now also calls
    `OtaRoutes::registerRoutes(gServer)` alongside
    `RestRoutes::registerRoutes(gServer)`, before `server.begin()`.
  - `src/main.cpp` — `setup()` now calls `OtaManager::begin()` (after
    `WifiManager::begin()`); `loop()` now also calls `OtaManager::handle()`
    and `OtaRoutes::handle()`, in addition to the existing
    `WifiManager::handle()`/`Buttons::handle()` — `loop()` stays fully
    non-blocking, matching plan §2's "loop() is essentially just OTA
    handling + housekeeping" note.
  - `data/www/index.html` — the Phase 1 smoke-test script now also
    displays `data.otaNetworkEnabled` from `/api/system/info` (a small
    `<p>` line above the raw JSON dump), still no framework/build step.
  - No new `lib_deps` — `ArduinoOTA` and `Update.h` are both part of the
    arduino-esp32 core already pulled in by the existing `framework =
    arduino` setting; PlatformIO's dependency graph picked them up
    automatically (visible in the `buildfs`/`pio run` library dependency
    graph output as `ArduinoOTA @ 2.0.0` / `Update @ 2.0.0`).

- **Build verification (Phase 2):** `pio run -e ld2420` and `pio run -e
  ld2450` both succeeded with zero errors, identical output between the
  two environments (expected — no radar-specific code touched this
  phase): **RAM 15.2% (49776/327680 bytes), Flash 58.5% (919469/1572864
  bytes)** — up from Phase 1's 54.7% (859929 bytes), a **+59540 byte
  (+3.8 percentage point)** increase from `ArduinoOTA` + `Update.h` +
  the new OTA route/manager code, in line with the "shouldn't add much"
  expectation — comfortably clear of the 1.5MB OTA app slot, not
  flagged as a concern. `pio run -t buildfs -e ld2420` and `-e ld2450`
  both succeeded, packing the updated `data/www/index.html` (plus the
  unchanged `data/sequences/README.md`) into `littlefs.bin` with no
  errors.

- **C++ standard bump (Phase 3, platformio.ini):** `EyeCommand`'s partial-
  target fields use `std::optional<float>` (task spec's recommendation for
  expressing "only these axes move"), which needs C++17; the espressif32
  Arduino toolchain (`xtensa-esp32-elf-g++`, gcc 8.4.0) defaults to
  `-std=gnu++11`. Added `build_unflags = -std=gnu++11` to `[env]` (this
  one **does** fall through to both envs, since neither overrides
  `build_unflags`) and `-std=gnu++17` appended directly to each
  `[env:ld2420]`/`[env:ld2450]` `build_flags` line. **Important discovery
  for future phases:** unlike `lib_deps` (which merges between `[env]`
  and `[env:xxx]`), `build_flags` does **not** merge — an `[env:xxx]`
  section that declares its own `build_flags` (both envs do, for
  `-D RADAR_LD2420`/`RADAR_LD2450`) fully replaces `[env]`'s
  `build_flags` rather than appending to it. Discovered by grepping the
  actual compiler invocation (`pio run -v`) and finding `-std=gnu++17`
  simply absent from `main.cpp.o`'s command line even though it was set
  in `[env]`. Any future phase adding to `[env]`'s `build_flags` must
  also add it to both per-env `build_flags` lines, not just `[env]`.

- **Phase 3 files added:**
  - `include/pin_map.h` — added `enum class ServoId` (`Pan, Tilt,
    LidUpperL, LidLowerL, LidUpperR, LidLowerR, Aux, Count`) and
    `kServoPins[]` (ServoId-indexed pin lookup). Placed here (not in
    `hal/servo_hal.h`) specifically so `storage/nvs_store.h` (servo
    calibration) can reference `ServoId` without a hal<->storage
    dependency cycle.
  - `src/motion/eye_pose.h/.cpp` — `EyePose` (6 floats: `panDeg`,
    `tiltDeg` degrees centered at 0; `lidUpperL/lidLowerL/lidUpperR/
    lidLowerR` normalized 0=closed..1=open), `Easing` enum
    (`Linear`/`EaseInOut`, the latter a smoothstep `3t²-2t³` blend),
    `CommandSource` enum (`Manual`/`Calibration`/`Gesture`/`PlayMode`/
    `Natural` — only the first two are produced by any Phase 3 code),
    `EyeCommand` (6× `std::optional<float>` partial target fields +
    `durationMs`/`easing`/`source`). `applyEasing()` is the only function
    body, in the `.cpp`.
  - `src/motion/command_queue.h/.cpp` — `xQueueCreate`/`xQueueSend`/
    `xQueueReceive` wrapper, fixed depth 8, `push()` is 0-tick
    non-blocking and drops the single oldest entry (not the new one) on
    a full queue so the newest command always gets in — safe to call
    from AsyncTCP/HTTP handler context.
  - `src/storage/nvs_store.h/.cpp` — added `ServoCalibration` struct
    (`minUs`/`centerUs`/`maxUs` uint16 + `inverted` bool; defaults
    1000/1500/2000/false — a conservative SG90-safe range chosen over
    the absolute hobby-servo extreme (~500-2500us) specifically because
    these are the defaults applied to **unconfigured** hardware, and
    driving an uncalibrated SG90 to a hard mechanical stop risks
    stripping its gears) and `getServoCalibration(ServoId)`/
    `setServoCalibration(ServoId, cal)`, backed by one NVS blob
    (`servocal` namespace, key `calTable`) holding a packed 7-entry
    `ServoCalibration[]` via `Preferences::getBytes`/`putBytes` — one
    read/write per call touches all 7 servos' calibration at once,
    matching plan §3's "small blob" NVS guidance instead of 28
    individual keys. A stored-blob size mismatch (e.g. a future
    `ServoCalibration` layout change) is treated the same as "nothing
    saved yet" rather than reinterpreting stale bytes.
  - `src/hal/servo_hal.h/.cpp` — `ServoHal::begin()` loads all 7
    calibrations and `Servo::attach(pin, minUs, maxUs)`s each channel
    (madhephaestus/ESP32Servo 3.2.1's actual API, confirmed against
    `.pio/libdeps/<env>/ESP32Servo/src/ESP32Servo.h` rather than assumed
    — `attach(pin)`/`attach(pin,min,max)`/`writeMicroseconds(int)`/
    `detach()`, no `write()` used since this HAL always deals in raw
    microseconds, never degrees). **Deviation/note:** the library itself
    internally enforces its own absolute `MIN_PULSE_WIDTH=500`/
    `MAX_PULSE_WIDTH=2500us` regardless of what's passed to `attach()`,
    so a calibration value outside that range is silently clamped
    tighter by ESP32Servo on top of this HAL's own clamp to the
    calibrated range in `setPulseUs()`. `getCalibration(ServoId)` reads
    a RAM cache (populated at `begin()`/`reapplyCalibration()`) rather
    than hitting NVS — needed because `MotionTask` calls it every ~20ms
    tick and `Preferences`/NVS is flash-backed and too slow/wear-costly
    to hit at that rate.
  - `src/motion/motion_task.h/.cpp` — the real-time core. Dedicated
    FreeRTOS task via `xTaskCreatePinnedToCore`, **Core 1, priority 3,
    4096-byte stack, ~50Hz via `vTaskDelayUntil`** (not `vTaskDelay`, to
    avoid drift accumulating from the tick body's own execution time).
    Core 1 keeps it off Core 0 (where WiFi/LWIP/AsyncTCP's own internal
    tasks mostly run), so network activity can't delay a tick; priority
    3 sits above Arduino's own `loop()` task (priority 1 by default on
    arduino-esp32) so OTA/button-polling housekeeping in `loop()` can't
    delay a tick either, while staying comfortably below WiFi/BT system
    tasks (typically ~18-23) so it never starves anything
    connectivity-critical. Per-axis interpolation state
    (`AxisState{startValue, targetValue, startTimeMs, durationMs,
    easing}`) for the 6 `EyePose` fields (indexed by `ServoId::Pan..
    LidLowerR` — `Aux` has no `EyePose` field and is intentionally
    excluded from interpolation, only reachable via `ServoHal` directly).
    **Interruption/re-targeting**, exactly per plan §2: `valueAt(axis,
    now)` is a pure function computing an axis's current interpolated
    value from `(start, target, startTime, duration, easing)` without
    mutating anything; `retarget()` calls `valueAt()` on the **current**
    axis state to get where it actually is *right now*, then overwrites
    `startValue` with that (not the old target) before setting the new
    target/duration/easing/startTime — so a new command's motion always
    begins from the true current position, never from wherever the
    previous command's target was heading. Each tick: drains
    `CommandQueue` completely (calling `retarget()` per set axis), then
    computes all 6 axes' current values via `valueAt()`, writes them to
    `ServoHal` (see mapping below), and publishes a copy into
    `gCurrentPose` under a `SemaphoreHandle_t` mutex
    (`xSemaphoreCreateMutex`), held only for the struct-copy — the only
    thread-safety concern in scope, since `AxisState[]` itself is never
    touched outside the motion task. `getCurrentPose()` takes the same
    mutex (50ms timeout) and returns a plain-value copy, safe to call
    from the AsyncTCP/HTTP task. **Degrees-to-pulse mapping assumption**
    (documented in code, needs real-hardware tuning once the ε-SERIES
    mechanism is assembled): pan/tilt treat the working gaze range as
    **±45°**, linearly mapped to the servo's calibrated `[minUs, maxUs]`,
    centered on `centerUs` at 0° — the low/high halves are scaled
    independently off `centerUs` so an asymmetric calibration (center
    not exactly halfway between min/max) still lands exactly on
    `centerUs` at 0° and exactly on `minUs`/`maxUs` at ∓45°/+45°.
    Eyelids map normalized 0..1 linearly across `[minUs, maxUs]`.
    `cal.inverted` flips the sign/direction in both mappings (for a
    servo mounted as a mirror image of its pair). No
    `commandGeneration` counter is implemented yet (plan §2 mentions
    one for gesture/play-mode coroutines to detect being superseded) —
    deferred to Phase 4, since nothing produces
    Gesture/PlayMode/Natural-sourced commands until then; `EyeCommand`'s
    `source` field is already in place for it to build on.
  - `src/api/servo_routes.h/.cpp` — new module (split out from
    `rest_routes.*`, mirroring Phase 2's `ota_routes.*` split, for the
    same "big enough, distinct enough concern" reason). Routes:
    - `GET /api/servos/config` — JSON array, all 7 servos
      (`servoId`/`name`/`minUs`/`centerUs`/`maxUs`/`inverted`).
    - `POST /api/servos/config` — validates `minUs < centerUs < maxUs`
      and all three within an absolute `400-2600us` bound (per task
      spec — note ESP32Servo's own tighter `500-2500us` clamp still
      applies underneath this, see servo_hal note above), then
      `NvsStore::setServoCalibration()` + `ServoHal::reapplyCalibration()`
      so the change is live immediately, no reboot.
    - `POST /api/servos/test` — validates bounds only, then calls
      `ServoHal::setPulseUs()` **directly**, bypassing
      `CommandQueue`/`MotionTask` entirely (deliberate, per plan §4 —
      raw calibration tool). **Documented known interaction** (code
      comment in `servo_routes.cpp`, not solved): `MotionTask` still
      ticks at 50Hz in the background and owns every axis's target
      except `Aux`; if a test pulse targets one of those 6 axes while
      `MotionTask` currently has a non-expired target for it, the very
      next tick (≤20ms) overwrites the test pulse. Acceptable per the
      task spec ("test calls only make visible sense while the axis is
      settled/idle... a full calibration-mode lock is a Phase 7/8
      UI-level concern") — not implemented here to avoid
      over-engineering a mode-lock this phase doesn't need.
    Body-parsing follows `rest_routes.cpp`'s existing
    accumulate-then-parse-on-final-chunk pattern (`ArBodyHandlerFunction`,
    static `String` buffer per route).
  - `src/api/rest_routes.cpp` — `GET /api/system/status`'s `pose` field
    is now `MotionTask::getCurrentPose()` serialized to a JSON object
    (was hardcoded `null` through Phase 2).
  - `src/net/web_server.cpp` — now also calls
    `ServoRoutes::registerRoutes(gServer)`.
  - `src/main.cpp` — `setup()` now calls `ServoHal::begin()` then
    `MotionTask::begin()` (in that order — calibration must be loaded
    and servos attached before MotionTask's first tick writes to them),
    placed after `Buttons::begin()` and before `WebServer::begin()`.
    `loop()` itself is **unchanged** — `MotionTask` is fully
    self-scheduling in its own FreeRTOS task, nothing to poll from
    `loop()`.
  - `data/www/index.html` — smoke-test script now also displays
    `data.pose` from `/api/system/status` (was always `null` through
    Phase 2) in a small `<p>` line, for a quick visual confirmation path
    once flashed to hardware.
  - No new `lib_deps` — `ESP32Servo` was already pinned in `[env]`'s
    `lib_deps` since Phase 0 (unused until now). FreeRTOS
    (`freertos/FreeRTOS.h`, `queue.h`, `task.h`, `semphr.h`) comes from
    the arduino-esp32 core, no new dependency.

- **Build verification (Phase 3):** `pio run -e ld2420` and `pio run -e
  ld2450` both succeeded with zero errors, identical output between the
  two environments (expected — no radar-specific code touched this
  phase): **RAM 15.5% (50808/327680 bytes), Flash 59.7% (939753/1572864
  bytes)** — up from Phase 2's 58.5% (919469 bytes), a **+20284 byte
  (+1.2 percentage point)** increase for ServoHAL + EyePose/EyeCommand +
  CommandQueue + MotionTask + the new `/api/servos/*` routes — small as
  expected, comfortably clear of the 1.5MB OTA app slot, not flagged as a
  concern (but watch this trend: Phase 4's gesture/play-mode engine and
  Phase 5's radar parsing are both still to come). `pio run -t buildfs -e
  ld2420` also succeeded, packing the updated `data/www/index.html`
  (pose display) and the unchanged `data/sequences/README.md` into
  `littlefs.bin` with no errors.

- **Phase 4 — axis ownership / interruption model, as actually
  implemented (plan §2, §5):**
  - `AxisState` (motion_task.cpp, was Phase 3-private) gained a
    `CommandSource source` field alongside its existing
    `startValue/targetValue/startTimeMs/durationMs/easing`. `retarget()`
    now takes a `source` param and stamps it on every retarget, and
    `applyCommand()` passes `cmd.source` through unchanged — no separate
    "who owns this axis" bookkeeping exists beyond this one field plus the
    timing fields already there.
  - **Free-axis rule** (exactly per the task spec, no embellishment): an
    axis is free for `NaturalModeCoupler` to touch when
    `axis.source == CommandSource::Natural || now >= axis.startTimeMs +
    axis.durationMs`. A Manual/Gesture/PlayMode-sourced interpolation
    that's still in flight is never free; the instant its own duration
    elapses, it becomes free regardless of what source set it.
  - `commandGeneration`: a file-local `std::atomic<uint32_t>` in
    motion_task.cpp (relaxed ordering — it's a "has anything changed"
    token, not used to order any other memory access), exposed as
    `MotionTask::bumpCommandGeneration()` / `::getCommandGeneration()`.
    Bumped by: `POST /api/eyes/gaze`, `POST /api/eyes/eyelids`
    (eyes_routes.cpp), `POST /api/gestures/{id}/trigger`
    (gesture_routes.cpp, bumped *before* calling
    `GestureEngine::trigger()` so the engine captures the fresh value),
    and `PlayModeManager::activate()` (bumped before starting the new
    mode's behavior). **Deliberately NOT bumped** by a play mode's own
    routine autonomous actions (idle's periodic drift/blink, curious's
    saccades) — only a genuinely new *external* intent bumps it, or the
    counter would increment continuously every tick and lose its meaning
    as "something new took over". `GestureEngine`'s and
    `PlayModeManager`'s own multi-keyframe coroutines (a gesture's
    remaining frames, a greeting sequence's remaining frames) capture the
    generation at playback start and compare before pushing each
    subsequent keyframe; a mismatch aborts the remainder cleanly (no
    further `CommandQueue::push()` calls) instead of continuing to fight
    whatever superseded them.
  - **Known nuance**: if a Manual command interrupts a mid-flight
    `greeting` sequence, `commandGeneration` correctly stops the sequence
    from pushing further keyframes, but `PlayModeManager`'s own
    `gActiveMode` bookkeeping is *not* automatically reset to anything
    else — `GET /api/playmodes/active` keeps reporting `"greeting"` until
    the user explicitly activates a different mode (or the sequence would
    have finished normally). Judged correct/expected: the generation
    mechanism's job is stopping the *fight over axes*, not silently
    relabeling the active mode out from under the user.

- **Phase 4 — NaturalModeCoupler (`src/motion/natural_mode.h/.cpp`), as
  implemented:**
  - Deliberately **not** a FreeRTOS task and does **not** push through
    `CommandQueue` (per the task spec) — it's a stateless pure function,
    `computeTargets(panDeg, tiltDeg) -> LidTargets{upperL,lowerL,upperR,
    lowerR}` (absolute openness targets, already clamped 0..1), called
    directly from inside `MotionTask`'s own tick. `MotionTask` retargets
    each lid `AxisState` directly (`maybeApplyNatural()`,
    motion_task.cpp) only when that axis is free (see above) *and* the
    candidate target differs from the axis's own current `targetValue` by
    more than **0.02** (the jitter guard from the task spec) — comparing
    directly against `AxisState.targetValue` means `NaturalModeCoupler`
    itself needs no internal "last pushed" state at all.
  - **Coupling formula actually used**: baseline resting openness
    **0.85**; `tiltNormalized = clamp(tiltDeg / 45°, -1, 1)`; `upper =
    clamp01(0.85 + tiltNormalized * 0.28)`; `lower = clamp01(0.85 +
    tiltNormalized * 0.12)`. Upper and lower deliberately use **different**
    coefficients (0.28 vs 0.12) so looking down drops the upper lid
    noticeably more than the lower one (mimicking real eyelid occlusion,
    per plan §5), rather than both lids moving identically. L and R get
    identical targets since gaze is shared-yoke (plan §1) — no per-eye
    signal exists to differentiate them from pan/tilt alone. Retarget
    smoothing duration: **200ms**, `Easing::EaseInOut`.
  - RAM-cached `naturalMode` flag (`NaturalModeCoupler::begin()` loads it
    from the existing Phase 1 NVS key once; `isEnabled()` is checked every
    ~20ms tick, `setEnabled()` updates both the cache and NVS) — same
    pattern as `ServoHal`'s calibration cache, for the same reason (NVS is
    too slow/wear-costly to hit at 50Hz).
  - **`setSuppressed()`/`isSuppressed()`**: a small addition beyond the
    task spec's literal wording, added to fix a real interaction bug found
    while implementing `sleep` play mode — see that section below for why.
  - **Skipped (stretch item, explicitly optional per the task spec)**: the
    plan's optional smaller pan-based asymmetric lid-tightening term
    ("slight asymmetric lid tightening on the trailing side during a fast
    saccade") — not implemented; would need saccade-velocity tracking this
    module otherwise doesn't need. `TODO(stretch, plan §5)` comment left
    in `natural_mode.h` at the exact spot.
  - **Skipped (stretch item, explicitly optional per the task spec)**:
    "saccade blink-through" (auto-blink on a combined gaze jump >~25°) —
    not implemented this phase. No code path currently tracks
    saccade-over-time to detect a "jump" distinct from a slow drift, and
    threading that through cleanly (plus making it toggle-able, per the
    plan's "configurable on/off") was judged more scope than this phase's
    budget justified given everything else in it. No `TODO` comment
    exists in code for this one specifically (there was no natural single
    insertion point without first building the saccade-detection
    machinery) — flagged here in PROGRESS.md instead, per the task's
    explicit instruction for this exact stretch item.

- **Phase 4 — GestureEngine (`src/motion/gesture_engine.h/.cpp`), as
  implemented:**
  - Ticked from inside `MotionTask`'s own tick (`GestureEngine::tick()`),
    not a separate FreeRTOS task, per plan §2. Pushes each due keyframe
    onto `CommandQueue` (chosen over directly poking `MotionTask`'s
    private `AxisState` — see file header comment — for a single,
    consistent application path, `applyCommand()`, across every command
    source; `NaturalModeCoupler` is the one deliberate exception, per its
    own header).
  - Gestures are **hand-authored C++ data tables** (`const EyeCommand[]`
    arrays built via small local helper functions `lidsCmd()`/
    `gazeCmd()`/`gazeAndLidsCmd()`/`oneEyeLidsCmd()`), not JSON — per the
    task spec, only play-mode *sequences* are JSON-driven.
  - Full gesture list implemented, matching plan §5 exactly (9 gestures,
    all IDs as given):
    - `blink` — close (90ms) → reopen to 0.85 baseline (110ms). 2 frames.
    - `wink_left` / `wink_right` — one eye's lids close (120ms) → reopen
      (150ms); the other eye's lids are left untouched (`std::nullopt`).
      2 frames each.
    - `surprise` — lids snap to 1.0 (fully open) + gaze reset to (0°,0°),
      90ms linear "snap" → settle to 0.85 baseline, 260ms easeInOut.
      2 frames.
    - `sleepy` — single keyframe, lids ease to ~0.35-0.40 over 400ms, held
      (no reopen keyframe — see the "known nuance" note below).
    - `squint` — single keyframe, lids to ~0.35-0.55 (asymmetric) +
      tiltDeg -8°, 220ms. **Alias**: `"suspicious"` is accepted as an
      alternate id for the same gesture (plan §5 lists it as
      "squint/suspicious" without picking one canonical name — `squint`
      chosen as canonical, the alias is resolved in
      `GestureEngine::findGesture()`).
    - `look_around_quick` — 3 gaze-only keyframes, quick linear saccades
      then an easeInOut return to center.
    - `double_blink` — close/open/close/open, 4 frames, ~300ms total.
    - `roll_eyes` — 5 gaze-only keyframes tracing a rough circle; lids
      deliberately left untouched so they track the sweep via
      `NaturalModeCoupler` if natural mode is on, per plan §5's explicit
      wording for this gesture.
  - **Known nuance** (matches the file's own code comment): a gesture that
    "holds" a pose (`sleepy`, `squint`) only holds it until something else
    reclaims the lid axes — most notably `NaturalModeCoupler`, if natural
    mode is on, which will start pulling the lids back toward its own
    baseline the instant the gesture's last keyframe finishes and the axis
    goes free. This is the same "coupling resumes smoothly from wherever
    the lid ended up" behavior plan §5 asks for, so it's treated as
    correct/expected, not a bug — but it does mean `sleepy`/`squint` read
    as more fleeting than "held" might suggest when natural mode is on.
  - Single active-playback slot: triggering a new gesture always
    overwrites whatever gesture was mid-playback (self-interruption, no
    generation check needed for that specific case — see motion_task.h's
    doc comment for why the generation counter's real job is stopping
    *other* producers, not this one).

- **Phase 4 — PlayModeManager (`src/motion/playmode_manager.h/.cpp`), as
  implemented:**
  - Also ticked from inside `MotionTask`'s own tick
    (`PlayModeManager::tick()`), not a separate task, per plan §2. Full
    six-mode list from plan §5, all IDs as given: `idle`, `curious`,
    `sleep`, `greeting`, `tracking`, `manual`.
  - Active mode is **RAM-only, not persisted to NVS** — always boots to
    `manual` (`PlayModeManager::begin()`), so a freshly-flashed or
    just-rebooted device never starts moving on its own before anything
    has explicitly activated a mode. Documented simplification: the plan
    doesn't call out NVS-persisting the active play mode, and
    always-boot-to-manual is the safer default for hardware bring-up
    (nothing should start swinging servos around before you've had a
    chance to check calibration).
  - `idle` — random pan/tilt drift (±8°/±5°, 900-1800ms, every
    2.5-5.5s) + periodic `blink` gesture (every 4-9s), both
    `CommandSource::PlayMode`, both **not** generation-bumping (routine
    autonomous behavior, see commandGeneration notes above).
  - `curious` — same shape, bigger/faster: ±35°/±20° saccades over
    300-700ms every 1.2-3.0s, blinks every 1.8-4.2s. **Scope
    simplification** (explicitly pre-approved in the task spec): fully
    code-parameterized (amplitude/frequency constants in
    playmode_manager.cpp), not JSON-driven. The plan's additional
    "occasional head-tilt-like asymmetric lid narrowing" flourish for
    curious mode is **not implemented** — would need a second lid-owning
    state machine competing with `NaturalModeCoupler` for the same axes,
    for a cosmetic detail on top of otherwise fully-functional,
    interruptible curious behavior. `TODO`/rationale comment left in
    `curiousTick()`.
  - `sleep` — on activation, one `EyeCommand` eases all 4 lids to 0.12 and
    gaze to (0°,0°) over 900ms, **plus calls
    `NaturalModeCoupler::setSuppressed(true)`**. This suppression call is
    the one addition beyond the task spec's literal design: without it,
    the moment the 900ms ease finishes and the lid axes go "free" again,
    `NaturalModeCoupler` (if natural mode is on) would immediately start
    pulling the lids back toward its own ~0.85 baseline — directly
    undoing sleep mode within a quarter-second of entering it. Every other
    mode's `enterMode()` path explicitly calls `setSuppressed(false)` on
    entry, so leaving sleep for anything else correctly restores natural
    coupling. While asleep, a rare (every 20-35s) tiny pan/tilt micro-
    drift (±2°/±1.5°) runs so the pose doesn't look perfectly dead, per
    plan §5's "motion nearly stops, occasional slow drift".
  - `greeting` — the concrete JSON-sequence deliverable. Loads and parses
    `/data/sequences/greeting.json` from LittleFS via ArduinoJson v7
    (`deserializeJson(doc, file)`) on activation, into a fixed
    `EyeCommand[16]` array (`kMaxSequenceKeyframes = 16`, plenty of
    headroom over the shipped 5-keyframe default). Plays back one keyframe
    at a time via the same generation-checked coroutine pattern as
    `GestureEngine`; when the sequence finishes, **automatically calls
    `PlayModeManager::activate("idle")`** to settle back to idle, per plan
    §5's "wake up → look at viewer → wide-eyed surprise → settle to idle".
    If the file is missing/invalid, falls back to starting idle mode
    directly (logged via `Serial`) rather than leaving the eyes doing
    nothing.
    - **`greeting.json` schema** (`data/sequences/greeting.json`, own
      `_comment` field documents this too): `{"keyframes": [ {panDeg?,
      tiltDeg?, lidUpperL?, lidLowerL?, lidUpperR?, lidLowerR?,
      durationMs, easing?}, ... ]}`. Every field except `durationMs` is
      optional (an absent field leaves that axis alone — same
      optional-fields shape as `EyeCommand` everywhere else in the
      engine). `easing` is `"linear"` or `"easeInOut"` (default if
      absent/unrecognized). The shipped default sequence (5 keyframes)
      tells exactly the story plan §5 asks for: eyes-closed-looking-down
      (60ms snap) → half-open waking (320ms) → look at viewer, pan/tilt
      centered-up (450ms) → wide-eyed surprise snap (180ms linear) →
      settle to idle's 0.85/(0°,0°) baseline (500ms).
  - `tracking` — **registered and selectable via the API, but its
    `tick()` behavior is currently a direct alias of `idle`'s** (calls the
    same `idleTick()` function) — radar doesn't exist until Phase 5. A
    `TODO(Phase 5): read RadarState's primary target...` comment sits at
    the exact spot in `enterMode()`'s `Mode::Tracking` case (and is
    cross-referenced from the mode-list `description` string returned by
    `GET /api/playmodes`, so this limitation is visible over the API too,
    not just in code). This is an intentional choice, not an oversight:
    the task spec asked for "a safe no-op/alias of idle" rather than a
    mode that visibly does nothing at all when a user tests the API
    before Phase 5 lands.
  - `manual` — `tick()` does nothing; `/api/eyes/*` and `/api/gestures/*`
    always flow straight through regardless of which mode is active (they
    always did, even in idle/curious/etc — "manual" only means
    *PlayModeManager itself* stays quiet, matching the task spec's
    clarification that other modes' autonomous commands aren't
    "blocked", they just get generation-superseded by a subsequent Manual
    command like anything else would).
  - Switching modes (`POST /api/playmodes/{id}/activate` →
    `PlayModeManager::activate()`) always bumps `commandGeneration` first
    (aborting whatever the previous mode had in flight — a greeting
    sequence, an idle-triggered gesture), then clears/sets
    `NaturalModeCoupler`'s suppression flag as appropriate, then starts
    the new mode's behavior.

- **Phase 4 — API routes added** (plan §4), all in three new files
  mirroring Phase 2/3's "one new route file per major feature area"
  convention:
  - `src/api/eyes_routes.h/.cpp`: `GET /api/eyes/pose` (thin wrapper
    around the existing `MotionTask::getCurrentPose()`), `POST
    /api/eyes/gaze` `{pan, tilt, durationMs?, easing?}` (degrees, at least
    one of pan/tilt required, source=Manual, bumps commandGeneration),
    `POST /api/eyes/eyelids` `{upperL?, lowerL?, upperR?, lowerR?,
    durationMs?}` (normalized 0..1, partial updates allowed, at least one
    field required, source=Manual, bumps commandGeneration). Body-parsing
    follows the established accumulate-then-parse-on-final-chunk
    `ArBodyHandlerFunction` pattern from servo_routes.cpp/rest_routes.cpp.
  - `src/api/gesture_routes.h/.cpp`: `GET /api/gestures` (id/label list
    from `GestureEngine::listGestures()`), `POST
    /api/gestures/{id}/trigger` (bumps commandGeneration, calls
    `GestureEngine::trigger(id, CommandSource::Gesture)`, 404s on an
    unknown id).
  - `src/api/playmode_routes.h/.cpp`: `GET /api/playmodes` (id/label/
    description list), `GET /api/playmodes/active` (current mode id),
    `POST /api/playmodes/{id}/activate` (calls
    `PlayModeManager::activate()`, 404s on an unknown id).
  - **Path-param routing deviation, deliberate**: ESPAsyncWebServer
    3.12.1's `{param}`/`pathArg()` capture requires the
    `ASYNCWEBSERVER_REGEX` build flag (pulls in `<regex>` — confirmed via
    `.pio/libdeps/ld2420/ESPAsyncWebServer/src/ESPAsyncWebServer.h`'s
    `#ifdef ASYNCWEBSERVER_REGEX` guards around `pathArg()`/the regex
    matcher), which is real flash cost on a target already being watched
    for headroom (see below). Instead, `POST /api/gestures/{id}/trigger`
    and `POST /api/playmodes/{id}/activate` are registered against
    `AsyncURIMatcher::dir("/api/gestures")` /
    `AsyncURIMatcher::dir("/api/playmodes")` (matches any
    `/api/<base>/<...>` request for that HTTP method — confirmed via
    `WebServer.cpp`'s `Type::BackwardCompatible` matcher: `(_value ==
    path) || path.startsWith(_value + "/")`), and the `{id}` segment is
    pulled out of `request->url()` by hand (prefix/suffix `String`
    matching), in the same spirit as this codebase's existing hand-rolled
    JSON body parsing rather than reaching for a heavier framework
    feature for a small, well-bounded need. Verified handler-registration
    order matters here (`AsyncWebServer::_handleRequest` iterates
    `_handlers` and uses the *first* match — confirmed directly in
    `WebServer.cpp`), so the more specific exact routes
    (`/api/playmodes/active`) are registered *before* the general
    BackwardCompatible-matching list routes (`/api/playmodes`) that would
    otherwise also match them.
  - `src/api/rest_routes.cpp`: `POST /api/system/config` extended to also
    accept `{"naturalMode": bool}` (alongside Phase 2's
    `otaNetworkEnabled`), calling `NaturalModeCoupler::setEnabled()`.
    `GET /api/system/info` gained `naturalMode`. `GET /api/system/status`
    gained `naturalMode` and its `playMode` field is now
    `PlayModeManager::getActiveModeId()` (was the Phase 1-3 hardcoded
    `"manual"` string stub).
  - `src/net/web_server.cpp`: registers all three new route modules
    alongside the existing ones, before `server.begin()`.
  - `data/www/index.html`: smoke-test script extended to also display
    `naturalMode` (from both `/api/system/info` and `/api/system/status`)
    and the real `playMode` (from `/api/system/status`) — still no
    framework/build step, Phase 7 does the real frontend.

- **Build verification (Phase 4):** `pio run -e ld2420` and `pio run -e
  ld2450` both succeeded with **zero errors and zero warnings**, identical
  output between the two environments (expected — no radar-specific code
  touched this phase): **RAM 16.2% (53180/327680 bytes), Flash 61.1%
  (961089/1572864 bytes)** — up from Phase 3's 59.7% (939753 bytes), a
  **+21336 byte (+1.4 percentage point)** increase for
  `NaturalModeCoupler` + `GestureEngine` (9 gesture tables) +
  `PlayModeManager` (6-mode state machine + JSON sequence parsing) + the
  three new route files — comfortably clear of the 1.5MB OTA app slot, not
  flagged as a concern (well under the 90% prompt threshold). `pio run -t
  buildfs -e ld2420` and `-e ld2450` both succeeded, packing
  `data/www/index.html` (naturalMode/playMode display),
  `data/sequences/README.md` (updated), and the new
  `data/sequences/greeting.json` into `littlefs.bin` with no errors
  (confirmed in the build log: `/sequences/greeting.json`,
  `/sequences/README.md`, `/www/index.html` all listed as packed).

- **Phase 5 — LD2420 library re-validation (`gsieben/LD2420GeoGab @ 1.0.0`),
  as actually decided:** Phase 0 flagged this pin for re-validation once a
  phase actually used it, and possibly falling back to
  `cyrixninja/LD2420`/`mikerussellnz/LD2420` or a hand-rolled parser. Having
  now actually inspected the installed library source under
  `.pio/libdeps/ld2420/LD2420GeoGab/` (not just its `library.json`), the
  verdict is: **keep it, no fallback needed.** It is a genuinely
  full-featured, well-documented driver, not the thin/undocumented stub
  Phase 0's caution suggested it might be:
  - Complete binary command-frame protocol implementation (`FD FC FB FA` /
    `04 03 02 01` header/footer, matching the plan's own confirmed research),
    config-mode guard pattern, Energy/Simple/Debug system modes, per-gate
    ABD (Automatic Background Detection) threshold read/write, auto-
    calibration, both a callback API (`setPresenceCallback`/
    `setDistanceCallback`/`setStatusCallback`/`setEnergyCallback`) and a
    poll API (`isPresent()`/`getLastStatus()`/`getLastDistance()`/
    `newDataAvailable()`).
  - `update()` (the per-loop poll entry point) is verified non-blocking by
    reading its actual implementation
    (`.pio/libdeps/ld2420/LD2420GeoGab/src/LD2420GeoGab.cpp`): it only ever
    drains bytes already sitting in the UART hardware FIFO
    (`while (sensorSerial->available())`) and returns immediately if
    there's nothing to read — exactly the "non-blocking poll(), no
    `Serial2.readString()`-style hang" behavior the task spec requires for
    a possibly-unconnected sensor.
  - `begin()` *does* block, but only ever **boundedly**: worst case is a
    2000ms "wait for UART silence" flush loop
    (`activateConfigMode()`'s dynamic-flush step) plus one 500ms command
    response timeout (`LD2420_CMD_TIMEOUT_MS`) if nothing ever responds —
    confirmed by reading `activateConfigMode()`/`waitForResponse()`
    directly, not assumed. ~2.5s worst case is fine specifically because
    `Ld2420Sensor::begin()` is only ever called from inside `RadarTask`'s
    own dedicated FreeRTOS task (`radar_task.cpp`), never from
    `setup()`/`loop()` — it cannot delay boot, the web server, or
    `MotionTask`.
  - Ships real documentation: `docs/HLK-LD2420-Product Manual V1.2.pdf`,
    a protocol command spreadsheet, and
    `references/LD2420-Technical-Reference_{en,de}.md`, plus 5 working
    Arduino/PlatformIO examples (`SimpleLoop`, `SimpleCallback`, `Energy`,
    `Calibration`, `Debug`) — this is the opposite of the "undocumented"
    risk Phase 0 flagged.
  - Only presence + distance are actually used by `Ld2420Sensor`
    (`isPresent()`/`getLastDistance()`, cm converted to mm) — no angle/
    position, matching the sensor's real hardware capability and plan §6's
    documented LD2420 limitation. The library's much larger surface
    (ABD calibration, Debug/Doppler frames, factory reset) is available but
    unused this phase — a reasonable "wrap what's needed" scope, not
    starting a second bespoke parser when a working one already exists.
  - `Ld2420Sensor::begin()` puts the sensor into `Energy` mode (the
    library's own documented "recommended" mode) for a finer internal
    distance estimate; if that best-effort config sequence itself fails
    (e.g. sensor responded to the initial comms check but then dropped),
    `poll()`/`isPresent()`/`getLastDistance()` still work against whatever
    mode the sensor last had saved in its own flash — not treated as fatal.
  - **Not independently verified this phase** (no physical LD2420 reachable
    — hardware smoke test of Phases 0-4 is using the only available board's
    serial port concurrently, per the task's explicit instruction not to
    touch it): the library's own fw v1.6.1-specific empirical notes (baud
    115200 vs 256000, exact ABD threshold behavior) are taken on faith from
    the library's documentation, not re-confirmed against real hardware.
    `GG_BAUDRATE`/`GG_TXPIN`/`GG_RXPIN` remain one-line `build_flags`
    overrides (already supported by the library itself) if bring-up finds
    different values needed — no code change required.

- **Phase 5 — `Ld2450Sensor` custom parser, as actually implemented
  (`src/radar/ld2450_sensor.h/.cpp`):** No physical LD2450 exists yet (plan's
  "future upgrade" note) — this driver is compile-and-logic-verified only,
  never run against real hardware this phase.
  - Frame: 4-byte header `AA FF 03 00` + 3 × 8-byte target blocks (`X int16
    LE`, `Y int16 LE`, `speed int16 LE`, `distance-resolution uint16 LE`) +
    2-byte footer `55 CC` = 30 bytes total (`kLd2450FrameBytes`), 256000
    baud (`kLd2450BaudRate`).
  - **Sign decoding, exactly as implemented** (`decodeLd2450Signed()`):
    ```cpp
    int16_t decodeLd2450Signed(uint16_t raw) {
      if (raw & 0x8000) return -static_cast<int16_t>(raw & 0x7FFF);
      return static_cast<int16_t>(raw);
    }
    ```
    i.e. bit 15 set → negative, magnitude in the low 15 bits; bit 15 clear →
    the raw value is the non-negative result as-is. This is the task spec's
    own corrected formula, used verbatim — **the plan's §6 shorthand
    (`raw & 0x8000 ? (raw & 0x7FFF) : -(raw)`) has its two branches
    inverted/wrong** relative to this (compare which branch negates), and is
    superseded by the task spec's explicit, double-checked version per the
    task's own instruction to verify this exact gotcha. Applied uniformly to
    X, Y, and speed; the 4th field (distance-resolution) is unsigned and
    parsed but not decoded with this function.
  - The distance-resolution field (block bytes 6-7) is parsed (to keep
    offsets correct for the remaining target blocks) but **not surfaced** in
    `RadarTarget` — no field in the sensor-agnostic `RadarState` shape (plan
    §6: `distanceMm`/`angleDeg`/`xMm`/`yMm`/`speedMmS`) corresponds to it
    cleanly, and neither `/api/radar/*` nor the tracking play-mode need it
    this phase.
  - Derived fields: `distanceMm = sqrt(x² + y²)`, `angleDeg =
    atan2(x, y) * 180/π` (0° = straight ahead/+Y, positive = sensor's
    right/+X) — an assumed axis convention (X = lateral, Y = forward),
    consistent with common LD2450 usage but **not independently verified**
    against a physical module.
  - **Empty-target-slot heuristic** (undocumented in the plan, added here):
    a target block reporting exactly `X == 0 && Y == 0` is treated as "slot
    unused" and skipped rather than a real target sitting exactly on the
    sensor — a common convention in other LD2450 implementations, but
    **not independently verified** this phase (no hardware to confirm
    against). Flagged clearly in code comments (`ld2450_sensor.cpp`) as a
    heuristic, not a confirmed protocol fact.
  - **Decoupled from I/O, per the task spec**: `parseLd2450Frame(const
    uint8_t *buf, size_t len, RadarState &out)` and `decodeLd2450Signed()`
    are free functions taking a raw byte buffer and returning/populating
    plain structs — no `Serial2`/hardware dependency at all — so they can be
    unit-tested later (e.g. a host-side PlatformIO test env) without a
    hardware harness. No test harness was actually set up this phase (not
    required per the task spec).
  - `Ld2450Sensor` (the class that owns the UART) is a small ring-buffer-less
    sliding-window reader: drains `Serial2.available()` into a 128-byte
    `rxBuf_` (headroom over the 30-byte frame for noise/resync), then scans
    every possible start offset for a valid header+footer match via
    `parseLd2450Frame()`, consuming through the matched frame's end on
    success (same "resync on any mismatch, one byte at a time" approach
    `LD2420GeoGab`'s own frame scanner uses). If the buffer fills without
    ever finding a valid frame, the newest half is kept and scanning
    continues — bounded memory, no possibility of a stuck/growing buffer.
    `begin()` is a plain `Serial2.begin(256000, SERIAL_8N1, RADAR_RX_PIN,
    RADAR_TX_PIN)` with no handshake (there's no config-mode protocol
    implemented for this custom parser) — it cannot itself fail or block, so
    it always returns `true`; actual "is a sensor really there" is inferred
    later from whether frames ever get parsed (see `RadarTask::getStatus()`
    below), exactly like the LD2420 path's fallback reasoning.
  - **Hardware-absent safety** (task spec's explicit requirement, applies to
    both sensors): `Serial2.available()` returns 0 immediately when nothing
    is wired up, so `poll()`'s drain loop and scan loop both execute zero
    iterations and return promptly — verified by reading the loop
    conditions, not assumed; no blocking read call (no `readString()`/
    `readBytesUntil()` with an implicit timeout) exists anywhere in this
    driver.

- **Phase 5 — `IRadarSensor` / `RadarState` shape
  (`src/radar/iradar_sensor.h`), as actually implemented:**
  - `RadarTarget{distanceMm, angleDeg, xMm, yMm, speedMmS}` — all
    `std::optional<float>`, matching `EyeCommand`'s existing
    partial-field convention from `motion/eye_pose.h` (Phase 3/4) exactly,
    per the task spec. A field a sensor can't provide is left
    `std::nullopt`, never a magic-number placeholder (e.g. LD2420 targets
    only ever populate `distanceMm`).
  - `RadarState{presence, targets[kRadarMaxTargets], targetCount,
    lastUpdateMs}` — a fixed-size array (`kRadarMaxTargets = 3`, sized for
    LD2450's max simultaneous targets; LD2420 only ever uses index 0) rather
    than a dynamic container, matching this codebase's existing avoidance of
    heap allocation in the motion/radar hot paths (e.g. `GestureEngine`'s
    fixed keyframe tables, `PlayModeManager`'s fixed
    `gGreetingKeyframes[16]`).
  - `IRadarSensor` is a small abstract base class (`begin()`/`poll()`/
    `getState()`) — genuinely virtual/polymorphic even though only one
    concrete implementation is ever compiled into a given build (selected by
    the existing `RADAR_LD2420`/`RADAR_LD2450` flag, mirroring
    `main.cpp`/`rest_routes.cpp`'s existing `radarVariant()` branching
    pattern), per the plan's explicit "IRadarSensor interface" ask — kept
    genuinely swappable rather than just a `namespace`-level free-function
    set like `ServoHal`, since the plan frames this specifically as a
    sensor-abstraction interface with two implementations.
  - **Deliberate design choice, beyond the plan's literal wording**:
    `IRadarSensor::getState()` is documented as **NOT** thread-safe by
    itself — only `RadarTask` ever calls a sensor's `begin()`/`poll()`/
    `getState()`, all from within `RadarTask`'s own FreeRTOS task. The
    actual thread-safe, cross-task-readable snapshot is
    `RadarTask::getState()` (mutex-guarded), matching `MotionTask`'s
    existing `AxisState[]` (task-private) vs. `gCurrentPose`
    (mutex-guarded, `MotionTask::getCurrentPose()`) split from Phase 3
    exactly. This is simpler and more consistent with the established
    codebase convention than making every `IRadarSensor` implementation
    independently thread-safe.

- **Phase 5 — `RadarTask` (`src/radar/radar_task.h/.cpp`), as actually
  implemented:**
  - Dedicated FreeRTOS task via `xTaskCreatePinnedToCore`, same idiom as
    `MotionTask::begin()` (Phase 3). **Core 0** (architecture plan §2:
    "Sensor & IO (Core 0)" — explicitly the opposite core from `MotionTask`,
    which owns Core 1's real-time servo loop), **priority 1** (below
    `MotionTask`'s 3; this task spends nearly all its time blocked in
    `vTaskDelay()` so it can't meaningfully starve `loop()`'s OTA/button
    housekeeping even at an equal-or-lower priority), **4096-byte stack**
    (same as `MotionTask` — comfortable margin; the largest stack user is
    `LD2420GeoGab::waitForResponse()`'s local 256-byte scan buffer, called
    only during `begin()`).
  - **Rate**: a plain `vTaskDelay(pdMS_TO_TICKS(30))` between `poll()`
    calls — NOT `vTaskDelayUntil` — per the task spec's explicit "this isn't
    a real-time control loop" guidance; 30ms sits in the middle of the
    spec's suggested 20-50ms range. Both sensors' own `poll()` implementations
    are what actually rate-limit meaningful work (`LD2420GeoGab::update()`'s
    internal 10ms throttle; `Ld2450Sensor::poll()`'s per-call frame scan), so
    this task's own tick rate just needs to be "frequent enough," not tuned.
  - `gSensor.begin()` is called exactly once, at the very start of the
    task's `for(;;)` loop function — see the LD2420/LD2450 write-ups above
    for why its bounded blocking is safe there and nowhere else. Its result
    is logged (`Serial.println`) but **not** treated as fatal — `poll()`
    keeps being called every tick regardless, so a sensor that isn't wired
    up yet, or that starts responding later, is still picked up (task spec's
    explicit "may not be attached yet" requirement).
  - Publishes a mutex-guarded `RadarState` snapshot after every `poll()`
    call — identical shape to `MotionTask::getCurrentPose()`'s pattern
    (`xSemaphoreCreateMutex`, held only for the struct copy,
    `RadarTask::getState()` takes it with a 50ms timeout and returns a
    plain-value copy on failure/timeout rather than blocking the caller).
  - `RadarTask::getStatus()` (backs `GET /api/radar/status`) derives
    `linkOk` from recency: `lastUpdateMs != 0 && (millis() - lastUpdateMs) <
    5000` — the task spec's own suggested "last 5s" window, not persisted
    or otherwise stateful beyond that.
  - Started from `main.cpp`'s `setup()` **before** `MotionTask::begin()`
    (previously `ServoHal::begin(); MotionTask::begin();` — now
    `ServoHal::begin(); RadarTask::begin(); MotionTask::begin();`) so the
    state mutex unconditionally exists before `MotionTask`'s task (which
    ticks `PlayModeManager`, which can call `RadarTask::getState()` from the
    `tracking` mode) ever starts running — belt-and-suspenders, since
    `PlayModeManager` always boots to `manual` anyway (Phase 4) so
    `tracking` can't actually be active this early regardless.

- **Phase 5 — tracking play-mode wiring
  (`src/motion/playmode_manager.cpp`), replacing the Phase 4
  `// TODO(Phase 5)` stub:**
  - `enterMode()`'s `Mode::Tracking` case now calls a real `startTracking()`
    (resets the rising-edge/retarget-timer state, then reuses `startIdle()`
    for baseline timers) instead of aliasing straight to `startIdle()`.
    `PlayModeManager::tick()`'s `Mode::Tracking` case now calls a real
    `trackingTick()` instead of falling through to `idleTick()`.
  - **Not gated by `#ifdef RADAR_LD2420`/`RADAR_LD2450`** — deliberately:
    `trackingTick()` reads `RadarTask::getState()` and decides its behavior
    per-tick based on whether **any current target actually has
    `angleDeg`**, not on which build flag is active. This keeps the logic
    correct automatically for both variants (LD2420 targets never populate
    `angleDeg`, so they always take the fallback path; LD2450 targets always
    do when present) without duplicating the mode's control flow per build,
    and stays correct if a future sensor's capability falls somewhere in
    between.
  - **Primary target selection** (task spec: "your call, document it"):
    the **closest** target (by `distanceMm`) among those that have
    `angleDeg` — chosen over "first in list" so a nearer person takes
    priority over whichever target the sensor happens to report first.
  - **LD2450 path (angle available)**: proportional gaze-following — each
    tick, if `now >= gTrackingNextRetargetMs`, pushes a `PlayMode`-sourced
    `EyeCommand` with `panDeg` set to the primary target's `angleDeg`
    (clamped to ±45°, matching `MotionTask`'s own `kGazeRangeDeg` working
    range — that constant is file-local to `motion_task.cpp` and not
    exported, so it's duplicated here as `kTrackingGazeRangeDeg` with a
    comment to keep the two in sync if the mapping ever changes), 280ms
    `EaseInOut`. **Damping**: `kTrackingMinRetargetMs = 300` — a retarget is
    only pushed once every ≥300ms even if the radar updates every tick,
    landing inside the task spec's suggested 200-400ms window, specifically
    to avoid a jittery/nervous-looking gaze on a sensor that can report
    ~10+ Hz. `tiltDeg` is deliberately left untouched (the LD2450 frame
    gives no elevation data — only lateral X/forward Y — so there's nothing
    principled to drive tilt with; it stays wherever `NaturalModeCoupler`/
    the previous command left it).
  - **LD2420 path (no angle data at all — either build, or an LD2450 build
    with no target currently detected)**: presence-triggered "alert" glance,
    exactly per plan §5's degradation note — on the **rising edge** of
    `state.presence` only (was false last tick, true this tick), triggers
    the existing `"surprise"` gesture (`GestureEngine::trigger("surprise",
    CommandSource::PlayMode)` — lids snap wide + gaze reset to center, see
    Phase 4's gesture table) rather than building a new bespoke glance
    animation, per the task spec's explicit "or reuse an existing gesture"
    option. Between rising edges, falls back to `idleTick()`'s baseline
    drift/blink so tracking mode never looks inert.
  - **Generation-bumping**: consistent with every other play mode's
    *routine autonomous* behavior (idle's drift/blinks, curious's saccades),
    neither the proportional retargeting nor the alert-glance trigger bumps
    `MotionTask`'s `commandGeneration` counter — only a genuinely external
    intent (a manual API call, a different play-mode activation) should, per
    the existing Phase 4 rule; tracking's own radar-driven pushes are
    exactly the kind of "the current mode's own routine output" the
    counter's doc comment says must NOT bump it.
  - Mode listing description (`GET /api/playmodes`) updated to describe the
    real Phase 5 behavior instead of the Phase 4 "safe no-op alias of idle"
    placeholder text.

- **Phase 5 — API routes (`src/api/radar_routes.h/.cpp`):**
  - `GET /api/radar/status` → `{sensorModel, linkOk, lastUpdateMs}`, a thin
    wrapper around `RadarTask::getStatus()`.
  - `GET /api/radar/latest` → `{presence, lastUpdateMs, targets: [...]}`.
    Each target object always has all five keys
    (`distanceMm`/`angleDeg`/`xMm`/`yMm`/`speedMmS`) present, but a field the
    active sensor can't provide serializes as JSON `null` rather than being
    omitted — a deliberate choice beyond the plan's literal "omit/absent"
    wording, so a frontend (Phase 7) or any other API consumer can rely on
    the key always existing and just check for `null`, rather than needing
    an `in`/`hasOwnProperty` check to tell "sensor can't provide this" apart
    from "a bug omitted a field."
  - Both routes registered in `src/net/web_server.cpp` alongside the
    existing route modules, following the established one-file-per-feature
    convention (`eyes_routes.*`, `servo_routes.*`, etc.) — no new body-
    parsing needed since both are `GET`s with no request body.

- **Phase 5 — WebSocket: explicitly deferred, not built.** No `/ws`
  endpoint or WebSocket infrastructure exists anywhere in this codebase —
  confirmed by checking `src/net/web_server.cpp` and the full route-file set
  (Phases 1-4 never added one). Per the task spec, building WS
  infrastructure from scratch as a tangent for radar telemetry alone was
  explicitly out of scope this phase; `GET /api/radar/latest` (REST
  polling) is the only telemetry path for now. The plan's §4 `WS /ws` topic
  `radar` line remains unimplemented — revisit when Phase 7's frontend
  actually needs a live-updating radar visualization page (plan §6's
  `/setup/radar.html`), at which point a general `/ws` endpoint would
  likely also want to carry pose/status telemetry, not just radar, making
  it a better-scoped standalone addition than a radar-only special case now.

- **Phase 5 — files added**: `src/radar/iradar_sensor.h`,
  `src/radar/ld2420_sensor.h/.cpp`, `src/radar/ld2450_sensor.h/.cpp`,
  `src/radar/radar_task.h/.cpp`, `src/api/radar_routes.h/.cpp`. **Files
  modified**: `src/motion/playmode_manager.cpp` (tracking mode wiring, mode
  description string), `src/net/web_server.cpp` (registers
  `RadarRoutes`), `src/main.cpp` (`RadarTask::begin()`, ordered before
  `MotionTask::begin()`). No `platformio.ini`/`partitions.csv` changes — the
  `gsieben/LD2420GeoGab` lib_dep was already pinned since Phase 0.

- **Build verification (Phase 5):** `pio run -e ld2420` and `pio run -e
  ld2450` **both succeeded with zero errors** — and, for the first time,
  the two environments' radar code genuinely differs (not just a
  `radarVariant` string), per the task's specific ask to verify that. Note:
  PlatformIO's default `src` filter compiles every `.cpp` under `src/`
  (including both `radar/ld2420_sensor.cpp` and `radar/ld2450_sensor.cpp`)
  into **both** environments regardless of which `RADAR_*` flag is active —
  confirmed in this phase's build logs, both files appear in both `pio run
  -e ld2420` and `-e ld2450` output — only `radar_task.cpp`'s
  `#if defined(RADAR_LD2420)/#elif defined(RADAR_LD2450)` actually decides
  which concrete `IRadarSensor` gets instantiated and linked into `main()`'s
  reachable call graph; the unused sensor class's code is left for the
  linker's own dead-code elimination (`--gc-sections`, already enabled by
  this toolchain) rather than excluded at the PlatformIO `src_filter` level
  — consistent with how this project already handles the `RADAR_LD2420`/
  `RADAR_LD2450` split everywhere else (a build-time `#ifdef`/`#if`, not a
  per-file source filter), and the flash numbers below suggest this is
  working as intended (the two environments differ by a plausible amount,
  not by the other sensor's + library's full unstripped size).
  - **`env:ld2420`**: RAM **16.9% (55460/327680 bytes)**, Flash **61.8%
    (971853/1572864 bytes)** — up from Phase 4's 61.1% (961089 bytes), a
    **+10764 byte (+0.7 percentage point)** increase for `IRadarSensor` +
    `Ld2420Sensor` (wrapping `LD2420GeoGab`) + `Ld2450Sensor` (compiled but
    unreferenced in this build's call graph) + `RadarTask` + the tracking
    mode wiring + `/api/radar/*`.
  - **`env:ld2450`**: RAM **16.4% (53600/327680 bytes)**, Flash **61.5%
    (968045/1572864 bytes)** — up from Phase 4's 61.1% (961089 bytes), a
    **+6956 byte (+0.4 percentage point)** increase. Smaller than
    `ld2420`'s delta as expected: `Ld2420Sensor`'s wrapped `LD2420GeoGab`
    calls are unreferenced here, so more of that code gets linker-stripped.
  - **This is the first phase where the two environments' RAM/flash numbers
    differ from each other** (previously always identical — no
    radar-specific code existed before Phase 5), exactly as the task
    anticipated. Both are comfortably clear of the 1.5MB OTA app slot and
    nowhere near the 90% flag-it-prominently threshold — **not** raising a
    flash-budget concern.
  - `pio run -t buildfs -e ld2420` and `-t buildfs -e ld2450` both
    succeeded, packing the unchanged `data/www/index.html`,
    `data/sequences/README.md`, and `data/sequences/greeting.json` into
    each environment's `littlefs.bin` with no errors (no `data/` changes
    this phase — radar has no frontend or sequence-file surface yet, per
    scope).
  - Not run this phase (per the task's explicit instruction): flashing or
    connecting to the physical board — another task was concurrently using
    COM4 for a Phases 0-4 hardware smoke test. All verification above is
    compile-only, exactly as expected/requested for this phase.

- **Phase 6 — `LedController` (`src/hal/led_controller.h/.cpp`), as
  implemented:** a lightweight cosmetic "mood glow" module, proportionate
  to the plan's own framing (plan §1: "optional future WS2812... for glow
  effects", not a lighting-effects engine). No WS2812 hardware is wired to
  `LED_DATA_PIN` (GPIO4, already declared in `pin_map.h` since Phase 0) —
  writing NeoPixel data to a floating pin is electrically harmless, so no
  hardware-presence detection was added; safety instead comes from the
  *feature* defaulting to off (`system/ledEnabled` NVS key, `false` by
  default, reserved since Phase 1 — reused as-is here, not duplicated).
  - **Simple polled module, not a FreeRTOS task** (per the task spec):
    `LedController::begin()` is called from `main.cpp`'s `setup()`
    (alongside `Buttons::begin()`, before `ServoHal::begin()`), and
    `LedController::handle()` is polled from `loop()` (alongside
    `Buttons::handle()`), same idiom as `WifiManager`/`OtaManager`/
    `Buttons`.
  - **Config model** (`LedController::LedConfig`): `enabled` (bool),
    `brightness` (uint8_t 0-255), `colorL`/`colorR` (`RgbColor{r,g,b}`,
    each 0-255), `effect` (`LedEffect` enum: `Off`, `Solid`, `Breathe` —
    exactly the three the task spec asked for, no rainbow/chase effects
    added).
  - **NVS layout**: `enabled` stays in the existing `system` namespace's
    `ledEnabled` key (Phase 1, unchanged). Everything else
    (`brightness`/`colorL`/`colorR`/`effect`) is packed into a new
    `NvsStore::LedColorConfig` struct (`brightness: uint8_t`,
    `colorL[3]`/`colorR[3]: uint8_t` R,G,B, `effect: uint8_t` — the raw
    `LedEffect` value, stored as a plain byte so `nvs_store.h` doesn't need
    to depend on `hal/led_controller.h` for an enum type) and written as
    one blob via `putBytes`/`getBytes` to a new `led` namespace, key
    `"cfg"` — mirrors `servocal`'s existing "small packed blob" pattern
    from Phase 3 exactly, including the same "stored-size mismatch ==
    treat as nothing saved" fallback (`NvsStore::getLedColorConfig()`).
    Defaults: brightness 128, both eyes a cool blue-ish `{0,120,255}`,
    effect `Off`.
  - **`LedController::begin()`**: `strip.begin()` (2-pixel
    `Adafruit_NeoPixel` on `LED_DATA_PIN`, `NEO_GRB + NEO_KHZ800`),
    immediately `clear()` + `show()` (inert on boot regardless of stored
    config — belt-and-suspenders on top of the `enabled=false` default),
    then loads `NvsStore::getLedEnabled()` + `NvsStore::getLedColorConfig()`
    into a RAM-cached `LedConfig`, and applies that initial state via the
    same `applyStatic()` path `handle()` uses for Off/Solid.
  - **`LedController::handle()`**: two paths.
    - `enabled && effect == Breathe`: rate-limited to **~40ms** between
      frames (`kBreatheUpdateIntervalMs`, inside the task spec's 30-50ms
      guidance) — computes a **triangle-wave** brightness (not sinusoidal;
      a cheap linear 0→peak→0 ramp reads as "close enough" to a sine
      breathe for a small ambient accent and avoids float trig) over a
      **3000ms** period (`kBreathePeriodMs`), floored at **15%** of the
      configured brightness (`kBreatheMinFraction`) rather than dimming
      fully to black each cycle, so it reads as a gentle pulse rather than
      a blink. Each frame: `strip.setBrightness(frameBrightness)` (the
      installed `Adafruit_NeoPixel@1.15.5`'s actual API — confirmed
      against `.pio/libdeps/ld2420/Adafruit NeoPixel/Adafruit_NeoPixel.h`/
      `.cpp` rather than assumed: `setPixelColor()` scales R/G/B by the
      *previously-set* `brightness` at call time, so `setBrightness()` is
      always called **before** `setPixelColor()` in this codebase's usage,
      never after), then `setPixelColor()` for both eyes' configured
      colors, then `show()`.
    - Otherwise (`Off`, or `Solid`, or disabled): a `gDirty` flag (set by
      `begin()`/`setConfig()`) gates a single `applyStatic()` call — so a
      static Off/Solid state calls `strip.show()` **once**, on change, not
      every `loop()` iteration, per the task spec's explicit "avoid
      needless show() calls" instruction.
  - **`LedController::getConfig()`/`setConfig()`**: `setConfig()` applies
    the full config to the RAM cache, persists `enabled` to
    `NvsStore::setLedEnabled()` and the rest to
    `NvsStore::setLedColorConfig()`, sets `gDirty = true` (so `handle()`
    re-applies Off/Solid on its very next tick) and resets the Breathe
    phase clock (`gLastBreatheUpdateMs = millis()`) so a Breathe
    config change always restarts its pulse cleanly from the bright end of
    the ramp rather than picking up mid-cycle at an arbitrary phase.
  - **`effectToName()`/`effectFromName()`**: `"off"`/`"solid"`/`"breathe"`
    string <-> `LedEffect` conversion, shared with `led_routes.cpp` for
    JSON (de)serialization.

- **Phase 6 — API routes (`src/api/led_routes.h/.cpp`)**, new file per the
  established one-file-per-feature-area convention:
  - `GET /api/led/config` — `{enabled, brightness, colorL:{r,g,b},
    colorR:{r,g,b}, effect}` (effect as its string name), a thin wrapper
    around `LedController::getConfig()`.
  - `POST /api/led/config` — **partial update**: the working copy starts
    from `LedController::getConfig()` (not a fresh default-constructed
    config), so any field omitted from the request body keeps its current
    value; only fields actually present in the JSON body are validated and
    applied. Validation: `brightness` and each RGB channel must be an
    integer 0-255 (`brightness_out_of_range`/`color_out_of_range`),
    `effect` must be one of the three known strings
    (`unknown_effect`) — all per the task spec's explicit ask. On success,
    calls `LedController::setConfig()` and responds with `{success: true,
    ...}` plus the resulting **full** config (not just the changed
    fields), so the caller always sees the complete current state.
    Body-parsing follows the established accumulate-then-parse-on-
    final-chunk `ArBodyHandlerFunction` pattern from
    `servo_routes.cpp`/`rest_routes.cpp`.
  - Registered in `src/net/web_server.cpp` alongside the existing route
    modules.

- **Phase 6 — `GET /api/system/info` gained `ledEnabled`**
  (`src/api/rest_routes.cpp`), matching how `otaNetworkEnabled` (Phase 2)
  and `naturalMode` (Phase 4) are already surfaced there: read from
  `LedController::getConfig().enabled` (the RAM cache), not raw NVS, so it
  always reflects what's actually applied. `GET /api/system/status` was
  **not** extended — the task spec's "if that fits naturally" note was
  judged to point at `/api/system/info` (where the other two boolean
  feature flags already live), not `/status` (which is pose/play-mode/
  connection-state oriented); kept intentionally brief, per the task's
  "don't over-add" instruction.

- **Phase 6 — files added**: `src/hal/led_controller.h/.cpp`,
  `src/api/led_routes.h/.cpp`. **Files modified**:
  `src/storage/nvs_store.h/.cpp` (new `led` namespace + `LedColorConfig`
  blob, `getLedColorConfig()`/`setLedColorConfig()` — `getLedEnabled()`/
  `setLedEnabled()` already existed from Phase 1, unchanged),
  `src/net/web_server.cpp` (registers `LedRoutes`), `src/main.cpp`
  (`LedController::begin()` in `setup()`, `LedController::handle()` in
  `loop()`), `src/api/rest_routes.cpp` (`ledEnabled` in
  `GET /api/system/info`). `include/pin_map.h` was **not** modified —
  `LED_DATA_PIN = 4` already existed from Phase 0/1 exactly as the plan's
  §1 pin table specifies (index 0 = left eye, index 1 = right eye, encoded
  directly as `kPixelLeft`/`kPixelRight` constants in
  `led_controller.cpp`). No `platformio.ini` changes — `Adafruit NeoPixel`
  was already pinned in `[env]`'s `lib_deps` since Phase 0, unused until
  now.

- **Build verification (Phase 6):** `pio run -e ld2420` and `pio run -e
  ld2450` both succeeded with **zero errors**.
  - **`env:ld2420`**: RAM **17.0% (55644/327680 bytes)**, Flash **62.7%
    (986421/1572864 bytes)** — up from Phase 5's 61.8% (971853 bytes), a
    **+14568 byte (+0.9 percentage point)** increase for
    `Adafruit_NeoPixel` actually getting linked in (pinned but unused
    since Phase 0) + `LedController` + `/api/led/*`.
  - **`env:ld2450`**: RAM **16.4% (53784/327680 bytes)**, Flash **62.5%
    (982833/1572864 bytes)** — up from Phase 5's 61.5% (968045 bytes), a
    **+14788 byte (+0.9 percentage point)** increase, essentially
    identical to `ld2420`'s delta as expected (LED code isn't
    radar-variant-dependent).
  - Both comfortably clear of the 1.5MB OTA app slot and nowhere near the
    90% flag-it-prominently threshold — **not** raising a flash-budget
    concern. `pio run -t buildfs -e ld2420` and `-t buildfs -e ld2450` both
    succeeded, packing the unchanged `data/www/index.html`,
    `data/sequences/README.md`, and `data/sequences/greeting.json` into
    each environment's `littlefs.bin` with no errors (no `data/` changes
    this phase — LED has no frontend surface yet, per scope; Phase 7
    handles `/setup/led.html`).
  - Not run this phase (per the task's explicit instruction): flashing or
    connecting to the physical board — compile-only verification is
    sufficient and expected, especially since no WS2812 hardware is
    physically wired to GPIO4 yet.

- **Phase 7 — plan-gap fix: `/api/wifi/*` routes.** The architecture plan's
  §4 API spec lists `GET /api/wifi/scan`, `POST /api/wifi/connect`, and
  `POST /api/wifi/forget`, but no phase 0-6 was ever actually assigned to
  wire these HTTP routes up — `WifiManager::connectToNetwork()` /
  `forgetNetwork()` / `getCachedScanResults()` have existed since Phase 1
  (`src/net/wifi_manager.h/.cpp`) with no route in front of them. This is a
  plan omission, not a deviation from an assigned task — Phase 7 is the
  first place a frontend actually needs them, so it closes the gap now.
  New `src/api/wifi_routes.h/.cpp`, following the established
  one-file-per-feature-area convention:
  - `GET /api/wifi/scan` — thin wrapper around
    `WifiManager::getCachedScanResults()`; returns `{results:[{ssid,
    rssiDbm, secure}, ...]}`. Does not trigger a fresh scan (the device
    only ever scans once, at boot, before AP mode starts) — the frontend's
    `setup/wifi.html` accordingly always shows a manual SSID text-entry
    fallback alongside the scanned dropdown.
  - `POST /api/wifi/connect {ssid, password}` — calls
    `WifiManager::connectToNetwork()` (itself non-blocking: saves to NVS,
    calls `WiFi.begin()`, returns immediately) and responds right away with
    `{success:true, status:"connecting"}`. `connectToNetwork()` has no own
    "did it work" return value — its outcome only becomes visible later via
    `WifiMode` — so, exactly as the task anticipated, the frontend must
    poll `GET /api/system/status`'s `wifiMode` field afterward to see
    `STA_CONNECTING` resolve to `STA_CONNECTED` or `STA_FAILED` (bounded
    ~15s timeout, `wifi_manager.cpp`).
  - `POST /api/wifi/forget` — calls `WifiManager::forgetNetwork()`, which
    itself does `delay(200); ESP.restart();` synchronously. Calling that
    directly from inside the HTTP handler risked the reboot tearing down
    the TCP connection before `AsyncWebServer` had actually flushed the
    JSON response to the client (the same class of problem Phase 2 solved
    for OTA). Fix: the handler sends the response immediately, then sets a
    `gForgetPending` flag + an 800ms deadline; a new `WifiRoutes::handle()`
    (polled from `main.cpp`'s `loop()`, registered alongside
    `OtaRoutes::handle()`) calls the real `WifiManager::forgetNetwork()`
    once that deadline passes — mirrors `ota_routes.cpp`'s deferred-restart
    pattern exactly, for the same underlying reason.
  - Registered in `src/net/web_server.cpp` alongside the other route
    modules; `WifiRoutes::handle()` added to `main.cpp`'s `loop()`.

- **Phase 7 — frontend files added** (`data/www/`, plain HTML/CSS/JS, no
  build step, no CDN dependencies — everything needed ships in the LittleFS
  image, per plan §7):
  - `css/app.css` — shared styling. CSS custom properties for light/dark
    (via `prefers-color-scheme`), mobile-first base layout (max-width
    720px content column, comfortable 40px+ tap targets), and the
    structural "control" (blue) vs "setup" (amber `--setup-accent`,
    `body.theme-setup`) header/theme split the plan calls for in place of
    auth — setup pages additionally render a persistent amber
    "you are in setup mode" banner under the header.
  - `js/api.js` — shared `Api` global: `get()`/`post()` JSON fetch
    wrappers, `pollEvery(fn, intervalMs, onError)` (the one seam every page
    polls status/pose/radar through, so a future WebSocket swap only needs
    this file to change), `throttle(fn, intervalMs)` (used by the gaze pad
    to cap drag-event request rate), and `uploadFile(path, file,
    onProgress)` (XMLHttpRequest-based, for OTA's progress bar — `fetch()`
    has no upload-progress event). Every named `Api.xxx()` convenience
    method maps 1:1 to a route verified directly from its route `.cpp` file
    in this codebase (not assumed from the plan's §4 sketch), per the
    task's explicit instruction to treat the plan's API spec as a rough
    guide only.
  - `index.html` — fetches `/api/system/status` + `/api/system/info` on
    load. If `wifiMode === "AP_SETUP"`, shows only a prominent "first-time
    setup" prompt linking to `setup/wifi.html` (this is what a
    captive-portal-redirected phone lands on, per `web_server.cpp`'s
    existing `onNotFound` → `/` redirect while in AP mode). Otherwise shows
    a dashboard (WiFi mode/IP, play mode, natural mode, radar variant,
    firmware version) and quick links into `control/*`/`setup/*`. Device
    "name" is NOT shown — `NvsStore::getDeviceName()` exists but is not
    exposed by any GET route as of Phase 6, so nothing invented a field the
    backend doesn't actually return; flagged here rather than silently
    fudged.
  - `control/manual.html` — 2D gaze pad (a circular `<div>`, pointer-events
    drag, works for touch + mouse) mapped to `POST /api/eyes/gaze`,
    throttled to ~1 request/70ms (~14/sec) via `Api.throttle`, 120ms
    `easeInOut` duration per command. Pad range assumes the documented
    ±45° working range from `motion_task.cpp` (Phase 3) — not itself
    exposed by any API field, so hardcoded as a named JS constant
    (`GAZE_RANGE_DEG`) with a comment, easy to change in one place if the
    real mechanism's usable range differs once assembled. Four eyelid
    sliders (0..1) → `POST /api/eyes/eyelids`, also throttled. Natural-mode
    checkbox → `POST /api/system/config {naturalMode}`. Gesture buttons
    populated from `GET /api/gestures`, each → `POST
    /api/gestures/{id}/trigger`. Live pose polling (`GET /api/eyes/pose`,
    700ms) updates the pad's dot position and eyelid sliders to reflect
    actual device state, suspended while the user is actively dragging (and
    for 300ms after release) so the two don't visibly fight.
  - `control/playmodes.html` — cards from `GET /api/playmodes`, active-mode
    highlight polled from `GET /api/playmodes/active` (2s), click → `POST
    /api/playmodes/{id}/activate`.
  - `control/status.html` — live dashboard: `GET /api/system/status` (700ms:
    wifiMode/ip/playMode/naturalMode/pose), `GET /api/system/info` (3s:
    uptime/freeHeap — chosen as the slower-poll pair per the task's own
    "reuse status's pose field" hint, since `/api/system/status` already
    embeds a full pose snapshot and a separate `/api/eyes/pose` poll would
    be redundant here), `GET /api/radar/latest` (700ms: presence + raw
    per-target fields).
  - `setup/wifi.html` — current mode/IP display; connect form with an
    SSID `<select>` populated from `GET /api/wifi/scan` (sorted by RSSI)
    *and* an always-visible manual-entry text field (selecting a dropdown
    option just fills the text field) since the scan may be empty/stale;
    password field; `POST /api/wifi/connect` then polls
    `/api/system/status` (1s, ~30s safety cap) until `wifiMode` resolves to
    `STA_CONNECTED`/`STA_FAILED`. "Forget network" button uses a native
    `confirm()` dialog (explicitly warning about the reboot) before `POST
    /api/wifi/forget`. Shown unconditionally, not just in AP_SETUP mode,
    per the plan's "AP-mode captive-portal setup + normal STA re-config"
    wording for this page.
  - `setup/calibration.html` — one card per servo from `GET
    /api/servos/config` (min/center/max number inputs, inverted checkbox,
    a separate test-pulse number input defaulting to the current center),
    "Test" → `POST /api/servos/test`, "Save" → `POST /api/servos/config`
    (client-side validates `min < center < max` before sending, mirroring
    the server's own validation in `servo_routes.cpp` so the error surfaces
    immediately rather than round-tripping). Explicit on-page note about
    `MotionTask` potentially overwriting a test pulse within ~20ms if
    autonomous motion is active, matching `servo_routes.cpp`'s own
    documented caveat.
  - `setup/radar.html` — polls `GET /api/radar/latest` (250ms) and `GET
    /api/radar/status` (2s). Per the task's explicit requirement, this
    page does **not** branch on which build variant is running — it
    inspects the actual JSON per target and switches between a `<canvas>`
    plot (used whenever *any* target has `xMm`/`yMm` or `angleDeg`+
    `distanceMm`) and a distance gauge/bar (fallback, used when only
    `distanceMm` is present) purely from what fields are actually non-null
    in the response, so the same unmodified page is correct against both
    `env:ld2420` and `env:ld2450` builds. Shows a presence badge and a note
    that full directional tracking needs the LD2450 upgrade, shown/hidden
    based on whether angle/position data is actually present rather than
    hardcoded to one build.
  - `setup/ota.html` — current `firmwareVersion`/`buildDate`/`radarVariant`/
    `chipId` from `/api/system/info`. Two independent upload widgets
    (firmware → `/api/ota/firmware`, filesystem → `/api/ota/filesystem`),
    each using `Api.uploadFile()` (`XMLHttpRequest`, not `fetch`, per the
    task's explicit requirement, for `upload.onprogress`) with a progress
    bar, then polls `GET /api/ota/status` after completion. Explicit
    reboot warning in both the page's setup-banner and the success message.
  - `setup/led.html` — form for `GET`/`POST /api/led/config` (enabled
    checkbox, brightness slider 0-255, `<input type="color">` for
    colorL/colorR — converted to/from the `{r,g,b}` JSON shape client-side
    — effect select `off`/`solid`/`breathe`). Explicit on-page note that no
    LED hardware is wired yet but the config still saves, matching
    `led_controller.h`'s own Phase 6 documented behavior.

- **Phase 7 — verification performed** (no physical ESP32 in this
  environment, per the task's explicit instruction not to flash/connect to
  the board — COM4 may still be in use elsewhere):
  - Served `data/www/` via `python -m http.server 8123` and drove every one
    of the 9 pages (`index.html` + 3 `control/*` + 5 `setup/*`) in the
    browser tool, both at default width and re-checked at an emulated
    375px-wide mobile viewport. All 9 pages: render their static layout
    correctly, show no JS syntax errors, and handle every failed
    `fetch()`/API call gracefully — either an inline `msg-error` element
    (index, playmodes, status, servo list, LED, OTA info) or a
    `console.warn` from `pollEvery`'s `onError` handler for routine polling
    failures (pose/status/radar polls) — never a blank page, an uncaught
    exception, or unhandled-rejection console spam. (Because
    `http.server`'s 404 response body isn't JSON, `Api`'s `res.json()`
    parse failure is itself caught internally and treated as "no body",
    exercising the exact error path a real device's `404 {"error":
    "not_found"}` JSON response would also take, just via a different
    trigger — confirms the graceful-failure path works either way.)
  - Confirmed the control/setup theme split is visually unambiguous
    (blue header + no banner on `control/*`; amber header + amber
    "maintenance mode" warning banner on every `setup/*` page) and that
    interactive elements (disabled-until-file-chosen upload buttons, mode
    cards, sliders, the gaze pad's drag thumb) behave correctly with no
    backend present.
  - Did not attempt to mock a full `/api/*` response set against the
    static server (Python's `http.server` can't easily impersonate the
    device's JSON API across 9 pages' worth of distinct endpoints within
    this phase's scope) — dynamic-rendering code paths (servo card
    generation, radar canvas plotting, LED color-picker round-trip) were
    manually re-read against the actual API shapes confirmed from each
    route's `.cpp` source (not the plan's §4 sketch) rather than
    exercised end-to-end in a browser. Full live-API verification happens
    once this build is flashed, per the task's own final-integration note.

- **Build verification (Phase 7):** all four required builds succeeded
  with zero errors.
  - `pio run -e ld2420`: RAM 17.0% (55668/327680 bytes), Flash 63.0%
    (990817/1572864 bytes) — up from Phase 6's 62.7% (986421 bytes), a
    **+4396 byte** increase for the new `src/api/wifi_routes.cpp` (the only
    new C++ this phase adds — the frontend itself lives entirely in
    `data/`, outside the app image).
  - `pio run -e ld2450`: RAM 16.4% (53808/327680 bytes), Flash 62.8%
    (987241/1572864 bytes) — up from Phase 6's 62.5% (982833 bytes), a
    **+4408 byte** increase, essentially identical to `ld2420`'s delta as
    expected (`wifi_routes.cpp` isn't radar-variant-dependent). Both
    comfortably clear of the 1.5MB OTA app slot.
  - `pio run -t buildfs -e ld2420` and `-t buildfs -e ld2450`: both
    succeeded, packing all 13 `data/` files (`/sequences/greeting.json`,
    `/sequences/README.md`, and the 11 new/updated `/www/**` frontend
    files) into `littlefs.bin`. **LittleFS size check** (the one budget
    this phase was told to watch closely): the produced `littlefs.bin` is
    983,040 bytes for both environments — that is `mklittlefs` always
    padding its output image to the full target partition size
    (`0xF0000` = 983,040 bytes, matching `partitions.csv`'s `littlefs`
    partition), **not** a measure of actual content usage. The real
    figure is the source `data/` directory's total size: **73,648 bytes
    (~72KB)** — `data/www/` 71,059 bytes (9 HTML pages + `css/app.css` +
    `js/api.js`) + `data/sequences/` 2,589 bytes (unchanged from Phase 4).
    That is **~7.5% of the 960KB partition's raw capacity** before
    accounting for any LittleFS per-file block overhead — nowhere near
    the ~960KB ceiling, not flagged as a concern. (`mklittlefs -l`'s
    exact-used-bytes introspection was attempted but errored with an
    internal block-count assertion against this image — not pursued
    further since the raw-source-size figure already makes the "is this
    close to full" question moot by a wide margin; no icon/font files
    were added, per the task's explicit guidance, keeping every page's
    payload to text only.)
  - Not run this phase (per the task's explicit instruction): flashing or
    connecting to the physical board.

- **Phase 8 — Integration pass, as actually performed.** Full read of every
  `.cpp`/`.h` under `src/`/`include/` (all ~40 files) plus every page under
  `data/www/`, cross-referenced against this file's Phase 0-7 notes and the
  architecture plan. The `code-review` skill was also run (path-scoped,
  high effort — no git history exists in this project directory, so its
  diff-based mode fell back to a full-scope read) as a second, independent
  pass; its findings are folded into the list below alongside what the
  manual read turned up. Two of the bugs found (the LittleFS path
  mismatches) are genuinely severe — the frontend and the "greeting" play
  mode have been non-functional in exactly the way a first real hardware
  bring-up would have discovered immediately, since no prior phase ever
  actually flashed a device or browsed to it. Every fix below was verified
  by a full `pio run`/`pio run -t buildfs` for both `ld2420`/`ld2450` after
  it landed (not just at the very end) — see the final build verification
  block for the last-word numbers.

  **Bugs found and fixed:**

  1. **Frontend completely unreachable at its real URLs (`src/net/web_server.cpp`).**
     PlatformIO's LittleFS image build uploads the `data/` directory's
     *contents* as the filesystem root — `data/www/index.html` lands at FS
     `/www/index.html`, not FS `/index.html` (confirmed directly against
     this project's own `pio run -t buildfs` output, which lists
     `/www/index.html`, `/www/control/manual.html`, etc.). `web_server.cpp`
     had served `LittleFS`'s root ("/") at the URL root ("/") since Phase 1
     (`serveStatic("/", LittleFS, "/")`), which was wrong the entire time:
     requesting `/` looked for FS `/index.html` (doesn't exist) and 404'd,
     and every other page (`/control/manual.html`, `/css/app.css`, ...)
     404'd the same way — including the AP-mode captive-portal's own
     redirect target (`handleNotFound()` redirects to `"/"`), so even the
     WiFi setup flow would have landed on a 404 instead of
     `setup/wifi.html`. This was invisible through Phase 7 because that
     phase's own frontend verification served `data/www/` directly with a
     throwaway `python -m http.server` (see Phase 7 notes above), which
     masked the FS-root-vs-`/www/`-subtree mismatch entirely — nothing
     before Phase 8 ever exercised the real device's actual LittleFS
     layout. **Fix**: `serveStatic("/", LittleFS, "/www/")` — now `/` looks
     for FS `/www/index.html` (exists), `/control/manual.html` maps to FS
     `/www/control/manual.html` (exists), etc. Verified every page's
     existing relative links (`../css/app.css`, `../index.html`,
     `control/manual.html`, ...) resolve correctly under this scheme by
     re-reading all 9 HTML pages — none needed changes, only the server's
     serving root did. `data/sequences/*.json` is intentionally left
     outside the served subtree (it's read directly off LittleFS by
     firmware, never over HTTP).

  2. **"greeting" play mode has always silently no-op'd to idle
     (`src/motion/playmode_manager.cpp`).** Same root cause as #1:
     `kGreetingPath` was `"/data/sequences/greeting.json"`, but the actual
     FS path (per the same buildfs evidence) is `/sequences/greeting.json`
     — the leading `/data` segment doesn't exist on the real filesystem, so
     `LittleFS.open(kGreetingPath, "r")` has failed on every single
     activation since Phase 4, and `startGreeting()`'s documented
     fallback-to-idle path (`loadGreetingSequence()` returns `false` ->
     "greeting sequence unavailable, falling back to idle") has been
     silently swallowing this the entire time — `POST
     /api/playmodes/greeting/activate` has always actually activated idle
     instead. **Fix**: corrected the constant to
     `"/sequences/greeting.json"`. Confirmed against the same `pio run -t
     buildfs` file listing used for fix #1.

  3. **Body-less POST requests hang forever (never respond) on 7 routes.**
     Every route registered as `server.on(path, method, noOpLambda,
     nullptr, bodyHandler)` — `POST /api/system/config` (rest_routes.cpp),
     `/api/servos/config` + `/api/servos/test` (servo_routes.cpp),
     `/api/eyes/gaze` + `/api/eyes/eyelids` (eyes_routes.cpp),
     `/api/led/config` (led_routes.cpp), `/api/wifi/connect`
     (wifi_routes.cpp) — never responds at all if the client POSTs with no
     body / `Content-Length: 0`. Confirmed directly against
     `.pio/libdeps/<env>/ESPAsyncWebServer/src/WebRequest.cpp`'s
     header-parse state machine: when there's no body, the parser jumps
     straight from `PARSE_REQ_HEADERS` to `PARSE_REQ_END` and calls
     `onRequest` immediately *without ever calling `onBody`* — so the
     no-op `onRequest` lambda left the request with no response ever sent,
     and the actual response-sending logic (which lived entirely inside
     `onBody`) never ran. This project's own frontend (`js/api.js`) never
     triggers this in practice (`Api.post()` always sends at least `"{}"`
     — see its `post = (path, body) => request("POST", path, body ===
     undefined ? {} : body)`), so it was invisible through Phase 7's
     browser-driven verification, but a bare `curl -X POST <url>` or any
     third-party API/voice/AI integration client (one of this project's
     explicit goals, per the plan's intro) easily could hit it. **Fix**:
     added `JsonHelpers::requireBody()` (see the new `api/json_helpers.*`
     below) as the `onRequest` callback for all 7 routes in place of the
     no-op lambda — it checks `request->contentLength() == 0` (true only
     when `onBody` never ran) and responds `400 missing_body` in that case;
     otherwise it's a no-op, since `onRequest` is guaranteed to fire only
     after every `onBody` call has already completed (confirmed from the
     same state-machine read), so the real response has already been sent
     by then.

  4. **`durationMs` sign/range unchecked, wraps to ~49.7 days
     (`src/api/eyes_routes.cpp`).** `POST /api/eyes/gaze` and `POST
     /api/eyes/eyelids` did `cmd.durationMs = reqDoc["durationMs"] | 200;`
     — ArduinoJson deserializes the JSON number as a signed `int`, then
     assigns it into `EyeCommand::durationMs` (`uint32_t`, `eye_pose.h`)
     with no range check. A client sending `{"pan":10,"durationMs":-1}`
     wraps `-1` to `4294967295` on that unsigned assignment;
     `motion_task.cpp`'s `valueAt()` then never sees `elapsed >=
     durationMs` become true for ~49.7 days, so that axis silently freezes
     at its start value instead of ever reaching the commanded target
     (found by the `code-review` skill pass, independently confirmed by
     reading `eye_pose.h`/`motion_task.cpp`). **Fix**: added
     `parseDurationMs()` (clamped to `[0, 60000]` ms, generous but bounded,
     matching this codebase's existing numeric-bound validation style for
     servo pulses/LED brightness/color) and used it in both handlers;
     out-of-range now responds `400 durationMs_out_of_range` instead of
     silently corrupting the command.

  5. **Three real cross-task races where an HTTP-task write and a
     MotionTask-task read/write the same unsynchronized state (found by the
     `code-review` skill pass, each independently confirmed by re-reading
     the two call sites involved):**
     - `src/motion/gesture_engine.cpp` — `GestureEngine::trigger()` (called
       directly from the AsyncTCP/HTTP task by `gesture_routes.cpp`'s `POST
       /api/gestures/{id}/trigger`) and `GestureEngine::tick()` (called
       every ~20ms from MotionTask's own task) shared `gActive`/
       `gKeyframeIndex`/`gNextDueMs`/`gCapturedGeneration`/`gActiveSource`
       with zero synchronization. An HTTP-triggered gesture landing
       mid-`tick()` could overwrite `gActive` with a different (possibly
       shorter) `GestureDef` between two of `tick()`'s reads of it, so
       `pushKeyframe()` could then index `gActive->keyframes[gKeyframeIndex]`
       with a stale index against the new array — an out-of-bounds read.
       (PlayModeManager's own idle/curious/tracking-triggered
       `GestureEngine::trigger()` calls all happen from *inside* MotionTask's
       own tick, so those were never actually racing `tick()` — only the
       HTTP-task path was.) **Fix**: added a short-held `SemaphoreHandle_t`
       around the whole body of `tick()` and `trigger()` (same "hold only for
       the critical section" idiom as `motion_task.cpp`'s `gPoseMutex`).
     - `src/motion/playmode_manager.cpp` — re-activating `"greeting"`
       (`PlayModeManager::activate()`, HTTP task) while a previous greeting
       sequence was still mid-playback (`greetingTick()`, MotionTask task)
       could rewrite `gGreetingKeyframes[]`/`gGreetingKeyframeCount`
       (`loadGreetingSequence()` resets the count to 0 and repopulates the
       array element-by-element) at the same instant `greetingTick()` was
       reading `gGreetingKeyframes[gGreetingIndex]` against the OLD
       count/index. **Fix**: added a `gGreetingMutex` around
       `startGreeting()`'s load-and-reset (portMAX_DELAY — it's an
       infrequent, HTTP-task-only call, blocking briefly there is fine) and
       around `greetingTick()`'s read (a short 5-tick bounded wait, so a
       slow LittleFS read on the HTTP-task side can never stall MotionTask's
       real-time loop — a timeout just skips that one ~20ms tick's greeting
       work and retries next tick, harmless for a one-shot sequence).
     - `src/hal/servo_hal.cpp` — `reapplyCalibration()` (HTTP task, `POST
       /api/servos/config`) does `gCalibration[i]=...; detach(); attach();
       writeMicroseconds(...)` with no synchronization, while MotionTask
       calls `setPulseUs()`/`getCalibration()` for every axis every ~20ms
       from its own task. A `setPulseUs()` landing between `detach()` and
       `attach()` for the same channel would silently write into a detached
       `Servo` instance (a dropped tick), and `getCalibration()` could read
       a torn `ServoCalibration` struct (a mix of old/new min/center/max)
       mid-assignment. **Fix**: one mutex guarding all of `setPulseUs()`/
       `reapplyCalibration()`/`getCalibration()`'s bodies — each is a
       handful of instructions or one hardware register write, so the
       added overhead on the 50Hz hot path is negligible.

  6. **`POST /api/system/reboot` referenced by the frontend but never
     implemented (plan §4 gap, same class as Phase 7's `/api/wifi/*`
     gap).** `data/www/js/api.js` has exposed `Api.reboot()` (`post
     "/api/system/reboot"`) since Phase 7, but no phase 1-7 ever actually
     registered that route (`rest_routes.cpp` only ever had `/api/system/
     info`, `/api/system/status`, `/api/system/config`) — and no page
     calls `Api.reboot()` either, so this was silent dead API surface, not
     a user-visible bug. **Fix**: implemented `POST /api/system/reboot` in
     `rest_routes.cpp`/`.h`, mirroring `ota_routes.cpp`'s/`wifi_routes.cpp`'s
     existing deferred-restart pattern (respond first, `ESP.restart()`
     ~500ms later via a new `RestRoutes::handle()` polled from `main.cpp`'s
     `loop()`, giving AsyncTCP time to flush the response first).

  7. **`STA_FAILED` was never actually observable over the API
     (`src/net/wifi_manager.cpp`, found by the `code-review` skill pass).**
     On an STA-connect timeout, `handle()` set `gMode =
     WifiMode::STA_FAILED` and then, in the very same call, unconditionally
     called `startApMode()` — which itself immediately overwrites `gMode`
     back to `AP_SETUP` before `handle()` returns. `STA_FAILED` was
     consequently never visible between two `loop()` iterations, so `GET
     /api/system/status`'s `wifiMode` field never actually reported it —
     and `data/www/setup/wifi.html` specifically polls that field every 1s
     watching for `wifiMode === 'STA_FAILED'` to show "Could not connect...
     falling back to setup AP." That branch was dead code; a user
     attempting a bad WiFi connect only ever saw the generic "~30s safety
     stop" fallback message, not the correct one. **Fix**: `STA_FAILED` is
     now held for `kStaFailedHoldMs` (2500ms) — long enough for the
     frontend's 1s poll to observe it at least once or twice — before a
     separate `handle()` branch actually calls `startApMode()`.

  **Cross-phase consistency cleanup (not bugs, but the "do all route files
  follow the same pattern" review this phase was asked to do):**

  - **`sendJson()`/`sendJsonError()` had been hand-duplicated nearly
    verbatim in 7 different route files** (`eyes_routes.cpp`,
    `servo_routes.cpp`, `gesture_routes.cpp`, `playmode_routes.cpp`,
    `led_routes.cpp`, `wifi_routes.cpp`, and inline in `rest_routes.cpp`) —
    the architecture plan's §9 file layout always named a single
    `api/json_helpers.*` for exactly this, but no phase 1-7 actually
    created it. Consolidated into new `src/api/json_helpers.h/.cpp`
    (`JsonHelpers::sendJson()`, `JsonHelpers::sendJsonError()`, plus the new
    `JsonHelpers::requireBody()` from fix #3 above) and every route file
    updated to use it instead of its own local copy. `src/api/ota_routes.cpp`
    was deliberately **left alone** — its response-building shape
    (conditional status code, different field set per success/failure) is
    genuinely different enough from the other routes' `{success, error}`
    shape that forcing it through the same two helpers would have made that
    file harder to read, not more consistent.
  - `src/api/radar_routes.h`'s doc comment said fields "are omitted as JSON
    null" (self-contradictory — omitted and present-as-null are different
    things); tightened to explicitly say every target key is always present
    and a missing capability serializes as `null`, matching
    `radar_routes.cpp`'s actual `handleLatest()` behavior exactly.

  **Reviewed and confirmed correct (no fix needed) — the "did later phases'
  assumptions about earlier phases hold up" checks this phase specifically
  asked for:**

  - `src/radar/ld2450_sensor.cpp`'s hand-rolled byte parser
    (`parseLd2450Frame()`/`decodeLd2450Signed()`/the sliding-window
    `poll()`): re-checked every buffer index against `kLd2450FrameBytes`/
    `kRxBufSize`, the empty-slot heuristic, and the resync-on-mismatch loop
    — all bounds-safe, no overread possible even with a maximally
    adversarial/noisy byte stream (verified by tracing the offsets by hand,
    not just re-reading the comments).
  - `src/radar/ld2420_sensor.cpp`, `src/radar/radar_task.cpp`,
    `src/motion/motion_task.cpp` — the `MotionTask`/`RadarTask` mutex
    patterns (`gPoseMutex`/`gStateMutex`, both "hold only for a plain
    struct copy, bounded-timeout read") are correct and consistently
    applied; this is the pattern the three fixes in item 5 above now also
    follow.
  - `src/storage/nvs_store.cpp` — every `Preferences::begin()` has a
    matching `end()` on every path (including every early-return path);
    no leaked NVS handles found anywhere in the file.
  - `src/hal/servo_hal.cpp` (pre-fix-5) / `src/motion/motion_task.cpp`'s
    `degreesToPulseUs()`/`normalizedToPulseUs()`/`clampToCalibration()` —
    pulse-width clamping is correct at both the calibrated-range level (this
    codebase) and cross-checked against ESP32Servo's own tighter absolute
    500-2500us clamp underneath it; no off-by-one at either boundary
    (`us < cal.minUs` / `us > cal.maxUs`, inclusive bounds behave as
    expected at exactly `minUs`/`maxUs`).
  - `src/motion/eye_pose.cpp`'s `applyEasing()` smoothstep — clamps `t` to
    `[0,1]` before use, correct zero-velocity-at-both-ends behavior.
  - **`data/sequences/greeting.json` vs. `playmode_manager.cpp`'s parser**
    (explicitly requested sanity-check): the file's integer-literal fields
    (e.g. `"panDeg": 0`, no decimal point) parse correctly against
    `kf["panDeg"].is<float>()` — confirmed by reading
    `ArduinoJson`'s actual installed source
    (`.pio/libdeps/ld2420/ArduinoJson/src/ArduinoJson/Variant/VariantData.hpp`):
    `isFloat()` returns `type_ & VariantTypeBits::NumberBit`, which is set
    for *any* JSON number regardless of whether it was written with a
    decimal point, so `is<float>()` is not actually testing "was this
    written as a float literal" — no bug here, despite that being a
    plausible-looking trap. (This check only became meaningful after fix
    #2 above made `greeting.json` actually loadable in the first place.)
  - **Two-environment parity** (explicitly requested check): grepped the
    full `src/` tree for `RADAR_LD2420`/`RADAR_LD2450` — confirmed the only
    *behavioral* branch is in `radar_task.cpp`/`.h` (which concrete
    `IRadarSensor` gets instantiated) plus two purely-cosmetic string
    branches (`rest_routes.cpp`'s `radarVariant()`, `main.cpp`'s boot
    banner); `playmode_manager.cpp`'s tracking-mode code deliberately does
    **not** branch on the macro (it branches on whether a target actually
    has `angleDeg`, by design — see Phase 5 notes) and its one mention of
    `RADAR_LD2420`/`RADAR_LD2450` is a comment explaining exactly that. No
    accidental environment-specific divergence found anywhere else.
  - Re-verified `EyeCommand`'s `std::optional<float>` fields survive the
    `xQueueSend`/`xQueueReceive` byte-copy through `CommandQueue` correctly
    (trivially-copyable for `float`, no UB) — this was Phase 3's own C++17
    build-flag workaround being trusted correctly by every later phase.

  **Considered and deliberately left alone (documented per this phase's
  explicit instruction, not silently dropped):**

  - **Cross-task reads of small, simple RAM-cached flags with no mutex**:
    `WifiManager::gMode`/`gApSsid`/`gScanResults[]` (read from the HTTP task
    via `wifi_routes.cpp`, written from the Arduino `loop()` task),
    `OtaManager::gEnabledCached`, `NaturalModeCoupler::gEnabled`/
    `gSuppressed`, `LedController::gConfig` (read every `loop()` tick,
    written from the HTTP task via `led_routes.cpp`) all follow this same
    pattern. `gScanResults[]` holding `String` (not just plain scalars) is
    the one member of this group with a theoretically real (if very
    unlikely in practice — `GET /api/wifi/scan` is a rarely-polled,
    setup-only endpoint, and a fresh scan only ever runs at boot or on an
    STA-timeout-to-AP transition) hazard, since Arduino's `String` isn't
    internally thread-safe either. Not converted to mutex-guarded snapshots
    like fix #5's three cases: those three were promoted specifically
    because their failure mode is a genuine out-of-bounds/torn-struct
    memory-safety issue reachable from *normal, expected* single-user
    operation (any gesture trigger, any calibration save, any greeting
    re-activation), whereas this group's failure mode is at worst a
    momentarily-stale display value, and doing it consistently for all four
    modules would be a broader architectural change (converting several
    independent modules' whole config-cache pattern to mutexes) out of
    proportion with the actual risk for a single-user local-network device.
  - **The chunked-body-accumulation buffer is a shared `static String` per
    route, not per-request** (`gGazeBodyBuffer`, `gConfigBodyBuffer`, etc.,
    one instance per route across all 7 body-parsing route files): two
    clients POSTing to the *same specific route* with a body that arrives
    split across multiple TCP segments, interleaved at the AsyncTCP layer,
    could corrupt each other's buffered body. In practice this needs two
    concurrent requests to the same endpoint with a multi-chunk body — this
    project's own JSON bodies are all a few dozen bytes (single-chunk in
    virtually every real case) and its target usage (one phone/PC/AI
    client at a time on a local network) makes simultaneous same-endpoint
    POSTs unlikely, but it's a real latent inconsistency with how
    `ota_routes.cpp`'s multipart handlers correctly use
    `request->_tempFile`/per-request state instead. A proper fix means
    moving every route's body buffer to `request->_tempObject`-based
    per-request storage — a mechanical but broad rewrite touching all 7
    body-parsing routes; left as a known limitation rather than risking
    that rewrite this late in the integration pass. Flagging explicitly
    here for a future pass.
  - **NVS write failures are silently ignored** (`src/storage/nvs_store.cpp`
    — every `putBytes`/`putString`/`putBool` call discards its return
    value, which is 0/false on a flash write failure): a full-NVS-partition
    or flash-fault scenario would let a route respond `{"success": true}`
    for a save that didn't actually persist, silently reverting to the old
    value on reboot. Not fixed: threading a real error result back up
    through `nvs_store.h`'s API and every one of its ~15 setters/each of
    their 5 calling route files is a broad, low-value change for a failure
    mode (NVS write failure on a few dozen bytes) that's rare in practice
    and, worse, `Preferences::getBytesLength()` mismatch already gives this
    codebase graceful degradation on the *read* side (falls back to
    defaults) even if a write silently failed.
  - `GestureEngine::isPlaying()` still reads `gActive` outside the new
    mutex added in fix #5 — left unguarded deliberately: it's a single
    pointer-sized read (not a multi-field struct), isn't currently called
    from anywhere in this codebase, and pointer-sized reads/writes are
    atomic in practice on this target, so guarding it added no real safety
    margin for the complexity of taking a lock in a function with no
    current callers.
  - `CommandQueue::push()`'s `bool` return value is discarded by every
    caller (`eyes_routes.cpp`, `gesture_engine.cpp`, `playmode_manager.cpp`)
    — this is `command_queue.h`'s own documented, intentional design ("a
    'latest intent' pipe... an intermediate command that never got applied
    is harmless, only the latest target per axis matters"), not an
    oversight; a dropped push only matters if it's *also* the very last
    command in a sequence, which given the 8-deep queue drained every
    ~20ms is exceptionally unlikely, and the existing design already
    accepts this trade-off explicitly.
  - `pio check`'s output (see below) includes ~66 "low:style unusedFunction"
    findings and 12 "high:error" preprocessor findings inside
    `ArduinoJson`'s own headers — both are tool limitations, not real
    issues (see that section below), and are not actionable.

- **`pio check -e ld2420` (cppcheck) results**, run as the final lint pass
  per the task's request:
  - **12 `high:error` findings, all inside
    `.pio/libdeps/ld2420/ArduinoJson/src/ArduinoJson/Polyfills/preprocessor.hpp`**
    ("failed to expand `ARDUINOJSON_BEGIN_PUBLIC_NAMESPACE`..."): cppcheck's
    preprocessor can't handle ArduinoJson v7's heavy macro/`##`-token-paste
    usage — this is a well-known cppcheck-vs-ArduinoJson-v7 limitation, not
    a real defect in library or project code (the library obviously
    compiles and runs correctly, per every successful `pio run` in this and
    every prior phase). Not actionable, not in project code.
  - **~66 `low:style unusedFunction` findings across every namespaced
    `.cpp` file in `src/`** (`Buttons::begin()`, `ServoHal::setPulseUs()`,
    every `NvsStore::get*()`/`set*()`, etc.): cppcheck's default
    single-translation-unit mode doesn't see that these functions are
    declared in a header and called from a *different* `.cpp` file — every
    one of these is actually called (confirmed by this phase's own manual
    read of the full call graph). Not actionable, false positives from the
    analysis mode used, not `--enable=all` project-wide whole-program
    analysis (which is what a full cppcheck project database would need,
    not available in this environment/invocation).
  - **2 `low:style noExplicitConstructor` findings, both inside library
    headers** (`Adafruit_NeoPixel`, `ESP32PWM`) — not project code, not
    actionable from this project.
  - No findings inside project code (`src/`, `include/`) beyond the
    `unusedFunction` false positives above — i.e., cppcheck did not surface
    anything this phase's manual read hadn't already found (or, in the
    `durationMs`/concurrency cases, already fixed).

- **Final build verification (Phase 8), all four required builds run after
  every fix above had landed:**
  - `pio run -e ld2420`: **SUCCESS**, zero errors. RAM 17.0% (55692/327680
    bytes), Flash 62.9% (989249/1572864 bytes) — actually **down slightly**
    from Phase 7's end state (990817 bytes / 63.0%), a net **-1568 bytes**:
    consolidating 7 duplicated `sendJson`/`sendJsonError` copies into the
    new shared `json_helpers.*` saved more than the new `/api/system/reboot`
    route, the `durationMs` validation, and the three new mutexes (fix #5)
    together added — comfortably clear of the 1.5MB OTA app slot either
    way.
  - `pio run -e ld2450`: **SUCCESS**, zero errors. RAM 16.4% (53840/327680
    bytes), Flash 62.7% (985665/1572864 bytes) — same story, essentially
    identical delta to `ld2420`'s (none of this phase's fixes are
    radar-variant-dependent).
  - `pio run -t buildfs -e ld2420`: **SUCCESS**, zero errors. Packed the
    same 13 `data/` files as Phase 7 (`/sequences/greeting.json`,
    `/sequences/README.md`, 11 `/www/**` files) — no frontend files were
    modified this phase (the fix was entirely server-side: which FS
    subtree gets served at the URL root).
  - `pio run -t buildfs -e ld2450`: **SUCCESS**, zero errors, identical file
    list to `ld2420`'s.
  - Not run this phase (per the task's explicit instruction): flashing or
    connecting to the physical board. All verification above is
    compile-only, as directed — the two bugs this phase found (frontend
    unreachable, greeting mode silently degraded) are exactly the kind of
    thing that would otherwise have surfaced immediately at the first real
    hardware bring-up, which is precisely why this integration pass existed.

## Hardware bring-up fixes (post-Phase 8)

First real flash to a physical ESP32 (after `esptool erase_flash` +
flashing both firmware and filesystem images) surfaced two bugs that no
compile-only verification in Phases 0-8 could have caught. Both are fixed
below; neither could be re-verified on real hardware from this session (no
board access here — flashing/monitoring was done by the orchestrator) — see
each fix's "Verification" note for what *was* checked and the reasoning for
why it should hold.

### Bug 1 — LittleFS mount failure (`E (225) esp_littlefs: partition "spiffs" could not be found`)

- **Root cause:** `src/main.cpp`'s only `LittleFS.begin(...)` call was
  `LittleFS.begin(true)` — format-on-fail, but every other argument left at
  its default. arduino-esp32's `LittleFSFS::begin()` signature (confirmed
  directly against
  `framework-arduinoespressif32/libraries/LittleFS/src/LittleFS.h`) is
  `bool begin(bool formatOnFail=false, const char *basePath="/littlefs",
  uint8_t maxOpenFiles=10, const char *partitionLabel="spiffs")` — the
  default `partitionLabel` is the **literal string `"spiffs"`**, regardless
  of the target partition's SubType. `partitions.csv`'s data partition is
  named `littlefs` (`littlefs, data, spiffs, 0x310000, 0xF0000,` — see that
  file's own header comment: the *SubType* column had to be the
  tool-compatible `spiffs` keyword since Phase 0, but the partition's
  *Name* column, which is what `LittleFS.begin()` actually searches by, was
  always `littlefs`, matching `board_build.filesystem = littlefs` in
  `platformio.ini`). So the default lookup-by-name search never found a
  partition literally named `"spiffs"` and mounting failed every boot, even
  though the correct partition (right SubType, right size, right offset)
  was sitting right there under its real name. This is a pure
  label-mismatch bug — it doesn't corrupt or reformat anything, it just
  never finds the partition to mount in the first place, hence
  `format()`'s own attempt (triggered by `formatOnFail=true`) also fails
  for the same reason (can't format a partition it can't find either).
- **Fix applied:** `src/main.cpp`, `setup()` — changed the call to
  `LittleFS.begin(true, "/littlefs", 10, "littlefs")`, passing the real
  partition label explicitly instead of relying on the mismatched default.
  Comment added at the call site explaining the label mismatch.
- **Other call sites checked (per the task's request to verify this is the
  *only* spot depending on the default `"spiffs"` label):**
  - Grepped the whole `src/` tree for `LittleFS.begin`, `esp_littlefs`,
    `esp_partition_find`, and `spiffs`/`SPIFFS` (case-insensitive): the only
    hit is this one call site in `main.cpp`. No other file calls
    `esp_littlefs`/`esp_partition_find*` directly — the codebase uses the
    Arduino `LittleFS` wrapper exclusively, as PROGRESS.md's earlier phases
    describe.
  - The one other place a partition gets looked up by a
    `spiffs`/filesystem-related identifier is `src/api/ota_routes.cpp`'s
    `POST /api/ota/filesystem` handler, which calls
    `Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)` (2 args — `label` left at
    its default). Read `Update.h`/`Updater.cpp` directly
    (`framework-arduinoespressif32/libraries/Update/src/`): `begin()`'s
    `label` parameter defaults to `NULL`
    (`Update.h:55`), and for `U_SPIFFS` it resolves the target partition via
    `esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
    ESP_PARTITION_SUBTYPE_DATA_SPIFFS, label)` (`Updater.cpp:142`) — with
    `label == NULL` this matches by **type + SubType only**, ignoring the
    partition's Name entirely. Since `partitions.csv`'s `littlefs` partition
    *does* have SubType `spiffs`, this call already finds it correctly
    regardless of its Name — confirmed not to share Bug 1's mistake, no
    change needed there.
- **Verification:** all four required builds
  (`pio run -e ld2420`/`-e ld2450`, `pio run -t buildfs -e ld2420`/`-e
  ld2450`) succeeded with zero errors (see combined results at the end of
  this section). Not hardware-verifiable from this session; the fix is a
  direct, mechanical match to the documented `LittleFSFS::begin()` default
  argument and the confirmed partition Name in `partitions.csv`, so there's
  no ambiguity left to resolve on real hardware beyond confirming the mount
  succeeds (orchestrator to reflash and confirm).

### Bug 2 — `RTCWDT_RTC_RESET` boot loop shortly after entering AP setup mode

- **Observed symptom:** after `[WiFi] AP setup mode: SSID=...]` printed, a
  burst of 8 `[E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND`
  lines, then a hard reset (`rst:0x10 (RTCWDT_RTC_RESET), boot:0x13
  (SPI_FAST_FLASH_BOOT)`) and the whole sequence repeating — a boot loop.
- **First finding — the 8 `NOT_FOUND` lines are exactly accounted for and
  harmless**, confirming the task's own suspicion: traced `main.cpp`'s
  `setup()` call order (`WifiManager::begin()` → `OtaManager::begin()` →
  `Buttons::begin()` → `LedController::begin()` → `ServoHal::begin()` →
  `RadarTask::begin()` → `MotionTask::begin()` → `WebServer::begin()`) against
  `NvsStore`'s namespace-per-call pattern
  (`src/storage/nvs_store.cpp`). On a freshly-erased board, only the `wifi`
  and `system` namespaces exist (created with `Preferences::begin(name,
  /*readOnly=*/false)` inside `NvsStore::begin()`, which creates a
  namespace on write-mode open if missing). `LedController::begin()` →
  `NvsStore::getLedColorConfig()` opens the never-yet-created `led`
  namespace **read-only** (`ledPrefs.begin(kLedNamespace, true)` in
  `nvs_store.cpp`) — 1 `NOT_FOUND`. `ServoHal::begin()`'s loop over all 7
  `ServoId`s calls `NvsStore::getServoCalibration()` once per id, each
  hitting `loadServoCalTable()`'s `servoCalPrefs.begin(kServoCalNamespace,
  /*readOnly=*/true)` against the equally-never-yet-created `servocal`
  namespace — 7 more `NOT_FOUND`s. **1 + 7 = 8**, matching the log exactly,
  each one a fast, synchronous open-then-fail (`Preferences`/NVS
  `nvs_open()` fails immediately when a namespace doesn't exist and the
  caller asked for read-only, it does not hang), and every caller already
  falls back to in-struct default values correctly (per Phase 3/6's own
  design) — this part of the log is expected "nothing saved yet" behavior
  on a blank board, not a bug, exactly per the task's own framing. No
  change made for these lines.
- **Root cause of the actual hang/reset:** the ~6.3s silent gap is
  `WifiManager::begin()` → `runScanOnce()`'s single blocking
  `WiFi.scanNetworks(/*async=*/false, ...)` call (`src/net/wifi_manager.cpp`),
  called once at boot, before `startApMode()`, exactly as Phase 1's own
  PROGRESS.md notes already flagged as "a known blocking call that can take
  several seconds on real hardware." On a freshly-erased board this call is
  slower than normal for a second reason beyond the general "WiFi scans are
  slow" risk already documented: erasing the flash also wipes any cached
  WiFi/PHY radio calibration data, so this first-ever
  `WiFi.scanNetworks()`/`esp_wifi_init()` has to perform a full one-time RF
  calibration pass in addition to the scan itself, on top of whatever NVS
  first-init cost is also being paid at the same time — stacking exactly
  the way the task's investigation hints suggested.
  - Confirmed the reset reason is genuinely the **RTC/hardware watchdog**,
    not the software task watchdog: `rst:0x10 (RTCWDT_RTC_RESET)` is the
    *exact* string documented in
    `framework-arduinoespressif32/tools/sdk/esp32/include/esp_hw_support/include/soc/rtc_wdt.h`
    as what prints "if you use [stage action] `RTC_WDT_STAGE_ACTION_RESET_RTC`"
    — i.e. this is the RTC_CNTL watchdog peripheral's own reset path, a
    completely separate hardware mechanism from `esp_task_wdt`
    (confirmed by reading `cores/esp32/main.cpp`: the software task
    watchdog for the Arduino `loopTask` — `loopTaskWDTEnabled` — starts
    `false` and nothing in this codebase ever calls `enableLoopWDT()`, so
    it was never even armed; it isn't the one firing here).
  - Checked whether arduino-esp32 normally leaves this disabled by the time
    `setup()` runs: `framework-arduinoespressif32/tools/sdk/esp32/sdkconfig`
    has `CONFIG_BOOTLOADER_WDT_ENABLE=y`, `CONFIG_BOOTLOADER_WDT_TIME_MS=9000`,
    and — critically — `CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE` is
    **not set** (i.e. off), meaning ESP-IDF's own startup path is configured
    to disable the bootloader's RTC watchdog itself, automatically, before
    `app_main()`/`setup()` ever runs — the Arduino core layer
    (`cores/esp32/esp32-hal-misc.c`'s `initArduino()`, `cores/esp32/main.cpp`)
    never touches `rtc_wdt_*` at all, consistent with normally not needing
    to. That the board still resets with that exact reason during
    `setup()`'s WiFi scan means something later in the boot path (the
    prevailing, best-supported explanation here: the WiFi/PHY driver's own
    first-time calibration safety net, given this is specifically a
    freshly-erased-flash first boot with no cached calibration data) is
    re-arming/relying on the RTC watchdog again well after `app_main()`
    handed off — i.e. exactly the scenario the task's fix-candidate #1
    described ("something re-arms/relies on the RTC WDT... need to
    explicitly feed/disable it around a long blocking operation this early
    in boot").
  - No infinite loop, deadlock, or non-returning NVS/`Preferences` call was
    found anywhere in the traced sequence (checked `wifi_manager.cpp`,
    `nvs_store.cpp`, `servo_hal.cpp`, `ota_manager.cpp`, `buttons.cpp`,
    `led_controller.cpp` end-to-end) — every NVS call in this early path
    either succeeds fast or fails fast with `NOT_FOUND`, ruling out
    fix-candidate #2. The delay is entirely attributable to the one
    documented-as-slow blocking call, ruling in fix-candidate #1/#3's
    combination (blocking scan + fresh-flash first-time cost stacking).
- **Fix applied:**
  - `src/net/wifi_manager.cpp`, `runScanOnce()`: added `#include
    "soc/rtc_wdt.h"` and a `rtc_wdt_feed()` call immediately before and
    immediately after the blocking `WiFi.scanNetworks(...)` call — feeding
    resets the watchdog's countdown to 0, giving the long blocking call the
    freshest possible timeout window on the way in, and covering the
    NVS-heavy `LedController::begin()`/`ServoHal::begin()` bursts that run
    shortly after `WifiManager::begin()` returns on the way out. This is
    the officially-documented, application-level way to manage this
    peripheral (per `soc/rtc_wdt.h`'s own header-comment usage example:
    "2) Reset counter of rtc_wdt: `rtc_wdt_feed();`") — it's a safe no-op if
    the watchdog happens to already be disabled, and real protection if
    something has it armed.
  - `src/main.cpp`, top of `setup()` (right after `Serial.begin()`/the
    settle `delay(200)`): added `#include "soc/rtc_wdt.h"` and an explicit
    `rtc_wdt_disable()` call. Belt-and-suspenders: normally redundant given
    IDF's own default startup already disables this before `setup()` runs
    (see sdkconfig finding above), but free of cost if so, and closes the
    gap for exactly this codebase's situation if any later re-arming
    happens for a reason not fully visible from static code (there's no
    hardware access from this session to single-step the WiFi driver's
    internal calibration path and confirm the *exact* re-arm point).
  - Both call sites carry code comments pointing at each other and
    summarizing the root-cause chain, per the task's request.
  - **Why this should resolve the observed reboot loop:** the two changes
    together mean that by the time the ~6.3s blocking scan call is entered,
    the RTC watchdog's countdown is freshly reset (via the pre-call
    `rtc_wdt_feed()`) and, in the common case, outright disabled (via
    `main.cpp`'s `rtc_wdt_disable()` at the top of `setup()`) — either one
    independently removes the window in which that watchdog could fire
    mid-scan; having both is deliberate redundancy given this can't be
    single-stepped on real hardware from this session to prove which of
    the two is the one actually in effect. Neither change touches *why*
    the scan itself is slow (that's inherent to a fresh board's one-time
    RF calibration + the already-documented general slowness of a
    synchronous whole-band scan) — it only ensures the watchdog can't
    interpret that legitimate, already-anticipated slowness as a hang.
    This is deliberately the minimal, targeted fix: it doesn't change
    `WifiManager`'s scan-then-AP architecture (still fully valid per Phase
    1's own "avoid scanning while AP is up" research finding), doesn't
    suppress or change the harmless `NOT_FOUND` NVS messages, and doesn't
    touch the unrelated `esp_task_wdt`/loop-task software watchdog
    (confirmed never armed, not implicated).
  - **Not changed / considered and rejected:** switching
    `WiFi.scanNetworks()` to async mode (fix-candidate #1's alternative) —
    would also work and avoids the blocking window entirely, but is a
    larger behavioral change to `WifiManager`'s boot sequence (cached scan
    results are currently populated synchronously before `startApMode()`
    runs; going async would need a new "scan pending" state threaded
    through `begin()`/`handle()`) for a problem the watchdog-feed fix
    already addresses directly and minimally — left as a possible future
    improvement, not applied here to keep this a bugfix, not a redesign,
    per the task's own framing.
- **Verification:** all four required builds succeeded with zero errors
  (see below). Cannot be verified on real hardware from this session (no
  board access) — the reasoning above is a full manual trace of the reset
  reason string, the sdkconfig watchdog-disable setting, and the exact call
  sequence between the AP-setup log line and the reset, cross-referenced
  against the actual framework source (`soc/rtc_wdt.h`,
  `cores/esp32/main.cpp`, `cores/esp32/esp32-hal-misc.c`,
  `tools/sdk/esp32/sdkconfig`) rather than assumed from memory. Orchestrator
  to reflash and confirm the boot loop is gone on the real board.

### Bug 2, corrected — the WiFi-scan/watchdog theory above was wrong; real cause is `ServoHal::begin()` hanging on the Aux servo's `attach()`

Re-test on real hardware after Bug 2's fix above showed the identical
`RTCWDT_RTC_RESET` boot loop, unchanged — the `rtc_wdt_feed()`/
`rtc_wdt_disable()` fix did **not** resolve it, and its placement theory
(that the multi-second gap was inside `WiFi.scanNetworks()`) was never
actually confirmed against real timestamps, only inferred from the
already-documented "scans can be slow" warning plus where the boot log
happened to stop. A follow-up instrumentation pass added timestamped
`[BOOT] <label> t=<millis>` `Serial.printf()`+`Serial.flush()` logging
around every sub-step of every `setup()` call (`main.cpp`,
`wifi_manager.cpp`, `servo_hal.cpp`, `led_controller.cpp`) to nail the real
location down from real-hardware serial capture instead of guessing again.

- **Corrected finding:** `WiFi.scanNetworks()` takes ~3.1s — slow, but it
  **completes and returns**, exactly as designed; it was never the hang.
  Across 5 consecutive, identical real-hardware boot cycles, the log
  always stopped at the same line and never printed anything after it:
  ```
  [BOOT] servo[6/aux]: before attach(pin=13) t=3830
  ```
  i.e. `ServoHal::begin()`'s loop over all 7 `ServoId`s successfully
  attaches and writes an initial pulse to servos 0-5 (`pan`, `tilt`,
  `lidUpperL`, `lidLowerL`, `lidUpperR`, `lidLowerR` — GPIO 32/33/25/26/27/14)
  in roughly 5ms each, completely normally, then calls
  `Servo::attach(13, ...)` for servo index 6 (`aux`, GPIO13 — `pin_map.h`'s
  spare/"reserved for future use" channel, nothing physically wired to it)
  and that call never returns. No later `[BOOT]` line, no
  `after attach()`/`after writeMicroseconds` line, nothing — the board
  simply goes silent until the RTC watchdog fires and resets it, then
  repeats identically.
- **Investigating *why* `attach(13)` specifically hangs:** read the
  installed ESP32Servo (`madhephaestus/ESP32Servo @ 3.2.1`) source in full
  (`.pio/libdeps/ld2420/ESP32Servo/src/ESP32Servo.{h,cpp}`,
  `ESP32PWM.{h,cpp}`) to check the obvious hypothesis — LEDC channel/timer
  exhaustion on the 7th simultaneous channel. **That hypothesis does not
  hold**, confirmed by tracing the library's own static allocator
  (`ESP32PWM::allocatenext()`/`ChannelUsed[]`/`timerCount[]`) by hand for
  this exact boot sequence: on a plain (non-S2/S3/C-series) ESP32,
  `NUM_PWM` is 16 and each of the 4 "timer" buckets holds up to 4 channels
  (`timerCount[i] < 4`), so 16 channels are available in total. Walking the
  7 sequential `attach()` calls through `allocatenext()`'s deterministic
  channel-assignment logic (each `Servo` owns its own `ESP32PWM pwm{false}`
  member, so allocation is per-instance against the shared static
  bookkeeping) hands out LEDC channels 0, 1, 8, 9, 2, 3, then **10** for
  Aux — the allocator finds Aux a free channel/timer slot without ever
  reaching its own `while(1)` exhaustion-halt branch. So the hang is not a
  bug in ESP32Servo's channel bookkeeping; it happens one layer further
  down, inside the actual hardware-configuration call that `attach()`
  eventually makes.
  - Confirmed via `core_version.h`
    (`framework-arduinoespressif32` package version 3.20017.0 actually
    ships Arduino-ESP32 core **2.0.17**, an IDF4.4-generation core) that
    `ESP32PWM::attachPin()`/`setup()` take the library's pre-3.0.0 code
    path — plain `ledcSetup()`/`ledcAttachPin()`/`ledcDetachPin()` (not the
    newer `ledcAttachChannel()`/`ledcDetach()` API) — confirmed against
    `cores/esp32/esp32-hal-ledc.c`, which only defines the old-style
    functions in this framework version.
  - The channel ESP32Servo hands to Aux (LEDC channel 10) decodes, by this
    same `esp32-hal-ledc.c`'s own `group=(chan/8)`/`timer=((chan/2)%4)`
    formula, to **group 1 (the "low speed" LEDC group on classic ESP32),
    timer 1** — and tracing the (group, timer) pairs used by the first 6
    servos (channels 0,1,8,9,2,3 → (0,0), (0,0), (1,0), (1,0), (0,1),
    (0,1)) shows that specific **group-1/timer-1 combination is never
    exercised until Aux's attach**, even though group 1 and timer 1 are
    each individually already in use elsewhere by that point. This is as
    far as the evidence in this repo's vendored/installed library and
    framework source goes — the actual `ledc_timer_config()`/
    `ledc_channel_config()` calls this eventually reaches live inside
    ESP-IDF's own `driver/ledc.c`, which isn't vendored here and wasn't
    read this pass, so the precise IDF-level mechanism of the hang isn't
    fully proven, only narrowed down to: it's a real, reproducible,
    hardware/driver-level stall specific to attaching this one particular,
    never-before-configured LEDC group/timer pairing on this particular
    pin, not a resource-exhaustion or logic bug in ESP32Servo itself.
- **Fix applied — lazy-attach for the unused Aux channel (permanent design
  choice, not a workaround):** `src/hal/servo_hal.cpp`:
  - Added a `bool gAttached[kServoCount]` tracking array and a private
    `attachIfNeeded(ServoId id)` helper that calls `Servo::attach()` only
    if not already attached.
  - `ServoHal::begin()`'s loop still eagerly attaches and writes an initial
    center pulse to all 6 real eye-mechanism servos exactly as before
    (unchanged behavior/ordering for `Pan`/`Tilt`/`LidUpperL`/`LidLowerL`/
    `LidUpperR`/`LidLowerR`); for `ServoId::Aux` specifically it now loads
    and caches its calibration (so `getCalibration(Aux)` still works
    immediately) but `continue`s past the `attach()`/`writeMicroseconds()`
    calls, leaving `gAttached[Aux] = false`.
  - `ServoHal::setPulseUs()` now calls `attachIfNeeded(id)` (inside the
    existing mutex) before writing — a no-op for the 6 real servos
    (already attached), and what actually attaches Aux the first time
    anything asks it to move.
  - `ServoHal::reapplyCalibration()` (called from `POST
    /api/servos/config`) already unconditionally does
    `detach()`+`attach()` regardless of prior state — `Servo::detach()` is
    a safe no-op if not currently attached — so it correctly attaches Aux
    on its first calibration save too; added `gAttached[i] = true` there
    so a later `setPulseUs()` doesn't redundantly re-attach.
  - Net effect: `POST /api/servos/test`/`POST /api/servos/config` targeting
    Aux still work exactly as before (attach happens transparently on that
    first call, well after boot, with no watchdog window to race against);
    the only change is *when* Aux gets attached, never *whether*. The 6
    real servos' boot-time behavior (attach order, initial center pulse)
    is completely unchanged.
  - Doc comments updated in `src/hal/servo_hal.h` (`begin()`/`setPulseUs()`)
    and `src/hal/servo_hal.cpp` (by `gAttached`/`attachIfNeeded()`)
    explaining the lazy-attach rationale and pointing back here.
  - This is treated as a permanent fix, not a stopgap: Aux is genuinely
    unused hardware right now (`pin_map.h` already calls GPIO13 "Spare PWM
    channel, reserved/exposed as 'aux servo'"), so deferring its attach
    until something actually asks it to move is a legitimate design choice
    that happens to also route around the one channel that hangs at boot.
- **Bug 2's original (ineffective) fix reverted, since the real cause is
  now fixed at its source:**
  - `src/main.cpp`: removed the `rtc_wdt_disable()` call and its
    `#include "soc/rtc_wdt.h"` (nothing else in this file uses
    `rtc_wdt_*`).
  - `src/net/wifi_manager.cpp`: removed both `rtc_wdt_feed()` calls in
    `runScanOnce()` and its `#include "soc/rtc_wdt.h"` (nothing else in
    this file uses `rtc_wdt_*`); kept a short factual note on
    `WiFi.scanNetworks()`'s ~3.1s real-hardware timing and a pointer to
    this section, since that timing observation itself is still accurate
    and useful context, just no longer treated as the reset's cause.
- **Temporary diagnostic instrumentation removed** now that it did its
  job: the `[BOOT] <label> t=<millis>` `Serial.printf()`+`Serial.flush()`
  calls and each file's local `bootLog()` helper were removed from
  `src/main.cpp`, `src/net/wifi_manager.cpp`, `src/hal/servo_hal.cpp`, and
  `src/hal/led_controller.cpp`. All of these files' normal,
  non-`[BOOT]`-prefixed log lines (`[FS] LittleFS mounted.`, `[WiFi]
  ...`, etc.) were left exactly as they were — only the temporary
  instrumentation was stripped.
- **Verification:** all four required builds succeeded with zero errors
  (see updated build verification below). Not hardware-verifiable from
  this session (no board access here) — the evidence above is a direct
  real-hardware serial capture (5 identical repeated boot cycles pinpointing
  the exact hanging line) plus a full manual trace of ESP32Servo's/
  ESP32PWM's actual channel-allocation source and the installed
  arduino-esp32 core version, not a guess. Orchestrator to reflash and
  confirm the boot loop is gone and that `POST /api/servos/test`/`POST
  /api/servos/config` still work correctly for the Aux channel on the real
  board.

### Bug 3 — radar UART RX/TX swapped versus this PCBA's actual wiring

> **Superseded (2026-09-22):** the real root cause was that the radar line
> reached the module's OT1 presence pad, not its OT2 UART output. The fix is
> a bodge wire from OT2 to GPIO22, with `RADAR_RX_PIN = 22` /
> `RADAR_TX_PIN = 17`. See `include/pin_map.h` and the hardware erratum in
> `architecture/ARCHITECTURE.md` §1. The 17/16 swap described below is no
> longer in the code.

- **Confirmed physical wiring:** the board booted cleanly after Bug 1/Bug 2's
  fixes (first clean boot on real hardware), and the user then confirmed
  this specific PCB assembly's radar header wires the HLK-LD2420 module's
  **RX pin straight to ESP32 GPIO16** and its **TX pin straight to ESP32
  GPIO17** — RX-labeled-to-RX-labeled, TX-labeled-to-TX-labeled, i.e. a
  **non-crossed** connection. UART requires each side's TX to reach the
  other side's **RX** — so a straight-through
  wiring means whichever GPIO the firmware treats as "RX" is actually
  sitting on the wire fed by the radar's RX pin (which never drives
  anything), not its TX. This wiring is hardwired into the PCBA (traces on
  the board) and can't be changed physically, so the fix has to live in
  software instead.
- **Root cause:** `include/pin_map.h` had `RADAR_RX_PIN = 16` /
  `RADAR_TX_PIN = 17`, i.e. it assumed the originally-expected *crossed*
  wiring (ESP32 GPIO16 fed by the radar's TX, ESP32 GPIO17 feeding the
  radar's RX) — reasonable at design time, but not what this PCBA actually
  has. Since ESP32's UART peripherals aren't tied to fixed physical pins
  (any GPIO can be routed to a UART's RX or TX line via the chip's GPIO
  matrix — `HardwareSerial`/the vendored `LD2420GeoGab` library both accept
  arbitrary RX/TX GPIOs), the correct fix is purely a matter of which GPIO
  each pin-role constant points at, not a wiring or library change.
- **Fix applied:** `include/pin_map.h` — swapped the two constants' values
  so `RADAR_RX_PIN = 17` (GPIO17, physically fed by the radar's TX on this
  PCBA) and `RADAR_TX_PIN = 16` (GPIO16, physically feeding the radar's RX
  on this PCBA). Expanded the doc comment above both constants to explain
  the straight-through wiring, why the role/GPIO pairing looks
  "backwards" at a glance, and that it's a deliberate compensation for this
  PCBA's fixed hardware wiring, not a mistake.
- **Consuming call sites checked (both already used the named constants,
  not hardcoded pin numbers — no code changes needed beyond `pin_map.h`):**
  - `src/radar/ld2420_sensor.cpp`, `Ld2420Sensor::begin()`: `radar_.begin(
    RADAR_TX_PIN, RADAR_RX_PIN)` (`LD2420GeoGab::begin(txPin, rxPin,
    baud)`).
  - `src/radar/ld2450_sensor.cpp`, `Ld2450Sensor::begin()`:
    `Serial2.begin(kLd2450BaudRate, SERIAL_8N1, RADAR_RX_PIN,
    RADAR_TX_PIN)`.
  - `src/radar/radar_task.h`'s file-header comment also just references the
    two named constants, not literal pin numbers — no change needed.
  - Grepped `src/radar/` for literal `16`/`17` as a final check: zero
    matches outside the constants' own definition in `pin_map.h` — so this
    fix is a true single-source-of-truth swap, not something that needed
    parallel edits scattered across both sensor variants.
- **Verification:** both `pio run -e ld2420` and `pio run -e ld2450`
  succeeded with zero errors (see updated build verification below),
  with **identical** RAM/flash usage to the pre-fix build (a plain
  `constexpr` value swap changes no code size). Not hardware-verifiable
  from this session (no board access here) — this fix is a direct,
  mechanical match to the physical wiring the user confirmed by hand on
  the real PCBA, so there's no ambiguity left to resolve beyond
  orchestrator reflash-and-confirm that the radar now links
  (`GET /api/radar/status`'s `linkOk` / actual presence detection).

### Bug 4 — two eyelid servos move backwards; default `inverted` now true for them

- **Symptom / cause:** on the real board, `ServoId::LidUpperL` (index 2) and
  `ServoId::LidLowerR` (index 5) moved opposite to the expected direction
  because of how the eye mechanism is mounted. The user worked around it at
  runtime via `POST /api/servos/config` with `inverted=true` for those two,
  which persists in the NVS `servocal` blob.
- **Fix applied:** made that the firmware default so a fresh/erased board
  behaves correctly out of the box. `src/storage/nvs_store.{h,cpp}`: new
  `NvsStore::getDefaultServoCalibration(ServoId)` returns
  `ServoCalibration`'s unchanged 1000/1500/2000 defaults with
  `inverted = (id == LidUpperL || id == LidLowerR)`; `loadServoCalTable()` now
  seeds each table entry from it before reading the blob, so both the "nothing
  saved" and "blob size mismatch" paths get the per-servo defaults.
  `src/api/servo_routes.cpp`: a `POST /api/servos/config` body that omits
  `inverted` now falls back to that servo's default instead of blanket `false`
  (the calibration page always sends it, so its behavior is unchanged).
- **Unchanged:** `ServoCalibration` layout and blob format, pulse ranges,
  storage behavior. No migration: boards with an already-saved blob (including
  the user's, which already has these two set true) keep their saved values.
  `servo_hal.cpp`, `motion_task.cpp` and `data/www/setup/calibration.html`
  were checked and hardcode no non-inverted assumption (they only read
  `cal.inverted`); no `data/` change, so no `buildfs` needed.
- **Verification:** `pio run -e ld2420`: SUCCESS, zero errors (RAM 17.0%,
  55740 bytes; Flash 63.1%, 992113 bytes). `pio run -e ld2450`: SUCCESS, zero
  errors (RAM 16.4%, 53880 bytes; Flash 62.8%, 988525 bytes). Not flashed /
  not tested on hardware.

### Build verification (current state: Bug 1 fix + Bug 2 corrected fix + Bug 3 radar RX/TX pin-role swap, instrumentation and ineffective workaround removed)

- `pio run -e ld2420`: **SUCCESS**, zero errors. RAM 17.0% (55700/327680
  bytes), Flash 62.9% (989321/1572864 bytes) — unchanged from the
  pre-Bug-3 build (constant-value swap only, no code-size impact).
- `pio run -e ld2450`: **SUCCESS**, zero errors. RAM 16.4% (53848/327680
  bytes), Flash 62.7% (985749/1572864 bytes) — likewise unchanged.
- `pio run -t buildfs -e ld2420` / `-e ld2450`: not re-run for Bug 3 (this
  fix touches no `data/` file — see Bug 1/Bug 2 entries above for the last
  confirmed-clean `buildfs` results, still valid since nothing under
  `data/` changed since).
- Not run from this session (no physical board access here — the
  orchestrator is flashing/monitoring the real board directly): actually
  booting the fixed image and confirming the radar links/reports presence
  via `GET /api/radar/status` and `GET /api/radar/latest`. The root-cause
  fix is reasoned through in detail above specifically because this step
  couldn't be done here.

## Calibration rework (2026-09-23)

- **Reference points:** `ServoCalibration` gained `closedUs`/`openUs` for the
  four lids. Normalized lid 0.0 == `closedUs` (upper and lower lid just
  touching, eyes straight ahead), 1.0 == `openUs`; the pair encodes the
  direction, so `inverted` no longer applies to lids. Pan/tilt `centerUs`
  is the "looking straight ahead" point. `minUs`/`maxUs` are hard safety
  limits only. Blink/wink/sleep/greeting now close to 0.0 (was 0.05/0.12).
- **NVS migration:** an old 8-byte-per-entry table is migrated on read
  (closed/open derived from min/max + old `inverted`), rewritten in the new
  layout on the next save. Verified on hardware.
- **Why the old Test button barely worked:** MotionTask rewrote all 6 axes
  every 20ms, and servos were `attach()`ed with the saved min/max (which
  ESP32Servo clamps to). Fixed with a calibration hold
  (`MotionTask::setCalibrationHold()`, auto-expires after 30s, refreshed by
  the page) and attaching every servo with the absolute 500-2500us range,
  clamping to the calibrated range in `ServoHal::setPulseUs()` instead.
  `ServoHal::setRawPulseUs()` is the unclamped calibration write.
- **New API:** `GET/POST /api/servos/hold`, `POST /api/servos/pose`;
  `/api/servos/config` returns `kind`, `closedUs`, `openUs`.
- **Calibration page:** guided steps (straight ahead -> lids closed -> lids
  open -> advanced limits) with sliders plus -10/-1/+1/+10 nudges that move
  the servo live, and reference-pose buttons.
- **Gesture tuning (2026-09-23):** `roll_eyes` was 4 eased corner points
  (stopped at each corner -> diamond); now ease to the left edge, a full
  circle of 16 linear 100ms segments (pan +/-20deg, tilt +/-16deg, ~1.6s),
  then ease back to center. `look_around_quick` (label now "Look around")
  slowed from 120-150ms linear saccades to eased 360-500ms glances with
  400ms holds on each side.
- **Gesture review (2026-09-23):** `sleepy`/`squint` were single keyframes,
  so once they finished the lid axes went "free" and NaturalModeCoupler
  (on by default) pulled the lids back to 0.85 within ~200ms — the "held"
  look never showed. Held looks now use hold keyframes (re-target to the
  same values, which keeps axis ownership) plus an explicit relax
  keyframe. Verified on hardware. Also: blink 100/40/160ms, winks hold
  300ms closed while the other eye narrows slightly, surprise holds wide
  700ms with a 4deg upward glance, sleepy droops (mainly upper lid) then
  nods off further before reopening, squint raises lower lids more than
  upper and relaxes after 1.5s, double_blink moves >= 100ms (70ms was
  shorter than an SG90 needs to reach closed). All durations are
  multiples of the 20ms tick.
- **Half-open lid point (2026-09-23):** natural-mode lids sat at different
  heights left vs right although closed/open were calibrated — the linear
  closed->open pulse mapping doesn't match non-linear, per-eye-different
  lid linkages (measured spans: upper L 944us vs upper R 576us). Added
  `ServoCalibration::halfUs` (normalized 0.5); lid mapping is now
  piecewise linear closed->half->open (firmware and calibration page
  share the formula). NVS V2 tables migrate with halfUs = midpoint
  (identical behavior until calibrated). API validates half strictly
  between closed and open. Calibration page: new "4. Half open" step and a
  "Compare both eyes" openness slider that sweeps all four lids together.
- **Stability fixes (2026-09-23)**, found while chasing "lids jump to ~85%
  open during the calibration compare slider":
  - `/api/system/info` now reports `resetReason` (esp_reset_reason():
    brownout / panic / task_watchdog / ...), so resets can be diagnosed
    without a serial cable. A reset looks exactly like that symptom: the
    calibration hold is RAM-only, so after reboot MotionTask drives the
    lids to rest (0.85) until the next slider request re-holds them.
  - WiFi: after a failed STA connect (15s) with *saved* credentials the
    device fell back to setup AP mode and never retried, so any restart
    during a slow router reconnect left it off the network (not even
    pingable) until power-cycled. It now retries the saved network every
    60s from fallback AP mode (skipped while a client is on the setup AP).
  - Web server: `serveStatic("/")` was registered before the API routes,
    so every GET /api/* first probed LittleFS for ~4 file variants (vfs_api
    "does not exist" log spam). Now registered last and filtered off
    /api/: GET /api/eyes/pose latency 136ms -> 46ms.
  - Not reproduced after these changes: 4x 0-50% sequential sweeps,
    12s of browser slider input, 180 overlapping pose requests — no
    resets, heap stable (~190KB).
  - Calibration-hold race fixed: when a hold expired, MotionTask cleared
    `gHoldUntilMs` after its holdActive() check, which could wipe a hold
    an HTTP request re-set in between and ease the lids to rest
    mid-calibration. The clear was unnecessary (an expired timestamp is
    inert) and is gone. MotionTask now logs "[Motion] Calibration hold
    started" / "ended (released|expired)" on serial.

## PR #1 review fixes (2026-09-23)

Fixes for the valid findings from a /code-review pass and CodeRabbit's
review of PR #1. Both build envs (ld2420, ld2450) compile; not yet
re-verified on hardware.

- LD2450 sign decoding was inverted: the high bit set means *positive*
  (manual's worked example: Y 0x86B1 -> +1713 mm, X 0x030E -> -782 mm;
  matches ESPHome). Every forward target decoded with negative Y, so
  tracking always slammed pan to +/-45. Speed is now converted cm/s -> mm/s.
- Motion deadlines are wrap-safe (`deadlineReached()` in eye_pose.h):
  play-mode/gesture timers used absolute `>=`/`<` compares, which break
  when millis() wraps at ~49.7 days of uptime.
- Superseded greeting (manual command / API gesture / calibration exit
  mid-sequence) now settles into idle instead of sitting in "greeting"
  forever doing nothing. A `gGreetingRunning` flag, cleared in activate()
  before its generation bump, keeps a re-activation from misfiring it.
- Manual eyelids are sticky: natural-mode coupling no longer reclaims a lid
  owned by a Manual command (it used to snap back ~150ms after every
  /api/eyes/eyelids call). Natural coupling resumes once a gesture,
  play-mode command or the post-calibration rest pose (now tagged Natural)
  retargets the lid. Axes also boot as Natural-owned.
- Aborted gestures reopen the lids: EyeCommand/AxisState carry a
  commandGeneration; on abort (or replacement) GestureEngine pushes the
  gesture's final (relax) keyframe, lids only, as `restoreOnly`, which
  MotionTask applies only to lids still owned by the aborted playback's
  generation. A blink interrupted by a gaze command reopens; one
  interrupted by an eyelid command or sleep mode does not. The generation
  counter now starts at 1 (0 = "stamp at push time").
- Web OTA aborts a stale, never-finalized Update (client disconnected
  mid-upload) before Update.begin(); previously every later upload failed
  until reboot.
- WiFi connect credentials are held in RAM and saved to NVS only on
  WL_CONNECTED; a mistyped password no longer destroys the last working
  credentials (the fallback retry reconnects to the old network).
- NVS: legacy calibration migration checks getBytes()'s length; a failed
  calibration save returns false and POST /api/servos/config answers 500
  `nvs_write_failed` instead of reporting success.
- Not changed: CodeRabbit's native-struct NVS encoding comment (same
  firmware/toolchain writes and reads the blobs; size-based layout
  detection is already static_assert-guarded). OTA/API authentication
  (web OTA routes, ArduinoOTA password) is deferred to a follow-up.

## Hardware verification of review fixes + WiFi hardening (2026-09-23)

Verified on the LD2420 device via web OTA: sticky manual eyelids,
gesture abort-restore, eyelid command beating a restore, superseded
greeting settling into idle, calibration-exit rest pose with natural
coupling resuming, and web OTA recovering from an interrupted upload.
Not hardware-testable here: LD2450 sign decoding, millis() wrap, NVS write
failure.

- **Gesture abort now immediate:** GestureEngine::tick() checked the
  command generation only when the current keyframe's deadline arrived, so
  an interrupted gesture held the lids for up to the rest of that keyframe
  (sleepy's 1.2s hold) before the abort-restore reopened them. The check
  now runs every tick; lids start reopening within one tick.
- **WiFi: wrong password could be saved as "working"** (found by testing
  POST /api/wifi/connect with a bad password on a connected device — twice
  locked the device out). Right after WiFi.begin() on a connected device,
  status() still reports the old link, and a momentary "connected" was also
  observed before the router rejected the handshake. A connect attempt now
  only succeeds when, continuously for 3s, the attempt got an IP
  (ARDUINO_EVENT_WIFI_STA_GOT_IP, reset per attempt), status() is
  WL_CONNECTED and esp_wifi_sta_get_ap_info() confirms association. The old
  link is dropped explicitly before a new attempt.
- **WiFi: lost-link fallback.** In STA_CONNECTED, 30s without a link (the
  driver's auto-reconnect keeps trying meanwhile) now falls back to the
  setup AP; from there the saved network is retried every 60s by taking
  the AP down and connecting like boot does (plain WIFI_STA), when no
  client is on the setup AP.
- `WiFi.persistent(false)`: credentials live only in our NVS namespace,
  not also in the WiFi driver's own flash config.
- Serial logging of STA associate / got-IP / disconnect-reason events.
- Verified: wrong password via the API -> device off-network, fell back,
  retried and was back on the saved network after 83s; a reboot afterwards
  reconnected with the (untouched) saved credentials.
- **Serial console** (src/net/serial_console.*, 115200 baud): `help`,
  `status`, `wifi [status]`, `wifi set <ssid> [password]` (quotes for
  spaces; saves immediately — recovery path with physical access),
  `wifi forget`, `reboot`. Verified on hardware (`wifi set` recovered the
  device after the second lock-out).
