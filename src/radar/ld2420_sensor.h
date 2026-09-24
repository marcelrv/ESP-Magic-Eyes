// ld2420_sensor.h — IRadarSensor implementation for the HLK-LD2420
// (distance/presence only, no angle), architecture plan §6, §10 Phase 5.
//
// Built on gsieben/LD2420GeoGab (see platformio.ini / PROGRESS.md "Radar
// library swap" note) — re-validated in Phase 5 against the actual installed
// library source (.pio/libdeps/<env>/LD2420GeoGab/src/) rather than assumed:
// it's a real, documented driver (full command-frame protocol, Energy/Simple/
// Debug modes, gate energy calibration, callbacks + poll getters), not a thin
// stub, so no fallback parser was needed. See PROGRESS.md Phase 5 notes for
// the full evaluation.
//
// Only presence + distance are surfaced (isPresent()/getLastDistance()) —
// this sensor has no per-target angle/position, matching plan §6's "LD2420
// has no angle" limitation. begin() puts the sensor into Energy mode
// (LD2420GeoGab's own recommended default) so getLastDistance() reflects its
// finer internal distance estimate rather than Simple mode's gate-centre
// approximation.

#pragma once

#include <LD2420GeoGab.h>

#include "radar/iradar_sensor.h"

class Ld2420Sensor : public IRadarSensor {
 public:
  bool begin() override;
  void poll() override;
  RadarState getState() override;

 private:
  LD2420GeoGab radar_;
  RadarState state_;
};
