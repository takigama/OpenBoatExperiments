// ARCHIVED - not part of the active build (this whole directory sits
// outside src/, so PlatformIO never compiles it). Snapshotted here because
// running the on-device SECP160r1/AES crypto match (~2.1s/candidate) inside
// this file's single-threaded loop() stalled the display/touch/LED and even
// the web server hard enough that it looked like the ESP32 had hung -
// confirmed on real hardware. Decision: move tag-identity crypto off the
// ESP32 entirely onto a bigger machine, bridged over MQTT (not yet
// designed/built as of this snapshot). This file is the reference for
// whatever ends up doing that resolution, since it has real improvements
// over full_featured.cpp's older crypto/scanning code, worth reusing:
//   - Continuous non-blocking BLE scan (duration=0) instead of the old
//     blocking per-loop scan window.
//   - Delta-order optimization: tries delta=0 across all known tags before
//     falling back to -1/+1, cutting typical-case latency.
//   - Device-time offset persisted to NVS (was RAM-only before, so full
//     rediscovery was needed on every reboot) with a smart first-boot seed
//     from the documented factory-reset baseline (FINDINGS.md 5.1) instead
//     of a stale hardcoded guess.
//   - Widen-search bug fix: tries EVERY never-seen known tag, not just the
//     first by array index (the old code got stuck only ever trying tag 0
//     when multiple tags had never matched yet).
//
// Original header comment follows, describing the merged build as it stood
// when archived:
//
// Display + RGB LED + touch + MOB alarm state machine, now merged with real
// BLE/crypto tag resolution (ported from full_featured.cpp) - for the
// ESP32-2432S028R "CYD" (Cheap Yellow Display) board.
//
// This replaces the earlier simulated-tag version of this file (small fixed
// set of tags with jittering fake RSSI) with real FMDN BLE scanning +
// SECP160r1/AES crypto matching against KNOWN_TAGS (from secrets.h /
// no_secrets.h - see FINDINGS.md section 1.2 for how to extract real EIKs).
// Ring/button-press GATT support and the unknown-tag-tracking table from
// full_featured.cpp are NOT included here - deliberately out of scope for
// this merge, still available in full_featured.cpp if wanted later.
//
// IMPORTANT PERFORMANCE CAVEAT: each generate_eid() candidate check costs
// ~2.1s on this hardware (see FINDINGS.md section 4 benchmark). Resolving
// one BLE frame can take up to 3 checks (delta=0 tried first across all
// known tags, then -1, then +1 - see process_pending_eid()), and a
// completely unrecognized frame can cost another ~21s in the widen-search
// before giving up. Because loop() is single-threaded, this crypto work
// will visibly stall the touchscreen/LED for that whole duration - the
// display/LED update lag you may see is this cost, not a bug. The BLE scan
// itself stays non-blocking throughout (see setup()), so sightings are
// never missed while this catches up, just displayed with output_ing delay.
//
// Hardware pin assignments confirmed against the board's own pinout doc
// (witnessmenow/ESP32-Cheap-Yellow-Display PINS.md), not guessed:
//   RGB LED: 3 discrete active-low GPIOs (NOT a WS2812) - RED=4 GREEN=16
//   BLUE=17. Driven via PWM (ledc) rather than plain digitalWrite so the
//   "all clear" green can be dim (easy on the eyes at night) while alarm
//   colors still hit full brightness.
//   Touch (XPT2046, separate SPI bus from the display's): CLK=25 MOSI=32
//   MISO=39 CS=33 IRQ=36.
//   Display (TFT_eSPI, ST7789): pins configured via platformio.ini
//   build_flags (matches TheHub/ESP32StandAloneHub/StandAloneHub's proven
//   config for the same board).
//
// State machine:
//   NORMAL_LIST  - dark green background, scrollable table of tracked tags
//                  (name + RSSI + estimated distance + seconds since last
//                  seen). Ignored tags sort to the bottom, shown greyed out.
//                  Tap a row to ignore/un-ignore that tag. Bottom bar:
//                  Settings (left) / Disable MOB (right).
//   SETTINGS     - adjust the DETECTED -> ACTIONED escalation timeout and
//                  the missing-tag threshold on-device (also available via
//                  /settings on the web UI).
//   DISABLE_MENU - preset duration buttons (1h/2h/4h/8h/until re-enabled) -
//                  MOB detection deliberately always starts back up ENABLED
//                  on every boot; only the ignored-tags list (below)
//                  persists across a reboot.
//   MOB_DETECTED - real automatic detection: a non-ignored known tag hasn't
//                  been seen (a real matching BLE frame) for longer than
//                  the missing-tag threshold (default 6s - see
//                  check_for_missing_tags()). Multi-receiver consensus /
//                  RSSI-trend refinements are still an open design question
//                  for the real deployment - see FINDINGS.md section 6.
//                  Display + LED flash dark-green/bright-red. Tap anywhere
//                  to silence - this adds the tag to the ignore list
//                  (persisted to NVS) and drops straight back to
//                  NORMAL_LIST. Un-ignore later via a NORMAL_LIST row tap.
//   MOB_ACTIONED - if not silenced within the configurable escalation
//                  timeout, the display + LED flash bright-red/bright-blue
//                  instead, as bright as this hardware can manage. Tap
//                  anywhere to silence (same behavior as MOB_DETECTED).
//   IGNORE_CONFIRM - reached from a NORMAL_LIST row tap; toggles that one
//                  tag's ignored state and returns to NORMAL_LIST.
//
// Touch input is debounced (see read_touch()) and dispatched on RELEASE,
// not press, with a movement threshold + cooldown (see dispatch_tap()) -
// this touchscreen's raw readings are noisy enough that press-time
// dispatch caused actions to double/triple-fire.
//
// Device-time offset: FMDN tags' internal EID-rotation clock isn't synced
// to real UTC (see FINDINGS.md section 5) - g_device_time_offset_seconds
// bridges real time to each tag's own clock, self-corrects at runtime
// (try_widen_and_correct_offset()), and is now persisted to NVS so that
// correction only has to happen once per tag-reset, not every reboot. On a
// fresh/first boot (nothing saved yet) it's seeded from the documented
// factory-calibration reference point all 3 currently-known tags share
// (DEVICE_TIME_BASELINE_EPOCH) rather than a stale hardcoded guess.
//
// Debug web interface (once connected to real WiFi - reuses the NVS
// credential + SoftAP config-portal flow proven in wifi_persistence_test.cpp):
//   /          - lists known tags with a "Force trigger now" button each
//                (skips straight to the alarm, for testing the
//                alarm/escalation UI without waiting for a real tag to
//                actually go missing) and the current device-time offset.
//   /settings  - adjust the DETECTED -> ACTIONED escalation timeout and the
//                missing-tag threshold.
//   /ignored   - view/un-ignore tags.
// Serial '1'..'9' also force-triggers MOB for that tag index.

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---- Timestamped Serial (see the other builds in this dir for why) ----
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

// ---- RGB LED (3 discrete active-low GPIOs, NOT a WS2812), PWM-driven ----
static const int LED_RED_PIN = 4;
static const int LED_GREEN_PIN = 16;
static const int LED_BLUE_PIN = 17;
static const int LED_PWM_FREQ = 5000;
static const int LED_PWM_RES_BITS = 8;

static const uint8_t LED_OFF = 0;
static const uint8_t LED_DIM = 40;  // "all clear" - dim, easy on the eyes at night
static const uint8_t LED_FULL = 255; // alarm colors - as bright as this hardware can manage

static void led_init() {
    ledcAttach(LED_RED_PIN, LED_PWM_FREQ, LED_PWM_RES_BITS);
    ledcAttach(LED_GREEN_PIN, LED_PWM_FREQ, LED_PWM_RES_BITS);
    ledcAttach(LED_BLUE_PIN, LED_PWM_FREQ, LED_PWM_RES_BITS);
}

// brightness: 0 (off) .. 255 (full on). LED is active-low, so the raw PWM
// duty (fraction of time the pin is driven HIGH) is inverted from the
// desired on-brightness.
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
    ledcWrite(LED_RED_PIN, 255 - r);
    ledcWrite(LED_GREEN_PIN, 255 - g);
    ledcWrite(LED_BLUE_PIN, 255 - b);
}

