// serial_console.h — minimal line-based command console on the USB serial
// port (115200 baud), mainly a recovery path for WiFi: with physical
// access, credentials can be set even when the device can't reach its
// network and nobody can get onto the setup AP.
//
// Commands (one per line, arguments with spaces in "double quotes"):
//   help                          list commands
//   status                        firmware, uptime, WiFi state, IP
//   wifi                          same as "wifi status"
//   wifi status                   WiFi mode, saved/connected SSID, IP, RSSI
//   wifi set <ssid> [password]    save credentials to NVS and connect now
//   wifi forget                   clear saved credentials and reboot to AP
//   reboot                        restart the device
//
// Non-blocking: handle() drains whatever serial input is available and
// returns; call it every loop() iteration.

#pragma once

namespace SerialConsole {

void begin();
void handle();

} // namespace SerialConsole
