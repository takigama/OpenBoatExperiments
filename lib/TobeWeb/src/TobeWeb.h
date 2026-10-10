/**
 * TobeWeb.h - web pages the same way in every firmware.
 *
 *   tobe::web::page(title, body)      the page frame: dark theme, "TOBE <name>" heading, mobile friendly
 *   tobe::web::systemSection()        the block every device shows: firmware version + update, WiFi, log, restart
 *   tobe::web::attach(server)         registers the routes that block uses (/sys/...)
 *   tobe::web::runSetupMode(...)      "WEBMODE": join the saved WiFi (or make an open network "TOBE-<name>-XXXX")
 *                                     and serve the setup page for 10 minutes, then restart. Never returns.
 *
 * A project with its own web UI keeps its own WebServer and its own pages; it wraps them with page(), puts
 * systemSection() somewhere on its main page, and calls attach() once. A project with settings that only make
 * sense in setup mode (EngineControl's ESP-NOW key) supplies them through SetupOptions.
 *
 * All pages are plain HTML forms - no scripts, no external files - so they work from any phone with no internet.
 */
#pragma once

#include <Arduino.h>
#include <WebServer.h>

namespace tobe {
namespace web {

/** HTML-escape text for use inside a page. */
String escape(const String &s);

/** The page frame. `title` is the page's own name ("Setup", "Routes"); "TOBE <firmware name>" is added. */
String page(const String &title, const String &body);

/**
 * The page frame in two halves, for pages too long to build as one String (stream them with
 * WebServer::sendContent in chunks). `extraHead` goes inside <head> - e.g. a refresh:
 * "<meta http-equiv=refresh content=2>".
 */
String pageHead(const String &title, const String &extraHead = "");
String pageTail();

/** A message box ("Saved."), for the top of a page body. */
String note(const String &text);

/** Firmware version + update, WiFi network, log, restart. Safe to embed in any page body. */
String systemSection();

/** Registers /sys/wifi, /sys/ota/check, /sys/ota/apply, /sys/upload, /sys/log, /sys/reboot on `server`. */
void attach(WebServer &server);

struct SetupOptions {
    /** Extra sections for the setup page (HTML, may be empty). */
    String (*extraHtml)() = nullptr;
    /** Register the routes those sections post to. */
    void (*extraRoutes)(WebServer &server) = nullptr;
    /** Called once the radio mode is set (e.g. to cap transmit power). */
    void (*onRadioUp)() = nullptr;
    /** Called when the page is ready: `where` says how to reach it (a display can show that). */
    void (*onReady)(const char *where) = nullptr;
    uint32_t minutes = 10;
};

/** The setup page: system section + the project's extras. Also what runSetupMode() serves at "/". */
String setupPage(const SetupOptions &opt, const String &message = "");

/**
 * Setup mode. Joins the saved WiFi if there is one, otherwise makes the open network, then serves the setup
 * page for `minutes` and restarts. Never returns. Open on purpose (convenience): anyone in range while it is
 * on can change the settings, so keep it short.
 */
[[noreturn]] void runSetupMode(const SetupOptions &opt = SetupOptions());

/** Early in setup(): true if the CLI's WEBMODE (or the web page) asked for setup mode on this boot. */
bool setupRequested();

}  // namespace web
}  // namespace tobe
