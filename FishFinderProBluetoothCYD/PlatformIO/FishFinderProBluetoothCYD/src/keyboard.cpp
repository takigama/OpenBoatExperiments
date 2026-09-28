#include "keyboard.h"

#include <ctype.h>
#include <string.h>

#include "touch.h"

namespace Keyboard {

namespace {

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kKeyBg = TFT_NAVY;
constexpr uint16_t kKeyBorder = TFT_DARKGREY;
constexpr uint16_t kText = TFT_WHITE;
constexpr uint16_t kFieldBg = TFT_BLACK;
constexpr uint16_t kFieldBorder = TFT_DARKGREY;

constexpr int kFieldY = 18, kFieldH = 20;
constexpr int kKeysTop = 44;
constexpr int kRowH = 39;  // 5 rows * 39 = 195, fits 44..239

enum class Mode { Lower, Upper, Symbols };

struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};

const char *kRow1Letters = "qwertyuiop";
const char *kRow2Letters = "asdfghjkl";
const char *kRow3Letters = "zxcvbnm";
const char *kRow1Symbols = "1234567890";
const char *kRow2Symbols = "-_=+[]{};";
const char *kRow3Symbols = "!@#$%^&*";

void drawKey(TFT_eSPI &tft, Rect r, const String &label, uint16_t bg = kKeyBg) {
    tft.fillRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, bg);
    tft.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, kKeyBorder);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kText, bg);
    tft.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
    tft.setTextDatum(TL_DATUM);
}

// Row of single-character keys, evenly dividing `width` px starting at
// x=0. width defaults to the full screen (rows 1/2 always span it all);
// row 3 in Symbols mode passes a narrower width to leave Backspace's
// space untouched instead of drawing symbol keys underneath it - see the
// comment above kBackBtn for why that overlap was a real bug, not just
// visual.
void drawCharRow(TFT_eSPI &tft, const char *chars, int y, bool upper, int width = 320) {
    int n = strlen(chars);
    int w = width / n;
    for (int i = 0; i < n; i++) {
        char c = upper ? toupper(chars[i]) : chars[i];
        drawKey(tft, {i * w, y, w, kRowH}, String(c));
    }
}

int charRowHit(const char *chars, int y, int px, int py, int width = 320) {
    if (py < y || py > y + kRowH) return -1;
    int n = strlen(chars);
    int w = width / n;
    int i = px / w;
    if (i < 0 || i >= n) return -1;
    return i;
}

void drawField(TFT_eSPI &tft, const String &prompt, const String &text, bool masked) {
    tft.fillRect(0, 0, 320, kFieldY + kFieldH, kBg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_DARKGREY, kBg);
    tft.drawString(prompt, 4, 2);

    tft.drawRect(4, kFieldY, 312, kFieldH, kFieldBorder);
    String shown = masked ? String("") : text;
    if (masked) {
        for (size_t i = 0; i < text.length(); i++) shown += "*";
    }
    tft.setTextColor(kText, kFieldBg);
    tft.drawString(shown, 8, kFieldY + 3);
}

}  // namespace

