#pragma once

// Persistent WiFi/BLE mode selection, stored in NVS. The ESP32-C3 has one
// radio shared between WiFi and BLE - confirmed via isolation testing that
// they can't run reliably at once (WiFi TX becomes unreliable the moment
// BLE is actively streaming, even after sequencing BLE to start only after
// WiFi joins) - so the two are mutually exclusive per boot rather than
// live-switched within a running session. Switching mode writes the new
// choice to NVS and reboots, exactly like WifiManager's credential-save
// flow, so setup() always starts from a clean slate with only the one
// radio subsystem it actually needs ever initialized.
//
// Defaults to BLE (not WiFi): the real deployment is a wired serial link
// into an RPi running OpenPlotter, so a fresh/unconfigured board should
// go straight to streaming sonar data. WiFi mode is opt-in, entered via
// the serial "wifi" command, for occasional admin (config/OTA) only.
namespace OpMode {

enum class Mode { Wifi, Ble };

Mode current();
void switchTo(Mode mode);  // saves to NVS and reboots - does not return

}  // namespace OpMode
