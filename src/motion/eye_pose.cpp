#include "motion/eye_pose.h"

#include <algorithm>

float applyEasing(Easing easing, float t) {
  t = std::min(1.0f, std::max(0.0f, t));

  switch (easing) {
    case Easing::EaseInOut:
      // Smoothstep: 3t^2 - 2t^3. Zero velocity at both endpoints, so a
      // retargeted axis (plan §2's interruption behavior) never has a
      // visible velocity discontinuity at the moment a new command lands,
      // even though position can still jump if the new target is far
      // away (that's expected — only the *ease*, not a full min-jerk
      // trajectory, is in scope here).
      return t * t * (3.0f - 2.0f * t);
    case Easing::Linear:
    default:
      return t;
  }
}
