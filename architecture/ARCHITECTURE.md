# ESP Magic Eyes — Architecture

This document describes the firmware and web-app architecture for the ESP Magic Eyes animatronic eye controller. It is derived from the original design plan used to build the project, updated to reflect what was actually implemented where it deviated from that plan (each such change is noted inline). For day-to-day usage see [README.md](../README.md) in the project root.

## Context

The project drives the nmrobots.com ε-SERIES animatronic eye mechanism: 6× SG90 servos (eye pan, eye tilt, and independent upper/lower eyelids per eye, with one spare PWM channel), an ESP32, a 24GHz mmWave radar (HLK-LD2420, with the HLK-LD2450 supported as a drop-in upgrade) for human-presence sensing, and an optional WS2812-style RGB LED per eye for glow effects.

The firmware is a complete, self-contained product: no hardcoded WiFi credentials (captive-portal setup), both web-based and network (IDE) OTA, a REST+WebSocket-capable API usable from a phone/PC/AI integration, a browser frontend served from onboard flash (not baked into firmware strings), servo calibration and radar visualization tooling, natural-looking coordinated eyelid/eyeball motion, gestures, and multiple interactive "play modes" — all on a non-blocking, interruptible real-time architecture so a new command can always smoothly take over from whatever the eyes are currently doing.

---

## 1. Hardware & Pin Map

| Function | GPIO | Notes |
|---|---|---|
| Servo 1 — Eye Pan (shared X for both eyes, per ε-SERIES design) | 32 | LEDC via ESP32Servo |
| Servo 2 — Eye Tilt (shared Y) | 33 | LEDC via ESP32Servo |
| Servo 3 — Left Upper Eyelid | 25 | LEDC via ESP32Servo |
| Servo 4 — Left Lower Eyelid | 26 | LEDC via ESP32Servo |
| Servo 5 — Right Upper Eyelid | 27 | LEDC via ESP32Servo |
| Servo 6 — Right Lower Eyelid | 14 | LEDC via ESP32Servo |
| Spare servo PWM (unused initially) | 13 | Reserved, exposed in API/config as "aux servo" |
| Radar UART2 RX (ESP32 receives radar serial out — LD2420 pad **OT2**) | 22 | See hardware erratum below |
| Radar UART2 TX (ESP32 sends radar RX, if needed for config) | 17 | UART2 default pin |
| RGB LED data (WS2812, single chain, 2 pixels: index 0 = left eye, index 1 = right eye) | 4 | Adafruit_NeoPixel; not wired in the initial build — code is present and safe with nothing connected, feature stays off (`ledEnabled=false`) until enabled in config |
| WiFi/factory-reset trigger | 0 (BOOT button) | Hold 5s during normal run → clear NVS WiFi creds + reboot into AP setup mode. Standard dev-board button, no extra wiring. |

Board: generic ESP32 WROOM-32 dev board (non-PSRAM), 4MB flash.

**Hardware erratum — radar RX on GPIO22.** The original design put radar RX on GPIO16, but the first PCBA wired that line to the HLK-LD2420's **OT1** pad, which is only a high/low presence output. The module's actual UART output is on **OT2** (5-pin header: VCC, GND, OT1, RX, OT2). On that board a bodge wire runs from OT2 to GPIO22, and `include/pin_map.h` sets `RADAR_RX_PIN = 22`. A board built without the bodge (or a fixed PCB revision routing OT2 to GPIO16) needs `RADAR_RX_PIN` changed to match. `include/pin_map.h` is the source of truth; PROGRESS.md's "Bug 3" (RX/TX swapped to 17/16) was an earlier, superseded diagnosis.

Open item for physical bring-up: confirm whether the ε-SERIES mechanism is "shared yoke" (one pan + one tilt servo move a shared eyeball carrier for both eyes, matching the pin table above) vs. fully independent per-eye pan/tilt. The motion-engine abstraction treats this as a config-level fact, not a code fork, so it's simple to adjust if the physical assembly differs.

---

## 2. Architecture Overview

Layered design:

