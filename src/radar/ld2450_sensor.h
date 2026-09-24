// ld2450_sensor.h — IRadarSensor implementation for the HLK-LD2450
// (up to 3 targets, position/angle/speed), architecture plan §6, §10
// Phase 5.
//
// Custom parser (no library dependency — plan §6: the frame format is small
// and fully specified) for the LD2450's target-output frame:
//   header `AA FF 03 00` + 3 x 8-byte target blocks + footer `55 CC`
//   (30 bytes total), 256000 baud.
// Each target block (little-endian): X int16, Y int16, speed int16,
// distance-resolution uint16.
//
// NOT hardware-tested this phase — no physical LD2450 exists yet (plan's
// "future upgrade" note); this driver only needs to compile and parse
// correctly against synthetic/no data. parseLd2450Frame() below is
// deliberately a free pure function (buffer in, RadarState out) so it's
// unit-testable in isolation from UART I/O later, per the task spec, even
// though no test harness is set up in this phase.

#pragma once

#include <cstddef>
#include <cstdint>

#include "radar/iradar_sensor.h"

// LD2450 default UART baud (plan §6). Sensor firmware doesn't have the
// LD2420's baud-version split as far as current research shows, so this
// isn't build-flag-overridable the way GG_BAUDRATE is for LD2420 — revisit
// if bring-up with a real module finds otherwise.
constexpr uint32_t kLd2450BaudRate = 256000;

// Total on-wire frame size: 4-byte header + 3 x 8-byte target blocks +
// 2-byte footer.
constexpr size_t kLd2450FrameBytes = 4 + 3 * 8 + 2;

// Decodes one LD2450 coordinate/speed field's non-standard sign encoding
// (not two's complement). Per the HLK-LD2450 manual's worked example and
// ESPHome's ld2450 driver: the high bit (0x8000) set means POSITIVE, with
// the magnitude in the low 15 bits (Y 0x86B1 -> +1713 mm); high bit clear
// means NEGATIVE, magnitude = the raw value (X 0x030E -> -782 mm). A target
// in front of the sensor therefore always has Y's high bit set. Speed is in
// cm/s, coordinates in mm.
int16_t decodeLd2450Signed(uint16_t raw);

// Pure parser: scans exactly `kLd2450FrameBytes` bytes starting at `buf` for
// a valid LD2450 frame (header + footer both present at the expected
// offsets) and, on success, fills `out` with up to 3 targets and returns
// true. An all-zero (X==0 && Y==0) target block is treated as "slot
// unused" (common LD2450 convention — not independently verified against
// physical hardware this phase, see class header) and skipped, so
// `out.targetCount` may be less than 3. Returns false (leaving `out`
// untouched) if `len < kLd2450FrameBytes` or the header/footer don't match
// at the start of `buf` — callers are expected to slide their scan window
// forward by one byte and retry on a false result (see Ld2450Sensor::poll()
// in the .cpp), the same "resync on any mismatch" approach as
// LD2420GeoGab's own frame scanner.
bool parseLd2450Frame(const uint8_t *buf, size_t len, RadarState &out);

class Ld2450Sensor : public IRadarSensor {
 public:
  bool begin() override;
  void poll() override;
  RadarState getState() override;

 private:
  // Frame is 30 bytes; this gives headroom for noise/partial frames between
  // resyncs without growing unboundedly.
  static constexpr size_t kRxBufSize = 128;
  uint8_t rxBuf_[kRxBufSize] = {};
  size_t rxLen_ = 0;
  RadarState state_;
};
