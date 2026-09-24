#include "radar/ld2420_sensor.h"

#include <Arduino.h>

#include "pin_map.h"
#include "storage/nvs_store.h"

bool Ld2420Sensor::begin() {
  // LD2420GeoGab::begin(txPin, rxPin, baud) — ESP32 TX (RADAR_TX_PIN) wires
  // to the sensor's RX, ESP32 RX (RADAR_RX_PIN) wires to the sensor's TX
  // (pin_map.h). Baud is a runtime NVS value (NvsStore::getRadarBaudRate(),
  // default 115200), not a compile-time flag — the sensor's actual baud
  // depends on its firmware version (115200 for fw >= v1.5.3, 256000 for
  // older) and isn't knowable in advance, so this is settable from the
  // Radar setup page instead of requiring a rebuild+reflash per bring-up
  // attempt.
  //
  // Bounded blocking (~<=2.5s worst case: a 2s "wait for silence" flush
  // plus one 500ms command timeout — see LD2420GeoGab.cpp's
  // activateConfigMode()) if nothing responds on the UART. Safe here only
  // because this runs inside RadarTask's own dedicated FreeRTOS task
  // (radar_task.cpp), never from setup()/loop() — it can't delay boot, the
  // web server, or MotionTask.
  bool ok = radar_.begin(RADAR_TX_PIN, RADAR_RX_PIN, NvsStore::getRadarBaudRate());
  if (ok) {
    // Energy mode: finer internal distance estimate + gate energies, the
    // library's own "recommended" mode (see LD2420GeoGab.h's setSystemMode
    // doc). Best-effort — if this particular activate/set/deactivate
    // sequence fails, radar_.update()/isPresent()/getLastDistance() still
    // work against whatever mode the sensor last had saved to its internal
    // flash, so a failure here isn't treated as fatal.
    if (radar_.activateConfigMode() == LD2420Error::None) {
      radar_.setSystemMode(LD2420SystemMode::Energy);
      radar_.deactivateConfigMode();
    }
  }
  // Deliberately return `ok` (not swallow the failure) so RadarTask can log
  // it, but poll() below keeps working either way — see this class's header
  // note and IRadarSensor::begin()'s doc comment: a sensor that isn't wired
  // up yet, or that shows up late, must not be treated as a permanent dead
  // end.
  return ok;
}

void Ld2420Sensor::poll() {
  // Non-blocking: LD2420GeoGab::update() only ever drains whatever bytes
  // are already sitting in the UART FIFO (Serial2.available()/read() in a
  // tight bounded loop) and returns immediately if there's nothing to read
  // — confirmed directly against
  // .pio/libdeps/ld2420/LD2420GeoGab/src/LD2420GeoGab.cpp's update(). Safe
  // to call every poll() tick even if begin() failed or no sensor is
  // physically wired up (sensorSerial is still a valid, just silent, UART
  // port in that case) — this is exactly the "no data ever arrives"
  // hardware case the task spec calls out.
  radar_.update();

  if (!radar_.newDataAvailable()) {
    return;
  }

  state_ = RadarState{};
  state_.presence = radar_.isPresent();
  if (state_.presence) {
    RadarTarget &t = state_.targets[0];
    // cm -> mm; no angle/x/y/speed available from this sensor (plan §6) —
    // left std::nullopt, not zeroed, so callers can tell "not provided"
    // apart from "provided as zero".
    t.distanceMm = static_cast<float>(radar_.getLastDistance()) * 10.0f;
    state_.targetCount = 1;
  }
  state_.lastUpdateMs = millis();
}

RadarState Ld2420Sensor::getState() { return state_; }
