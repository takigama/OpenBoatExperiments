#pragma once

#include <TFT_eSPI.h>

// The panel, driven the way the stored display settings (tobe::cyd - see lib/TobeCyd) say: the same firmware
// runs on a CYD with an ILI9341 and on a CYD2USB with an ST7789V. TFT_eSPI is compiled for ILI9341_2 (which both
// accept); inversion, colour order and orientation are set at run time here, after every tft.init().
namespace Panel {

// tft.init(), then the stored settings. `repaint` is called after the settings change while this screen is up
// (the DISPLAY command, the web page): redraw everything - the panel's contents are not what the screen code
// thinks they are. Pass nullptr for a throwaway instance (touch calibration).
void init(TFT_eSPI &tft, void (*repaint)());

// Registers the DISPLAY command (and the settings with the web page). Once, after tobe::cli.begin().
void begin();

}  // namespace Panel
