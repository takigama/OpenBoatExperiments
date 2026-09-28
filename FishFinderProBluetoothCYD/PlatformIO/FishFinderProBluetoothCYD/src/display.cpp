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
constexpr uint16_t kSeafloor = 0x8A22;   // approx "saddle brown" in RGB565, for the waterfall view

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

// Both buttons here use their exact drawn rects for hit-testing (see
// isUpdateButtonAt()/isModeButtonAt()) - trusting the touch driver's real
// 2-point calibration (see touch.cpp) rather than the padded/shifted
// hit-zone hack an earlier version of this file needed before that
// calibration existed. Proven reliable since via the keyboard/WiFi
// picker screens, which already rely on exact-rect hits.
constexpr int kUpdateBtnX = 68, kUpdateBtnY = 2, kUpdateBtnW = 95, kUpdateBtnH = 22;
constexpr int kModeBtnX = 169, kModeBtnY = 2, kModeBtnW = 73, kModeBtnH = 22;

void drawUpdateButton() {
    tft.drawRoundRect(kUpdateBtnX, kUpdateBtnY, kUpdateBtnW, kUpdateBtnH, 4, kLabel);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("Check Update", kUpdateBtnX + kUpdateBtnW / 2, kUpdateBtnY + kUpdateBtnH / 2);
    tft.setTextDatum(TL_DATUM);
}

// Defined further down (after ViewMode/s_viewMode exist, see the "Sonar
// waterfall view" section) - forward-declared so drawStatusBar() below
// can call it.
void drawModeButton();

void drawStatusBar(const SonarBle::Reading &reading, bool bleConnected) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kValue, kBg);
    tft.fillRect(0, 0, 62, kStatusBarH, kBg);  // temp text's own width, cleared before redraw
    tft.setCursor(4, 6);
    printPadded(String(reading.tempC, 1) + "C", 6);

    drawUpdateButton();
    drawModeButton();
    drawBatteryIcon(248, 5, reading.battery);
    drawSignalIcon(294, 5, bleConnected ? rssiToBars(reading.rssi) : 0);

    tft.drawFastHLine(0, kStatusBarH, 320, kLabel);
}

// --- Depth/fish-depth rolling history -------------------------------------
//
// Shared by both view modes: the table (10 columns per block, 4 blocks
// stacked, newest at top-left) and the waterfall (one column of history
// per pixel across its drawable width - see further down). Sized for the
// waterfall's needs (the larger of the two); the table just reads the
// first kColsPerBlock*kBlocks entries of the same buffer.
constexpr int kHistoryLen = 300;

constexpr int kColsPerBlock = 10;
constexpr int kBlocks = 4;
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

// --- Sonar "waterfall" view ------------------------------------------------
//
// The classic scrolling fish-finder look: newest reading at the right
// edge, older readings scroll left as time passes, depth traced as a
// filled "seafloor" from the bottom of the screen up to the reading, fish
// drawn as a small dot at their own depth when detected. One history
// column per pixel across the drawable width - reuses the same
// s_history[] the table view does (see above), just reading further into
// it and drawing it differently.

enum class ViewMode { Table, Waterfall };
ViewMode s_viewMode = ViewMode::Table;
bool s_needsFullRedraw = true;

constexpr int kLegendW = 22;
constexpr int kWaterfallTop = kStatusBarH + 1;
constexpr int kWaterfallBottom = 239;
constexpr int kWaterfallH = kWaterfallBottom - kWaterfallTop;
constexpr int kWaterfallCols = 320 - kLegendW;  // <= kHistoryLen, checked with a static_assert below
static_assert(kWaterfallCols <= kHistoryLen, "history buffer too small for the waterfall's drawable width");

void drawWaterfall() {
    tft.fillRect(kLegendW, kWaterfallTop, kWaterfallCols, kWaterfallH, kBg);

    // Auto-scaled to whatever's actually in view, not the device's
    // configured range setting - simpler than trusting the range-index
    // mapping, and adapts naturally as the boat moves into deeper/
    // shallower water. Floor + headroom keep it from zooming in too hard
    // on a handful of very shallow readings.
    float maxDepth = 5.0f;
    for (int i = 0; i < kWaterfallCols; i++) {
        if (s_history[i].depthValid && s_history[i].depthM > maxDepth) maxDepth = s_history[i].depthM;
    }
    maxDepth *= 1.1f;

    for (int i = 0; i < kWaterfallCols; i++) {
        const HistEntry &h = s_history[i];
        int x = 319 - i;
        if (h.depthValid) {
            int y = kWaterfallTop + (int)((h.depthM / maxDepth) * kWaterfallH);
            y = constrain(y, kWaterfallTop, kWaterfallBottom);
            tft.drawFastVLine(x, y, kWaterfallBottom - y, kSeafloor);
        }
        if (h.fishValid) {
            int fy = kWaterfallTop + (int)((h.fishM / maxDepth) * kWaterfallH);
            fy = constrain(fy, kWaterfallTop, kWaterfallBottom);
            tft.fillCircle(x, fy, 2, kWarn);
        }
    }

    tft.fillRect(0, kWaterfallTop, kLegendW, kWaterfallH, kBg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("0", 2, kWaterfallTop);
    tft.drawString(String(maxDepth, 0), 2, kWaterfallBottom - 8);
    tft.setTextDatum(TL_DATUM);
}

void drawModeButton() {
    tft.drawRoundRect(kModeBtnX, kModeBtnY, kModeBtnW, kModeBtnH, 4, kLabel);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString(s_viewMode == ViewMode::Table ? "Sonar" : "Table", kModeBtnX + kModeBtnW / 2,
                    kModeBtnY + kModeBtnH / 2);
    tft.setTextDatum(TL_DATUM);
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
        s_needsFullRedraw = true;  // next connect always redraws fresh
        return;
    }

    if (s_needsFullRedraw) {
        s_needsFullRedraw = false;
        tft.fillRect(0, kStatusBarH + 1, 320, 240 - kStatusBarH - 1, kBg);
        if (s_viewMode == ViewMode::Table) drawHistoryHeader();
    }
    if (reading.frameCount != s_lastFrameCount) {
        s_lastFrameCount = reading.frameCount;
        pushHistory(reading);
    }
    if (s_viewMode == ViewMode::Table) {
        drawHistoryBlocks();
    } else {
        drawWaterfall();
    }
}

bool isUpdateButtonAt(int x, int y) {
    return x >= kUpdateBtnX && x <= kUpdateBtnX + kUpdateBtnW && y >= kUpdateBtnY && y <= kUpdateBtnY + kUpdateBtnH;
}

bool isModeButtonAt(int x, int y) {
    return x >= kModeBtnX && x <= kModeBtnX + kModeBtnW && y >= kModeBtnY && y <= kModeBtnY + kModeBtnH;
}

void cycleViewMode() {
    s_viewMode = s_viewMode == ViewMode::Table ? ViewMode::Waterfall : ViewMode::Table;
    s_needsFullRedraw = true;
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