// ---- Touch (XPT2046, separate SPI bus from the display) ----
static const int XPT2046_CLK = 25;
static const int XPT2046_MOSI = 32;
static const int XPT2046_MISO = 39;
static const int XPT2046_CS = 33;
static const int XPT2046_IRQ = 36;

static SPIClass g_touch_spi(VSPI);
static XPT2046_Touchscreen g_touch(XPT2046_CS, XPT2046_IRQ);
static TFT_eSPI tft = TFT_eSPI();

static const uint16_t DARK_GREEN = tft.color565(0, 60, 0);
static const uint16_t DARK_RED = tft.color565(60, 0, 0);
static const uint16_t BRIGHT_RED = tft.color565(255, 0, 0);
static const uint16_t BRIGHT_BLUE = tft.color565(0, 0, 255);

// ---- WiFi persistence (same as wifi_persistence_test.cpp) ----
static const char *NVS_WIFI_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASS = "pass";
static const char *SETUP_AP_SSID = "OpenBoat-Setup";
static const char *SETUP_AP_PASSWORD = "setup1234";

static Preferences g_prefs;
static WebServer g_web_server(80);
static DNSServer g_dns_server;

static bool load_saved_wifi(String &ssid, String &pass) {
    g_prefs.begin(NVS_WIFI_NAMESPACE, true);
    ssid = g_prefs.getString(NVS_KEY_SSID, "");
    pass = g_prefs.getString(NVS_KEY_PASS, "");
    g_prefs.end();
    return ssid.length() > 0;
}

static void save_wifi(const String &ssid, const String &pass) {
    g_prefs.begin(NVS_WIFI_NAMESPACE, false);
    g_prefs.putString(NVS_KEY_SSID, ssid);
    g_prefs.putString(NVS_KEY_PASS, pass);
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
    "<title>OpenBoat WiFi Setup</title></head><body>"
    "<h2>OpenBoat MOB Hub</h2>"
    "<form method='POST' action='/save'>"
    "SSID<input name='ssid' maxlength='32' required><br>"
    "Password<input name='pass' type='password' maxlength='64'><br>"
    "<input type='submit' value='Save &amp; Reboot'>"
    "</form></body></html>";

static void handle_wifi_root() { g_web_server.send(200, "text/html", CONFIG_FORM_HTML); }

static void handle_wifi_save() {
    String ssid = g_web_server.arg("ssid");
    String pass = g_web_server.arg("pass");
    if (ssid.length() == 0) {
        g_web_server.send(400, "text/plain", "SSID required.");
        return;
    }
    save_wifi(ssid, pass);
    g_web_server.send(200, "text/html", "<html><body><h3>Saved. Rebooting...</h3></body></html>");
    delay(1000);
    ESP.restart();
}

static void run_config_portal() {
    Serial.println("No working WiFi credentials - starting config portal.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("Connect to WiFi \"%s\" (password: %s), then browse to http://%s/\n",
                  SETUP_AP_SSID, SETUP_AP_PASSWORD, apIP.toString().c_str());
    g_dns_server.start(53, "*", apIP);
    g_web_server.on("/", HTTP_GET, handle_wifi_root);
    g_web_server.on("/save", HTTP_POST, handle_wifi_save);
    g_web_server.onNotFound(handle_wifi_root);
    g_web_server.begin();
    while (true) {
        g_dns_server.processNextRequest();
        g_web_server.handleClient();
    }
}

// ---- Real known tags (from GoogleFindMyTools EIKs) ----
static const int K = 10;               // rotation period exponent
static const uint32_t ROTATION_PERIOD = 1UL << K; // 1024s

struct KnownTag {
    const char *name;
    uint8_t eik[32];
};

#if __has_include("secrets.h")
#include "secrets.h" // real tag EIKs, gitignored
#else
#include "no_secrets.h" // same KNOWN_TAGS[] structure, just empty
#endif

static const int NUM_TAGS = sizeof(KNOWN_TAGS) / sizeof(KNOWN_TAGS[0]);

// Per-tag runtime state (parallel to KNOWN_TAGS, same indices).
static int g_known_rssi[NUM_TAGS];
static unsigned long g_known_last_seen_ms[NUM_TAGS]; // 0 = never seen this boot

// Stable identity string for NVS ignore-list keying and web links - every
// known tag always has a name (unlike the old simulated build's fake
// unknown-EID example).
static String tag_key(int idx) { return String(KNOWN_TAGS[idx].name); }
static String tag_short_label(int idx) { return String(KNOWN_TAGS[idx].name); }

// Rough rule-of-thumb log-distance path-loss estimate, NOT calibrated
// against these specific tags - see FINDINGS.md section 6, real distance
// needs sea-trial calibration.
static float estimate_distance_m(int rssi) {
    const float RSSI_REF_AT_1M = -59.0f;
    const float PATH_LOSS_EXPONENT = 2.0f;
    return pow(10.0f, (RSSI_REF_AT_1M - rssi) / (10.0f * PATH_LOSS_EXPONENT));
}

// ---- Ignored tags (persisted in NVS - survives reboot, unlike the MOB
// disable timer below) ----
static const char *NVS_IGNORE_NAMESPACE = "ignore";
static const char *NVS_IGNORE_KEY = "list";

static String load_ignored_csv() {
    g_prefs.begin(NVS_IGNORE_NAMESPACE, true);
    String csv = g_prefs.getString(NVS_IGNORE_KEY, "");
    g_prefs.end();
    return csv;
}

static void save_ignored_csv(const String &csv) {
    g_prefs.begin(NVS_IGNORE_NAMESPACE, false);
    g_prefs.putString(NVS_IGNORE_KEY, csv);
    g_prefs.end();
}

static bool is_ignored(const String &key) {
    String csv = load_ignored_csv();
    return csv.indexOf("," + key + ",") >= 0;
}

static void add_ignored(const String &key) {
    if (is_ignored(key)) return;
    String csv = load_ignored_csv();
    if (csv.length() == 0) csv = ",";
    csv += key + ",";
    save_ignored_csv(csv);
    Serial.printf("Ignoring tag \"%s\" (persisted).\n", key.c_str());
}

static void remove_ignored(const String &key) {
    String csv = load_ignored_csv();
    csv.replace("," + key + ",", ",");
    save_ignored_csv(csv);
    Serial.printf("Un-ignored tag \"%s\".\n", key.c_str());
}

// Ignored tags sort to the bottom of the on-screen table - this computes
// that display order without permuting KNOWN_TAGS itself, so both drawing
// and row-tap hit-testing use the same order.
static void compute_display_order(int order[]) {
    int w = 0;
    for (int i = 0; i < NUM_TAGS; i++)
        if (!is_ignored(tag_key(i))) order[w++] = i;
    for (int i = 0; i < NUM_TAGS; i++)
        if (is_ignored(tag_key(i))) order[w++] = i;
}

// ---- MOB escalation timeout + missing-tag threshold + device-time offset
// (all persisted - genuine tuning/state that should survive reboot, unlike
// the disable timer below) ----
static const char *NVS_MOBCFG_NAMESPACE = "mobcfg";
static const char *NVS_KEY_ACTION_SECS = "actionSecs";
static const char *NVS_KEY_MISSING_SECS = "missingSecs";
static const char *NVS_KEY_DEV_OFFSET = "devOffset";
static uint32_t g_action_timeout_secs = 10;  // default
static uint32_t g_missing_threshold_secs = 6; // default - how long a tag can go unseen before it's MOB

// All 3 currently-known tags reset to a factory/manufacturing-calibration
// device-time near this point after a battery pull (empirically confirmed -
// see FINDINGS.md section 5.1) - used only as the starting guess when no
// self-corrected offset has been saved yet (see setup()); this is a real
// documented reference point, not a rough guess.
static const uint32_t DEVICE_TIME_BASELINE_EPOCH = 1741145000UL; // ~2025-03-05 00:00 UTC
static uint32_t g_device_time_offset_seconds = 0; // 0 = not yet set - seeded in setup()

static void load_mob_settings() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, true);
    g_action_timeout_secs = g_prefs.getUInt(NVS_KEY_ACTION_SECS, 10);
    g_missing_threshold_secs = g_prefs.getUInt(NVS_KEY_MISSING_SECS, 6);
    g_device_time_offset_seconds = g_prefs.getUInt(NVS_KEY_DEV_OFFSET, 0);
    g_prefs.end();
}

