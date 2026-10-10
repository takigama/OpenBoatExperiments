#include "TobeWeb.h"

#include <Update.h>
#include <WiFi.h>

#include "Tobe.h"
#include "TobeLog.h"
#include "TobeOta.h"
#include "TobeWifi.h"

namespace tobe {
namespace web {

namespace {

ota::Info s_lastCheck;          // the result of the last /sys/ota/check, which /sys/ota/apply installs
bool s_haveCheck = false;

// Dark theme, permanently: these are read at night on a boat.
const char *kStyle =
    "<style>"
    "body{font-family:sans-serif;background:#0a1929;color:#e6e6e6;max-width:520px;margin:1.5em auto;padding:0 1em}"
    "a{color:#6ab0ff}h1{font-size:1.4em;margin:.2em 0}h2,h3{color:#fff}"
    "input,select,textarea{background:#142838;color:#e6e6e6;border:1px solid #33475c;padding:.5em;"
    "width:100%;box-sizing:border-box;margin:.25em 0 .8em}"
    "button{background:#1c3450;color:#fff;border:1px solid #33475c;padding:.6em 1em;width:100%;margin:.2em 0}"
    "button:hover{background:#27456a}hr{border-color:#26384b}table{border-collapse:collapse}"
    "td,th{padding:.2em .6em;border-bottom:1px solid #26384b;text-align:left}"
    ".n{background:#16283c;padding:.7em;border-left:4px solid #6cf;margin:.8em 0}"
    ".bad{border-left-color:#f66}small{color:#9ab}"
    "</style>";

String scanOptions() {
    // Offered as suggestions on the SSID field; typing a name by hand still works (hidden networks).
    // Cached: a scan takes a couple of seconds and the page may be reloaded often.
    static String cached;
    static uint32_t at = 0;
    if (cached.length() && millis() - at < 60000) return cached;
    String out;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) out += "<option value=\"" + escape(WiFi.SSID(i)) + "\">";
    WiFi.scanDelete();
    cached = out.length() ? out : String("<!-- none -->");
    at = millis();
    return cached;
}

void sendPage(WebServer &srv, const String &title, const String &body) {
    srv.send(200, "text/html", page(title, body));
}

}  // namespace

String escape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
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

String pageHead(const String &title, const String &extraHead) {
    String h;
    h.reserve(700);
    h += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width, initial-scale=1'><title>";
    h += escape(tobe::title()) + " - " + escape(title) + "</title>" + kStyle + extraHead + "</head><body>";
    h += "<h1>" + escape(tobe::title()) + "</h1><small>" + escape(title) + " &middot; build " + String(FW_BUILD) +
         "</small>";
    return h;
}

String pageTail() { return "</body></html>"; }

String page(const String &title, const String &body) {
    String h = pageHead(title);
    h.reserve(h.length() + body.length() + 16);
    h += body;
    h += pageTail();
    return h;
}

String note(const String &text) { return "<p class=n>" + escape(text) + "</p>"; }

String systemSection() {
    String h;
    h.reserve(2200);

    h += "<hr><h3>Firmware</h3><p>Running build " + String(FW_BUILD) + ".</p>";
    if (s_haveCheck && s_lastCheck.available) {
        h += "<p>Update available: build " + String((unsigned)s_lastCheck.entry.build) + "</p>"
             "<form method=GET action=/sys/ota/apply><button>Update now</button></form>";
    } else if (s_haveCheck && s_lastCheck.ok) {
        h += "<p>Up to date (GitHub has build " + String((unsigned)s_lastCheck.entry.build) + ").</p>";
    }
    h += "<form method=GET action=/sys/ota/check><button>Check for updates (GitHub)</button></form>";
    // A direct upload next to the GitHub path, for quick iteration while developing. No MD5 here: it is a
    // deliberate upload over the local network, not a fetch from the open internet.
    h += "<form method=POST action=/sys/upload enctype='multipart/form-data'>"
         "<small>or upload a .bin built here:</small><input type=file name=fw accept='.bin'>"
         "<button>Upload and install</button></form>";

    h += "<hr><h3>WiFi</h3>";
    if (wifi::mode() == wifi::Mode::AP)
        h += "<p>Setup network <b>" + escape(wifi::apSsid()) + "</b>. Pick the network to join:</p>";
    else if (WiFi.status() == WL_CONNECTED)
        h += "<p>Joined <b>" + escape(WiFi.SSID()) + "</b> (" + WiFi.localIP().toString() + ").</p>";
    else
        h += "<p>Not joined to a network.</p>";
    if (wifi::hasSaved()) h += "<small>Saved network: " + escape(wifi::savedSsid()) + "</small>";
    h += "<form method=POST action=/sys/wifi>Network name"
         "<input name=ssid list=nets autocomplete=off><datalist id=nets>";
    if (wifi::mode() == wifi::Mode::AP) h += scanOptions();
    h += "</datalist>Password<input type=password name=pass autocomplete=off>"
         "<button>Save and restart</button></form>";

    h += "<hr><h3>Log and restart</h3>"
         "<form method=GET action=/sys/log><button>Show the recent log</button></form>"
         "<form method=POST action=/sys/reboot><button>Restart now</button></form>";
    return h;
}

void attach(WebServer &server) {
    WebServer *srv = &server;

    server.on("/sys/wifi", HTTP_POST, [srv]() {
        String ssid = srv->arg("ssid");
        if (!ssid.length()) {
            sendPage(*srv, "WiFi", note("Enter a network name.") + systemSection());
            return;
        }
        sendPage(*srv, "WiFi", note("Saved \"" + ssid + "\". Restarting to join it...") +
                                   "<p>If it joins, the new address is on your router; if not, this device "
                                   "makes its own network again.</p>");
        wifi::saveAndReboot(ssid, srv->arg("pass"));
    });

    server.on("/sys/ota/check", HTTP_GET, [srv]() {
        s_lastCheck = ota::check();
        s_haveCheck = true;
        String msg = s_lastCheck.ok ? (s_lastCheck.available ? "An update is available."
                                                             : "Already on the latest build.")
                                    : ("Could not check: " + s_lastCheck.error);
        sendPage(*srv, "Firmware", (s_lastCheck.ok ? note(msg) : "<p class='n bad'>" + escape(msg) + "</p>") +
                                       systemSection());
    });

    server.on("/sys/ota/apply", HTTP_GET, [srv]() {
        if (!s_haveCheck || !s_lastCheck.available) {
            sendPage(*srv, "Firmware", note("Nothing to install - check for updates first.") + systemSection());
            return;
        }
        // The reply goes out first; the download then blocks this task and ends in a restart.
        sendPage(*srv, "Updating",
                 note("Downloading build " + String((unsigned)s_lastCheck.entry.build) +
                      ". The device restarts by itself in about a minute - this page will not update.") +
                     "<p>Reload in a minute.</p>");
        srv->client().flush();
        delay(200);
        if (!ota::apply(s_lastCheck.entry.url, s_lastCheck.entry.md5))
            logf("web: update failed: %s", ota::lastError().c_str());
    });

    server.on(
        "/sys/upload", HTTP_POST,
        [srv]() {
            bool ok = !Update.hasError();
            sendPage(*srv, "Upload",
                     ok ? note("Installed. Restarting...")
                        : "<p class='n bad'>Upload failed: " + escape(Update.errorString()) + "</p>");
            if (ok) restart(800);
        },
        [srv]() {
            HTTPUpload &up = srv->upload();
            if (up.status == UPLOAD_FILE_START) {
                logf("web: upload %s", up.filename.c_str());
                Update.begin(UPDATE_SIZE_UNKNOWN);
            } else if (up.status == UPLOAD_FILE_WRITE) {
                if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
            } else if (up.status == UPLOAD_FILE_END) {
                if (!Update.end(true)) Update.printError(Serial);
            } else if (up.status == UPLOAD_FILE_ABORTED) {
                Update.abort();
            }
        });

    server.on("/sys/log", HTTP_GET, [srv]() { srv->send(200, "text/plain", logText()); });

    server.on("/sys/reboot", HTTP_POST, [srv]() {
        sendPage(*srv, "Restart", note("Restarting..."));
        restart(800);
    });
}

String setupPage(const SetupOptions &opt, const String &message) {
    String body;
    if (message.length()) body += note(message);
    body += "<p>Setup mode. It ends by itself after " + String(opt.minutes) + " minutes and the device restarts.</p>";
    if (opt.extraHtml) body += opt.extraHtml();
    body += systemSection();
    return page("Setup", body);
}

bool setupRequested() { return wifi::takeSetupModeRequest(); }

void runSetupMode(const SetupOptions &opt) {
    static WebServer srv(80);
    String where;

    bool joined = false;
    if (wifi::hasSaved()) {
        joined = wifi::joinSaved();
        if (opt.onRadioUp) opt.onRadioUp();
        if (joined) where = "http://" + WiFi.localIP().toString() + "/ (on your WiFi \"" + wifi::savedSsid() + "\")";
    }
    if (!joined) {
        wifi::startAp();   // open network, by choice - see the header
        if (opt.onRadioUp) opt.onRadioUp();
        where = "http://192.168.4.1/ (join the WiFi network \"" + wifi::apSsid() + "\" first)";
    }
    logf("SETUP MODE: open %s - runs %u minutes, then restarts", where.c_str(), (unsigned)opt.minutes);

    srv.on("/", HTTP_GET, [&opt]() { srv.send(200, "text/html", setupPage(opt)); });
    attach(srv);
    if (opt.extraRoutes) opt.extraRoutes(srv);
    srv.onNotFound([]() {
        srv.sendHeader("Location", "/");
        srv.send(302, "text/plain", "");
    });
    srv.begin();
    if (opt.onReady) opt.onReady(where.c_str());

    uint32_t endAt = millis() + opt.minutes * 60UL * 1000UL;
    uint32_t lastSay = 0;
    while ((int32_t)(endAt - millis()) > 0) {
        srv.handleClient();
        if (millis() - lastSay > 30000) {
            lastSay = millis();
            logf("SETUP MODE: %s", where.c_str());
        }
        delay(5);
    }
    logf("SETUP MODE: time is up - restarting");
    restart(200);
    for (;;) delay(1000);   // restart() returns only for the compiler's benefit
}

}  // namespace web
}  // namespace tobe
