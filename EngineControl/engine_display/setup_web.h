/**
 * setup_web.h - the little setup web page: set the ESP-NOW secret and (optionally) WiFi details.
 *
 * CANONICAL COPY. Also duplicated (by hand) at ../can_sim/setup_web.h - keep byte-identical.
 * Needs fleet_security.h included first, and <WebServer.h>, <WiFi.h>, <Preferences.h>.
 *
 * Reached three ways:
 *   - the HELM's normal web page (it is on your WiFi): the same /key and /wifi handlers are registered on it
 *   - serial `WEBMODE` on a C3 or CYD: the board restarts into "setup mode" - it joins the WiFi it has saved
 *     (if any), otherwise starts its own open network "EC-xxxx" (page at http://192.168.4.1/) - and serves
 *     this page for 10 minutes, then restarts normally
 *
 * No password protects the page or its network (a deliberate choice for convenience): anyone in range while
 * setup mode is on could set a key. Keep setup mode short, and compare key fingerprints afterwards (`KEY?`).
 */
#pragma once

#define SW_MODE_MINUTES 10

struct SetupWebCtx {
    Preferences *prefs;
    bool         prefs_ok;
    const char  *device;       /* "HELM display", "CYD display", "can_sim" */
    int          build;
    void       (*on_key_changed)(void);   /* forget joined boards etc.; may be NULL */
};

static String sw_escape(const String &in)
{
    String out;
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '&') out += "&amp;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

static String sw_page(const SetupWebCtx &c, const String &note)
{
    String saved_ssid = c.prefs_ok ? c.prefs->getString("ssid", "") : String("");
    String h;
    h.reserve(2400);
    h += "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width'><title>EngineControl setup</title>"
         "<style>body{font-family:sans-serif;background:#0a1929;color:#eee;padding:20px;max-width:480px;margin:auto}"
         "input{display:block;width:100%;margin:6px 0 12px;padding:8px;background:#142838;color:#eee;border:1px solid #33475c}"
         "button{padding:10px 18px;background:#1c3450;color:#fff;border:1px solid #33475c}"
         "h3{margin-top:28px}.n{background:#16283c;padding:10px;border-left:4px solid #6cf}</style></head><body>";
    h += "<h2>" + sw_escape(String(c.device)) + " setup</h2><p>Firmware build " + String(c.build) + "</p>";
    if (note.length()) h += "<p class=n>" + sw_escape(note) + "</p>";

    h += "<h3>ESP-NOW secret</h3>";
    if (g_fsec_have_key) {
        char fp[12];
        snprintf(fp, sizeof(fp), "%08lX", (unsigned long)fsec_fingerprint());
        h += "<p>A key is set. Fingerprint <b>" + String(fp) + "</b> - it must read the same on every board.</p>";
    } else {
        h += "<p><b>No key is set</b> - ESP-NOW is off on this board.</p>";
    }
    h += "<form method=POST action=/key>Passphrase (12 or more characters; the same on every board)"
         "<input type=password name=phrase autocomplete=off><button>Save key and restart</button></form>";

    h += "<h3>WiFi (optional)</h3><p>Saved network: <b>" + (saved_ssid.length() ? sw_escape(saved_ssid) : String("none")) + "</b></p>"
         "<form method=POST action=/wifi>Network name<input name=ssid>Password<input type=password name=pass>"
         "<button>Save and restart</button></form>";

    h += "<h3>Restart</h3><form method=POST action=/reboot><button>Restart now (leave setup mode)</button></form>";
    h += "</body></html>";
    return h;
}

