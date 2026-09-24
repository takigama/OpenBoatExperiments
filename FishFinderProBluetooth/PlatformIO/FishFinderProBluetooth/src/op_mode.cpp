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
    // Defaults to BLE, not WiFi: the real deployment is a wired serial
    // link into an RPi, so a fresh/unconfigured board should go straight
    // to streaming sonar data rather than sitting in AP mode waiting for
    // WiFi setup nobody's going to do. WiFi mode is opt-in via the serial
    // "wifi" command when you actually need it (admin/OTA).
    bool wifi = p.getBool("wifi", false);
    p.end();
    return wifi ? Mode::Wifi : Mode::Ble;
}

void switchTo(Mode mode) {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    p.putBool("wifi", mode == Mode::Wifi);
    p.end();
    DebugLog::logf("mode: switching to %s, restarting", mode == Mode::Ble ? "BLE" : "WiFi");
    delay(200);  // let the log line/HTTP response flush before the reset
    ESP.restart();
}

}  // namespace OpMode