static void save_mob_settings() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, false);
    g_prefs.putUInt(NVS_KEY_ACTION_SECS, g_action_timeout_secs);
    g_prefs.putUInt(NVS_KEY_MISSING_SECS, g_missing_threshold_secs);
    g_prefs.end();
}

static void save_device_time_offset() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, false);
    g_prefs.putUInt(NVS_KEY_DEV_OFFSET, g_device_time_offset_seconds);
    g_prefs.end();
}

// ---- MOB disable (deliberately NOT persisted - always starts enabled) ----
static bool g_mob_disabled = false;
static bool g_mob_disabled_indefinite = false;
static unsigned long g_mob_disabled_until_ms = 0;

static bool mob_is_disabled() {
    if (!g_mob_disabled) return false;
    if (g_mob_disabled_indefinite) return true;
    if ((long)(millis() - g_mob_disabled_until_ms) >= 0) {
        g_mob_disabled = false; // timer expired - re-arm
        return false;
    }
    return true;
}

// ---- Overall state machine ----
enum class Screen { NORMAL_LIST, SETTINGS, DISABLE_MENU, MOB_DETECTED, MOB_ACTIONED, IGNORE_CONFIRM };
static Screen g_screen = Screen::NORMAL_LIST;
static int g_triggering_tag_idx = -1;  // which tag caused the current alarm
static int g_selected_tag_idx = -1;    // which tag IGNORE_CONFIRM is acting on
static unsigned long g_mob_detected_at_ms = 0;
static int g_scroll_offset = 0;
static bool g_needs_redraw = true;
static unsigned long g_flash_last_phase = (unsigned long)-1;

static void trigger_mob(int tag_idx) {
    if (mob_is_disabled()) {
        Serial.printf("Ignoring MOB trigger for tag %d - MOB detection is currently disabled.\n", tag_idx);
        return;
    }
    if (is_ignored(tag_key(tag_idx))) {
        Serial.printf("Ignoring MOB trigger for tag %d - tag is on the ignore list.\n", tag_idx);
        return;
    }
    Serial.printf("[MOB] Triggered for tag %d (%s)\n", tag_idx, tag_short_label(tag_idx).c_str());
    g_triggering_tag_idx = tag_idx;
    g_mob_detected_at_ms = millis();
    g_screen = Screen::MOB_DETECTED;
    g_flash_last_phase = (unsigned long)-1;
}

// Silencing an alarm means "this tag's alarm is dealt with" - it ignores
// the tag outright (persisted to NVS) and returns straight to normal
// monitoring, rather than parking on a separate acknowledged-but-still-
// missing screen. Un-ignore later via a NORMAL_LIST row tap.
static void silence_alarm() {
    if (g_triggering_tag_idx >= 0) {
        add_ignored(tag_key(g_triggering_tag_idx));
        Serial.printf("[MOB] Silenced by touch - %s now ignored.\n", tag_short_label(g_triggering_tag_idx).c_str());
    }
    g_screen = Screen::NORMAL_LIST;
    g_triggering_tag_idx = -1;
    g_needs_redraw = true;
}

// Real automatic MOB detection: a (non-ignored) tag that's gone longer than
// g_missing_threshold_secs since its last real BLE sighting is MOB.
//
// Only auto-triggers from NORMAL_LIST (the idle view) - a genuinely missing
// tag's last_seen never advances on its own, so without this guard,
// silencing an alarm (which just changes g_screen) would see the same tag
// "still missing" on the very next loop() and immediately re-trigger it.
// Restricting to NORMAL_LIST means a silenced/actioned/menu screen
// suppresses new triggers until you're back to actually watching the list.
static void check_for_missing_tags() {
    if (g_screen != Screen::NORMAL_LIST) return;
    unsigned long now = millis();
    for (int i = 0; i < NUM_TAGS; i++) {
        if (g_known_last_seen_ms[i] == 0) continue; // not seen even once yet
        if (now - g_known_last_seen_ms[i] >= g_missing_threshold_secs * 1000UL) {
            trigger_mob(i);
            return;
        }
    }
}

// Records a fresh real sighting of a tag (a matching BLE frame). Logs it to
// serial, and if this is the tag currently causing an active alarm (before
// the user has silenced it), auto-clears back to normal. Once silenced, a
// tag is ignored outright (see silence_alarm()), so this doesn't need to
// cover that case separately.
static void mark_tag_seen(int idx) {
    g_known_last_seen_ms[idx] = millis();
    Serial.printf("[SEEN] %s rssi=%ddBm\n", tag_short_label(idx).c_str(), g_known_rssi[idx]);

    if (idx == g_triggering_tag_idx &&
        (g_screen == Screen::MOB_DETECTED || g_screen == Screen::MOB_ACTIONED)) {
        Serial.printf("[MOB] %s is back - clearing alarm.\n", tag_short_label(idx).c_str());
        g_screen = Screen::NORMAL_LIST;
        g_triggering_tag_idx = -1;
        g_needs_redraw = true;
    }
}

// ---- Crypto: FMDN EID generation (SECP160r1 + AES-256-ECB) - see
// FINDINGS.md sections 1 and 3 for the full derivation and mbedTLS gotchas ----
static const uint8_t TEST_KEY[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint32_t TEST_TIMESTAMP = 1700000000UL;
static const char *EXPECTED_EID_HEX = "6f3bcc7d38665e6cadf7ca48e9ce6d3ea3942d83";

static const char *SECP160R1_P  = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFF";
static const char *SECP160R1_A  = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFC";
static const char *SECP160R1_B  = "1C97BEFC54BD7A8B65ACF89F81D4D4ADC565FA45";
static const char *SECP160R1_GX = "4A96B5688EF573284664698968C38BB913CBFC82";
static const char *SECP160R1_GY = "23A628553168947D59DCC912042351377AC5FB32";
static const char *SECP160R1_N  = "0100000000000000000001F4C8F927AED3CA752257";

static mbedtls_ecp_group g_grp;
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_ctr_drbg;

static int load_secp160r1(mbedtls_ecp_group *grp) {
    int ret;
    if ((ret = mbedtls_mpi_read_string(&grp->P, 16, SECP160R1_P)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->A, 16, SECP160R1_A)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->B, 16, SECP160R1_B)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->G.X, 16, SECP160R1_GX)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->G.Y, 16, SECP160R1_GY)) != 0) return ret;
    if ((ret = mbedtls_mpi_lset(&grp->G.Z, 1)) != 0) return ret;
    if ((ret = mbedtls_mpi_read_string(&grp->N, 16, SECP160R1_N)) != 0) return ret;
    grp->pbits = mbedtls_mpi_bitlen(&grp->P);
    grp->nbits = mbedtls_mpi_bitlen(&grp->P);
    return 0;
}

static int crypto_init() {
    mbedtls_ecp_group_init(&g_grp);
    int ret = load_secp160r1(&g_grp);
    if (ret != 0) return ret;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    return mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func, &g_entropy, NULL, 0);
}

static void build_and_encrypt_r_dash(const uint8_t identity_key[32],
                                      uint32_t masked_timestamp,
                                      uint8_t r_dash_out[32]) {
    uint8_t ts_bytes[4] = {
        (uint8_t)(masked_timestamp >> 24),
        (uint8_t)(masked_timestamp >> 16),
        (uint8_t)(masked_timestamp >> 8),
        (uint8_t)(masked_timestamp)};

    uint8_t data[32];
    memset(data, 0xFF, 11);
    data[11] = (uint8_t)K;
    memcpy(&data[12], ts_bytes, 4);
    memset(&data[16], 0x00, 11);
    data[27] = (uint8_t)K;
    memcpy(&data[28], ts_bytes, 4);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, identity_key, 256);
    // mbedtls_aes_crypt_ecb only does ONE 16-byte block per call - the data
    // structure is 32 bytes (2 blocks), so this needs two calls.
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, data, r_dash_out);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, data + 16, r_dash_out + 16);
    mbedtls_aes_free(&aes);
}

