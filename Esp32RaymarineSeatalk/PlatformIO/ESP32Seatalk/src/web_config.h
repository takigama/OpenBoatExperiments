#pragma once

// Minimal config web server: WiFi join form (AP mode) or status + OTA
// controls (STA mode). No styling/JS framework yet - functional first,
// this is the thing every other feature (SignalK/MQTT checkboxes, demo
// mode) will eventually get added onto, not a finished UI.
namespace WebConfig {

void begin();

// Pumps the synchronous WebServer - call every loop() iteration.
void handleClient();

}  // namespace WebConfig
