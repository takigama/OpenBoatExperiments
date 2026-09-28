#pragma once

// The WiFi-mode screen: entered only via OpMode::Mode::Wifi (the
// on-screen "Check Update" button in BLE mode reboots into this). Joins
// saved WiFi (or shows AP-mode setup instructions), checks for an OTA
// update, and shows a touch button for "Do OTA" / "Back to Sonar".
// Separate from Display/BLE mode entirely - the two are never active in
// the same boot (see op_mode.h).
namespace OtaUi {

void begin();  // blocks briefly (WiFi join attempt) - call once from setup()
void loop();   // call every loop() iteration while in WiFi mode

}  // namespace OtaUi