// Uses the persistent g_grp/g_ctr_drbg context set up in crypto_init(). This
// is the ~2.1s-per-call cost referenced throughout this file.
static int generate_eid(const uint8_t identity_key[32], uint32_t timestamp,
                         uint8_t eid_out[20]) {
    uint32_t masked_timestamp = timestamp & ~(ROTATION_PERIOD - 1UL);

    uint8_t r_dash[32];
    build_and_encrypt_r_dash(identity_key, masked_timestamp, r_dash);

    mbedtls_mpi r_dash_mpi, r;
    mbedtls_mpi_init(&r_dash_mpi);
    mbedtls_mpi_init(&r);

    int ret = mbedtls_mpi_read_binary(&r_dash_mpi, r_dash, 32);
    if (ret == 0) ret = mbedtls_mpi_mod_mpi(&r, &r_dash_mpi, &g_grp.N);

    mbedtls_ecp_point R;
    mbedtls_ecp_point_init(&R);

    if (ret == 0) {
        ret = mbedtls_ecp_mul(&g_grp, &R, &r, &g_grp.G, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&R.X, eid_out, 20);
    }

    mbedtls_ecp_point_free(&R);
    mbedtls_mpi_free(&r_dash_mpi);
    mbedtls_mpi_free(&r);

    return ret;
}

static bool self_check() {
    uint8_t eid[20];
    if (generate_eid(TEST_KEY, TEST_TIMESTAMP, eid) != 0) return false;

    char computed_hex[41];
    for (int i = 0; i < 20; i++) sprintf(&computed_hex[i * 2], "%02x", eid[i]);
    computed_hex[40] = '\0';

    return strcmp(computed_hex, EXPECTED_EID_HEX) == 0;
}

// ---- Live clock: network time (via already-connected WiFi) or manual
// entry once at boot, tracked via millis() after that ----
static uint32_t g_base_unix_time = 0;
static uint32_t g_base_millis = 0;

static uint32_t current_unix_time() {
    return g_base_unix_time + (millis() - g_base_millis) / 1000UL;
}

// Days since 1970-01-01 for a given civil (proleptic Gregorian) date.
// Howard Hinnant's well-known constant-time algorithm - avoids depending on
// strptime()/timegm() being present in this toolchain's libc at all.
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// Parses an RFC 7231 HTTP-date header, e.g. "Wed, 21 Oct 2015 07:28:00 GMT",
// into a Unix timestamp.
static uint32_t parse_http_date_to_unix(const String &date) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = date.substring(5, 7).toInt();
    String monStr = date.substring(8, 11);
    int year = date.substring(12, 16).toInt();
    int hour = date.substring(17, 19).toInt();
    int minute = date.substring(20, 22).toInt();
    int second = date.substring(23, 25).toInt();

    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (monStr == months[i]) {
            month = i + 1;
            break;
        }
    }

    long days = days_from_civil(year, month, day);
    return (uint32_t)days * 86400UL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + (uint32_t)second;
}

// Reads the Date header off a plain HTTP response (every HTTP response has
// one - no JSON API dependency, no TLS needed) to seed the clock. Assumes
// WiFi is already connected (unlike full_featured.cpp's version of this,
// this file's own NVS-based WiFi flow already handled that). Returns 0 and
// fills *out_unix_time on success; nonzero if the request fails, so the
// caller can fall back to manual entry.
static int fetch_time_from_network(uint32_t *out_unix_time) {
    HTTPClient http;
    const char *headerKeys[] = {"Date"};
    http.collectHeaders(headerKeys, 1);
    http.begin("http://www.gstatic.com/generate_204");
    int code = http.GET();
    if (code != 204 && code != 200) {
        Serial.printf("Time request failed, HTTP code %d\n", code);
        http.end();
        return -1;
    }

    String dateHeader = http.header("Date");
    http.end();

    if (dateHeader.length() < 25) {
        Serial.println("No usable Date header in the response.");
        return -1;
    }

    *out_unix_time = parse_http_date_to_unix(dateHeader);
    return 0;
}

static void prompt_for_time() {
    Serial.println();
    Serial.println("Enter the current UNIX timestamp (e.g. `date +%s` in a terminal) and press Enter:");

    String line = "";
    while (true) {
        if (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) break;
                continue;
            }
            Serial.write(c); // local echo
            line += c;
        } else {
            delay(5);
        }
    }
    Serial.println();

    g_base_unix_time = (uint32_t)line.toInt();
    g_base_millis = millis();
    Serial.printf("Received %d chars, parsed unix_time=%lu\n", line.length(), (unsigned long)g_base_unix_time);
}

// ---- BLE scanning ----
static const BLEUUID FMDN_SERVICE_UUID((uint16_t)0xFEAA);

// onResult() runs inside the BLE stack's own task (BTC_TASK), which must
// stay responsive - it must NOT do the crypto (ECC scalar mult is too slow
// to run there). It only does the cheap frame-format check and hands the
// 20-byte candidate off to loop() via this queue, which runs at normal app
// priority.
static const int PENDING_QUEUE_LEN = 8;
struct PendingEid {
    uint8_t eid[20];
    int rssi;
};
static PendingEid g_pending[PENDING_QUEUE_LEN];
static volatile int g_pending_count = 0;
// onResult() (BLE stack task) and loop() (app task) can run on different
// cores concurrently - this guards the shared queue between them.
static portMUX_TYPE g_pending_mux = portMUX_INITIALIZER_UNLOCKED;

class FmdnAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveServiceData()) return;
        if (!advertisedDevice.getServiceDataUUID().equals(FMDN_SERVICE_UUID)) return;

        String data = advertisedDevice.getServiceData();
        if (data.length() < 21) return; // frame type (1) + EID (20)

        uint8_t frame_type = (uint8_t)data[0];
        if (frame_type != 0x40 && frame_type != 0x41) return;

        portENTER_CRITICAL(&g_pending_mux);
        if (g_pending_count < PENDING_QUEUE_LEN) {
            PendingEid &slot = g_pending[g_pending_count];
            memcpy(slot.eid, data.c_str() + 1, 20);
            slot.rssi = advertisedDevice.getRSSI();
            g_pending_count++;
        }
        portEXIT_CRITICAL(&g_pending_mux);
    }
};

// Self-healing offset drift correction. An "unknown" EID might just be a
// known tag drifted outside the usual +/-1 window. Rather than brute-force
// searching every unknown frame against every tag (expensive -
// ~2.1s/candidate), only widen the search for the tag(s) most likely to
// have actually drifted, and only out to a bounded extra range, not a full
// re-discovery. Persists the corrected offset to NVS so this only has to
// happen again if a tag itself gets reset.
//
// Tries EVERY never-seen tag, not just one - if two or more tags have
// never matched yet (e.g. right after several battery pulls, or right
// after this firmware is first installed), we don't know which one a given
// unmatched frame belongs to, so picking only the first never-seen tag and
// stopping there means the others can never resolve at all. Once every
// known tag has been seen at least once, falls back to the cheaper
// single-tag heuristic (least-recently-seen is most likely to have
// drifted).
static const int WIDE_SEARCH_WINDOWS = 5; // +/-5 windows ~= 85 minutes of drift tolerance

