#include "motion/natural_mode.h"

#include <algorithm>
#include <cmath>

#include "storage/nvs_store.h"

namespace {

bool gEnabled = true;
bool gSuppressed = false;

constexpr float kBaselineOpenness = 0.85f;
// Matches motion_task.cpp's own kGazeRangeDeg (the ±45° working gaze
// range) — duplicated here rather than shared via a header because it's a
// single named constant with an obvious single source of truth in the
// plan (§1/§10 Phase 3's degrees-to-pulse mapping), not a value that could
// drift out of sync silently.
constexpr float kGazeRangeDeg = 45.0f;
constexpr float kUpperLidGazeCoupling = 0.28f;
constexpr float kLowerLidGazeCoupling = 0.12f;

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

} // namespace

namespace NaturalModeCoupler {

void begin() { gEnabled = NvsStore::getNaturalModeEnabled(); }

bool isEnabled() { return gEnabled; }

void setEnabled(bool enabled) {
  gEnabled = enabled;
  NvsStore::setNaturalModeEnabled(enabled);
}

void setSuppressed(bool suppressed) { gSuppressed = suppressed; }

bool isSuppressed() { return gSuppressed; }

LidTargets computeTargets(float panDeg, float tiltDeg) {
  (void)panDeg; // reserved for the optional pan-based term, see header TODO
  float tiltNormalized = tiltDeg / kGazeRangeDeg; // -1..1, positive = looking up
  tiltNormalized = std::min(1.0f, std::max(-1.0f, tiltNormalized));

  float upper = clamp01(kBaselineOpenness + tiltNormalized * kUpperLidGazeCoupling);
  float lower = clamp01(kBaselineOpenness + tiltNormalized * kLowerLidGazeCoupling);

  return LidTargets{upper, lower, upper, lower};
}

} // namespace NaturalModeCoupler
