// pin_map.h — ESP Magic Eyes hardware pin map
//
// Named constants for every GPIO used by the ε-SERIES animatronic eye
// controller. See architecture plan §1 for the source hardware table and
// bring-up notes.
//
// Board: generic ESP32 WROOM-32 dev board (non-PSRAM), 4MB flash.

#pragma once

#include <cstddef>
#include <cstdint>

// --- Servos (LEDC PWM via ESP32Servo) --------------------------------
// Shared-yoke gaze: one pan + one tilt servo drive both eyeballs.
// Eyelids are independent per eye (upper/lower, left/right).
constexpr uint8_t SERVO_PAN         = 32; // Eye Pan (shared X, both eyes)
constexpr uint8_t SERVO_TILT        = 33; // Eye Tilt (shared Y, both eyes)
constexpr uint8_t SERVO_LID_UPPER_L = 25; // Left Upper Eyelid
constexpr uint8_t SERVO_LID_LOWER_L = 26; // Left Lower Eyelid
constexpr uint8_t SERVO_LID_UPPER_R = 27; // Right Upper Eyelid
constexpr uint8_t SERVO_LID_LOWER_R = 14; // Right Lower Eyelid
constexpr uint8_t SERVO_AUX         = 13; // Spare PWM channel, reserved/exposed as "aux servo"

// ServoId — indexes the 7 servo channels above. Declared here (rather than
// in hal/servo_hal.h) so both the HAL and storage/nvs_store.h (servo
// calibration) can reference it without a hal<->storage dependency cycle.
enum class ServoId : uint8_t {
  Pan = 0,
  Tilt,
  LidUpperL,
  LidLowerL,
  LidUpperR,
  LidLowerR,
  Aux,
  Count // sentinel — number of servo channels, not a real servo
};

// ServoId-indexed pin lookup, order must match the ServoId enum above.
constexpr uint8_t kServoPins[static_cast<size_t>(ServoId::Count)] = {
  SERVO_PAN, SERVO_TILT, SERVO_LID_UPPER_L, SERVO_LID_LOWER_L,
  SERVO_LID_UPPER_R, SERVO_LID_LOWER_R, SERVO_AUX,
};

// --- Radar (HLK-LD2420 or HLK-LD2450, UART2) --------------------------
// Originally the UART2 default pins (RX 16 / TX 17); RX moved to GPIO22 —
// see below and the hardware erratum in architecture/ARCHITECTURE.md §1.
//
// Root cause found (bring-up, 2026-09-22): the physical board had ESP32
// RX wired to the module's OT1 pad — a simple high/low presence output,
// not the module's real UART TX line. Per the HLK-LD2420 datasheet, the
// module's actual serial-out pin is labeled OT2, not OT1/TX (5-pin
// header: VCC, GND, OT1, RX, OT2). Rather than reflow the existing
// OT1 joint, a fresh wire runs from the module's OT2 pad to GPIO22
// (unused elsewhere in this pin map, not a strapping pin) and RX is
// pointed there instead of GPIO16/17. TX stays on GPIO17 (ESP32 TX ->
// module RX, already correct). Baud rate is a separate, NVS-backed
// runtime setting now (NvsStore::getRadarBaudRate(), settable from the
// Radar setup page) rather than a compile-time GG_BAUDRATE flag — see
// radar/ld2420_sensor.cpp.
constexpr uint8_t RADAR_RX_PIN = 22; // ESP32 UART2 RX <- radar OT2 (module's real UART TX pin)
constexpr uint8_t RADAR_TX_PIN = 17; // ESP32 UART2 TX -> radar RX

// --- RGB LED (WS2812, optional/future) ---------------------------------
// Single chain, 2 pixels: index 0 = left eye, index 1 = right eye.
// Not wired yet at project start; code is feature-flagged off until
// enabled in config.
constexpr uint8_t LED_DATA_PIN = 4;

// --- Factory reset / WiFi setup trigger ---------------------------------
// GPIO0 is the standard dev-board BOOT button (active-low, internal
// pull-up). Hold 5s during normal run to clear NVS WiFi creds and
// reboot into AP setup mode. No extra wiring required.
constexpr uint8_t FACTORY_RESET_BUTTON_PIN = 0; // GPIO0 == BOOT button