static bool try_widen_and_correct_offset(const uint8_t seen_eid[20], uint32_t real_now, int rssi) {
    int candidates[NUM_TAGS];
    int num_candidates = 0;
    for (int i = 0; i < NUM_TAGS; i++) {
        if (g_known_last_seen_ms[i] == 0) candidates[num_candidates++] = i;
    }
    if (num_candidates == 0) {
        int worst_idx = 0;
        for (int i = 1; i < NUM_TAGS; i++) {
            if (g_known_last_seen_ms[i] < g_known_last_seen_ms[worst_idx]) worst_idx = i;
        }
        candidates[num_candidates++] = worst_idx;
    }

    uint32_t now = real_now - g_device_time_offset_seconds;

    for (int c = 0; c < num_candidates; c++) {
        int tag_idx = candidates[c];
        for (int delta = -WIDE_SEARCH_WINDOWS; delta <= WIDE_SEARCH_WINDOWS; delta++) {
            if (delta >= -1 && delta <= 1) continue; // already covered by the normal pass
            uint32_t t = now + (uint32_t)(delta * (int)ROTATION_PERIOD);
            uint8_t candidate[20];
            if (generate_eid(KNOWN_TAGS[tag_idx].eik, t, candidate) != 0) continue;

            if (memcmp(candidate, seen_eid, 20) == 0) {
                int32_t delta_seconds = delta * (int32_t)ROTATION_PERIOD;
                uint32_t old_offset = g_device_time_offset_seconds;
                g_device_time_offset_seconds = (uint32_t)((int32_t)old_offset - delta_seconds);
                save_device_time_offset();
                Serial.printf("[AUTO-CORRECT] %s was actually delta=%d away - offset corrected %lu -> %lu "
                              "(drift was ~%ld s)\n",
                              KNOWN_TAGS[tag_idx].name, delta, (unsigned long)old_offset,
                              (unsigned long)g_device_time_offset_seconds, (long)delta_seconds);
                g_known_rssi[tag_idx] = rssi;
                mark_tag_seen(tag_idx);
                return true;
            }
        }
    }
    return false;
}

// Does the actual crypto comparison - called from loop(), never from the
// BLE callback. Checks delta=0 across ALL known tags first (the common
// case - a tag currently in its rotation window), then -1, then +1, rather
// than exhausting one tag's full +/-1 window before moving to the next -
// this was identified but not yet implemented as "the obvious next
// optimization" earlier in this project (see FINDINGS.md section 4);
// typical-case latency drops from up to 9 generate_eid() calls to as few
// as 1.
static void process_pending_eid(const uint8_t seen_eid[20], int rssi) {
    uint32_t real_now = current_unix_time();
    uint32_t now = real_now - g_device_time_offset_seconds;

    static const int DELTA_ORDER[3] = {0, -1, 1};
    for (int d = 0; d < 3; d++) {
        uint32_t t = now + (uint32_t)(DELTA_ORDER[d] * (int)ROTATION_PERIOD);
        for (int tag_idx = 0; tag_idx < NUM_TAGS; tag_idx++) {
            uint8_t candidate[20];
            if (generate_eid(KNOWN_TAGS[tag_idx].eik, t, candidate) != 0) continue;
            if (memcmp(candidate, seen_eid, 20) == 0) {
                g_known_rssi[tag_idx] = rssi;
                mark_tag_seen(tag_idx);
                return;
            }
        }
    }

    if (try_widen_and_correct_offset(seen_eid, real_now, rssi)) {
        return; // wasn't actually unknown - just a drifted known tag, now corrected
    }

    Serial.print("[UNKNOWN] eid=");
    for (int i = 0; i < 20; i++) Serial.printf("%02x", seen_eid[i]);
    Serial.println();
}

// ---- Debug/settings web UI ----
static void handle_debug_root() {
    unsigned long now = millis();
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta http-equiv='refresh' content='5'>"
                  "<title>OpenBoat MOB debug</title></head><body>";
    html += "<h2>Known tags</h2><table border=1 cellpadding=6>";
    html += "<tr><th>Tag</th><th>RSSI</th><th>Last seen</th><th>Ignored?</th><th>Action</th></tr>";
    for (int i = 0; i < NUM_TAGS; i++) {
        bool ignored = is_ignored(tag_key(i));
        String rssi_str = g_known_last_seen_ms[i] == 0 ? "--" : String(g_known_rssi[i]);
        String last_seen =
            g_known_last_seen_ms[i] == 0 ? "never" : String((now - g_known_last_seen_ms[i]) / 1000) + "s ago";
        html += "<tr><td>" + tag_short_label(i) + "</td><td>" + rssi_str + "</td><td>" + last_seen + "</td><td>" +
                (ignored ? "yes" : "no") +
                "</td><td>"
                "<form method='POST' action='/trigger' style='display:inline'>"
                "<input type='hidden' name='tag' value='" +
                String(i) + "'>"
                            "<input type='submit' value='Force trigger now'></form></td></tr>";
    }
    html += "</table>";
    html += "<p>Real BLE scanning is active. 'Force trigger now' skips straight to the alarm, for "
            "testing the alarm/escalation UI without waiting for a real tag to actually go missing.</p>";
    html += "<p>Device-time offset: " + String(g_device_time_offset_seconds) +
            "s (self-corrects automatically, persisted across reboots).</p>";
    html += "<p>MOB detection currently: <b>" + String(mob_is_disabled() ? "DISABLED" : "enabled") + "</b></p>";
    html += "<p><a href='/settings'>Settings</a> | <a href='/ignored'>Ignored tags</a></p>";
    html += "</body></html>";
    g_web_server.send(200, "text/html", html);
}

static void handle_debug_trigger() {
    if (!g_web_server.hasArg("tag")) {
        g_web_server.send(400, "text/plain", "missing tag");
        return;
    }
    int idx = g_web_server.arg("tag").toInt();
    if (idx < 0 || idx >= NUM_TAGS) {
        g_web_server.send(400, "text/plain", "bad tag index");
        return;
    }
    trigger_mob(idx);
    g_web_server.sendHeader("Location", "/");
    g_web_server.send(303);
}

static void handle_settings_root() {
    String html = "<!DOCTYPE html><html><body><h2>MOB Settings</h2>"
                  "<form method='POST' action='/settings/save'>"
                  "Detected -&gt; Actioned escalation timeout (seconds): "
                  "<input name='secs' type='number' min='1' max='3600' value='" +
                  String(g_action_timeout_secs) +
                  "'><br><br>"
                  "Missing-tag threshold (seconds before a silent tag is MOB): "
                  "<input name='missingSecs' type='number' min='2' max='3600' value='" +
                  String(g_missing_threshold_secs) +
                  "'><br><br>"
                  "<input type='submit' value='Save'></form>"
                  "<p><a href='/'>Back</a></p></body></html>";
    g_web_server.send(200, "text/html", html);
}

static void handle_settings_save() {
    if (g_web_server.hasArg("secs")) {
        long secs = g_web_server.arg("secs").toInt();
        if (secs > 0) {
            g_action_timeout_secs = (uint32_t)secs;
            Serial.printf("Saved new MOB escalation timeout: %lu s\n", (unsigned long)g_action_timeout_secs);
        }
    }
    if (g_web_server.hasArg("missingSecs")) {
        long secs = g_web_server.arg("missingSecs").toInt();
        if (secs > 0) {
            g_missing_threshold_secs = (uint32_t)secs;
            Serial.printf("Saved new missing-tag threshold: %lu s\n", (unsigned long)g_missing_threshold_secs);
        }
    }
    save_mob_settings();
    g_web_server.sendHeader("Location", "/settings");
    g_web_server.send(303);
}

static void handle_ignored_root() {
    String html = "<!DOCTYPE html><html><body><h2>Ignored tags</h2><ul>";
    for (int i = 0; i < NUM_TAGS; i++) {
        if (is_ignored(tag_key(i))) {
            html += "<li>" + tag_short_label(i) +
                    " <form method='POST' action='/ignored/remove' style='display:inline'>"
                    "<input type='hidden' name='key' value='" +
                    tag_key(i) +
                    "'>"
                    "<input type='submit' value='Un-ignore'></form></li>";
        }
    }
    html += "</ul><p><a href='/'>Back</a></p></body></html>";
    g_web_server.send(200, "text/html", html);
}

static void handle_ignored_remove() {
    if (g_web_server.hasArg("key")) remove_ignored(g_web_server.arg("key"));
    g_web_server.sendHeader("Location", "/ignored");
    g_web_server.send(303);
}

// ---- Touch reading ----
struct TouchPoint {
    int16_t x, y;
    bool valid;
};

