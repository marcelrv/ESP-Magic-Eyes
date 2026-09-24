#include "radar/ld2450_sensor.h"

#include <Arduino.h>

#include <cmath>
#include <cstring>

#include "pin_map.h"

namespace {

constexpr uint8_t kHeader[4] = {0xAA, 0xFF, 0x03, 0x00};
constexpr uint8_t kFooter[2] = {0x55, 0xCC};
constexpr size_t kTargetBlockBytes = 8;
constexpr size_t kTargetSlots = 3;

uint16_t readU16LE(const uint8_t *buf, size_t offset) {
  return static_cast<uint16_t>(buf[offset]) | (static_cast<uint16_t>(buf[offset + 1]) << 8);
}

}  // namespace

int16_t decodeLd2450Signed(uint16_t raw) {
  if (raw & 0x8000) {
    return static_cast<int16_t>(raw & 0x7FFF);
  }
  return static_cast<int16_t>(-static_cast<int16_t>(raw));
}

bool parseLd2450Frame(const uint8_t *buf, size_t len, RadarState &out) {
  if (len < kLd2450FrameBytes) return false;
  if (std::memcmp(buf, kHeader, sizeof(kHeader)) != 0) return false;
  if (std::memcmp(buf + kLd2450FrameBytes - sizeof(kFooter), kFooter, sizeof(kFooter)) != 0) return false;

  RadarState result;  // built up locally so a rejected frame never partially overwrites `out`
  size_t offset = sizeof(kHeader);
  for (size_t i = 0; i < kTargetSlots; ++i) {
    uint16_t xRaw = readU16LE(buf, offset);
    uint16_t yRaw = readU16LE(buf, offset + 2);
    uint16_t speedRaw = readU16LE(buf, offset + 4);
    // Byte offset+6..7 ("distance resolution") is parsed by position but not
    // currently surfaced in RadarTarget — no field in the sensor-agnostic
    // RadarState shape (plan §6) maps to it cleanly, and neither the
    // tracking play-mode nor /api/radar/* need it this phase.
    offset += kTargetBlockBytes;

    int16_t x = decodeLd2450Signed(xRaw);
    int16_t y = decodeLd2450Signed(yRaw);
    int16_t speed = decodeLd2450Signed(speedRaw);

    // Empty-slot heuristic (common LD2450 convention, NOT independently
    // verified against physical hardware this phase — see header): a
    // target block reporting exactly (0,0) is treated as "no target in
    // this slot" rather than a real target sitting exactly on the sensor.
    if (x == 0 && y == 0) continue;

    if (result.targetCount >= kRadarMaxTargets) break;
    RadarTarget &t = result.targets[result.targetCount++];
    t.xMm = static_cast<float>(x);
    t.yMm = static_cast<float>(y);
    t.speedMmS = static_cast<float>(speed) * 10.0f; // sensor reports cm/s
    t.distanceMm = std::sqrt(static_cast<float>(x) * x + static_cast<float>(y) * y);
    // atan2(x, y): 0 deg = straight ahead (+y), positive = to the sensor's
    // right (+x) — matches MotionTask's pan convention closely enough to
    // feed directly into a gaze target (see playmode_manager.cpp's
    // tracking-mode wiring).
    t.angleDeg = std::atan2(static_cast<float>(x), static_cast<float>(y)) * (180.0f / static_cast<float>(M_PI));
  }
  result.presence = result.targetCount > 0;

  out = result;
  return true;
}

bool Ld2450Sensor::begin() {
  // Unlike LD2420GeoGab::begin(), there's no command/response handshake to
  // perform here (no library, no config-mode protocol implemented) — this
  // is a pure UART open, which cannot itself fail or block waiting on the
  // sensor. Actual link presence is inferred later from whether frames
  // ever get parsed (see RadarTask::getStatus()'s linkOk/lastUpdateMs
  // windowing in radar_task.cpp) — always returning true here is correct,
  // not a shortcut.
  Serial2.begin(kLd2450BaudRate, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  rxLen_ = 0;
  return true;
}

void Ld2450Sensor::poll() {
  // Drain whatever's currently in the UART FIFO into rxBuf_. Bounded by
  // Serial2.available()'s own return value each call — never blocks, and
  // returns immediately (0 iterations) if no physical sensor is wired up,
  // per the task spec's "no data ever arrives" requirement.
  while (Serial2.available() > 0) {
    if (rxLen_ >= kRxBufSize) {
      // No valid frame found before filling the buffer (noise, or a
      // partial frame straddling a resync) — keep the newest half and
      // keep scanning forward rather than growing unboundedly or
      // deadlocking on a corrupt stream.
      size_t keep = kRxBufSize / 2;
      std::memmove(rxBuf_, rxBuf_ + (kRxBufSize - keep), keep);
      rxLen_ = keep;
    }
    rxBuf_[rxLen_++] = static_cast<uint8_t>(Serial2.read());
  }

  // Scan for a frame starting anywhere in the buffer (handles UART framing
  // noise / a mid-frame boot) — same "slide forward one byte on mismatch"
  // approach as LD2420GeoGab's own frame scanner (waitForResponse() in
  // LD2420GeoGab.cpp), just against the free parseLd2450Frame() function
  // instead of an inline scan.
  for (size_t start = 0; start + kLd2450FrameBytes <= rxLen_; ++start) {
    RadarState parsed;
    if (parseLd2450Frame(rxBuf_ + start, rxLen_ - start, parsed)) {
      parsed.lastUpdateMs = millis();
      state_ = parsed;

      size_t consumed = start + kLd2450FrameBytes;
      size_t remaining = rxLen_ - consumed;
      std::memmove(rxBuf_, rxBuf_ + consumed, remaining);
      rxLen_ = remaining;
      return;  // one frame is plenty for this poll() tick; the rest waits for the next one
    }
  }
}

RadarState Ld2450Sensor::getState() { return state_; }
