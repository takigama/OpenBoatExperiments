#pragma once

#include <Arduino.h>

// Joins the SSID/password saved in NVS (Preferences, namespace "wifi").
// Falls back to a SoftAP ("ESP32Seatalk-XXXX", open, at 192.168.4.1) when
// there's nothing saved, or the join times out - either way, the caller
// (main.cpp) should start the config web server regardless of which mode
// this lands in, since AP mode needs it to receive new credentials and STA
// mode needs it for the rest of the device's config screens.
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

// AP-mode SSID, e.g. "ESP32Seatalk-A4CF" (last two MAC bytes) - used by
// both the AP setup itself and the config page's "you're on AP mode"
// banner.
String apSsid();

}  // namespace WifiManager