String run(TFT_eSPI &tft, const String &prompt, const String &initial, bool masked) {
    String text = initial;
    Mode mode = Mode::Lower;

    // Wider than the letter keys - gives real touch imprecision more
    // margin around the two keys someone leans on hardest (backspace to
    // fix typos, shift to capitalize).
    constexpr Rect kShiftBtn{0, kKeysTop + kRowH * 2, 56, kRowH};
    constexpr Rect kBackBtn{320 - 56, kKeysTop + kRowH * 2, 56, kRowH};
    constexpr Rect kModeBtn{0, kKeysTop + kRowH * 3, 64, kRowH};
    constexpr Rect kSpaceBtn{64, kKeysTop + kRowH * 3, 160, kRowH};
    constexpr Rect kEnterBtn{224, kKeysTop + kRowH * 3, 96, kRowH};

    auto redraw = [&]() {
        tft.fillScreen(kBg);
        drawField(tft, prompt, text, masked);

        bool upper = mode == Mode::Upper;
        const char *r1 = mode == Mode::Symbols ? kRow1Symbols : kRow1Letters;
        const char *r2 = mode == Mode::Symbols ? kRow2Symbols : kRow2Letters;
        const char *r3 = mode == Mode::Symbols ? kRow3Symbols : kRow3Letters;

        drawCharRow(tft, r1, kKeysTop, upper);
        drawCharRow(tft, r2, kKeysTop + kRowH, upper);

        if (mode == Mode::Symbols) {
            drawCharRow(tft, r3, kKeysTop + kRowH * 2, false, 320 - kBackBtn.w);
        } else {
            drawKey(tft, kShiftBtn, "Shift", mode == Mode::Upper ? TFT_OLIVE : kKeyBg);
            int w = (320 - kShiftBtn.w - kBackBtn.w) / (int)strlen(r3);
            for (size_t i = 0; i < strlen(r3); i++) {
                char c = upper ? toupper(r3[i]) : r3[i];
                drawKey(tft, {kShiftBtn.w + (int)i * w, kKeysTop + kRowH * 2, w, kRowH}, String(c));
            }
        }
        drawKey(tft, kBackBtn, "Bksp");
        drawKey(tft, kModeBtn, mode == Mode::Symbols ? "ABC" : "123");
        drawKey(tft, kSpaceBtn, "Space");
        drawKey(tft, kEnterBtn, "Done", TFT_DARKGREEN);
    };

    redraw();

    for (;;) {
        int x, y;
        if (!Touch::pollTap(&x, &y)) {
            delay(10);
            continue;
        }

        bool upper = mode == Mode::Upper;
        const char *r1 = mode == Mode::Symbols ? kRow1Symbols : kRow1Letters;
        const char *r2 = mode == Mode::Symbols ? kRow2Symbols : kRow2Letters;
        const char *r3 = mode == Mode::Symbols ? kRow3Symbols : kRow3Letters;

        // Fixed-position special keys are checked FIRST, each against its
        // own exact rect, before any of the char-row fallback logic runs -
        // previously these were two independent if-chains, so a tap that
        // landed just outside kBackBtn's rect (ordinary touch imprecision)
        // fell through into the row-3 letter math instead of just missing
        // cleanly, silently typing a character instead of backspacing.
        // Checking these first and unconditionally means a genuine hit on
        // one of them can never be reinterpreted as something else.
        int idx;
        if (kBackBtn.hit(x, y)) {
            if (text.length()) text.remove(text.length() - 1);
        } else if (kModeBtn.hit(x, y)) {
            mode = mode == Mode::Symbols ? Mode::Lower : Mode::Symbols;
        } else if (kSpaceBtn.hit(x, y)) {
            text += ' ';
        } else if (kEnterBtn.hit(x, y)) {
            return text;
        } else if (mode != Mode::Symbols && kShiftBtn.hit(x, y)) {
            mode = mode == Mode::Upper ? Mode::Lower : Mode::Upper;
        } else if ((idx = charRowHit(r1, kKeysTop, x, y)) >= 0) {
            text += upper ? (char)toupper(r1[idx]) : r1[idx];
        } else if ((idx = charRowHit(r2, kKeysTop + kRowH, x, y)) >= 0) {
            text += upper ? (char)toupper(r2[idx]) : r2[idx];
        } else if (mode == Mode::Symbols &&
                   (idx = charRowHit(r3, kKeysTop + kRowH * 2, x, y, 320 - kBackBtn.w)) >= 0) {
            text += r3[idx];
        } else if (mode != Mode::Symbols && y >= kKeysTop + kRowH * 2 && y <= kKeysTop + kRowH * 3) {
            int w = (320 - kShiftBtn.w - kBackBtn.w) / (int)strlen(r3);
            int i = (x - kShiftBtn.w) / w;
            if (i >= 0 && i < (int)strlen(r3)) text += upper ? (char)toupper(r3[i]) : r3[i];
        }

        redraw();
    }
}

}  // namespace Keyboard
