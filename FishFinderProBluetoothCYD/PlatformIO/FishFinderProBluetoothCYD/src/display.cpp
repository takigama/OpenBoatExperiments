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

// --- Depth/fish-depth rolling history table --------------------------------
//
// Table view only - a shifting array of entries, one per column position
// (10 per block, 4 blocks stacked, newest at top-left). The waterfall
// view (further down) keeps its own separate buffer instead of sharing
// this one, since it's addressed by fixed screen position rather than
// age - see that section for why.
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

    // Font 1 (the compact fixed 6x8 GLCD font), not font 2, for the
    // values themselves - one decimal place needs a 4th character
    // ("12.3") and font 2's digits are too wide for that to fit an
    // ~29px column without overlapping the neighbor. Centered vertically
    // in the row by eye (font 1 is 8px tall vs the 20px row height).
    tft.setTextFont(1);
    tft.setTextDatum(TC_DATUM);
    for (int i = 0; i < kColsPerBlock; i++) {
        const HistEntry &h = s_history[histOffset + i];
        bool valid = isFish ? h.fishValid : h.depthValid;
        float m = isFish ? h.fishM : h.depthM;
        bool isCurrent = histOffset == 0 && i == 0;  // column "t" of the top block - the single newest reading
        tft.setTextColor(isCurrent ? kCurrent : kValue, kBg);
        int cx = kLabelColW + i * kColW + kColW / 2;
        tft.fillRect(kLabelColW + i * kColW, y, kColW, kRowH, kBg);
        tft.drawString(valid ? String(m, 1) : "--", cx, y + 6);
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
// Classic sweep-style fish-finder display: a write head advances one
// column per NEW sonar reading (not once per display-timer tick) and
// redraws only that single column - depth traced as a filled "seafloor"
// from the bottom up to the reading, fish as a small dot at their own
// depth - then a bright sweep line is drawn one column ahead of it. It
// wraps back to the left edge on reaching the right, overwriting the
// oldest column first, like a radar sweep. Next reading's column-redraw
// naturally erases that sweep line along with whatever old data was
// there, so nothing needs separate erasing.
//
// An earlier version instead kept one shared, shifting history array and
// redrew the whole ~300-column plot from scratch every ~200ms - visibly
// flickery even after adding a double-buffered sprite, since the image
// itself was being rebuilt every frame, not just torn. Touching one
// column instead of three hundred is both simpler and the actual fix.

enum class ViewMode { Table, Waterfall };
ViewMode s_viewMode = ViewMode::Table;
bool s_needsFullRedraw = true;

constexpr int kLegendW = 22;
constexpr int kWaterfallTop = kStatusBarH + 1;
constexpr int kWaterfallBottom = 239;
constexpr int kWaterfallH = kWaterfallBottom - kWaterfallTop;
constexpr int kWaterfallCols = 320 - kLegendW;
constexpr uint16_t kSweep = TFT_GREEN;

struct WaterfallCol {
    bool depthValid;
    float depthM;
    bool fishValid;
    float fishM;
};
WaterfallCol s_waterfallCols[kWaterfallCols] = {};
int s_waterfallHead = 0;  // column about to be (over)written next, in [0, kWaterfallCols)

// Committed vertical scale, in meters - persists across readings and is
// only reconsidered once per full sweep (see maybeShrinkWaterfallScale())
// rather than recomputed from scratch on every column. Grown immediately
// by any single reading that needs more headroom (never clips), but only
// ever shrunk at the wrap point - otherwise ordinary frame-to-frame noise
// in the depth readings would rescale, and so visibly reshape, columns
// that were already drawn.
float s_waterfallScaleMax = 5.0f;

int waterfallY(float m) {
    int y = kWaterfallTop + (int)((m / s_waterfallScaleMax) * kWaterfallH);
    return constrain(y, kWaterfallTop, kWaterfallBottom);
}

void drawWaterfallLegend() {
    tft.fillRect(0, kWaterfallTop, kLegendW, kWaterfallH, kBg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("0", 2, kWaterfallTop);
    tft.drawString(String(s_waterfallScaleMax, 0), 2, kWaterfallBottom - 8);
}

// Redraws just column `col`'s data (not the sweep line) - shared by the
// per-reading incremental draw and the one-time full redraw below.
void drawWaterfallColumn(int col) {
    int x = kLegendW + col;
    const WaterfallCol &c = s_waterfallCols[col];
    tft.drawFastVLine(x, kWaterfallTop, kWaterfallH, kBg);  // erase whatever was here before
    if (c.depthValid) {
        int y = waterfallY(c.depthM);
        tft.drawFastVLine(x, y, kWaterfallBottom - y, kSeafloor);
    } else {
        // No reading for this column (out-of-water frame, or just not
        // filled in yet) - mark it rather than leaving it visually
        // identical to an on-screen reading of exactly 0.
        tft.drawPixel(x, kWaterfallTop, kLabel);
    }
    if (c.fishValid) tft.fillCircle(x, waterfallY(c.fishM), 2, kWarn);
}

void drawWaterfallSweepLine() { tft.drawFastVLine(kLegendW + s_waterfallHead, kWaterfallTop, kWaterfallH, kSweep); }

// Called once per full sweep (head wrapping back to 0, roughly once a
// minute at the sonar's frame rate) - a natural, rare point to reconsider
// whether the scale can shrink now that whatever needed the current one
// has scrolled fully out of view.
void maybeShrinkWaterfallScale() {
    float rawMax = 5.0f;
    for (int i = 0; i < kWaterfallCols; i++) {
        if (s_waterfallCols[i].depthValid && s_waterfallCols[i].depthM > rawMax) rawMax = s_waterfallCols[i].depthM;
    }
    float neededMax = (float)ceil((rawMax * 1.1f) / 5.0f) * 5.0f;
    if (neededMax < s_waterfallScaleMax) {
        s_waterfallScaleMax = neededMax;
        drawWaterfallLegend();
    }
}

// Called on every new sonar reading (see update()), regardless of which
// view is currently on screen - keeps the buffer current so switching
// into Waterfall mid-trip shows real recent history instead of a blank
// sweep. Only touches the panel when Waterfall is actually the one
// visible right now.
void pushWaterfallColumn(const SonarBle::Reading &reading) {
    bool depthValid = reading.valid && !reading.outOfWater;
    s_waterfallCols[s_waterfallHead] = {depthValid, reading.depthM, reading.fishValid, reading.fishDepthM};

    bool visible = s_viewMode == ViewMode::Waterfall;
    if (depthValid) {
        float neededMax = (float)ceil((reading.depthM * 1.1f) / 5.0f) * 5.0f;
        if (neededMax > s_waterfallScaleMax) {
            s_waterfallScaleMax = neededMax;
            if (visible) drawWaterfallLegend();
        }
    }
    if (visible) drawWaterfallColumn(s_waterfallHead);

    s_waterfallHead = (s_waterfallHead + 1) % kWaterfallCols;
    if (s_waterfallHead == 0) maybeShrinkWaterfallScale();

    if (visible) drawWaterfallSweepLine();
}

// One-time full repaint - on entering Waterfall mode and on reconnect
// (see s_needsFullRedraw in update()). Every other frame only touches
// the single column that just changed.
void fullRedrawWaterfall() {
    drawWaterfallLegend();
    for (int i = 0; i < kWaterfallCols; i++) drawWaterfallColumn(i);
    drawWaterfallSweepLine();
}

void drawModeButton() {
    // "Sonar"/"Table" aren't the same pixel width in this proportional
    // font, so drawString()'s own glyph-bounds erase can leave a sliver of
    // the previous label behind at the edge - clear the whole interior
    // first rather than trusting that erase to cover a narrower string.
    tft.fillRect(kModeBtnX + 1, kModeBtnY + 1, kModeBtnW - 2, kModeBtnH - 2, kBg);
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
        if (s_viewMode == ViewMode::Table) {
            drawHistoryHeader();
        } else {
            fullRedrawWaterfall();
        }
    }
    if (reading.frameCount != s_lastFrameCount) {
        s_lastFrameCount = reading.frameCount;
        pushHistory(reading);
        pushWaterfallColumn(reading);
    }
    // Waterfall doesn't need a per-tick redraw here - pushWaterfallColumn()
    // above already drew the new column (and moved the sweep line) the
    // moment new data arrived, and does nothing when it didn't.
    if (s_viewMode == ViewMode::Table) drawHistoryBlocks();
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

bool isWaterfallMode() { return s_viewMode == ViewMode::Waterfall; }

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
