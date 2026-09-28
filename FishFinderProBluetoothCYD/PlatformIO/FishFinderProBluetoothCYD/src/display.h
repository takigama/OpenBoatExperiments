#pragma once

#include "sonar_ble.h"

// TFT_eSPI-driven UI on the CYD's built-in 2.8" screen: status bar
// (temp/battery/signal + a "Check Update" button that reboots into WiFi
// mode - see op_mode.h) plus a scrolling depth/fish-depth history table.
namespace Display {

void begin();
void update(const SonarBle::Reading &reading, bool bleConnected);

// True if (x,y) - in the same touch-driver screen coordinates as
// Touch::pollTap() - falls within the "Check Update" button drawn in the
// status bar. main.cpp owns actually acting on a tap (switching modes);
// this just knows where its own button is.
bool isUpdateButtonAt(int x, int y);

// One-off diagnostic: draws labeled reference lines across the full
// nominal 320x240 landscape canvas so the true physical edge of this
// panel can be measured directly against a photo, instead of guessed at
// - see platformio.ini's history of wrong guesses for why. Not called
// from normal operation.
void drawCalibrationGrid();

}  // namespace Display
