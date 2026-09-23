#pragma once

#include <Arduino.h>

// Joins the SSID/password saved in NVS (Preferences, namespace "wifi").
// Falls back to a SoftAP ("FishFinder-XXXX", open, at 192.168.4.1) when
// there's nothing saved, or the join times out - either way, the caller
// (main.cpp) should start the config web server regardless of which mode
// this lands in, since AP mode needs it to receive new credentials.
//
// Deliberately the same runtime-config pattern Esp32RaymarineSeatalk uses
// (NVS, not compiled-in) rather than the compile-time secrets.h/
// no_secrets.h split this project started with - that pattern is fine for
// firmware that's only ever flashed locally, but this project does
// GitHub-hosted OTA, which means the compiled .bin itself gets published.
// A hardcoded WiFi password would ship inside every release binary in
// plaintext, defeating the whole point of gitignoring secrets.h in the
// first place.
namespace WifiManager {

enum class Mode { STA, AP };

// Blocks for up to ~15s attempting to join saved credentials before
// falling back to AP. Called once from setup().
Mode begin();

Mode currentMode();

// Persists new credentials and reboots to attempt joining them - simplest
// way to get a clean WiFi stack restart rather than juggling teardown of
// whichever mode was previously active.
void saveCredentialsAndReboot(const String &ssid, const String &password);

// AP-mode SSID, e.g. "FishFinder-A4CF" (last two MAC bytes) - used by
// both the AP setup itself and the config page's "you're on AP mode"
// banner.
String apSsid();

}  // namespace WifiManager
