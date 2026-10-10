/**
 * TobeFleetUi.h - how a person sets the ESP-NOW secret: serial commands KEY / KEYSHOW / KEYCLEAR, and the
 * "ESP-NOW secret" section (with its POST /key route) for the setup web page.
 *
 * Header-only, like fleet_security.h (include that first, and in the same file: both keep their state in
 * file-level variables). Needs TobeCli and TobeWeb.
 *
 *     fleet_ui_begin(&prefs, &prefs_ok, on_key_changed);     // once, in setup(), after tobe::cli.begin(...)
 *     // setup mode:   opt.extraHtml = fleet_ui_web_html;  opt.extraRoutes = fleet_ui_web_routes;
 *     // a normal page: server.send(... + fleet_ui_web_html()); fleet_ui_web_routes(server);
 *
 * `on_key_changed` runs after the key was changed or cleared, before the restart (forget joined boards, the old
 * pairing used the old key); it may be NULL. A key change always restarts, so the radio comes up with it.
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>

#include <TobeCli.h>
#include <TobeLog.h>
#include <TobeWeb.h>

#include "fleet_security.h"

typedef struct {
    Preferences *prefs;
    bool        *prefs_ok;
    void       (*on_key_changed)(void);
} fleet_ui_ctx_t;

static fleet_ui_ctx_t g_fleet_ui = { NULL, NULL, NULL };

static inline bool fleet_ui_ok(void) { return g_fleet_ui.prefs && g_fleet_ui.prefs_ok && *g_fleet_ui.prefs_ok; }

static inline void fleet_ui_fingerprint(char out[12])
{
    snprintf(out, 12, "%08lX", (unsigned long)fsec_fingerprint());
}

/* ---- serial ---- */

static void fleet_cli_key(const char *phrase)
{
    if (strlen(phrase) < FSEC_MIN_PASSPHRASE) {
        tobe::console.printf("the passphrase must be at least %d characters\n", FSEC_MIN_PASSPHRASE);
        return;
    }
    tobe::console.println("working out the key (a second or two)...");
    if (!fleet_ui_ok() || !fsec_set_passphrase(*g_fleet_ui.prefs, true, phrase)) {
        tobe::console.println("could not save the key (flash unavailable)");
        return;
    }
    if (g_fleet_ui.on_key_changed) g_fleet_ui.on_key_changed();
    char fp[12];
    fleet_ui_fingerprint(fp);
    tobe::console.printf("key saved, fingerprint %s (it must read the same on every board) - restarting\n", fp);
    tobe::restart(300);
}

static void fleet_cli_keyshow(const char *)
{
    if (g_fsec_have_key) {
        char fp[12];
        fleet_ui_fingerprint(fp);
        tobe::console.printf("key is set, fingerprint %s\n", fp);
    } else {
        tobe::console.println("no key set - ESP-NOW is off. Type  KEY <passphrase>");
    }
}

static void fleet_cli_keyclear(const char *)
{
    if (fleet_ui_ok()) fsec_clear(*g_fleet_ui.prefs, true);
    if (g_fleet_ui.on_key_changed) g_fleet_ui.on_key_changed();
    tobe::console.println("key cleared - restarting with ESP-NOW off");
    tobe::restart(300);
}

static const tobe::CliCommand kFleetCli[] = {
    { "KEY",      "<passphrase>", "set the shared ESP-NOW secret (12+ characters, same on every board), then restart", fleet_cli_key, tobe::CLI_SECRET },
    { "KEYSHOW",  "",             "is a key set? its fingerprint (same on every board with the same key)",            fleet_cli_keyshow, 0 },
    { "KEYCLEAR", "",             "forget the key (ESP-NOW goes off), then restart",                                  fleet_cli_keyclear, 0 },
};

/* ---- web ---- */

static String fleet_ui_web_html(void)
{
    String h = "<hr><h3>ESP-NOW secret</h3>";
    if (g_fsec_have_key) {
        char fp[12];
        fleet_ui_fingerprint(fp);
        h += "<p>A key is set. Fingerprint <b>" + String(fp) + "</b> - it must read the same on every board.</p>";
    } else {
        h += "<p><b>No key is set</b> - ESP-NOW is off on this board.</p>";
    }
    h += "<form method=POST action=/key>Passphrase (" + String(FSEC_MIN_PASSPHRASE) +
         " or more characters; the same on every board)"
         "<input type=password name=phrase autocomplete=off><button>Save key and restart</button></form>";
    return h;
}

static void fleet_ui_web_routes(WebServer &server)
{
    server.on("/key", HTTP_POST, [&server]() {
        String phrase = server.arg("phrase");
        auto reply = [&server](const String &msg) {
            server.send(200, "text/html", tobe::web::page("ESP-NOW secret", tobe::web::note(msg) + fleet_ui_web_html()));
        };
        if (phrase.length() < FSEC_MIN_PASSPHRASE) {
            reply("The passphrase must be at least " + String(FSEC_MIN_PASSPHRASE) + " characters.");
            return;
        }
        if (!fleet_ui_ok() || !fsec_set_passphrase(*g_fleet_ui.prefs, true, phrase.c_str())) {
            reply("Could not save the key (flash unavailable).");
            return;
        }
        if (g_fleet_ui.on_key_changed) g_fleet_ui.on_key_changed();
        char fp[12];
        fleet_ui_fingerprint(fp);
        reply(String("Key saved, fingerprint ") + fp + ". Restarting to apply it...");
        tobe::logf("web: ESP-NOW key saved, fingerprint %s - restarting", fp);
        tobe::restart(1500);
    });
}

static void fleet_ui_begin(Preferences *prefs, bool *prefs_ok, void (*on_key_changed)(void))
{
    g_fleet_ui.prefs = prefs;
    g_fleet_ui.prefs_ok = prefs_ok;
    g_fleet_ui.on_key_changed = on_key_changed;
    tobe::cli.add(kFleetCli, sizeof(kFleetCli) / sizeof(kFleetCli[0]));
}
