// buttons.h — non-blocking button polling.
//
// Phase 1 scope: just the factory-reset (BOOT) button long-press. Other
// HAL modules (servo_hal, led_controller) land in Phase 3/6.

#pragma once

namespace Buttons {

// Configures FACTORY_RESET_BUTTON_PIN as INPUT_PULLUP. Call once from
// setup().
void begin();

// Polls the button state using millis()-based timing (no blocking).
// Call every loop() iteration. When the button (active-low) has been
// held continuously for the long-press threshold (5s), triggers
// WifiManager::forgetNetwork() exactly once per hold.
void handle();

} // namespace Buttons
