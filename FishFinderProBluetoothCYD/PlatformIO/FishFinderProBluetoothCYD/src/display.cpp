#include "display.h"

#include <TFT_eSPI.h>

namespace Display {

namespace {

// Touch (XPT2046) is confirmed on its own separate SPI bus (SCK=25,
// MOSI=32, MISO=39, CS=33 - checked against ../../EngineControl's
// already-working config for this same board), not sharing the display's
// SPI bus at all. SD slot pins aren't confirmed yet. Neither is wired up
// in this project yet (see README "Next steps") - driven high here
// anyway as harmless defensive practice for whenever they are.
constexpr int kTouchCsPin = 33;
constexpr int kSdCsPin = 5;

TFT_eSPI tft;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kLabel = TFT_DARKGREY;
constexpr uint16_t kValue = TFT_WHITE;
constexpr uint16_t kWarn = TFT_YELLOW;
constexpr uint16_t kOk = TFT_GREEN;
constexpr uint16_t kBad = TFT_RED;
constexpr uint16_t kCurrent = TFT_CYAN;  // the single newest reading, column "t"

// setTextColor(fg, bg) makes TFT_eSPI erase the previous glyph pixels
// automatically on redraw, as long as the new string is padded to at
// least the old one's width - avoids a full-screen clear (and the
// flicker that'd cause) on every update() at ~4.5Hz.
void printPadded(const String &s, int minLen) {
    String padded = s;
    while (padded.length() < (size_t)minLen) padded += ' ';
    tft.print(padded);
}

// --- Status bar icons ---------------------------------------------------

constexpr int kStatusBarH = 26;

// Raw battery scale is 0-6 (see FishFinderProBluetooth's README, "Frame
// format") - 6 segments matches that directly, one per level.
void drawBatteryIcon(int x, int y, int level) {
    constexpr int w = 34, h = 16, nub = 3;
    tft.drawRoundRect(x, y, w, h, 3, kValue);
    tft.fillRect(x + w, y + 4, nub, h - 8, kValue);

    uint16_t color = level >= 3 ? kOk : (level == 2 ? kWarn : kBad);
    constexpr int segW = 4, segGap = 1, segH = h - 6;
    for (int i = 0; i < 6; i++) {
        int segX = x + 3 + i * (segW + segGap);
        int segY = y + 3;
        if (i < level) {
            tft.fillRect(segX, segY, segW, segH, color);
        } else {
            // Must clear to background first - a segment that was lit
            // (filled) on a previous draw and has since dropped below
            // `level` would otherwise keep showing its old fill color
            // forever, since drawRect only touches the border.
            tft.fillRect(segX, segY, segW, segH, kBg);
            tft.drawRect(segX, segY, segW, segH, kLabel);
        }
    }
}

// 4 ascending bars, phone-style. bars=0 means "not connected" (all
// hollow) - RSSI thresholds below are rough, not calibrated against this
// specific radio.
int rssiToBars(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

void drawSignalIcon(int x, int y, int bars) {
    constexpr int barW = 4, gap = 2, baseH = 4, stepH = 4, iconH = 16;
    for (int i = 0; i < 4; i++) {
        int h = baseH + i * stepH;
        int bx = x + i * (barW + gap);
        int by = y + (iconH - h);
        if (i < bars) {
            tft.fillRect(bx, by, barW, h, kValue);
        } else {
            tft.drawRect(bx, by, barW, h, kLabel);
        }
    }
}

constexpr int kUpdateBtnX = 90, kUpdateBtnY = 2, kUpdateBtnW = 140, kUpdateBtnH = 22;

void drawUpdateButton() {
    tft.drawRoundRect(kUpdateBtnX, kUpdateBtnY, kUpdateBtnW, kUpdateBtnH, 4, kLabel);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("Check Update", kUpdateBtnX + kUpdateBtnW / 2, kUpdateBtnY + kUpdateBtnH / 2);
    tft.setTextDatum(TL_DATUM);
}

void drawStatusBar(const SonarBle::Reading &reading, bool bleConnected) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kValue, kBg);
    tft.fillRect(0, 0, 62, kStatusBarH, kBg);  // temp text's own width, cleared before redraw
    tft.setCursor(4, 6);
    printPadded(String(reading.tempC, 1) + "C", 6);

    drawUpdateButton();
    drawBatteryIcon(248, 5, reading.battery);
    drawSignalIcon(294, 5, bleConnected ? rssiToBars(reading.rssi) : 0);

