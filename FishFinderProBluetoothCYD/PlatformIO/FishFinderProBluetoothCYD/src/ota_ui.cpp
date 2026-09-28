#include "ota_ui.h"

#include <TFT_eSPI.h>
#include <WiFi.h>

#include "debug_log.h"
#include "keyboard.h"
#include "op_mode.h"
#include "ota_manager.h"
#include "touch.h"
#include "web_config.h"
#include "wifi_manager.h"

namespace OtaUi {

namespace {

TFT_eSPI tft;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kLabel = TFT_DARKGREY;
constexpr uint16_t kValue = TFT_WHITE;
constexpr uint16_t kWarn = TFT_YELLOW;
constexpr uint16_t kOk = TFT_GREEN;

enum class State { Checking, UpdateAvailable, UpToDate };
State s_state = State::Checking;
OtaManager::UpdateInfo s_info;

struct Btn {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
    void draw(const char *label, uint16_t color) const {
        tft.drawRoundRect(x, y, w, h, 4, color);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(color, kBg);
        tft.drawString(label, x + w / 2, y + h / 2);
        tft.setTextDatum(TL_DATUM);
    }
};

constexpr Btn kDoOtaBtn{60, 140, 200, 30};
constexpr Btn kBackBtn{60, 185, 200, 30};

void drawMessage(const String &line1, const String &line2, uint16_t color) {
    tft.fillRect(0, 40, 320, 90, kBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(color, kBg);
    tft.drawString(line1, 160, 60);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString(line2, 160, 90);
    tft.setTextDatum(TL_DATUM);
}

void redraw() {
    tft.fillRect(0, 130, 320, 100, kBg);  // clear button area before redrawing whichever applies
    switch (s_state) {
        case State::Checking:
            drawMessage("Checking for update...", "", kLabel);
            kBackBtn.draw("Back to Sonar", kLabel);
            break;
        case State::UpdateAvailable:
            drawMessage("Update available", "build " + String(s_info.build), kOk);
            kDoOtaBtn.draw("Do OTA", kOk);
            kBackBtn.draw("Back to Sonar", kLabel);
            break;
        case State::UpToDate:
            drawMessage("Up to date", "running build " + String(FW_BUILD), kValue);
            kBackBtn.draw("Back to Sonar", kLabel);
            break;
    }
}

void runCheck() {
    s_state = State::Checking;
    redraw();
    s_info = OtaManager::checkForUpdate();
    s_state = s_info.available ? State::UpdateAvailable : State::UpToDate;
    redraw();
}

// Shown when "Setup from phone" is picked instead of an on-device
// network/password entry - the AP+web portal are already running
// regardless (see begin()), this is just showing the matching
// instructions. Blocks until "Back" is tapped, still pumping
// WebConfig::handleClient() so a phone-submitted join still works (and
// reboots) while this is showing.
void waitPhoneInstructions() {
    tft.fillScreen(kBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(kWarn, kBg);
    tft.drawString("WiFi setup needed", 160, 40);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("Connect to \"" + WifiManager::apSsid() + "\"", 160, 80);
    tft.drawString("then open 192.168.4.1", 160, 102);
    tft.setTextDatum(TL_DATUM);
    constexpr Btn kBack{60, 190, 200, 30};
    kBack.draw("Back", kLabel);

    for (;;) {
        WebConfig::handleClient();
        int x, y;
        if (Touch::pollTap(&x, &y) && kBack.hit(x, y)) return;
        delay(10);
    }
}

// Blocking, entered only in AP mode (no saved WiFi, or the saved network
// didn't join - see wifi_manager.h): scans for nearby networks and lets
// the user pick one (or enter a name manually) plus a password via the
// on-screen keyboard, or fall back to phone-based setup. Only returns via
// WifiManager::saveCredentialsAndReboot() rebooting - i.e. in practice,
// never returns at all.
void runWifiSetup() {
    tft.fillScreen(kBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("Scanning for WiFi...", 160, 120);
    tft.setTextDatum(TL_DATUM);

    constexpr int kMaxShown = 4;  // leaves room for manual/phone/back buttons below without overflowing
    int n = WiFi.scanNetworks();
    if (n > kMaxShown) n = kMaxShown;
    if (n < 0) n = 0;

    for (;;) {
        tft.fillScreen(kBg);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(2);
        tft.setTextColor(kValue, kBg);
        tft.drawString("Select a WiFi network", 160, 10);
        tft.setTextDatum(TL_DATUM);

        constexpr int kRowH = 28, kListTop = 24;
        Btn rows[kMaxShown];
        for (int i = 0; i < n; i++) {
            rows[i] = {10, kListTop + i * kRowH, 300, kRowH - 4};
            String label = WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)";
            rows[i].draw(label.c_str(), kValue);
        }
        Btn manualBtn{10, kListTop + n * kRowH + 6, 300, 28};
        manualBtn.draw("Enter manually", kLabel);
        Btn phoneBtn{10, kListTop + (n + 1) * kRowH + 10, 300, 28};
        phoneBtn.draw("Setup from phone", kLabel);
        Btn backBtn{10, kListTop + (n + 2) * kRowH + 14, 300, 28};
        backBtn.draw("Back to Sonar (skip WiFi setup)", kLabel);

        int x = -1, y = -1;
        while (x < 0) {
            WebConfig::handleClient();
            if (Touch::pollTap(&x, &y)) break;
            delay(10);
        }

        if (backBtn.hit(x, y)) {
            OpMode::switchTo(OpMode::Mode::Ble);  // does not return
        }

        int hitRow = -1;
        for (int i = 0; i < n; i++) {
            if (rows[i].hit(x, y)) hitRow = i;
        }

        String ssid;
        if (hitRow >= 0) {
            ssid = WiFi.SSID(hitRow);
        } else if (manualBtn.hit(x, y)) {
            ssid = Keyboard::run(tft, "WiFi network name", "", false);
            if (ssid.isEmpty()) continue;  // cancelled - re-show the picker
        } else if (phoneBtn.hit(x, y)) {
            waitPhoneInstructions();
            continue;  // tapped Back - re-show the picker
        } else {
            continue;  // tap missed every control - redraw and wait again
        }

        String pass = Keyboard::run(tft, "Password for \"" + ssid + "\"", "", true);
        WifiManager::saveCredentialsAndReboot(ssid, pass);  // does not return
    }
}

}  // namespace

void begin() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(kBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(kLabel, kBg);
    tft.drawString("FishFinderProBluetoothCYD - WiFi mode", 160, 15);
    tft.setTextDatum(TL_DATUM);

    Touch::begin();

    drawMessage("Connecting to WiFi...", "", kLabel);
    WifiManager::Mode mode = WifiManager::begin();
    WebConfig::begin();  // needed either way - AP mode for setup, STA mode for /log and manual OTA

    if (mode == WifiManager::Mode::AP) {
        runWifiSetup();  // blocking - only "returns" via a reboot
    } else {
        runCheck();
    }
}

void loop() {
    WebConfig::handleClient();

    int x, y;
    if (!Touch::pollTap(&x, &y)) return;

    if (s_state == State::UpdateAvailable && kDoOtaBtn.hit(x, y)) {
        drawMessage("Updating...", "downloading build " + String(s_info.build), kWarn);
        bool ok = OtaManager::applyUpdate(s_info);  // reboots on success; only returns on failure
        if (!ok) {
            drawMessage("Update failed", "check /log over WiFi for details", kWarn);
            DebugLog::logf("ota_ui: applyUpdate() failed");
        }
        return;
    }
    if (kBackBtn.hit(x, y)) {  // drawn (and tappable) in every state - see redraw()
        OpMode::switchTo(OpMode::Mode::Ble);  // does not return
    }
}

}  // namespace OtaUi
