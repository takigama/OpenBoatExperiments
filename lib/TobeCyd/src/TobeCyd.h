/**
 * TobeCyd.h - one firmware for every CYD ("Cheap Yellow Display", ESP32-2432S028).
 *
 * The 2.8" CYDs share the ESP32, the pins and the touch controller but not the panel: the original (one USB port)
 * has an ILI9341, the CYD2USB (two USB ports) an ST7789V, and the two want different inversion / colour order /
 * orientation. Rather than one build per board, the firmware always drives the panel the same way and a handful
 * of settings - kept in NVS - say how this particular unit wants it:
 *
 *     board     CYD (one USB port) | CYD2USB (two USB ports) | custom   - a preset for the three flags below
 *     invert    colour inversion on/off
 *     bgr       colour order: BGR (the MADCTL BGR bit) or RGB
 *     rotate    rotate the picture (and touch) by 180 degrees
 *
 * This library holds the settings and the ways to change them: the "Display" section for a web page and the
 * DISPLAY serial command. It knows nothing about the panel driver - the project passes an `apply` function that
 * does the real work with whichever stack it uses (TFT_eSPI, ESP_Panel, ...). Same settings, same page, same
 * command in every CYD firmware.
 *
 *     tobe::cyd::Config c;
 *     c.apply = myApply;                 // push tobe::cyd::get() into the panel (invert / colour order / rotation)
 *     c.test = myTestPattern;            // optional: draw colour bars so the settings can be judged
 *     tobe::cyd::begin(c);               // after tobe::cli.begin(...)
 *     // page body:   + tobe::cyd::webHtml();      tobe::cyd::webRoutes(server);
 *     // setup mode:  opt.extraHtml / extraRoutes - call these through small wrappers, see TobeFleetUi.h
 *
 * A project that must know the settings before it brings the panel up (ESP_Panel sets the colour order at
 * begin()) calls tobe::cyd::load() first; begin() does it too if it has not been done.
 */
#pragma once

#include <Arduino.h>
class WebServer;  // the page code includes <WebServer.h> itself (TFT_eSPI.h must not come before it)

namespace tobe {
namespace cyd {

enum Preset : uint8_t { PRESET_CYD = 0, PRESET_CYD2USB = 1, PRESET_CUSTOM = 2 };

struct Settings {
    uint8_t preset = PRESET_CYD2USB;
    bool invert = true;     // CYD2USB defaults: what FishFinderProBluetoothCYD was verified with
    bool bgr = true;
    bool rotate180 = false;
};

struct Config {
    /** Push get() into the panel. May be called at any time after begin() (web / serial changes). */
    void (*apply)(const Settings &s) = nullptr;
    /** Optional: draw a colour test pattern (and leave it up for a few seconds). false: no screen is up. */
    bool (*test)() = nullptr;
    /** Optional: called when the rotation changed, before the restart (e.g. forget the touch calibration). */
    void (*rotationChanged)() = nullptr;
    /** true if the colour order can be changed with the panel running; false: a change restarts. */
    bool liveColourOrder = true;
    /** Call apply() from begin() with the stored settings (leave off if the project applies at panel init). */
    bool applyAtBegin = false;
    /** NVS namespace. */
    const char *nvsNamespace = "tobedisp";
};

/** Read the stored settings (defaults if none). Safe to call more than once. */
void load();

/** The current settings. */
const Settings &get();

/** The preset's own flags (invert / bgr; rotate180 is never part of a preset). */
Settings presetSettings(uint8_t preset);

/** Register the DISPLAY command; remember `cfg`. Call after tobe::cli.begin(). */
void begin(const Config &cfg);

/**
 * Change the settings: stores them and applies them. A rotation change (or a colour-order change when the panel
 * cannot do that live) needs a restart: it is returned, and done here unless `restartNow` is false (a web handler
 * sends its reply first, then calls tobe::restart() itself).
 */
bool set(const Settings &s, bool restartNow = true);

/** One line, e.g. "CYD2USB, inverted, BGR, normal orientation". */
String describe();

/** The "Display" section of a web page (HTML, starts with <hr>). */
String webHtml();

/** Register POST /display and POST /display/test on `server`. */
void webRoutes(WebServer &server);

}  // namespace cyd
}  // namespace tobe