static void sw_register(WebServer &srv, const SetupWebCtx &ctx_in, bool serve_root = true)
{
    static SetupWebCtx ctx;   /* handlers outlive this call */
    ctx = ctx_in;

    if (serve_root)
        srv.on("/", HTTP_GET, [&srv]() { srv.send(200, "text/html", sw_page(ctx, "")); });

    srv.on("/key", HTTP_POST, [&srv]() {
        String phrase = srv.arg("phrase");
        if (phrase.length() < FSEC_MIN_PASSPHRASE) {
            srv.send(200, "text/html", sw_page(ctx, "The passphrase must be at least " + String(FSEC_MIN_PASSPHRASE) + " characters."));
            return;
        }
        if (!fsec_set_passphrase(*ctx.prefs, ctx.prefs_ok, phrase.c_str())) {
            srv.send(200, "text/html", sw_page(ctx, "Could not save the key (flash unavailable)."));
            return;
        }
        if (ctx.on_key_changed) ctx.on_key_changed();
        char fp[12];
        snprintf(fp, sizeof(fp), "%08lX", (unsigned long)fsec_fingerprint());
        srv.send(200, "text/html", sw_page(ctx, String("Key saved, fingerprint ") + fp + ". Restarting to apply it..."));
        Serial.printf("Setup page: key saved, fingerprint %s - restarting\n", fp);
        delay(1500);
        ESP.restart();
    });

    srv.on("/wifi", HTTP_POST, [&srv]() {
        String ssid = srv.arg("ssid");
        if (!ssid.length() || !ctx.prefs_ok) {
            srv.send(200, "text/html", sw_page(ctx, "Enter a network name."));
            return;
        }
        ctx.prefs->putString("ssid", ssid);
        ctx.prefs->putString("pass", srv.arg("pass"));
        srv.send(200, "text/html", sw_page(ctx, "WiFi details saved (" + ssid + "). Restarting..."));
        Serial.printf("Setup page: WiFi \"%s\" saved - restarting\n", ssid.c_str());
        delay(1500);
        ESP.restart();
    });

    srv.on("/reboot", HTTP_POST, [&srv]() {
        srv.send(200, "text/html", sw_page(ctx, "Restarting..."));
        delay(1000);
        ESP.restart();
    });
}

/* Setup mode: never returns (ends in a restart). Joins the saved WiFi if there is one, otherwise starts an
 * open network of its own, and serves the page for SW_MODE_MINUTES. `on_wifi_up` lets a sketch adjust the radio
 * after the mode is set (the C3 caps its transmit power there); `on_ready` is told where the page is (the CYD shows it on
 * its screen). */
static void sw_run_setup_mode(const SetupWebCtx &ctx, void (*on_wifi_up)(void), void (*on_ready)(const char *where))
{
    static WebServer srv(80);
    String ssid = ctx.prefs_ok ? ctx.prefs->getString("ssid", "") : String("");
    String pass = ctx.prefs_ok ? ctx.prefs->getString("pass", "") : String("");
    String where;

    bool joined = false;
    if (ssid.length()) {
        WiFi.mode(WIFI_STA);
        if (on_wifi_up) on_wifi_up();
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
        joined = WiFi.status() == WL_CONNECTED;
        if (joined) where = "http://" + WiFi.localIP().toString() + "/ (on your WiFi \"" + ssid + "\")";
    }
    if (!joined) {
        uint8_t mac[6];
        fsec_my_mac(mac);
        char name[16];
        snprintf(name, sizeof(name), "EC-%02X%02X", mac[4], mac[5]);
        WiFi.mode(WIFI_AP);
        if (on_wifi_up) on_wifi_up();
        WiFi.softAP(name);   /* open network, by choice - see the header */
        where = String("http://192.168.4.1/ (join the WiFi network \"") + name + "\" first)";
    }
    Serial.printf("SETUP MODE: open %s - runs %d minutes, then restarts\n", where.c_str(), SW_MODE_MINUTES);

    sw_register(srv, ctx);
    srv.begin();
    if (on_ready) on_ready(where.c_str());
    uint32_t t_end = millis() + SW_MODE_MINUTES * 60UL * 1000UL;
    uint32_t last_say = 0;
    while ((int32_t)(t_end - millis()) > 0) {
        srv.handleClient();
        if (millis() - last_say > 30000) {
            last_say = millis();
            Serial.printf("SETUP MODE: %s\n", where.c_str());
        }
        delay(5);
    }
    Serial.println("SETUP MODE: time is up - restarting");
    delay(200);
    ESP.restart();
}
