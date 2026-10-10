/**
 * TobeWifi.h - WiFi the same way in every firmware.
 *
 *   - the network name and password are kept in NVS (flash) under the namespace you give to configure(),
 *     keys "ssid" and "pass" - set the namespace to whatever the project already used so boards keep their
 *     saved WiFi across a firmware update
 *   - begin() joins the saved network; if there is none (or the join times out) it makes an open network
 *     "TOBE-<name>-XXXX" (page at http://192.168.4.1/) so the board can always be reached
 *   - the radio fixes for particular chips live here, not in each project (see applyTxCap())
 *
 * Boards that must stay OFF WiFi until told (EngineControl's ESP-NOW nodes) do not call begin(); they use
 * the pieces: savedSsid(), save(), joinSaved(), startAp().
 */
#pragma once

#include <Arduino.h>

namespace tobe {
namespace wifi {

struct Config {
    const char *nvsNamespace = "wifi";   // where "ssid" / "pass" live (and the "webmode" request flag)
    uint32_t joinTimeoutMs = 15000;
    bool apFallback = true;              // begin(): make the setup network when there is nothing to join
    void (*onRadioUp)() = nullptr;       // called right after the radio mode is set (project radio tweaks)
    void (*onWait)() = nullptr;          // called every ~50 ms while waiting for a join - e.g. tobe::cli.tick(), so
                                         // the serial command line is alive during the (up to 15 s) join
};

enum class Mode { None, STA, AP };

/** Call once, early in setup(), before anything else here. */
void configure(const Config &cfg);

/** Join the saved network, else (if cfg.apFallback) start the setup network. Blocks up to joinTimeoutMs. */
Mode begin();

Mode mode();
bool connected();                 // STA mode and associated
String ip();                      // STA address, or the AP's 192.168.4.1

// ---- saved credentials
bool hasSaved();
String savedSsid();
String savedPassword();
void save(const String &ssid, const String &password);
void clearSaved();
/** Save and restart, so the new network is joined from a clean radio. */
void saveAndReboot(const String &ssid, const String &password);
/** Forget the saved network and restart: the next boot finds nothing saved and makes the setup network. */
void forgetAndReboot();

// ---- the pieces begin() is made of
bool joinSaved();                 // STA join with the saved credentials; false if none or it timed out
void startAp();                   // open network "TOBE-<name>-XXXX"
String apSsid();

/**
 * Chip-specific radio limit, applied automatically by joinSaved()/startAp() and exported for code that starts
 * the radio itself (ESP-NOW only boards).
 * ESP32-C3: on the modules we use the default 19.5 dBm never transmits (association and broadcast both fail
 * while receiving is fine); 17 dBm and below always work, so the cap is 15 dBm. Other chips: no change.
 */
void applyTxCap();

// ---- "restart into setup mode" request, honoured early in the next boot (see TobeWeb)
void requestSetupMode();
bool takeSetupModeRequest();

}  // namespace wifi
}  // namespace tobe
