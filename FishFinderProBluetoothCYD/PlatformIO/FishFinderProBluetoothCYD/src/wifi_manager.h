#pragma once

#include <Arduino.h>

// Joins the SSID/password saved in NVS (Preferences, namespace "wifi").
// Falls back to a SoftAP ("FishFinderCYD-XXXX", open, at 192.168.4.1)
// when there's nothing saved, or the join times out - either way, the
// caller (main.cpp) should start the config web server regardless of
// which mode this lands in, since AP mode needs it to receive new
// credentials. Same runtime-NVS pattern as FishFinderProBluetooth's
// wifi_manager.h - see that file for the full "why not compile-time
// secrets" reasoning, which applies identically here.
namespace WifiManager {

enum class Mode { STA, AP };

// Blocks for up to ~15s attempting to join saved credentials before
// falling back to AP. Called once on entering WiFi mode.
Mode begin();

Mode currentMode();

// Persists new credentials and reboots to attempt joining them.
void saveCredentialsAndReboot(const String &ssid, const String &password);

// AP-mode SSID, e.g. "FishFinderCYD-A4CF" (last two MAC bytes).
String apSsid();

}  // namespace WifiManager
