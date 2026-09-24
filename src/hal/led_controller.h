// led_controller.h — NeoPixel-based cosmetic "mood glow" LED controller
// (architecture plan §1/§4/§10 Phase 6). Drives a 2-pixel WS2812 chain on
// LED_DATA_PIN (GPIO4, pin_map.h) — index 0 = left eye, index 1 = right
// eye, per plan §1. Deliberately a small "glow accent" feature, not a
// lighting-effects engine: three effects only (Off/Solid/Breathe).
//
// No hardware is wired to GPIO4 yet (plan's "future addition" note) —
// writing NeoPixel data to a floating/unconnected pin is electrically
// harmless, so no hardware-detection is needed here. The *feature* itself
// defaults to off (NvsStore::getLedEnabled()'s existing `false` default,
// reserved since Phase 1) so nothing lights up unexpectedly once hardware
// does get wired later.
//
// Simple module polled from loop() (same pattern as WifiManager::handle()/
// Buttons::handle()/OtaManager::handle()) — this is a low-rate cosmetic
// effect, not real-time control, so it doesn't need its own FreeRTOS task.

#pragma once

#include <cstdint>

#include <Arduino.h> // String

namespace LedController {

enum class LedEffect : uint8_t {
  Off = 0,
  Solid = 1,
  Breathe = 2,
};

struct RgbColor {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

struct LedConfig {
  bool enabled = false; // mirrors NvsStore's existing "system"/ledEnabled key
  uint8_t brightness = 128;
  RgbColor colorL{0, 120, 255};
  RgbColor colorR{0, 120, 255};
  LedEffect effect = LedEffect::Off;
};

// Initializes the NeoPixel strip, loads config from NVS (the existing
// "system"/ledEnabled key + the new "led" namespace's brightness/color/
// effect blob), and applies the initial state (off, by default). Call once
// from setup(), any time after NvsStore::begin().
void begin();

// Drives the Breathe effect's time-based brightness animation via
// millis(), rate-limited to ~40ms between frame updates. For a static
// Off/Solid state, strip.show() is called once on change, not repeatedly.
// Call every loop() iteration.
void handle();

// Returns the current in-RAM config (already applied + persisted).
LedConfig getConfig();

// Applies a full config (range validation is the API layer's job — see
// api/led_routes.cpp) and persists it: `enabled` to the existing
// "system"/ledEnabled NVS key (Phase 1), everything else to the new "led"
// namespace's blob (NvsStore::setLedColorConfig()).
void setConfig(const LedConfig &cfg);

// String <-> LedEffect helpers, shared with api/led_routes.cpp for JSON
// (de)serialization ("off" / "solid" / "breathe").
const char *effectToName(LedEffect effect);
bool effectFromName(const String &name, LedEffect &outEffect);

} // namespace LedController
