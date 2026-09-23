#pragma once

// Minimal config web server: WiFi join form (AP mode) or status + OTA
// controls (STA mode). No styling/JS framework yet - functional first,
// this is the thing every other feature (SignalK/MQTT checkboxes, demo
// mode) will eventually get added onto, not a finished UI.
namespace WebConfig {

void begin();

// Pumps the synchronous WebServer - call every loop() iteration.
void handleClient();

// Drives the nav-data test cycle (if toggled on via the web UI) - real
// instruments expect a continuous ~1/sec stream, not a one-shot burst, so
// this needs to run from the main loop rather than block inside an HTTP
// handler the way the lamp test does. Call every loop() iteration,
// alongside handleClient().
void tick();

}  // namespace WebConfig
