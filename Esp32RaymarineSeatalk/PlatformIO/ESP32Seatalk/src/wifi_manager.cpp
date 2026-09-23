#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>

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
    snprintf(buf, sizeof(buf), "ESP32Seatalk-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

bool joinSaved() {
    Preferences p = prefs();
    String ssid = p.getString("ssid", "");
    String pass = p.getString("pass", "");
    p.end();

    if (ssid.isEmpty()) {
        Serial.println("wifi: no saved credentials");
        return false;
    }

    Serial.printf("wifi: joining \"%s\"...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > kJoinTimeoutMs) {
            Serial.println("wifi: join timed out");
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
    }
    Serial.printf("wifi: joined, IP %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void startAp() {
    s_apSsid = computeApSsid();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(s_apSsid.c_str());
    Serial.printf("wifi: AP mode - SSID \"%s\", IP %s\n", s_apSsid.c_str(),
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
    Serial.printf("wifi: saved credentials for \"%s\", rebooting\n", ssid.c_str());
    delay(200);  // let the response/log flush before the reset
    ESP.restart();
}

String apSsid() { return s_apSsid; }

}  // namespace WifiManager
