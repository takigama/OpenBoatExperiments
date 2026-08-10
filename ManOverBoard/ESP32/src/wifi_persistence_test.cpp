// WiFi persistent-credential test: proves credentials survive re-flashing
// (stored in NVS, a flash partition separate from the app - `pio run -t
// upload` only overwrites the app partition, so these persist across
// reflashes; only `pio run -t erase` / a full chip erase wipes them) and
// exercises a SoftAP + captive-portal flow to set them the first time, no
// serial/laptop connection required once flashed.
//
// Flow:
//   1. On boot, try to load ssid/pass from NVS (Preferences, namespace
//      "wifi").
//   2. If found, try to connect as a station (10s timeout).
//   3. If either step fails (no saved creds, or the saved ones don't work
//      anymore), fall into config-portal mode: start our own SoftAP
//      ("OpenBoat-Setup"), serve a one-page HTML form on every URL (so
//      phones' captive-portal auto-popup catches it), save whatever's
//      submitted to NVS, then reboot into normal station mode.
//
// This is deliberately isolated from BLE scanning for now, same as every
// other feature test in this directory - see continuous_scan_diagnostic.cpp
// for the BLE side and full_featured.cpp for the crypto/ring/stats build.
// Each builds via its own PlatformIO environment (`-e <name>`, see
// platformio.ini) - no file renaming needed to switch between them.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// Prefixes every printed line with "[t=<ms since boot>]" - see the other
// builds in this directory for the fuller explanation of how/why this
// works (it's a Print subclass swapped in via #define Serial).
class TimestampedSerial : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }
    int available() { return Serial.available(); }
    int read() { return Serial.read(); }

    size_t write(uint8_t c) override {
        if (at_line_start) {
            at_line_start = false;
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "[t=%8lu] ", millis());
            Serial.write((const uint8_t *)buf, n);
        }
        size_t ret = Serial.write(c);
        if (c == '\n') at_line_start = true;
        return ret;
    }

private:
    bool at_line_start = true;
};

static TimestampedSerial TSerial;
#define Serial TSerial

static const char *NVS_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";

// SoftAP identity during config mode - password protects it from randoms
// joining and resetting the boat's WiFi while docked. WPA2 requires >= 8
// characters.
static const char *SETUP_AP_SSID = "OpenBoat-Setup";
static const char *SETUP_AP_PASSWORD = "setup1234";

static Preferences g_prefs;
static WebServer g_web_server(80);
static DNSServer g_dns_server;

static bool load_saved_credentials(String &ssid, String &pass) {
    g_prefs.begin(NVS_NAMESPACE, true); // read-only
    ssid = g_prefs.getString(NVS_KEY_SSID, "");
    pass = g_prefs.getString(NVS_KEY_PASS, "");
    g_prefs.end();
    return ssid.length() > 0;
}

static void save_credentials(const String &ssid, const String &pass) {
    g_prefs.begin(NVS_NAMESPACE, false); // read-write
    g_prefs.putString(NVS_KEY_SSID, ssid);
    g_prefs.putString(NVS_KEY_PASS, pass);
    g_prefs.end();
}

static void clear_saved_credentials() {
    g_prefs.begin(NVS_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
}

static bool try_connect_sta(const String &ssid, const String &pass, unsigned long timeout_ms) {
    Serial.printf("Connecting to saved WiFi \"%s\"", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            Serial.println();
            Serial.println("Connect timed out.");
            WiFi.disconnect(true);
            return false;
        }
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

static const char *CONFIG_FORM_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OpenBoat WiFi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:400px;margin:2em auto;padding:0 1em}"
    "input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}"
    "input[type=submit]{background:#2a6;color:#fff;border:0;padding:10px}</style>"
    "</head><body>"
    "<h2>OpenBoat MOB Hub</h2>"
    "<p>Enter the boat's WiFi details. Saved on-device (NVS) - survives firmware updates.</p>"
    "<form method='POST' action='/save'>"
    "SSID<input name='ssid' maxlength='32' required>"
    "Password<input name='pass' type='password' maxlength='64'>"
    "<input type='submit' value='Save &amp; Reboot'>"
    "</form></body></html>";

static void handle_root() {
    g_web_server.send(200, "text/html", CONFIG_FORM_HTML);
}

static void handle_save() {
    String ssid = g_web_server.arg("ssid");
    String pass = g_web_server.arg("pass");

    if (ssid.length() == 0) {
        g_web_server.send(400, "text/plain", "SSID required.");
        return;
    }

    save_credentials(ssid, pass);
    Serial.printf("Saved new credentials for \"%s\" via config portal.\n", ssid.c_str());

    g_web_server.send(200, "text/html",
                       "<html><body><h3>Saved. Rebooting...</h3></body></html>");
    delay(1000);
    ESP.restart();
}

// Runs forever (never returns) - config mode has nothing else to do, so a
// blocking loop here is fine, unlike the eventual BLE-scanning main loop
// this will run alongside once merged into the full build.
static void run_config_portal() {
    Serial.println("No working WiFi credentials - starting config portal.");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("Connect to WiFi \"%s\" (password: %s), then browse to http://%s/\n",
                  SETUP_AP_SSID, SETUP_AP_PASSWORD, apIP.toString().c_str());

    // Redirects every DNS query to our own IP, so phones' captive-portal
    // detection pops the config form up automatically instead of requiring
    // the user to manually type the IP.
    g_dns_server.start(53, "*", apIP);

    g_web_server.on("/", HTTP_GET, handle_root);
    g_web_server.on("/save", HTTP_POST, handle_save);
    g_web_server.onNotFound(handle_root); // catch-all so any captive-portal probe URL works
    g_web_server.begin();

    while (true) {
        g_dns_server.processNextRequest();
        g_web_server.handleClient();
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== WiFi persistent-credential test ===");
    Serial.println("Type 'c' + Enter at any time to clear saved credentials and reboot into the config portal.");

    String ssid, pass;
    bool have_saved = load_saved_credentials(ssid, pass);

    bool connected = false;
    if (have_saved) {
        connected = try_connect_sta(ssid, pass, 10000UL);
    } else {
        Serial.println("No saved credentials found in NVS.");
    }

    if (!connected) {
        run_config_portal(); // never returns
    }

    Serial.println("Steady state: WiFi connected using saved NVS credentials.");
}

void loop() {
    static unsigned long last_tick = 0;
    if (millis() - last_tick >= 2000) {
        last_tick = millis();
        Serial.printf("[OTHER WORK] tick, WiFi status=%d, IP=%s\n",
                      (int)WiFi.status(), WiFi.localIP().toString().c_str());
    }

    if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'c' || c == 'C') {
            Serial.println("Clearing saved credentials and rebooting...");
            clear_saved_credentials();
            delay(500);
            ESP.restart();
        }
    }
}
