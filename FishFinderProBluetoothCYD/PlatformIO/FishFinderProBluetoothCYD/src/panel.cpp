#include "panel.h"

#include <TobeCyd.h>
#include <TobeLog.h>

#include "touch.h"

namespace Panel {

namespace {

TFT_eSPI *s_tft = nullptr;
void (*s_repaint)() = nullptr;

void cmd(TFT_eSPI &t, uint8_t c, std::initializer_list<uint8_t> data) {
    t.writecommand(c);
    for (uint8_t d : data) t.writedata(d);
}

// TFT_eSPI is built for the ILI9341_2 driver (the original CYD's), and tft.init() has just run its init. A CYD2USB
// has an ST7789V: reset it and give it its own init - TFT_eSPI's ST7789_Init.h (the JLX240 sequence the community's
// cyd2usb recipe uses) - instead of leaving it on the ILI9341 one. Inversion is left to the display setting (the
// recipe's own INVON is cancelled by its TFT_INVERSION_OFF anyway).
void st7789Init(TFT_eSPI &t) {
    cmd(t, 0x01, {});  // SWRESET
    delay(150);
    cmd(t, 0x11, {});  // SLPOUT
    delay(120);
    cmd(t, 0x13, {});  // NORON
    cmd(t, 0x36, {TFT_MAD_BGR});
    cmd(t, 0xB6, {0x0A, 0x82});
    cmd(t, 0xB0, {0x00, 0xE0});  // RAMCTRL
    cmd(t, 0x3A, {0x55});        // COLMOD: 16 bit
    delay(10);
    cmd(t, 0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33});  // PORCTRL
    cmd(t, 0xB7, {0x35});                          // GCTRL
    cmd(t, 0xBB, {0x28});                          // VCOMS
    cmd(t, 0xC0, {0x0C});                          // LCMCTRL
    cmd(t, 0xC2, {0x01, 0xFF});                    // VDVVRHEN
    cmd(t, 0xC3, {0x10});                          // VRHS
    cmd(t, 0xC4, {0x20});                          // VDVSET
    cmd(t, 0xC6, {0x0F});                          // FRCTR2
    cmd(t, 0xD0, {0xA4, 0xA1});                    // PWCTRL1
    cmd(t, 0xE0, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17});  // PVGAMCTRL
    cmd(t, 0xE1, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E});  // NVGAMCTRL
    cmd(t, 0x2A, {0x00, 0x00, 0x00, 0xEF});
    cmd(t, 0x2B, {0x00, 0x00, 0x01, 0x3F});
    delay(120);
    cmd(t, 0x29, {});  // DISPON
    delay(120);
}

void applySettings(TFT_eSPI &tft, const tobe::cyd::Settings &s) {
    // setRotation sets the width/height (landscape) and writes MADCTL for the ILI9341; write MADCTL again for the
    // panel and colour order chosen. Landscape: ILI9341 = MV, ST7789 = MX|MV; the 180 degree flip flips MX and MY.
    tft.setRotation(s.rotate180 ? 3 : 1);
    uint8_t madctl = TFT_MAD_MV | (s.controller == tobe::cyd::CTRL_ST7789 ? TFT_MAD_MX : 0) |
                     (s.bgr ? TFT_MAD_BGR : TFT_MAD_RGB);
    if (s.rotate180) madctl ^= (TFT_MAD_MX | TFT_MAD_MY);
    tft.writecommand(TFT_MADCTL);
    tft.writedata(madctl);
    tft.invertDisplay(s.invert);
}

void onApply(const tobe::cyd::Settings &s) {
    if (!s_tft) return;
    applySettings(*s_tft, s);
    if (s_repaint) s_repaint();
}

// Four colour bars + a white one and the words, so inversion / colour order can be judged at a glance.
bool onTest() {
    if (!s_tft) return false;
    TFT_eSPI &t = *s_tft;
    const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
    const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
    for (int i = 0; i < 4; i++) {
        t.fillRect(i * 80, 0, 80, 200, bars[i]);
        t.setTextDatum(MC_DATUM);
        t.setTextFont(2);
        t.setTextColor(i == 3 ? TFT_BLACK : TFT_WHITE, bars[i]);
        t.drawString(names[i], i * 80 + 40, 100);
    }
    t.fillRect(0, 200, 320, 40, TFT_BLACK);
    t.setTextColor(TFT_WHITE, TFT_BLACK);
    t.drawString("each bar must be the colour it says", 160, 220);
    t.setTextDatum(TL_DATUM);
    delay(4000);
    if (s_repaint) s_repaint();
    return true;
}

}  // namespace

void init(TFT_eSPI &tft, void (*repaint)()) {
    tft.init();
    const tobe::cyd::Settings &s = tobe::cyd::get();
    if (s.controller == tobe::cyd::CTRL_ST7789) st7789Init(tft);
    applySettings(tft, s);
    if (repaint) {  // a real screen, not a throwaway instance
        s_tft = &tft;
        s_repaint = repaint;
    }
}

void begin() {
    tobe::cyd::Config c;
    c.apply = onApply;
    c.test = onTest;
    c.rotationChanged = Touch::forgetCalibration;  // the touch map no longer fits; asked for again after the restart
    tobe::cyd::begin(c);
}

}  // namespace Panel
