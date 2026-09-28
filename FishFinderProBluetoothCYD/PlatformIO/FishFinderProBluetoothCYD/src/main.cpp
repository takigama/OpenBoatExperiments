#include <Arduino.h>

#include "debug_log.h"
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

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::logf("FishFinderProBluetoothCYD build %d booted", FW_BUILD);

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
    if (s_mode == OpMode::Mode::Wifi) {
        OtaUi::loop();
        return;
    }

    SonarBle::loop();

    int tx, ty;
    if (Touch::pollTap(&tx, &ty) && Display::isUpdateButtonAt(tx, ty)) {
        OpMode::switchTo(OpMode::Mode::Wifi);  // does not return
    }

    // loop() itself runs far faster than the sonar's ~4.5Hz frame rate -
    // redrawing on every single iteration was pure waste. Once every
    // 500ms is plenty for a display meant to be glanced at.
    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = millis();
        Display::update(SonarBle::latest(), SonarBle::connected());
    }

    // Periodic liveness line, same convention as FishFinderProBluetooth.
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        DebugLog::logf("status: ble_connected=%d frames=%u heap=%u", SonarBle::connected(),
                        SonarBle::latest().frameCount, ESP.getFreeHeap());
    }
}
