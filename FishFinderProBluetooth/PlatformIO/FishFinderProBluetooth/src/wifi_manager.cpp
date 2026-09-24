#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>

#include "debug_log.h"

namespace WifiManager {

namespace {

constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr const char *kPrefsNamespace = "wifi";

Mode s_mode = Mode::AP;
String s_apSsid;

Preferences prefs() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    return p;
}

String computeApSsid() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "FishFinder-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

bool joinSaved() {
    Preferences p = prefs();
    String ssid = p.getString("ssid", "");
    String pass = p.getString("pass", "");
    p.end();

    if (ssid.isEmpty()) {
        DebugLog::logf("wifi: no saved credentials");
        return false;
    }

    DebugLog::logf("wifi: joining \"%s\"...", ssid.c_str());
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());
    // Some ESP32-C3 modules (this board's batch included) have a marginal
    // antenna match that causes reflections back into the PA at full TX
    // power, breaking association/broadcast entirely even though RX is
    // fine - confirmed via isolation testing (scans/RSSI always good,
    // association/AP-broadcast both fail at default power, both work
    // reliably capped here). Not a dead radio, just can't run at max power.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > kJoinTimeoutMs) {
            DebugLog::logf("wifi: join timed out");
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
    }
    DebugLog::logf("wifi: joined, IP %s", WiFi.localIP().toString().c_str());
    return true;
}

void startAp() {
    s_apSsid = computeApSsid();
    WiFi.mode(WIFI_AP);
    delay(100);  // let the mode switch settle before softAP() - same reasoning as the STA-side fix
    bool ok = WiFi.softAP(s_apSsid.c_str());
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // see the matching comment in joinSaved() - same fix, same reason
    DebugLog::logf("wifi: softAP() returned %s, mode=%d", ok ? "true" : "false", (int)WiFi.getMode());
    DebugLog::logf("wifi: AP mode - SSID \"%s\", IP %s", s_apSsid.c_str(),
                    WiFi.softAPIP().toString().c_str());
}

}  // namespace

Mode begin() {
    if (joinSaved()) {
        s_mode = Mode::STA;
    } else {
        startAp();
        s_mode = Mode::AP;
    }
    return s_mode;
}

Mode currentMode() { return s_mode; }

void saveCredentialsAndReboot(const String &ssid, const String &password) {
    Preferences p = prefs();
    p.putString("ssid", ssid);
    p.putString("pass", password);
    p.end();
    DebugLog::logf("wifi: saved credentials for \"%s\", rebooting", ssid.c_str());
    delay(200);  // let the response/log flush before the reset
    ESP.restart();
}

String apSsid() { return s_apSsid; }

}  // namespace WifiManager