    tft.drawFastHLine(0, kStatusBarH, 320, kLabel);
}

// --- Depth/fish-depth rolling history table ------------------------------
//
// 10 columns per block (newest on the left), stacked as 4 blocks going
// further back in time top-to-bottom - the screen's free space below the
// status bar fits 40 total samples this way instead of just 10.

constexpr int kColsPerBlock = 10;
constexpr int kBlocks = 4;
constexpr int kHistoryLen = kColsPerBlock * kBlocks;
constexpr int kLabelColW = 22;
constexpr int kColW = (320 - kLabelColW) / kColsPerBlock;
constexpr int kHeaderY = kStatusBarH + 4;
constexpr int kRowH = 20;
constexpr int kBlockGap = 6;
constexpr int kBlockStep = 2 * kRowH + kBlockGap;
constexpr int kFirstBlockY = kHeaderY + 16;

struct HistEntry {
    bool depthValid;
    float depthM;
    bool fishValid;
    float fishM;
};
HistEntry s_history[kHistoryLen] = {};
uint32_t s_lastFrameCount = 0;

// New reading goes in index 0 (top-left = "t", most recent); everything
// else shifts along the flattened array and the oldest entry (bottom
// block, rightmost column) falls off the end - a rolling window, not a
// full log.
void pushHistory(const SonarBle::Reading &reading) {
    for (int i = kHistoryLen - 1; i > 0; i--) s_history[i] = s_history[i - 1];
    s_history[0] = {reading.valid && !reading.outOfWater, reading.depthM, reading.fishValid, reading.fishDepthM};
}

// Column headers only make sense for the top (most recent) block - lower
// blocks are just "further back", not worth re-labeling with correct
// offsets for the little the extra text would add.
void drawHistoryHeader() {
    tft.setTextDatum(TC_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(kLabel, kBg);
    for (int i = 0; i < kColsPerBlock; i++) {
        int cx = kLabelColW + i * kColW + kColW / 2;
        String label = i == 0 ? String("t") : ("-" + String(i));
        tft.drawString(label, cx, kHeaderY);
    }
    tft.setTextDatum(TL_DATUM);
}

// histOffset selects which slice of s_history this row reads from - 0
// for the newest block, kColsPerBlock for the next-oldest, etc.
void drawHistoryRow(int y, const char *label, bool isFish, int histOffset) {
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(label, 2, y);

    tft.setTextDatum(TC_DATUM);
    for (int i = 0; i < kColsPerBlock; i++) {
        const HistEntry &h = s_history[histOffset + i];
        bool valid = isFish ? h.fishValid : h.depthValid;
        float m = isFish ? h.fishM : h.depthM;
        bool isCurrent = histOffset == 0 && i == 0;  // column "t" of the top block - the single newest reading
        tft.setTextColor(isCurrent ? kCurrent : kValue, kBg);
        int cx = kLabelColW + i * kColW + kColW / 2;
        tft.fillRect(kLabelColW + i * kColW, y, kColW, kRowH, kBg);
        tft.drawString(valid ? String(m, 0) : "--", cx, y);
    }
    tft.setTextDatum(TL_DATUM);
}

void drawHistoryBlocks() {
    for (int b = 0; b < kBlocks; b++) {
        int y = kFirstBlockY + b * kBlockStep;
        drawHistoryRow(y, "S", false, b * kColsPerBlock);
        drawHistoryRow(y + kRowH, "F", true, b * kColsPerBlock);
        if (b > 0) tft.drawFastHLine(0, y - kBlockGap / 2, 320, kLabel);
    }
}

}  // namespace

void begin() {
    pinMode(kTouchCsPin, OUTPUT);
    digitalWrite(kTouchCsPin, HIGH);
    pinMode(kSdCsPin, OUTPUT);
    digitalWrite(kSdCsPin, HIGH);

    tft.init();
    // Landscape - see platformio.ini for how the earlier "this panel is
    // only 240 wide"/"unfixable corruption" conclusions both turned out
    // to be wrong. Full 320x240 genuinely works. Flip to 3 if the board
    // ends up mounted upside down.
    tft.setRotation(1);
    tft.fillScreen(kBg);
}

void update(const SonarBle::Reading &reading, bool bleConnected) {
    drawStatusBar(reading, bleConnected);

    if (!bleConnected) {
        tft.fillRect(0, kStatusBarH + 1, 320, 240 - kStatusBarH - 1, kBg);
        tft.setTextDatum(TL_DATUM);
        tft.setTextFont(4);
        tft.setTextColor(kWarn, kBg);
        tft.setCursor(10, 70);
        printPadded("Searching for sonar...", 20);
        s_lastFrameCount = 0;  // next connect always redraws the table header fresh
        return;
    }

    if (s_lastFrameCount == 0) {
        tft.fillRect(0, kStatusBarH + 1, 320, 240 - kStatusBarH - 1, kBg);
        drawHistoryHeader();
    }
    if (reading.frameCount != s_lastFrameCount) {
        s_lastFrameCount = reading.frameCount;
        pushHistory(reading);
    }
    drawHistoryBlocks();
}

bool isUpdateButtonAt(int x, int y) {
    // NOT the button's drawn bounds - the touch driver's raw-to-pixel
    // mapping is a naive linear 0-4095 assumption (see touch.cpp) that
    // turned out to have the wrong scale, not just an offset: repeated
    // taps across this button's full ~140px drawn width only produced
    // roughly a 50px spread in mapped coordinates, clustered around
    // x=130-180, y=29-44 - well below the button's own drawn y=2-24.
    // Rather than chase a proper calibration for one button, this hit
    // zone is set directly from that measured cluster, generously
    // padded, instead of from the button's own (differently-scaled)
    // drawn rectangle.
    return x >= 100 && x <= 220 && y >= 15 && y <= 60;
}

void drawCalibrationGrid() {
    tft.fillScreen(kBg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);

    for (int x = 0; x <= 319; x += 40) {
        tft.drawFastVLine(x, 0, 240, kBad);
        tft.setTextColor(kBad, kBg);
        tft.setCursor(x + 2, 2);
        tft.print(x);
    }
    tft.drawFastVLine(319, 0, 240, kBad);

    for (int y = 0; y <= 239; y += 40) {
        tft.drawFastHLine(0, y, 320, kOk);
        tft.setTextColor(kOk, kBg);
        tft.setCursor(2, y + 2 > 225 ? y - 12 : y + 2);
        tft.print(y);
    }
}

}  // namespace Display