// Raw touched()/getPoint() readings from this touchscreen bounce - toggling
// rapidly for tens of ms around a real press/release, not just right at the
// edges. This is the first of three layers dealing with it (see also
// dispatch_tap()'s move-threshold + cooldown in handle_touch_input()):
// smooths the raw touched()/not-touched signal itself so a single glitch
// doesn't immediately flip state.
static bool g_touch_raw_state = false;
static bool g_touch_debounced_state = false;
static unsigned long g_touch_state_change_ms = 0;
static const unsigned long TOUCH_DEBOUNCE_MS = 40;

static TouchPoint read_touch() {
    TouchPoint p{0, 0, false};

    bool raw = g_touch.touched();
    if (raw != g_touch_raw_state) {
        g_touch_raw_state = raw;
        g_touch_state_change_ms = millis();
    }
    if (millis() - g_touch_state_change_ms >= TOUCH_DEBOUNCE_MS) {
        g_touch_debounced_state = g_touch_raw_state;
    }
    if (!g_touch_debounced_state) return p;

    TS_Point raw_pt = g_touch.getPoint();
    // Calibration constants are the commonly-cited raw ADC range for this
    // board's landscape orientation - not individually calibrated for this
    // specific unit, may need adjustment if touches land noticeably off.
    p.x = constrain(map(raw_pt.x, 200, 3700, 0, tft.width() - 1), 0, tft.width() - 1);
    p.y = constrain(map(raw_pt.y, 240, 3800, 0, tft.height() - 1), 0, tft.height() - 1);
    p.valid = true;
    return p;
}

// ---- Layout constants ----
static const int ROW_H = 28;
static const int HEADER_H = 24;       // title bar height, used by every screen
static const int TABLE_HEADER_H = 20; // NORMAL_LIST's extra column-header row
static const int LIST_TOP = HEADER_H + TABLE_HEADER_H; // where table data rows start
static const int BUTTON_H = 40;

static const int COL_TAG_X = 4;
static const int COL_RSSI_X = 140;
static const int COL_DIST_X = 195;
static const int COL_SEEN_X = 255;

// ---- Drawing ----
static void draw_normal_screen() {
    tft.fillScreen(DARK_GREEN);
    tft.setTextColor(TFT_WHITE, DARK_GREEN);
    tft.setTextSize(2);
    tft.setCursor(4, 2);
    tft.print("OpenBoat MOB Monitor");

    // Table header row
    tft.setTextSize(1);
    tft.setCursor(COL_TAG_X, HEADER_H + 6);
    tft.print("Tag");
    tft.setCursor(COL_RSSI_X, HEADER_H + 6);
    tft.print("RSSI");
    tft.setCursor(COL_DIST_X, HEADER_H + 6);
    tft.print("Dist");
    tft.setCursor(COL_SEEN_X, HEADER_H + 6);
    tft.print("Seen");

    int list_bottom = tft.height() - BUTTON_H;
    tft.drawFastHLine(0, LIST_TOP - 1, tft.width(), TFT_WHITE);
    tft.drawFastVLine(COL_RSSI_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);
    tft.drawFastVLine(COL_DIST_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);
    tft.drawFastVLine(COL_SEEN_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);

    int max_scroll = max(0, NUM_TAGS * ROW_H - (list_bottom - LIST_TOP));
    g_scroll_offset = constrain(g_scroll_offset, 0, max_scroll);

    int order[NUM_TAGS];
    compute_display_order(order);

    unsigned long now = millis();
    int first_row = g_scroll_offset / ROW_H;
    int y = LIST_TOP - (g_scroll_offset % ROW_H);
    for (int row = first_row; row < NUM_TAGS && y < list_bottom; row++, y += ROW_H) {
        int i = order[row];
        bool ignored = is_ignored(tag_key(i));
        tft.setTextColor(ignored ? TFT_LIGHTGREY : TFT_WHITE, DARK_GREEN);

        tft.setCursor(COL_TAG_X, y + 8);
        tft.print(tag_short_label(i));
        if (ignored) tft.print(" (ign)");

        if (g_known_last_seen_ms[i] == 0) {
            tft.setCursor(COL_RSSI_X, y + 8);
            tft.print("never seen");
        } else {
            float dist = estimate_distance_m(g_known_rssi[i]);
            tft.setCursor(COL_RSSI_X, y + 8);
            tft.printf("%d", g_known_rssi[i]);
            tft.setCursor(COL_DIST_X, y + 8);
            tft.printf("~%.1f", dist);
            tft.setCursor(COL_SEEN_X, y + 8);
            tft.printf("%lus", (now - g_known_last_seen_ms[i]) / 1000UL);
        }

        tft.drawFastHLine(0, y + ROW_H - 1, tft.width(), TFT_DARKGREEN);
    }

    // Bottom bar: Settings (left) | Disable MOB (right)
    int bar_y = tft.height() - BUTTON_H;
    tft.fillRect(0, bar_y, tft.width(), BUTTON_H, TFT_DARKGREY);
    tft.drawFastVLine(tft.width() / 2, bar_y, BUTTON_H, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(10, bar_y + 10);
    tft.print("Settings");
    tft.setCursor(tft.width() / 2 + 10, bar_y + 10);
    tft.print(mob_is_disabled() ? "DISABLED (tap)" : "Disable MOB");
}

static const char *DISABLE_MENU_LABELS[] = {"1 hour", "2 hours", "4 hours", "8 hours", "Until re-enabled", "Cancel"};
static const int DISABLE_MENU_COUNT = 6;

static void draw_disable_menu() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print("Disable MOB for how long?");

    int y = HEADER_H + 10;
    for (int i = 0; i < DISABLE_MENU_COUNT; i++) {
        tft.fillRect(10, y, tft.width() - 20, BUTTON_H - 6, TFT_DARKGREY);
        tft.setCursor(20, y + 10);
        tft.print(DISABLE_MENU_LABELS[i]);
        y += BUTTON_H;
    }
}

static const int SETTINGS_BTN_W = 70;
static const int SETTINGS_BTN_H = 34;
static const int SETTINGS_ROW1_LABEL_Y = HEADER_H + 4;
static const int SETTINGS_ROW1_Y = SETTINGS_ROW1_LABEL_Y + 14;
static const int SETTINGS_ROW2_LABEL_Y = SETTINGS_ROW1_Y + SETTINGS_BTN_H + 12;
static const int SETTINGS_ROW2_Y = SETTINGS_ROW2_LABEL_Y + 14;
static const int SETTINGS_INFO_Y = SETTINGS_ROW2_Y + SETTINGS_BTN_H + 10;
static const int SETTINGS_BACK_Y = SETTINGS_INFO_Y + 20;

static const uint32_t SETTINGS_ACTION_STEP_SECS = 1;
static const uint32_t SETTINGS_ACTION_MIN_SECS = 5;
static const uint32_t SETTINGS_ACTION_MAX_SECS = 300;
static const uint32_t SETTINGS_MISSING_STEP_SECS = 1;
static const uint32_t SETTINGS_MISSING_MIN_SECS = 2;
static const uint32_t SETTINGS_MISSING_MAX_SECS = 120;

static void draw_adjust_row(int label_y, int row_y, const char *label, uint32_t value) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, label_y);
    tft.print(label);

    tft.fillRect(10, row_y, SETTINGS_BTN_W, SETTINGS_BTN_H, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(35, row_y + 8);
    tft.print("-");

    char buf[8];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)value);
    tft.setCursor(tft.width() / 2 - 24, row_y + 8);
    tft.print(buf);

    tft.fillRect(tft.width() - 10 - SETTINGS_BTN_W, row_y, SETTINGS_BTN_W, SETTINGS_BTN_H, TFT_DARKGREY);
    tft.setCursor(tft.width() - 10 - SETTINGS_BTN_W + 25, row_y + 8);
    tft.print("+");
}

