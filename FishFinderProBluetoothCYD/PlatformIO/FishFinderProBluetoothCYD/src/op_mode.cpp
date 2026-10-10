#include "op_mode.h"

#include <Arduino.h>
#include <Preferences.h>

#include <TobeLog.h>

namespace OpMode {

namespace {
constexpr const char *kPrefsNamespace = "opmode";
}  // namespace

Mode current() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/true);
    bool wifi = p.getBool("wifi", false);
    p.end();
    return wifi ? Mode::Wifi : Mode::Ble;
}

void switchTo(Mode mode) {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    p.putBool("wifi", mode == Mode::Wifi);
    p.end();
    tobe::logf("mode: switching to %s, restarting", mode == Mode::Ble ? "BLE" : "WiFi");
    delay(200);  // let the log line flush before the reset
    ESP.restart();
}

}  // namespace OpMode
