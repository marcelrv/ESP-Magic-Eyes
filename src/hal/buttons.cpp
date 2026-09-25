#include "hal/buttons.h"

#include <Arduino.h>

#include "net/auth.h"
#include "net/wifi_manager.h"
#include "pin_map.h"

namespace {

constexpr uint32_t kLongPressMs = 5000;

bool gPressed = false;
uint32_t gPressStartMs = 0;
bool gTriggered = false; // guards against firing forgetNetwork() repeatedly

} // namespace

namespace Buttons {

void begin() { pinMode(FACTORY_RESET_BUTTON_PIN, INPUT_PULLUP); }

void handle() {
  // Active-low: LOW means pressed.
  bool down = digitalRead(FACTORY_RESET_BUTTON_PIN) == LOW;

  if (down && !gPressed) {
    // Just pressed.
    gPressed = true;
    gPressStartMs = millis();
    gTriggered = false;
  } else if (down && gPressed) {
    if (!gTriggered && (millis() - gPressStartMs) >= kLongPressMs) {
      gTriggered = true;
      Serial.println("[Buttons] Factory-reset button held 5s — clearing passwords, forgetting WiFi, rebooting.");
      // Physical access is the recovery path for forgotten passwords.
      Auth::clearAll();
      WifiManager::forgetNetwork(); // reboots; does not return in practice
    }
  } else if (!down && gPressed) {
    // Released before threshold, or after triggering (reboot already
    // in flight in the latter case).
    gPressed = false;
    gTriggered = false;
  }
}

} // namespace Buttons
