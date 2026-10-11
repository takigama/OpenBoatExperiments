#include "panel.h"

#include <TobeCyd.h>
#include <TobeLog.h>

#include "touch.h"

namespace Panel {

namespace {

TFT_eSPI *s_tft = nullptr;
void (*s_repaint)() = nullptr;

void applySettings(TFT_eSPI &tft, const tobe::cyd::Settings &s) {
    // setRotation writes MADCTL with the compile-time colour order; write it again with the chosen one.
    // Landscape: 1 = MV, 3 = MX|MY|MV (the 180 degree flip).
    tft.setRotation(s.rotate180 ? 3 : 1);
    uint8_t madctl = TFT_MAD_MV | (s.rotate180 ? (TFT_MAD_MX | TFT_MAD_MY) : 0) | (s.bgr ? TFT_MAD_BGR : TFT_MAD_RGB);
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
    applySettings(tft, tobe::cyd::get());
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
