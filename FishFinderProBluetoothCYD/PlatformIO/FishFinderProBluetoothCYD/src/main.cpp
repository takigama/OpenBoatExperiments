#include <Arduino.h>

#include <Tobe.h>
#include <TobeCli.h>
#include <TobeLog.h>
#include <TobeOta.h>
#include <TobeWeb.h>
#include <TobeWifi.h>
#include "display.h"
#include "op_mode.h"
#include "ota_ui.h"
#include "sonar_ble.h"
#include "touch.h"

// WiFi and BLE are mutually exclusive per boot, not live-switched - same
// reasoning as FishFinderProBluetooth's op_mode.h (this board's single
// radio can't run both reliably at once). BLE mode (the sonar display) is
// the default; the on-screen "Check Update" button reboots into WiFi
// mode (ota_ui.cpp) for occasional admin.
OpMode::Mode s_mode;

// ---- command line. WiFi and BLE cannot run together on this chip, so the built-in UPDATE (which joins WiFi) is
// replaced by one that only runs in WiFi mode.
void cmdMode(const char *args) {
    if (strcasecmp(args, "wifi") == 0) {
        OpMode::switchTo(OpMode::Mode::Wifi);  // does not return
    } else if (strcasecmp(args, "ble") == 0) {
        OpMode::switchTo(OpMode::Mode::Ble);  // does not return
    } else {
        tobe::console.printf("mode is %s. MODE WIFI or MODE BLE switches (restarts).\n",
                             s_mode == OpMode::Mode::Wifi ? "WIFI" : "BLE");
    }
}

void cmdUpdate(const char *) {
    if (s_mode != OpMode::Mode::Wifi) {
        tobe::console.print("updates need WiFi mode - MODE WIFI, then UPDATE (or tap Check Update on the screen)\n");
        return;
    }
    String msg;
    if (!tobe::ota::updateNow(&msg)) tobe::console.printf("%s\n", msg.c_str());
}

void statusHook() {
    tobe::console.printf("  mode %s", s_mode == OpMode::Mode::Wifi ? "WIFI" : "BLE");
    if (s_mode == OpMode::Mode::Ble)
        tobe::console.printf(", sonar %s, %u frames", SonarBle::connected() ? "connected" : "not connected",
                             (unsigned)SonarBle::latest().frameCount);
    tobe::console.print("\n");
}

const tobe::CliCommand kCommands[] = {
    {"MODE", "<wifi|ble>", "switch between WiFi (admin, updates) and BLE (sonar display) mode - restarts", cmdMode, 0},
    {"UPDATE", "", "WiFi mode: check GitHub for a newer firmware and install it", cmdUpdate, 0},
};

void setup() {
    tobe::console.begin(115200);
    delay(500);
    tobe::logf("%s booted", tobe::titleWithVersion().c_str());

    tobe::wifi::Config wcfg;
    wcfg.nvsNamespace = "wifi";
    tobe::wifi::configure(wcfg);
    tobe::cli.begin("FISHCYD> ", kCommands, sizeof(kCommands) / sizeof(kCommands[0]));
    tobe::cli.setStatusHook(statusHook);

    // WEBMODE asked for the setup page: WiFi only, no display / BLE this boot
    if (tobe::web::setupRequested()) tobe::web::runSetupMode();

    s_mode = OpMode::current();
    if (s_mode == OpMode::Mode::Ble) {
        Display::begin();
        Touch::begin();
        SonarBle::begin();
    } else {
        OtaUi::begin();
    }
}

void loop() {
    tobe::cli.tick();
    if (s_mode == OpMode::Mode::Wifi) {
        OtaUi::loop();
        return;
    }

    SonarBle::loop();

    int tx, ty;
    if (Touch::pollTap(&tx, &ty)) {
        if (Display::isUpdateButtonAt(tx, ty)) {
            OpMode::switchTo(OpMode::Mode::Wifi);  // does not return
        } else if (Display::isModeButtonAt(tx, ty)) {
            Display::cycleViewMode();
        }
    }

    // loop() itself runs far faster than the sonar's ~4.5Hz frame rate -
    // redrawing on every single iteration was pure waste. The table view
    // is meant to be glanced at, so 500ms is plenty there; the waterfall
    // reads better closer to the sonar's own frame rate, so it gets a
    // shorter interval.
    static uint32_t lastDisplayUpdate = 0;
    uint32_t displayIntervalMs = Display::isWaterfallMode() ? 200 : 500;
    if (millis() - lastDisplayUpdate >= displayIntervalMs) {
        lastDisplayUpdate = millis();
        Display::update(SonarBle::latest(), SonarBle::connected());
    }

    // Periodic liveness line, same convention as FishFinderProBluetooth.
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        if (!tobe::cli.quiet())
            tobe::logf("status: ble_connected=%d frames=%u heap=%u", SonarBle::connected(),
                       SonarBle::latest().frameCount, ESP.getFreeHeap());
    }
}
