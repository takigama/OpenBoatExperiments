#pragma once

// Minimal web UI, entered only while in WiFi mode (see op_mode.h): WiFi
// credential entry when in AP mode, status + OTA check/apply when
// joined. Same dark-mode styling as FishFinderProBluetooth/
// Esp32RaymarineSeatalk, copied rather than shared since these are
// separate firmware projects with no common library between them.
namespace WebConfig {

void begin();
void handleClient();  // call every loop() iteration while in WiFi mode

}  // namespace WebConfig
