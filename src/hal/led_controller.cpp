#include "hal/led_controller.h"

#include <Adafruit_NeoPixel.h>

#include "pin_map.h"
#include "storage/nvs_store.h"

namespace {

constexpr uint16_t kPixelCount = 2;
constexpr uint16_t kPixelLeft = 0;  // index 0 = left eye, per plan §1
constexpr uint16_t kPixelRight = 1; // index 1 = right eye

// Breathe effect timing. Rate-limited well inside the task spec's 30-50ms
// guidance so strip.show() isn't called every loop() iteration for what is
// a slow, ambient "glow" animation, not a real-time effect.
constexpr uint32_t kBreatheUpdateIntervalMs = 40;
constexpr uint32_t kBreathePeriodMs = 3000; // one full dim->bright->dim cycle
// Breathe never dims all the way to 0 — it stays a visible, gentle pulse
// rather than blinking on/off every cycle.
constexpr float kBreatheMinFraction = 0.15f;

Adafruit_NeoPixel gStrip(kPixelCount, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

LedController::LedConfig gConfig;
bool gDirty = true;               // forces one apply on the next handle()/begin()
uint32_t gLastBreatheUpdateMs = 0;

// Triangle-wave brightness (0..peakBrightness), NOT sinusoidal — cheap
// (no float trig needed beyond the linear ramp) and visually close enough
// to a sine breathe for a small ambient accent.
uint8_t triangleBrightness(uint32_t nowMs, uint8_t peakBrightness) {
  uint32_t phase = nowMs % kBreathePeriodMs;
  float t = static_cast<float>(phase) / static_cast<float>(kBreathePeriodMs); // 0..1
  float tri = (t < 0.5f) ? (t * 2.0f) : (2.0f - t * 2.0f);                    // 0->1->0
  float fraction = kBreatheMinFraction + (1.0f - kBreatheMinFraction) * tri;
  return static_cast<uint8_t>(fraction * static_cast<float>(peakBrightness));
}

// Applies the strip's current *static* state (Off or Solid) and calls
// show() exactly once. Breathe's own animated frames are driven separately
// from handle() — this is only used for the non-animated states (and to
// seed Breathe's very first frame indirectly via begin()'s initial off
// state).
void applyStatic() {
  if (!gConfig.enabled || gConfig.effect == LedController::LedEffect::Off) {
    gStrip.clear();
    gStrip.show();
    return;
  }
  // Solid (and any other non-Breathe enabled state): fixed per-eye color
  // at the configured brightness.
  gStrip.setBrightness(gConfig.brightness);
  gStrip.setPixelColor(kPixelLeft, gConfig.colorL.r, gConfig.colorL.g, gConfig.colorL.b);
  gStrip.setPixelColor(kPixelRight, gConfig.colorR.r, gConfig.colorR.g, gConfig.colorR.b);
  gStrip.show();
}

} // namespace

namespace LedController {

void begin() {
  gStrip.begin();
  gStrip.clear();
  gStrip.show(); // inert on boot regardless of config — safe even if unwired

  gConfig.enabled = NvsStore::getLedEnabled();
  NvsStore::LedColorConfig stored = NvsStore::getLedColorConfig();
  gConfig.brightness = stored.brightness;
  gConfig.colorL = {stored.colorL[0], stored.colorL[1], stored.colorL[2]};
  gConfig.colorR = {stored.colorR[0], stored.colorR[1], stored.colorR[2]};
  gConfig.effect = static_cast<LedEffect>(stored.effect);

  applyStatic(); // no-op strip write if disabled/Off (the Phase 1 default)
  gDirty = false;
  gLastBreatheUpdateMs = millis();
}

void handle() {
  if (gConfig.enabled && gConfig.effect == LedEffect::Breathe) {
    uint32_t now = millis();
    if (now - gLastBreatheUpdateMs >= kBreatheUpdateIntervalMs) {
      gLastBreatheUpdateMs = now;
      uint8_t frameBrightness = triangleBrightness(now, gConfig.brightness);
      gStrip.setBrightness(frameBrightness);
      gStrip.setPixelColor(kPixelLeft, gConfig.colorL.r, gConfig.colorL.g, gConfig.colorL.b);
      gStrip.setPixelColor(kPixelRight, gConfig.colorR.r, gConfig.colorR.g, gConfig.colorR.b);
      gStrip.show();
    }
    return;
  }

  // Off/Solid: only ever re-apply (and show()) once, right after a config
  // change — not every loop() tick.
  if (gDirty) {
    applyStatic();
    gDirty = false;
  }
}

LedConfig getConfig() { return gConfig; }

void setConfig(const LedConfig &cfg) {
  gConfig = cfg;

  NvsStore::setLedEnabled(gConfig.enabled); // existing Phase 1 "system" key
  NvsStore::LedColorConfig stored;
  stored.brightness = gConfig.brightness;
  stored.colorL[0] = gConfig.colorL.r;
  stored.colorL[1] = gConfig.colorL.g;
  stored.colorL[2] = gConfig.colorL.b;
  stored.colorR[0] = gConfig.colorR.r;
  stored.colorR[1] = gConfig.colorR.g;
  stored.colorR[2] = gConfig.colorR.b;
  stored.effect = static_cast<uint8_t>(gConfig.effect);
  NvsStore::setLedColorConfig(stored);

  gDirty = true;                    // handle() applies Off/Solid on its next tick
  gLastBreatheUpdateMs = millis();  // restart Breathe's phase cleanly on any change
}

const char *effectToName(LedEffect effect) {
  switch (effect) {
    case LedEffect::Solid: return "solid";
    case LedEffect::Breathe: return "breathe";
    case LedEffect::Off:
    default: return "off";
  }
}

bool effectFromName(const String &name, LedEffect &outEffect) {
  if (name == "off") { outEffect = LedEffect::Off; return true; }
  if (name == "solid") { outEffect = LedEffect::Solid; return true; }
  if (name == "breathe") { outEffect = LedEffect::Breathe; return true; }
  return false;
}

} // namespace LedController
