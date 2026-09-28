#pragma once

// Persistent WiFi/BLE mode selection, stored in NVS. Same reasoning and
// same pattern as FishFinderProBluetooth's op_mode.h: this board's ESP32
// has one radio shared between WiFi and BLE, confirmed unreliable to run
// both at once, so they're mutually exclusive per boot rather than
// live-switched within a running session. Switching mode writes the new
// choice to NVS and reboots, so setup() always starts from a clean slate
// with only the one radio subsystem it actually needs ever initialized.
//
// Defaults to BLE (not WiFi): this is a sonar display, so a fresh/
// unconfigured board should go straight to that. WiFi mode is opt-in,
// entered via the on-screen "Check Update" button, for occasional admin
// (config/OTA) only.
namespace OpMode {

enum class Mode { Wifi, Ble };

Mode current();
void switchTo(Mode mode);  // saves to NVS and reboots - does not return

}  // namespace OpMode
