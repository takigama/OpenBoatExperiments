#include "op_mode.h"

#include <Arduino.h>
#include <Preferences.h>

#include "debug_log.h"

namespace OpMode {

namespace {
constexpr const char *kPrefsNamespace = "opmode";
}  // namespace

Mode current() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/true);
    bool ble = p.getBool("ble", false);
    p.end();
    return ble ? Mode::Ble : Mode::Wifi;
}

void switchTo(Mode mode) {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    p.putBool("ble", mode == Mode::Ble);
    p.end();
    DebugLog::logf("mode: switching to %s, restarting", mode == Mode::Ble ? "BLE" : "WiFi");
    delay(200);  // let the log line/HTTP response flush before the reset
    ESP.restart();
}

}  // namespace OpMode
