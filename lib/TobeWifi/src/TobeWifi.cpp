#include "TobeWifi.h"

#include <Preferences.h>
#include <WiFi.h>

#include "Tobe.h"
#include "TobeLog.h"

namespace tobe {
namespace wifi {

namespace {

Config s_cfg;
Mode s_mode = Mode::None;
String s_apSsid;

String getStr(const char *key) {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, true)) return String();
    String v = p.getString(key, "");
    p.end();
    return v;
}

void putStr(const char *key, const String &v) {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, false)) return;
    p.putString(key, v);
    p.end();
}

}  // namespace

void configure(const Config &cfg) { s_cfg = cfg; }

void applyTxCap() {
#if CONFIG_IDF_TARGET_ESP32C3
    WiFi.setTxPower(WIFI_POWER_15dBm);
#endif
}

bool hasSaved() { return getStr("ssid").length() > 0; }
String savedSsid() { return getStr("ssid"); }
String savedPassword() { return getStr("pass"); }

void save(const String &ssid, const String &password) {
    putStr("ssid", ssid);
    putStr("pass", password);
}

void clearSaved() {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, false)) return;
    p.remove("ssid");
    p.remove("pass");
    p.end();
}

void saveAndReboot(const String &ssid, const String &password) {
    save(ssid, password);
    logf("wifi: saved \"%s\", restarting to join it", ssid.c_str());
    restart(300);
}

void forgetAndReboot() {
    clearSaved();
    logf("wifi: saved network forgotten, restarting");
    restart(300);
}

bool joinSaved() {
    String ssid = savedSsid();
    if (ssid.isEmpty()) {
        logf("wifi: no saved network");
        return false;
    }
    logf("wifi: joining \"%s\"...", ssid.c_str());
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.begin(ssid.c_str(), savedPassword().c_str());
    // Set the cap right after begin(): the radio is up by then, and it has to be in place before the first
    // frame matters. (Not setSleep(false): with BLE running the IDF aborts on that.)
    applyTxCap();
    if (s_cfg.onRadioUp) s_cfg.onRadioUp();

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > s_cfg.joinTimeoutMs) {
            logf("wifi: join timed out");
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
    }
    s_mode = Mode::STA;
    logf("wifi: joined, IP %s", WiFi.localIP().toString().c_str());
    return true;
}

void startAp() {
    s_apSsid = apName();
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    delay(100);   // let the mode switch settle before softAP()
    bool ok = WiFi.softAP(s_apSsid.c_str());   // open network, by choice: it is only up for setup
    applyTxCap();
    if (s_cfg.onRadioUp) s_cfg.onRadioUp();
    s_mode = Mode::AP;
    logf("wifi: setup network \"%s\" %s, IP %s", s_apSsid.c_str(), ok ? "up" : "FAILED to start",
         WiFi.softAPIP().toString().c_str());
}

Mode begin() {
    if (joinSaved()) return s_mode;
    if (s_cfg.apFallback) startAp();
    return s_mode;
}

Mode mode() { return s_mode; }
bool connected() { return s_mode == Mode::STA && WiFi.status() == WL_CONNECTED; }

String ip() {
    if (s_mode == Mode::AP) return WiFi.softAPIP().toString();
    return WiFi.localIP().toString();
}

String apSsid() { return s_apSsid; }

void requestSetupMode() {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, false)) return;
    p.putBool("webmode", true);
    p.end();
}

bool takeSetupModeRequest() {
    Preferences p;
    if (!p.begin(s_cfg.nvsNamespace, false)) return false;
    bool want = p.getBool("webmode", false);
    if (want) p.putBool("webmode", false);
    p.end();
    return want;
}

}  // namespace wifi
}  // namespace tobe
