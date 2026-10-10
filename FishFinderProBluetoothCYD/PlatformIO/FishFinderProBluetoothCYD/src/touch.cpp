#include "touch.h"

#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include <TobeLog.h>

namespace Touch {

namespace {

constexpr int kIrqPin = 36;
constexpr int kMosiPin = 32;
constexpr int kMisoPin = 39;
constexpr int kClkPin = 25;
constexpr int kCsPin = 33;

// VSPI, not HSPI - the display already claims HSPI (see platformio.ini's
// USE_HSPI_PORT), and touch is confirmed on a genuinely separate SPI
// peripheral/pin set from the display, so it gets the other one.
SPIClass s_touchSpi(VSPI);
XPT2046_Touchscreen s_ts(kCsPin, kIrqPin);
bool s_wasTouched = false;

// Naive fallback (assumes the raw ADC range spans 0-4095 linearly across
// the screen) - replaced by runCalibration()'s real values the moment
// calibration data exists in NVS. Confirmed wrong in practice: taps
// across a ~140px-wide on-screen button only produced ~50px of spread
// under this assumption, so it's a starting point only, not trusted for
// anything needing real precision (like keyboard keys).
float s_scaleX = 319.0f / 4095.0f, s_offsetX = 0;
float s_scaleY = 239.0f / 4095.0f, s_offsetY = 0;

bool rawTouch(int *rx, int *ry, int *rz) {
    // See the fuller comment this replaced in an earlier version of this
    // file: the library's update() silently stops taking readings after
    // the first "not touched" check unless an interrupt on the IRQ pin
    // (which doesn't seem to fire on this board) resets its internal
    // `isrWake` flag. Forcing it true every poll turns this into a plain
    // polling driver instead.
    s_ts.isrWake = true;
    if (!s_ts.touched()) return false;
    TS_Point p = s_ts.getPoint();
    *rx = p.x;
    *ry = p.y;
    *rz = p.z;
    return true;
}

// Blocking: waits out any touch already in progress, then waits for a
// fresh press and returns its raw coordinates. Used by calibration only -
// pollTap() below is the non-blocking one everything else uses.
void waitForRawTap(int *rx, int *ry) {
    int x, y, z;
    while (rawTouch(&x, &y, &z)) delay(20);
    while (!rawTouch(&x, &y, &z)) delay(20);
    *rx = x;
    *ry = y;
    delay(30);  // debounce - let the mechanical/resistive contact settle
}

void drawCrosshair(TFT_eSPI &tft, int x, int y) {
    tft.drawFastHLine(x - 10, y, 21, TFT_YELLOW);
    tft.drawFastVLine(x, y - 10, 21, TFT_YELLOW);
}

// Standard 2-point calibration: tap two known on-screen points, derive
// the linear scale+offset that maps this panel's real raw ADC range onto
// screen pixels, save it to NVS so this only ever needs to happen once
// per board. Owns its own TFT_eSPI instance rather than taking one in -
// harmless (TFT_eSPI instances are thin wrappers over the same
// hardware), and keeps this file self-contained.
void runCalibration() {
    TFT_eSPI tft;
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Touch calibration - tap the crosshair", 160, 200);

    constexpr int kX1 = 20, kY1 = 20, kX2 = 300, kY2 = 220;
    drawCrosshair(tft, kX1, kY1);
    int rx1, ry1;
    waitForRawTap(&rx1, &ry1);

    tft.fillScreen(TFT_BLACK);
    tft.drawString("Touch calibration - tap the crosshair", 160, 20);
    drawCrosshair(tft, kX2, kY2);
    int rx2, ry2;
    waitForRawTap(&rx2, &ry2);

    s_scaleX = float(kX2 - kX1) / float(rx2 - rx1);
    s_offsetX = kX1 - rx1 * s_scaleX;
    s_scaleY = float(kY2 - kY1) / float(ry2 - ry1);
    s_offsetY = kY1 - ry1 * s_scaleY;

    Preferences p;
    p.begin("touchcal", false);
    p.putFloat("sx", s_scaleX);
    p.putFloat("ox", s_offsetX);
    p.putFloat("sy", s_scaleY);
    p.putFloat("oy", s_offsetY);
    p.end();

    tobe::logf("touch: calibrated sx=%.4f ox=%.1f sy=%.4f oy=%.1f", s_scaleX, s_offsetX, s_scaleY, s_offsetY);

    tft.fillScreen(TFT_BLACK);
    tft.drawString("Calibration saved", 160, 120);
    delay(600);
}

}  // namespace

void begin() {
    s_touchSpi.begin(kClkPin, kMisoPin, kMosiPin, kCsPin);
    s_ts.begin(s_touchSpi);
    s_ts.setRotation(1);  // match the display's tft.setRotation(1)

    Preferences p;
    p.begin("touchcal", /*readOnly=*/true);
    bool haveCal = p.isKey("sx");
    if (haveCal) {
        s_scaleX = p.getFloat("sx", s_scaleX);
        s_offsetX = p.getFloat("ox", s_offsetX);
        s_scaleY = p.getFloat("sy", s_scaleY);
        s_offsetY = p.getFloat("oy", s_offsetY);
    }
    p.end();

    if (!haveCal) runCalibration();
}

bool pollTap(int *x, int *y) {
    int rx, ry, rz;
    bool touched = rawTouch(&rx, &ry, &rz);
    bool isNewTap = touched && !s_wasTouched;
    s_wasTouched = touched;

    static uint32_t lastLog = 0;
    if (millis() - lastLog >= 500) {
        lastLog = millis();
        tobe::logf("touch: touched=%d", touched);
    }

    if (!isNewTap) return false;

    int px = constrain(int(rx * s_scaleX + s_offsetX), 0, 319);
    int py = constrain(int(ry * s_scaleY + s_offsetY), 0, 239);
    tobe::logf("touch: tap raw x=%d y=%d z=%d -> screen x=%d y=%d", rx, ry, rz, px, py);
    *x = px;
    *y = py;
    return true;
}

}  // namespace Touch