```
┌──────────────────────────────────────────────────────────────┐
│ Web Frontend (LittleFS static files: HTML/CSS/JS, no build)  │
│   /setup/*   (WiFi, calibration, radar test, OTA, LED cfg)   │
│   /control/* (manual control, gestures, play modes, status)  │
└───────────────────────▲────────────────────────────────────--┘
                         │ REST (JSON) — polled, see note below
┌───────────────────────┴────────────────────────────────────--┐
│ API Layer (ESPAsyncWebServer routes, src/api/*)                │
│  - validates input, translates to Command objects              │
└───────────────────────▲────────────────────────────────────--┘
                         │ FreeRTOS queue (CommandQueue)
┌───────────────────────┴────────────────────────────────────--┐
│ Control Logic (Core 1, MotionTask)                              │
│  - MotionTask (~50Hz): per-axis eased interpolation → servos   │
│  - NaturalModeCoupler: derives eyelid targets from gaze target │
│  - GestureEngine: keyframe playback (blink/wink/surprise/...)  │
│  - PlayModeManager: idle/curious/sleep/greeting/tracking/...   │
└───────────────────────▲────────────────────────────────────--┘
                         │ RadarState (mutex-guarded)
┌───────────────────────┴────────────────────────────────────--┐
│ Sensor & IO                                                     │
│  - RadarTask (Core 0): UART parse (LD2420/LD2450/none, NVS cfg) │
│  - ServoHal (ESP32Servo wrapper), LedController (NeoPixel)      │
│  - WifiManager (STA/AP + captive portal + NVS creds)            │
│  - OtaManager (ArduinoOTA) + web multipart upload (ota_routes)  │
└──────────────────────────────────────────────────────────────┘
```

**Note on realtime telemetry**: the original design allowed for a WebSocket telemetry channel (`/ws`) alongside REST. No WebSocket infrastructure was implemented — no phase of the build needed it, and the frontend's `js/api.js` `pollEvery()` helper covers live status/pose/radar updates via REST polling at page-appropriate intervals (fast for the radar visualization page, slower elsewhere). The API layer is written so a WS channel could be added later without restructuring routes.

### Concurrency & interruptibility model