static void draw_settings_screen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print("Settings");

    draw_adjust_row(SETTINGS_ROW1_LABEL_Y, SETTINGS_ROW1_Y, "Detected -> Actioned timeout:", g_action_timeout_secs);
    draw_adjust_row(SETTINGS_ROW2_LABEL_Y, SETTINGS_ROW2_Y, "Missing-tag threshold:", g_missing_threshold_secs);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, SETTINGS_INFO_Y);
    tft.printf("WiFi IP: %s", WiFi.localIP().toString().c_str());

    tft.fillRect(10, SETTINGS_BACK_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(20, SETTINGS_BACK_Y + 10);
    tft.print("Back");
}

static void draw_mob_flash_screen(uint16_t colorA, uint16_t colorB, unsigned long period_ms, const char *headline) {
    unsigned long phase = (millis() / period_ms) % 2;
    if (phase == g_flash_last_phase) return; // no change yet, skip the redraw
    g_flash_last_phase = phase;

    uint16_t bg = (phase == 0) ? colorA : colorB;
    tft.fillScreen(bg);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setTextSize(3);
    tft.setCursor(10, 40);
    tft.print(headline);
    tft.setTextSize(2);
    tft.setCursor(10, 100);
    if (g_triggering_tag_idx >= 0) {
        tft.print("Missing: ");
        tft.print(tag_short_label(g_triggering_tag_idx));
    }
    tft.setTextSize(1);
    tft.setCursor(10, tft.height() - 20);
    tft.print("Tap anywhere to silence");
}

static const int IGNORE_CONFIRM_YES_Y = 100;
static const int IGNORE_CONFIRM_CANCEL_Y = 150;

