#pragma once

// Minimal web UI: status + OTA only (no WiFi-join flow - WiFi is
// hardcoded via secrets.h for this bench/R&D tool, see main.cpp). Same
// dark-mode styling and OTA section layout as Esp32RaymarineSeatalk, for
// consistency across the two projects.
namespace WebConfig {

void begin();
void handleClient();  // call every loop() iteration

}  // namespace WebConfig