- **CommandQueue**: a small FreeRTOS queue (depth 8) of `EyeCommand` structs — a *partial* target (`std::optional<float>` per axis: pan, tilt, 4 eyelids) plus `durationMs`, an `Easing` (`Linear`/`EaseInOut`), and a `CommandSource` (`Manual`/`Calibration`/`Gesture`/`PlayMode`/`Natural`). Both the API layer (HTTP handlers, running in AsyncTCP task context) and `GestureEngine`/`PlayModeManager` push into it — never write servo state directly. On overflow, the single oldest entry is dropped so the newest command always gets in.
- **MotionTask** (dedicated FreeRTOS task, **Core 1, priority 3, 4096-byte stack, ~50Hz via `vTaskDelayUntil`** — chosen so timing doesn't drift from the tick body's own execution time, and so the task sits above Arduino's own `loop()` task (priority 1) without approaching WiFi/BT system task priorities) owns per-axis interpolation state (`AxisState{startValue, targetValue, startTimeMs, durationMs, easing, source}`). Each tick it drains the queue, and **any new command immediately re-targets the axes it touches from their current interpolated value** (never from the old target) — this is the interruption mechanism: a manual API call always wins over a gesture/sequence in progress.
- **`commandGeneration`**: a monotonically increasing counter bumped whenever an externally-sourced command sequence starts (a manual `/api/eyes/*` call, a gesture trigger, or a play-mode activation). `GestureEngine` and `PlayModeManager`'s keyframe-playback coroutines capture the generation when they start a sequence and check it before enqueueing each subsequent keyframe — if it's moved on, they abort cleanly instead of fighting for control. This is how switching play modes (or issuing a manual command) cleanly cancels whatever was running.
- **NaturalModeCoupler** does **not** run as a separate task or go through `CommandQueue` (a queued command competing every tick would cause visible jitter). It runs as a plain function called directly inside `MotionTask`'s tick: for each lid axis, if that axis is currently "free" (its `source` tag is `Natural`, or its last command's interpolation has finished) and natural mode is enabled, it computes a lid-openness bias from the current gaze and retargets that axis directly with `source=Natural` and a short (~150-250ms) smoothing duration — only when the computed bias has changed meaningfully (>0.02) since the last update, to avoid needless micro-retargeting. An explicit lid command or gesture "owns" the axis until its own interpolation completes, then ownership implicitly reverts.
- **GestureEngine** and **PlayModeManager** are lightweight state machines ticked from inside `MotionTask`'s tick (not separate FreeRTOS tasks), since they only ever produce `EyeCommand`s.
- **RadarTask** (Core 0, priority 1, ~30ms poll loop — no need for `vTaskDelayUntil` precision here, this isn't a real-time control loop) owns UART2 read + parse + a mutex-guarded `RadarState`. `PlayModeManager`'s `tracking` mode reads `RadarState` (never touches UART directly).
- `AsyncWebServer` / `AsyncTCP` / `DNSServer` / `ArduinoOTA` are event-driven or handled from `loop()` — `loop()` itself is just `WifiManager::handle()`, `Buttons::handle()`, `OtaManager::handle()`, `OtaRoutes::handle()`, `WifiRoutes::handle()`, and `LedController::handle()` — none of it blocking.
- Every place shared state crosses a task boundary (`MotionTask`'s pose snapshot, `RadarTask`'s `RadarState`, `GestureEngine`'s playback state, `PlayModeManager`'s sequence buffer, `ServoHal`'s calibration reapply) is protected by a short-held FreeRTOS mutex — this was tightened up during the Phase 8 integration review, which found and fixed three real cross-task races.

---

## 3. Persistent Storage

**NVS (`Preferences.h`)** — small, survives app-only OTA (unlike LittleFS content unless the FS partition is also reflashed):
- `wifi` namespace: `ssid`, `password`, `hostname` (default `esp-magic-eyes`)
- `system` namespace: `devName` (default `Magic Eyes`), `naturalMode` (bool, default `true`), `ledEnabled` (bool, default `false`), `otaNetEn` (bool, default `true`), `radarBaud` (default 115200, LD2420 only), `radarType` (`RadarType` 0 none / 1 LD2420 / 2 LD2450, default LD2420 — stored values must never be renumbered)
- `servocal` namespace: one packed blob (`calTable`) holding all 7 servos' `ServoCalibration` — one read/write touches all 7 at once. Older blob layouts (V1 without closed/open, V2 without half) are recognized by size and migrated on read, then rewritten on the next save. Defaults: `minUs=1000, centerUs=1500, maxUs=2000` (a conservative SG90-safe range for *unconfigured* hardware); `LidUpperL`/`LidLowerR` default to closed=max/open=min because the mechanism mounts them mirrored.
- All `NvsStore` functions hold one mutex: they are called from `loop()`, the AsyncTCP task and `setup()`.

**Calibration model** (`ServoCalibration`, `src/storage/nvs_store.h`):
- `minUs`/`maxUs` are hard safety limits: every motion-engine write is clamped to them (`ServoHal::setPulseUs()`).
- Pan/tilt: `centerUs` is "looking straight ahead"; ±45° maps piecewise-linearly onto `minUs`..`centerUs`..`maxUs`; `inverted` flips direction.
- Lids: three measured points — normalized 0.0 = `closedUs` (upper and lower lid just touching), 0.5 = `halfUs`, 1.0 = `openUs` — mapped piecewise linearly, because the lid linkages are non-linear and differ per eye. The points encode direction, so `inverted` does not apply to lids. `halfUs` must lie strictly between closed and open.
- Every servo is `attach()`ed with ESP32Servo's full 500–2500 µs range, so the calibration page can probe past the saved limits via `ServoHal::setRawPulseUs()` (clamped only to 500–2500).
- `led` namespace: one packed blob holding `brightness`/`colorL`/`colorR`/`effect`.
- `auth` namespace: one blob (`hashes`) holding `adminHa1`, `userHa1` (HTTP Digest HA1, `MD5("<user>:ESP Magic Eyes:<password>")`) and `otaMd5` (`MD5(<admin password>)` for ArduinoOTA), each 32 hex characters or empty (= that level has no password). A single blob, so all three change together in one atomic NVS write. No blob means no passwords. If NVS can't be read, or the blob has the wrong size or content, the device fails closed (see §4a). Only hashes are stored. The realm and the usernames `admin`/`user` are baked into the hashes, so changing them would invalidate every stored password.

**LittleFS** — frontend + data files, mounted from the custom partition (see §8), served at URL root from the `/www/` directory within the filesystem image:
- `/www/...` — the web frontend (`index.html`, `css/`, `js/`, `control/`, `setup/`)
- `/sequences/greeting.json` — the `greeting` play mode's scripted keyframe sequence (data-driven, not hardcoded — see §5)

---

## 4. REST API

Base path `/api`. Every route needs the **Control** or **Admin** password once that level has one (see §4a). All bodies JSON (ArduinoJson v7 `JsonDocument` — v7 removed `StaticJsonDocument`/`DynamicJsonDocument` in favor of a single heap-backed `JsonDocument`, which is used consistently across every route file). Response helpers are centralized in `src/api/json_helpers.h/.cpp` (added during the Phase 8 integration pass to de-duplicate what had been 7 near-identical implementations).

**System**
- `GET /api/system/info` — firmware version, build date, chip id, uptime, free heap, radar model compiled in, `otaNetworkEnabled`, `ledEnabled`
- `GET /api/system/status` — current play mode, WiFi mode/IP, `naturalMode`, current pose snapshot
- `POST /api/system/config` `{otaNetworkEnabled?, naturalMode?}` — partial update (admin; the manual page uses `/api/eyes/natural` for the natural-mode toggle)
- `POST /api/system/reboot`

**WiFi / Setup**
- `GET /api/wifi/scan` — cached scan results (scanned only before the AP comes up — at boot, or asynchronously when falling back to the AP — to avoid the documented `WiFi.scanNetworks()` vs. active-AP conflict)
- `POST /api/wifi/connect` `{ssid, password}` — attempts an STA connect and saves the credentials to NVS only once the link has held for 3 s (a mistyped password never replaces working credentials); the frontend polls `/api/system/status`'s `wifiMode` to observe the outcome (connecting is not instant)
- `POST /api/wifi/forget` — clears NVS creds, reboots to AP setup mode (deferred restart so the HTTP response flushes first)

**Servo calibration**
- `GET /api/servos/config` — current calibration table (all 7 servos, with `kind`: gaze / lid / aux)
- `POST /api/servos/config` — gaze/aux `{servoId, minUs, centerUs, maxUs, inverted?}` (`minUs < centerUs < maxUs`); lids `{servoId, minUs, maxUs, closedUs, openUs, halfUs?}` (closed/open within min/max, half strictly between). Absolute bound 400–2600 µs, applied live without a reboot.
- `POST /api/servos/test` `{servoId, pulseUs}` and `POST /api/servos/pose` `{pulses: [{servoId, pulseUs}, ...]}` — raw, uneased pulses for the calibration page; both (re)start the calibration hold.
- `GET/POST /api/servos/hold` `{enabled}` — the calibration hold: `MotionTask` writes nothing for 30 s (refreshed by the page) so raw pulses stay put. On release/expiry the axes re-sync to the servos' real positions and ease back to the rest pose.

**Manual eye control**
- `POST /api/eyes/gaze` `{pan, tilt, durationMs, easing}` — degrees, ±45° convention (see §6's degrees-to-pulse mapping), `source=Manual`
- `POST /api/eyes/eyelids` `{upperL?, lowerL?, upperR?, lowerR?, durationMs}` — normalized 0 (closed) .. 1 (open), each field optional/partial
- `POST /api/eyes/natural` `{enabled}` — natural-mode toggle, control level
- `GET /api/eyes/pose` — current pose snapshot

**Gestures**
- `GET /api/gestures` — `blink`, `wink_left`, `wink_right`, `surprise`, `sleepy`, `squint` (alias `suspicious`), `look_around_quick`, `double_blink`, `roll_eyes`
- `POST /api/gestures/{id}/trigger`

**Play modes**
- `GET /api/playmodes` — `idle`, `curious`, `sleep`, `greeting`, `tracking`, `manual`
- `POST /api/playmodes/{id}/activate` — bumps `commandGeneration`, cleanly cancels whatever the previous mode was doing
- `GET /api/playmodes/active`

**Radar**
- `GET /api/radar/status` — configured sensor model (`LD2420`/`LD2450`/`NONE`), link state, last-seen timestamp
- `GET/POST /api/radar/config` — `{type: "none"|"ld2420"|"ld2450", baudRate}` (baud applies to the LD2420 only); a POST saves to NVS and reboots
- `GET /api/radar/latest` — last parsed reading; every target field is always present, and a field the active sensor can't provide (e.g. angle on LD2420) is JSON `null` rather than a placeholder value — the frontend adapts to whichever fields are present rather than branching on which radar type is configured

**LED**
- `GET/POST /api/led/config` `{enabled, brightness, colorL:{r,g,b}, colorR:{r,g,b}, effect}` — `effect` is one of `off`/`solid`/`breathe`

**OTA**
- `POST /api/ota/firmware` — multipart upload, streams into `Update.write()` (`U_FLASH`)
- `POST /api/ota/filesystem` — multipart upload of a LittleFS image (`U_SPIFFS` — this is the correct `Update.h` mode constant for the LittleFS-backed partition too, not a typo)
- `GET /api/ota/status` — result of the last OTA attempt
- ArduinoOTA (network/IDE OTA) runs independently on port 3232, always attempted once WiFi reaches `STA_CONNECTED` (toggleable via `otaNetworkEnabled`). Requires the admin password when one is set (`upload_flags = --auth=...`).

**Auth**
- `GET /api/auth/status` — public: `{controlPassword, adminPassword, adminProtected, otaRestartRequired}`
- `POST /api/auth/password` `{level: "control"|"admin", password}` — admin; `""` clears that level, otherwise 4–64 characters. The password crosses the network in plain text here (no TLS).

### 4a. Password protection

`src/net/auth.*`. Two levels, each **open until its password is set**:

| Level | Username | Covers |
|---|---|---|
| Public | — | `GET /api/auth/status` only |
| Control | `user` (or `admin`) | static pages outside `/setup`, `/api/eyes/*`, `/api/gestures*`, `/api/playmodes*`, `/api/system/info`, `/api/system/status`, `/api/radar/status`, `/api/radar/latest` |
| Admin | `admin` (or `user` while no admin password is set) | `/setup/*`, `/api/wifi/*`, `/api/servos/*`, `/api/radar/config`, `/api/led/*`, `/api/ota/*`, `/api/system/reboot`, `/api/system/config`, `/api/auth/password`, ArduinoOTA |

The level comes from the URL alone (`Auth::requiredLevel()`); a URL containing a dot segment (`/.`, which covers `/./` and `/../`), `..`, `//` or `\` is treated as Admin, since LittleFS resolves `.` and `..` (`/./setup/wifi.html` would otherwise be served as a control-level page). HTTP **Digest** is used so the browser's own login prompt works for pages, `fetch` and XHR, and the password itself never crosses the network.

**Two enforcement points.** ESPAsyncWebServer runs middleware only when the whole request has arrived, after every `onBody`/`onUpload` callback. Every POST handler here acts inside `onBody`, and web OTA writes flash inside `onUpload`, so a middleware-only check would act first and answer 401 afterwards. Therefore:
- `Auth::middleware()` is registered on the server and sends the 401 challenge (or 429). This covers GETs, static files and POSTs without a body.
- `JsonHelpers::collectJsonBody()` and the OTA upload handler call `Auth::allowed()` on every chunk. On failure they drop the data without responding, and the middleware then sends the single 401.

New POST routes get this for free as long as they use `collectJsonBody()`. A route that reads a body or upload any other way must call `Auth::allowed()` itself.

**Brute-force brake.** After 10 wrong credentials within 60 s, every protected request gets 429 for 30 s. A request with no `Authorization` header doesn't count.

**ArduinoOTA** gets `setPasswordHash(otaMd5)` when it starts. It keeps the first password it is given until reboot, so after the admin password is changed or cleared, `OtaManager` leaves network OTA stopped until the next restart (it fails closed). `/api/auth/status` reports `otaRestartRequired`, and the Security page offers a restart.

**Recovery** needs physical access: the BOOT-button 5 s hold (which also clears WiFi) or serial `auth reset`. Both report failure if NVS can't be written. The BOOT path then does not reset WiFi or reboot, so a failed reset never looks like a successful one.

**Fail closed on storage errors.** If the stored hashes can't be loaded at boot, `Auth` refuses every protected request, ArduinoOTA isn't started, and `/api/auth/status` reports `storageError: true`. A successful `auth reset` or BOOT-button reset recovers.

**Known limitations:**
- Plain HTTP: `POST /api/auth/password` carries the new password in clear text.
- The library does not track the Digest nonces it issues, so a captured request can be replayed to the same method and URI.
- Someone on the LAN can trigger the 429 lockout on purpose.
- The setup AP password (`eyes-setup`) is still hard-coded.

---

## 5. Motion Design — Natural Mode, Gestures, Play Modes

**Natural eyelid coupling**: baseline lid openness ≈0.85 (not fully open, for an organic resting look). The implemented coupling formula ties lid bias to `tiltDeg` only: an upper-lid coefficient of 0.28 and a lower-lid coefficient of 0.12 (asymmetric, so looking down closes the upper lid noticeably more than the lower one, mimicking real eyelid occlusion) — looking up nudges both lids more open. A pan-based asymmetric term and "blink-through on large saccades" were both scoped as optional stretch goals in the original design and were not implemented; both are marked with TODOs in `natural_mode.cpp`/`motion_task.cpp` for future work. Explicit lid commands and gestures own their axis until their motion completes, then coupling resumes smoothly (no snapping) — see §2's ownership model. A `NaturalModeCoupler::setSuppressed()` mechanism (added beyond the original spec) prevents coupling from fighting `sleep` mode's closed-lid pose once its ease-in finishes.

**Gestures** (keyframed, ~150-400ms, hand-authored C++ tables — not JSON, since they're small and fixed): `blink`, `wink_left`, `wink_right`, `surprise` (lids snap wide + a small gaze reset), `sleepy` (slow half-close, held), `squint`/`suspicious` (partial close + slight downward tilt — one canonical id with an alias), `look_around_quick`, `double_blink`, `roll_eyes` (circular gaze sweep, lids track via natural-mode coupling).

**Play modes**:
- `idle` — low-key periodic micro-drift + occasional natural blinks (implemented via randomized timers triggering tiny gaze drift and the `blink` gesture)
- `curious` — larger randomized saccades + more frequent blinks; amplitude/frequency are parameterized in code rather than JSON-driven (a deliberate scope simplification — the ask for "data-driven, not hardcoded" behavior content is satisfied concretely by `greeting`'s JSON sequence below; fully data-driven curiosity behavior was judged disproportionate to this mode's value)
- `sleep` — eases lids toward ~0.1-0.15 openness, motion nearly stops
- `greeting` — one-shot scripted sequence loaded from `/sequences/greeting.json` on LittleFS (wake up → look at viewer → wide-eyed surprise → settle to idle) — proves sequence content is data-driven, not hardcoded, and can be edited/replaced without a firmware rebuild
- `tracking` — reads `RadarState` each tick. With an **LD2450** (angle/position available), issues damped (~300ms minimum retarget interval, to avoid nervous-looking jitter) proportional gaze-following toward the closest target with angle data, mapped to the ±45° pan/tilt convention. With an **LD2420** (distance/presence only, no angle), there is no direction to follow, so tracking degrades to a presence-triggered "alert" glance (the `surprise` gesture) on a rising edge of presence, falling back to idle drift between events — exactly the "presence-triggered alert glance rather than true directional tracking" behavior anticipated in the original design for that sensor. With no radar configured, `RadarTask::getState()` is always empty, so tracking behaves exactly like idle.
- `manual` — no autonomous behavior; `/api/eyes/*` and `/api/gestures/*` always work regardless of active mode, but activating `manual` is the recommended mode for external integrations (phone/voice/vision AI) so nothing autonomous competes for `commandGeneration`.

Switching play modes bumps `commandGeneration`, cleanly aborting any in-flight `PlayMode`-sourced gesture/sequence per §2's interruption mechanism.

---

## 6. Radar Abstraction

`IRadarSensor` interface (`src/radar/iradar_sensor.h`): `begin()`, `poll()` (called from `RadarTask`), and a mutex-guarded `getState() → RadarState{presence, targets:[{distanceMm, angleDeg?, xMm?, yMm?, speedMmS?}], lastUpdateMs}` — fields a sensor can't provide use `std::optional` (matching `EyeCommand`'s existing partial-field convention) rather than a magic-number placeholder.

- **`Ld2420Sensor`**: built on **`gsieben/LD2420GeoGab @ 1.0.0`** — this library was directly inspected (not just taken on faith) during Phase 5 and found to be a genuine, well-documented HLK-LD2420 driver (the plan's originally-named `bolukan/ld2420` turned out, on inspection, to actually be a renamed LD2410 library fork, not LD2420, and was swapped out back in Phase 0). Distance/presence only, no angle — this limitation is surfaced honestly in both the `tracking` play mode (§5) and the radar visualization page rather than hidden. The driver's UART read is fully non-blocking/timeout-based so it degrades gracefully with no physical sensor connected.
- **`Ld2450Sensor`**: a custom parser (no external library — the frame format is small and fully reverse-engineered), decoupled as a pure function (`parseLd2450Frame()`) from the UART I/O so it's testable independent of hardware. Frame: header `AA FF 03 00`, three 8-byte target blocks (X int16 LE, Y int16 LE, speed int16 LE, distance-resolution uint16 LE), footer `55 CC`, 256000 baud. **Sign decoding** (a documented gotcha in this protocol): a negative value is transmitted as `0x8000 + value` rather than two's-complement — decoded as `raw & 0x8000 ? -(int16_t)(raw & 0x7FFF) : (int16_t)raw`.
- Selected at runtime: the radar type (`none`/`ld2420`/`ld2450`) is an NVS setting (`NvsStore::getRadarType()`, default `ld2420`) chosen on the Radar setup page. `RadarTask::begin()` reads it once at boot and allocates only that driver; with `none` it creates no sensor and no task, so nothing opens or polls the UART. Changing the type reboots the device. Both drivers are always compiled in (about 4 KB of flash), so there is a single firmware image.
- `RadarTask`: Core 0, priority 1, ~30ms poll loop, mutex-guarded `RadarState` snapshot (same idiom as `MotionTask`'s pose snapshot).

The radar test/visualization page (`setup/radar.html`) adapts its rendering to whichever fields are actually present in `/api/radar/latest`'s response, rather than hardcoding behavior per radar type.

---

## 7. Web Frontend

Plain HTML/CSS/JS served from LittleFS `/www` (no Node/build toolchain, no CDN dependencies — works fully offline on-device). Structure:

```
data/www/
  index.html          landing page — links to Control / Setup, live status; prompts WiFi setup if in AP mode
  css/app.css          shared styling, mobile-first, distinct "control" vs "setup" (amber/maintenance) theming
  js/api.js            shared fetch wrapper, pollEvery() live-refresh helper, throttle(), XHR upload-with-progress helper
  control/
    manual.html         2D drag gaze pad + eyelid sliders + natural-mode toggle + gesture buttons
    playmodes.html       play mode cards (activate/status)
    status.html          live telemetry dashboard (pose, radar, heap/uptime)
  setup/
    wifi.html             AP-mode captive-portal setup + normal STA re-config (scan + manual entry, connect, forget)
    calibration.html      per-servo min/center/max controls with live "test" button, save
    radar.html            live radar visualization/test page
    ota.html               firmware/filesystem upload with progress bar, current version display
    led.html                LED enable/brightness/color/effect config
    security.html           control/admin passwords
```

The `/setup/*` vs `/control/*` split also marks the password boundary: `/setup/*` and its API routes need the admin password, and everything else needs the control password (§4a). No page has login code: the browser handles the Digest challenge itself. `index.html` shows a warning while no password is set.

**Serving note**: the LittleFS filesystem image root (as `pio run -t buildfs` packs it) is the *contents* of `data/`, so the frontend actually lives at FS path `/www/*`. `web_server.cpp` serves `LittleFS "/www/"` at URL `"/"` — this was fixed during the Phase 8 integration review after being missed in Phase 7 (which validated pages via a local static file server, not the real device-served path).

Captive portal: while in AP setup mode, all unmatched requests (`server.onNotFound`) redirect to `/` so phones/laptops auto-pop the "sign in to network" prompt, landing on `index.html`'s WiFi-setup prompt.

---

## 8. OTA & Partitioning

Custom `partitions.csv` (4MB flash):
```
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,        data, nvs,      0x9000,   0x5000,
otadata,    data, ota,      0xe000,   0x2000,
app0,       app,  ota_0,    0x10000,  0x180000,
app1,       app,  ota_1,    0x190000, 0x180000,
littlefs,   data, spiffs,   0x310000, 0xF0000,
```
Two 1.5MB OTA app slots, ~960KB LittleFS (as built, the frontend + data files use well under 10% of that).

**Note**: the partition's SubType is `spiffs`, not `littlefs` — the pinned toolchain's `esptoolpy` (4.5.1) doesn't recognize `littlefs` as a valid SubType keyword (added in a later esptool/IDF release). The partition **name** is still `littlefs` and `board_build.filesystem = littlefs` in `platformio.ini` still makes it mount and format as LittleFS — only the CSV's SubType label differs from what a newer toolchain would allow.

- **Network OTA**: `ArduinoOTA`, toggleable, enables `pio run -t upload --upload-port <device-ip>` directly from VSCode/PlatformIO. With an admin password set, this also needs `upload_flags = --auth=<admin password>` (§4a).
- **Web OTA**: `setup/ota.html` + `/api/ota/firmware` / `/api/ota/filesystem`, hand-written against `Update.h` (no ready-made drop-in like ElegantOTA was found to be confirmed-compatible with the ESPAsyncWebServer fork in use, so this is a small first-party handler).
- Both app slots mean a failed OTA can roll back; `Update.h` handles the slot-switch/verify automatically.
- **Web flasher (USB)**: `.github/workflows/webflash.yml` publishes `webflash/index.html` to GitHub Pages with two channels, each a directory holding a `manifest.json` and the `.bin` files. `latest/` is built from `main` on every deploy. `stable/` is copied unchanged from the newest non-prerelease GitHub Release. Pushing a `v*` tag builds that tag and attaches the same files to its Release. Tags containing a `-` are marked prereleases and never become stable. The run then starts a deploy on `main`: the `github-pages` environment only accepts deployments from `main`, so the Pages job never runs for a tag. The page uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Web Serial, Chrome/Edge only). `webflash/assemble.sh` writes each manifest.
  - The manifest lists **separate parts** (bootloader, partitions, `boot_app0`, app at `app0`, LittleFS) rather than one `merge_bin` image, because `merge_bin` fills the gaps with 0xFF and would wipe NVS. As a result, an install without "erase" is a settings-preserving update.
  - The app and LittleFS offsets come from the `partitions.csv` of the tree being built, so a release keeps its own layout. The bootloader, partitions and `boot_app0` offsets (0x1000, 0x8000, 0xe000) are the ESP32 Arduino defaults and are hardcoded.
  - Nothing writes to LittleFS at runtime, so reflashing it loses no data. Installing always boots `app0`, because `boot_app0` resets otadata.
  - Release routine: set `FIRMWARE_VERSION` in `include/version.h` to the release number (it is not taken from the tag), tag `vX.Y.Z` on `main`, then set `main` to the next `-dev` version.

---

## 9. Project Layout & Toolchain

```
ESP-Magic-Eyes/
  platformio.ini          single env:esp32dev (radar type is a runtime setting)
  partitions.csv
  .vscode/                 PlatformIO-generated IDE config
  include/                 pin_map.h, version.h — shared headers
  src/
    main.cpp
    hal/                    servo_hal.*, led_controller.*, buttons.*
    net/                    wifi_manager.*, ota_manager.*, web_server.*, auth.*, serial_console.*
    api/                    rest_routes.*, ota_routes.*, servo_routes.*, eyes_routes.*, gesture_routes.*,
                             playmode_routes.*, radar_routes.*, led_routes.*, wifi_routes.*, auth_routes.*,
                             json_helpers.*
    motion/                 eye_pose.*, command_queue.*, motion_task.*, natural_mode.*, gesture_engine.*,
                             playmode_manager.*
    radar/                  iradar_sensor.h, ld2420_sensor.*, ld2450_sensor.*, radar_task.*
    storage/                nvs_store.*
  data/www/                 frontend (see §7) — uploaded to LittleFS via `pio run -t uploadfs`
  data/sequences/           greeting.json (play-mode sequence content)
  PROGRESS.md               phase-by-phase build log, including every deviation from the original plan
```

`platformio.ini` (as built):
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
board_build.partitions = partitions.csv
board_build.filesystem = littlefs
monitor_speed = 115200
build_unflags = -std=gnu++11
build_flags = -std=gnu++17
lib_deps =
  esp32async/AsyncTCP @ 3.5.0
  esp32async/ESPAsyncWebServer @ 3.12.1
  madhephaestus/ESP32Servo @ 3.2.1
  adafruit/Adafruit NeoPixel @ 1.15.5
  bblanchon/ArduinoJson @ 7.4.3
  gsieben/LD2420GeoGab @ 1.0.0
```
(The original ESP32Async/ESPAsyncWebServer packages are referenced in lowercase `esp32async/...` form as PlatformIO's registry resolved them; both are the actively-maintained fork of the long-unmaintained `me-no-dev/ESPAsyncWebServer`/`AsyncTCP`.)

---

## Hardware bring-up checklist

For whoever flashes a fresh board:
1. First-boot AP setup flow: connect to `MagicEyes-Setup-XXXX`, browse to any HTTP address, confirm the captive portal redirects to the WiFi setup page, enter home WiFi credentials, confirm the device reconnects in STA mode.
2. Servo calibration: work through `setup/calibration.html`'s guided steps (straight ahead → lids closed → lids open → half open → limits) for each servo, then use "Compare both eyes" to check left and right lids match.
3. Gestures and manual gaze control: verify each gesture button and the gaze pad produce the expected physical motion; adjust the pan/tilt ±45° convention or per-servo calibration if the physical range doesn't match.
4. Natural mode: toggle on/off and visually confirm eyelids track gaze plausibly.
5. Radar: with the physical LD2420 wired per `include/pin_map.h` (RX GPIO22 from OT2, TX GPIO17 — see the hardware erratum in §1), confirm `setup/radar.html` shows presence detection; note that HLK-LD2420 firmware versions vary in UART baud rate (115200 vs. 256000) and TX/RX pin roles — verify against the specific module before assuming the driver's default settings are correct.
6. OTA: perform one web OTA update and one network (`pio run -t upload --upload-port <ip>`) OTA update to confirm both paths work before relying on them for future updates.

---

## Status & open items

All nine build phases (scaffold → integration) are complete. PROGRESS.md is the frozen build log: useful for *why* something was done, but parts of it are superseded (e.g. its two build environments and the radar "Bug 3" pin swap). This document and the code are current.

**Hardware-verified** (LD2420 board): WiFi setup, fallback and recovery (including a wrong password via the API), calibration (hold, NVS migration, half-open point), gestures and abort-restore, sticky manual eyelids, greeting → idle, web OTA (including recovery from an interrupted upload), serial console.

**Not yet verified on hardware:** LD2450 decoding and tracking, `millis()` wrap (~49.7 days), NVS write-failure handling, the PR #2 changes (runtime radar selection, AP kept up during retries, async fallback scan, pulse-exact calibration-hold exit, per-request body buffers), and password protection (§4a) in any respect: browser prompts, body/upload guards, lockout, ArduinoOTA `--auth`, and recovery.

**Deliberately deferred:**
- HTTPS, and a configurable setup-AP password (see §4a's known limitations).
- WebSocket telemetry (`/ws`) — REST polling only (see §2).
- Optional motion extras from the original plan: pan-based asymmetric lid tightening during fast saccades (`TODO` in `natural_mode.h`), saccade blink-through (auto-blink on a >~25° gaze jump), and curious mode's asymmetric lid narrowing.

**Hardware gotchas already handled in code** (details in the code comments):
- The Aux servo channel (GPIO13) hangs `Servo::attach()` when attached as the 7th channel at boot, so it is attached lazily on first use (`servo_hal.cpp`).
- LittleFS must be mounted with partition label `littlefs` (`main.cpp`); the library default is `spiffs`.
- Radar RX is on GPIO22 via a bodge wire from the LD2420's OT2 pad (§1 erratum).
- Two lids are mounted mirrored (`LidUpperL`, `LidLowerR`); their default calibration accounts for it.
- SG90 lid moves need ≥ ~100 ms to complete; gesture keyframes are multiples of the 20 ms motion tick.
