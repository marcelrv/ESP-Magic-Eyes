# ESP Magic Eyes — agent notes

ESP32 (Arduino + PlatformIO) firmware and web UI for an animatronic eye
mechanism: 6 servos, optional mmWave radar, optional RGB LEDs.

## Where to look
- `architecture/ARCHITECTURE.md` — current design, API, storage,
  calibration model, and "Status & open items". Read this first.
- `include/pin_map.h` — GPIO map; the source of truth over any doc.
- `PROGRESS.md` — frozen historical build log. Useful for *why*; parts are
  superseded (two build envs, radar "Bug 3"). Never append to it.
- `README.md` — end-user docs; keep it non-technical.

## Build and verify
- `pio run` — single `esp32dev` env. `pio run -t buildfs` / `-t uploadfs`
  for `data/` (the web UI lives under `data/www/`).
- There are no unit tests and no CI build. A clean `pio run` with no new
  warnings in `src/` is the only automated check; behavior needs the real
  board, so say what was and wasn't verified on hardware.
- Radar type (none / LD2420 / LD2450) is a runtime NVS setting, not a build
  flag — don't reintroduce `#ifdef`s for it.

## Threading model (most past bugs broke this)
- AsyncTCP task: every HTTP handler (`src/api/*`).
- `loop()` (core 1, prio 1): WiFi manager, buttons, OTA, LED, serial console,
  deferred restarts.
- MotionTask (core 1, prio 3, 50 Hz): servos, gestures, play modes, natural
  mode. API handlers talk to it via `CommandQueue` and atomics.
- RadarTask (core 0): only when a radar is configured.
- State shared across these needs a mutex or atomic (see `NvsStore`,
  `ServoHal`, `GestureEngine`, WiFi scan cache). Never hand out pointers to
  shared `String`s — return copies.

## Conventions
- POST routes: `server.on(path, HTTP_POST, JsonHelpers::requireBody, nullptr, handler)`
  and start the handler with `JsonHelpers::collectJsonBody()`. No global
  body buffers.
- Settings that change hardware setup (radar type/baud) are saved, then
  applied by a deferred `ESP.restart()` after the response is sent.
- NVS values persist across firmware versions: never renumber stored enums;
  blob layout changes need a size-detected migration (see `servocal`).
- Timers: use `deadlineReached()` (wrap-safe), not raw `>=` on `millis()`.
- Match the existing style: dense comments explaining *why*, LF line
  endings, C++17.
- Git: never add Claude/AI co-author or attribution lines to commits or PRs.