static void draw_ignore_confirm_screen() {
    bool already_ignored = g_selected_tag_idx >= 0 && is_ignored(tag_key(g_selected_tag_idx));

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print(already_ignored ? "Un-ignore this tag?" : "Ignore this tag?");
    tft.setCursor(10, 50);
    if (g_selected_tag_idx >= 0) tft.print(tag_short_label(g_selected_tag_idx));

    tft.fillRect(10, IGNORE_CONFIRM_YES_Y, tft.width() - 20, BUTTON_H, already_ignored ? TFT_DARKGREY : TFT_RED);
    tft.setCursor(20, IGNORE_CONFIRM_YES_Y + 12);
    tft.print(already_ignored ? "Yes, un-ignore this tag" : "Yes, ignore this tag");

    tft.fillRect(10, IGNORE_CONFIRM_CANCEL_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setCursor(20, IGNORE_CONFIRM_CANCEL_Y + 12);
    tft.print("Cancel");
}

static void render_current_screen() {
    switch (g_screen) {
    case Screen::MOB_DETECTED:
        draw_mob_flash_screen(DARK_GREEN, BRIGHT_RED, 400, "MAN OVERBOARD");
        return; // manages its own phase-based redraw, not the dirty flag
    case Screen::MOB_ACTIONED:
        draw_mob_flash_screen(BRIGHT_RED, BRIGHT_BLUE, 150, "!! TAKING ACTION !!");
        return;
    default:
        break;
    }

    if (!g_needs_redraw) return;
    g_needs_redraw = false;

    switch (g_screen) {
    case Screen::NORMAL_LIST: draw_normal_screen(); break;
    case Screen::SETTINGS: draw_settings_screen(); break;
    case Screen::DISABLE_MENU: draw_disable_menu(); break;
    case Screen::IGNORE_CONFIRM: draw_ignore_confirm_screen(); break;
    default: break;
    }
}

// ---- Touch dispatch - see dispatch_tap() below for when/how this gets called ----
static void handle_touch_for_screen(const TouchPoint &p) {
    switch (g_screen) {
    case Screen::NORMAL_LIST: {
        int button_y = tft.height() - BUTTON_H;
        if (p.y >= button_y) {
            if (p.x < tft.width() / 2) {
                g_screen = Screen::SETTINGS;
            } else if (mob_is_disabled()) {
                g_mob_disabled = false;
                Serial.println("MOB re-enabled by touch.");
            } else {
                g_screen = Screen::DISABLE_MENU;
            }
        }
        break;
    }
    case Screen::SETTINGS: {
        bool left = p.x < 10 + SETTINGS_BTN_W;
        bool right = p.x >= tft.width() - 10 - SETTINGS_BTN_W;
        if (p.y >= SETTINGS_ROW1_Y && p.y < SETTINGS_ROW1_Y + SETTINGS_BTN_H) {
            if (left && g_action_timeout_secs > SETTINGS_ACTION_MIN_SECS) {
                g_action_timeout_secs -= SETTINGS_ACTION_STEP_SECS;
                save_mob_settings();
            } else if (right && g_action_timeout_secs < SETTINGS_ACTION_MAX_SECS) {
                g_action_timeout_secs += SETTINGS_ACTION_STEP_SECS;
                save_mob_settings();
            }
        } else if (p.y >= SETTINGS_ROW2_Y && p.y < SETTINGS_ROW2_Y + SETTINGS_BTN_H) {
            if (left && g_missing_threshold_secs > SETTINGS_MISSING_MIN_SECS) {
                g_missing_threshold_secs -= SETTINGS_MISSING_STEP_SECS;
                save_mob_settings();
            } else if (right && g_missing_threshold_secs < SETTINGS_MISSING_MAX_SECS) {
                g_missing_threshold_secs += SETTINGS_MISSING_STEP_SECS;
                save_mob_settings();
            }
        } else if (p.y >= SETTINGS_BACK_Y && p.y < SETTINGS_BACK_Y + BUTTON_H) {
            g_screen = Screen::NORMAL_LIST;
        }
        break;
    }
    case Screen::DISABLE_MENU: {
        int idx = (p.y - (HEADER_H + 10)) / BUTTON_H;
        static const uint32_t HOURS[] = {1, 2, 4, 8};
        if (idx >= 0 && idx <= 3) {
            g_mob_disabled = true;
            g_mob_disabled_indefinite = false;
            g_mob_disabled_until_ms = millis() + HOURS[idx] * 3600000UL;
            Serial.printf("MOB disabled for %lu hour(s).\n", (unsigned long)HOURS[idx]);
            g_screen = Screen::NORMAL_LIST;
        } else if (idx == 4) {
            g_mob_disabled = true;
            g_mob_disabled_indefinite = true;
            Serial.println("MOB disabled until re-enabled.");
            g_screen = Screen::NORMAL_LIST;
        } else if (idx == 5) {
            g_screen = Screen::NORMAL_LIST;
        }
        break;
    }
    case Screen::MOB_DETECTED:
    case Screen::MOB_ACTIONED:
        silence_alarm();
        break;
    case Screen::IGNORE_CONFIRM:
        // Both buttons drop straight back to the normal screen.
        if (p.y >= IGNORE_CONFIRM_YES_Y && p.y < IGNORE_CONFIRM_YES_Y + BUTTON_H) {
            if (g_selected_tag_idx >= 0) {
                String key = tag_key(g_selected_tag_idx);
                if (is_ignored(key)) remove_ignored(key);
                else add_ignored(key);
            }
            g_screen = Screen::NORMAL_LIST;
            g_selected_tag_idx = -1;
            g_triggering_tag_idx = -1;
        } else if (p.y >= IGNORE_CONFIRM_CANCEL_Y && p.y < IGNORE_CONFIRM_CANCEL_Y + BUTTON_H) {
            g_screen = Screen::NORMAL_LIST;
            g_selected_tag_idx = -1;
            g_triggering_tag_idx = -1;
        }
        break;
    }
    g_needs_redraw = true;
}

// A short, low-movement press on a table row ignores/un-ignores that tag -
// distinguished from a scroll drag by dispatch_tap()'s move-distance
// tracking, so dragging the list never accidentally opens this.
static void handle_row_tap(int16_t y) {
    int list_bottom = tft.height() - BUTTON_H;
    if (y < LIST_TOP || y >= list_bottom) return;
    int tapped_row = (y - LIST_TOP + g_scroll_offset) / ROW_H;
    if (tapped_row < 0 || tapped_row >= NUM_TAGS) return;
    int order[NUM_TAGS];
    compute_display_order(order);
    g_selected_tag_idx = order[tapped_row];
    g_screen = Screen::IGNORE_CONFIRM;
    g_needs_redraw = true;
}

static bool g_touch_was_down = false;
static int16_t g_touch_prev_x = 0, g_touch_prev_y = 0;
static int16_t g_touch_down_x = 0, g_touch_down_y = 0;
static int g_touch_moved_total = 0;
static const int TAP_MOVE_THRESHOLD = 20; // px - a press+release that never strayed this far from its start counts as a tap

// Every screen's buttons/rows fire on RELEASE, not press - dispatching on
// press meant a noisy/bouncy contact could re-trigger the same button
// several times during what was physically one tap. A cooldown after any
// action fires adds a second layer of protection against release-time
// bounce re-firing the same tap.
static unsigned long g_last_action_ms = 0;
static const unsigned long TOUCH_ACTION_COOLDOWN_MS = 250;

static void dispatch_tap(int16_t x, int16_t y) {
    if (millis() - g_last_action_ms < TOUCH_ACTION_COOLDOWN_MS) return;
    g_last_action_ms = millis();

    TouchPoint p{x, y, true};
    if (g_screen == Screen::NORMAL_LIST) {
        int button_y = tft.height() - BUTTON_H;
        if (y >= button_y) handle_touch_for_screen(p);
        else handle_row_tap(y);
    } else {
        handle_touch_for_screen(p);
    }
}

static void handle_touch_input() {
    TouchPoint p = read_touch();
    bool edge_down = p.valid && !g_touch_was_down;
    bool edge_up = !p.valid && g_touch_was_down;

    if (edge_down) {
        g_touch_down_x = p.x;
        g_touch_down_y = p.y;
        g_touch_moved_total = 0;
    } else if (p.valid) {
        // Distance from the ORIGINAL touch-down point, not a running sum of
        // frame-to-frame deltas - summing every frame's jitter (loop() runs
        // hundreds of times a second) blew past the threshold within a
        // fraction of a second even for a rock-steady finger. Tracking
        // distance from the start instead means a stationary but noisy
        // touch stays bounded near zero, as it should.
        int dist_from_down = abs(p.x - g_touch_down_x) + abs(p.y - g_touch_down_y);
        if (dist_from_down > g_touch_moved_total) g_touch_moved_total = dist_from_down;

        // Live-scroll while dragging the list - the only thing that happens
        // WHILE held; every other action only fires at release above, so a
        // noisy press can't fire it more than once.
        if (g_screen == Screen::NORMAL_LIST && g_touch_down_y < tft.height() - BUTTON_H) {
            g_scroll_offset += (g_touch_prev_y - p.y);
            g_needs_redraw = true;
        }
    }

    if (edge_up && g_touch_moved_total < TAP_MOVE_THRESHOLD) {
        dispatch_tap(g_touch_down_x, g_touch_down_y);
    }

    if (p.valid) {
        g_touch_prev_x = p.x;
        g_touch_prev_y = p.y;
    }
    g_touch_was_down = p.valid;
}

// ---- LED state, kept in lockstep with the display's flash phase ----
static void update_led() {
    switch (g_screen) {
    case Screen::NORMAL_LIST:
    case Screen::SETTINGS:
    case Screen::DISABLE_MENU:
    case Screen::IGNORE_CONFIRM:
        led_set(LED_OFF, LED_DIM, LED_OFF); // dim green - all clear
        break;
    case Screen::MOB_DETECTED: {
        unsigned long phase = (millis() / 400) % 2;
        if (phase == 0) led_set(LED_OFF, LED_FULL, LED_OFF); // green
        else led_set(LED_FULL, LED_OFF, LED_OFF);            // red
        break;
    }
    case Screen::MOB_ACTIONED: {
        unsigned long phase = (millis() / 150) % 2;
        if (phase == 0) led_set(LED_FULL, LED_OFF, LED_OFF); // red
        else led_set(LED_OFF, LED_OFF, LED_FULL);            // blue
        break;
    }
    }
}

static void update_mob_state_machine() {
    if (g_screen == Screen::MOB_DETECTED) {
        unsigned long elapsed_ms = millis() - g_mob_detected_at_ms;
        if (elapsed_ms >= (unsigned long)g_action_timeout_secs * 1000UL) {
            Serial.println("[MOB] Escalating to ACTIONED - not silenced in time.");
            g_screen = Screen::MOB_ACTIONED;
            g_flash_last_phase = (unsigned long)-1;
        }
    }
}

// '1'..'9' force-triggers MOB for that tag index (skips waiting for the
// real missing-tag timeout) - useful for quickly testing the alarm/
// escalation UI on the bench. Same as the debug web page's "Force trigger
// now" button.
static void check_serial_commands() {
    if (!Serial.available()) return;
    char c = (char)Serial.read();
    if (c >= '1' && c <= '9') {
        int idx = c - '1';
        if (idx < NUM_TAGS) trigger_mob(idx);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== MOB display/LED/touch/BLE state-machine (full build) ===");

    led_init();
    led_set(LED_OFF, LED_OFF, LED_OFF);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    g_touch_spi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    g_touch.begin(g_touch_spi);
    g_touch.setRotation(1);

    if (crypto_init() != 0) {
        Serial.println("crypto_init() failed - halting.");
        while (true) delay(1000);
    }
    Serial.print("Self-check against known test vector: ");
    Serial.println(self_check() ? "PASS" : "FAIL");

    load_mob_settings();

    String ssid, pass;
    bool have_saved = load_saved_wifi(ssid, pass);
    bool connected = have_saved && try_connect_sta(ssid, pass, 10000UL);
    if (!connected) {
        run_config_portal(); // never returns
    }

    uint32_t network_time;
    if (fetch_time_from_network(&network_time) == 0) {
        g_base_unix_time = network_time;
        g_base_millis = millis();
        Serial.printf("Clock set from network: unix_time=%lu\n", (unsigned long)g_base_unix_time);
    } else {
        Serial.println("Falling back to manual timestamp entry.");
        prompt_for_time();
    }

    if (g_device_time_offset_seconds == 0) {
        g_device_time_offset_seconds = g_base_unix_time - DEVICE_TIME_BASELINE_EPOCH;
        save_device_time_offset();
        Serial.printf("No saved device-time offset yet - seeding from documented factory baseline: %lu\n",
                      (unsigned long)g_device_time_offset_seconds);
    } else {
        Serial.printf("Using saved device-time offset: %lu\n", (unsigned long)g_device_time_offset_seconds);
    }

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    // Continuous, non-blocking scan (duration=0) - see
    // continuous_scan_diagnostic.cpp for why the blocking start(duration,...)
    // overload is unusable here: it would stall this loop() for the whole
    // scan window, which now also has to keep the display/touch responsive.
    pBLEScan->start(0, nullptr, false);

    g_web_server.on("/", HTTP_GET, handle_debug_root);
    g_web_server.on("/trigger", HTTP_POST, handle_debug_trigger);
    g_web_server.on("/settings", HTTP_GET, handle_settings_root);
    g_web_server.on("/settings/save", HTTP_POST, handle_settings_save);
    g_web_server.on("/ignored", HTTP_GET, handle_ignored_root);
    g_web_server.on("/ignored/remove", HTTP_POST, handle_ignored_remove);
    g_web_server.begin();
    Serial.printf("Debug web UI: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.println("Serial '1'..'9' force-triggers MOB for that tag index.");

    g_needs_redraw = true;
}

void loop() {
    g_web_server.handleClient();
    check_serial_commands();

    // Drain whatever onResult() queued since the last pass, then run the
    // (comparatively slow - see the file header comment) crypto comparison
    // here, safely off the BLE stack's own task.
    PendingEid local[PENDING_QUEUE_LEN];
    int count;
    portENTER_CRITICAL(&g_pending_mux);
    count = g_pending_count;
    memcpy(local, g_pending, sizeof(PendingEid) * count);
    g_pending_count = 0;
    portEXIT_CRITICAL(&g_pending_mux);

    for (int i = 0; i < count; i++) {
        process_pending_eid(local[i].eid, local[i].rssi);
    }

    check_for_missing_tags();
    update_mob_state_machine();
    update_led();
    render_current_screen();
    handle_touch_input();
}
