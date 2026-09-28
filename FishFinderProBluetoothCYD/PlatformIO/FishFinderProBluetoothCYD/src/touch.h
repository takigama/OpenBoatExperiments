#pragma once

#include <Arduino.h>

// XPT2046 resistive touch - confirmed on its own SEPARATE hardware SPI
// bus from the display (SCK=25, MOSI=32, MISO=39, CS=33), not sharing
// the display's MISO/MOSI/SCLK - see platformio.ini/README. Uses the
// XPT2046_Touchscreen library rather than TFT_eSPI's built-in touch
// support, since that assumes touch shares the display's own SPI bus.
namespace Touch {

void begin();

// True once per new touch-down (not held/repeating) - simple edge
// detection, good enough for tap-a-button UI. x/y are already in screen
// pixel coordinates (0-319, 0-239), matching the display's rotation.
bool pollTap(int *x, int *y);

}  // namespace Touch
