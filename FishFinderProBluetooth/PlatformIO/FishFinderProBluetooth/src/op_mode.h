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
namespace OpMode {

enum class Mode { Wifi, Ble };

Mode current();
void switchTo(Mode mode);  // saves to NVS and reboots - does not return

}  // namespace OpMode
