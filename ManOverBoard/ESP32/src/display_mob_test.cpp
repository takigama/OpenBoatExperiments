// Display + RGB LED + touch + MOB alarm state machine, driven by REAL BLE
// FMDN scanning - for the ESP32-2432S028R "CYD" (Cheap Yellow Display)
// board. Runs in "tag unknown" mode: tags are tracked purely by their
// rotating 20-byte EID (no name resolution against the real underlying
// identity), since that needs the SECP160r1/AES crypto match against a
// tag's EIK, and that crypto is NOT done on this board (see below).
//
// On-device crypto was tried and reverted: the ~2.1s/candidate cost stalled
// the display/touch/LED/web-server hard enough on real hardware to look
// like a full hang, since it all runs in this file's single, non-
// preemptible loop(). Tag-identity resolution is planned to move off the
// ESP32 entirely onto a bigger machine, bridged over MQTT (not yet
// designed/built as of this file) - once that exists, it'll be the real
// mechanism for tracking a NAMED tag's rotation reliably. Until then, an
// unnamed tag's rotation is guessed at heuristically (see
// find_ignore_swap_candidate() below); a named tag deliberately opts out of
// that guess entirely and just uses the plain missing-tag alarm (see
// check_for_missing_tags()) - guessing wrong about a tag specific enough to
// have a name attached feels like the wrong trade to make blind. That
// on-device-crypto attempt is preserved for reference in
// ../archive_onboard_crypto/ (outside src/, not compiled).
//
// Because there's no crypto here, matching an incoming EID against tags
// already being tracked is a trivial byte comparison - no meaningful CPU
// cost, so this can't reintroduce the hang. Up to MAX_TAGS distinct EIDs
// are tracked at once; each is identified on screen by a shortened hex
// label (first 3 / last 3 hex chars), or by its custom name if it has one.
//
// Enrolled vs. ignored - the whole trust model, replacing an earlier
// RSSI-proximity-gated auto-enroll design that kept mis-tracking (and even
// mis-remembering across reboots) devices that merely wandered close once:
//
// - ANY EID heard at all gets a g_tags[] slot (see find_or_create_tag_slot())
//   and starts out IGNORED - there's no proximity gate anymore. Ignored
//   just means "seen, but not trusted for alarms" - it never triggers MOB
//   detection (see check_for_missing_tags()), and if it goes 5 minutes
//   without a beacon it's silently pruned (see check_for_stale_ignored_tags()).
// - Tapping a row promotes/demotes between ignored and ENROLLED (see
//   ENROLL_CONFIRM below) - only an enrolled tag can trigger a real MOB
//   alarm, and only an enrolled tag is remembered across a reboot (see
//   NVS_ENROLLED_NAMESPACE / load_enrolled_tags_into_table()). Un-enrolling
//   returns a tag to ignored and clears its name, if it had one - naming
//   only ever applies to an already-enrolled tag (see the /enrolled web
//   page), so "named" implies "enrolled" as an invariant everywhere in this
//   file.
// - Rotation recovery (find_ignore_swap_candidate(), called from
//   check_for_missing_tags() right as an UNNAMED enrolled tag is about to
//   be declared missing): search the ignored tags for one that's appeared
//   recently (visible for at most 2x the missing-tag threshold) - if found,
//   swap their roles (the missing tag becomes ignored, the recent arrival
//   becomes enrolled) instead of firing a false alarm. A NAMED tag never
//   does this swap in either direction - it always triggers a normal alarm
//   if it goes missing, and (being enrolled, hence never ignored) can never
//   be picked as someone else's swap candidate either.
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
//                  (shortened EID/name + RSSI + estimated distance +
//                  seconds since last seen). Ignored tags sort to the
//                  bottom, shown greyed out. Tap a row to enroll/un-enroll
//                  that tag (see ENROLL_CONFIRM). Bottom bar: Settings
//                  (left) / Disable MOB (right). If zero tags are enrolled
//                  (and MOB detection is enabled, and it's been a minute
//                  since boot), a thin red bar appears just above that
//                  bottom bar and the LED flashes red/green - see
//                  no_tags_warning_active(), draw_normal_screen(), and
//                  update_led(). Deliberately NOT a full-screen takeover
//                  like a real per-tag alarm - "you haven't set anything up
//                  yet" is a much lower-stakes situation than "a tag is
//                  actually missing".
//   SETTINGS     - adjust the DETECTED -> ACTIONED escalation timeout and
//                  the missing-tag threshold on-device (also available via
//                  /settings on the web UI), plus the WiFi on/off toggle
//                  (see WIFI_WARNING below).
//   DISABLE_MENU - preset duration buttons (1h/2h/4h/8h/until re-enabled) -
//                  MOB detection deliberately always starts back up ENABLED
//                  on every boot; only the enrolled-tags list (below)
//                  persists across a reboot.
//   MOB_DETECTED - a real, enrolled tracked tag hasn't been seen for longer
//                  than the missing-tag threshold (default 6s) and no
//                  ignore-swap candidate was found (see
//                  check_for_missing_tags()) - g_triggering_tag_idx is
//                  always a valid tag index here, never a sentinel; the
//                  "zero tags enrolled" case is a NORMAL_LIST bar, not this
//                  screen (see above). Multi-receiver consensus / RSSI-trend
//                  refinements are still an open design question for the
//                  real deployment - see FINDINGS.md section 6. Display +
//                  LED flash dark-green/bright-red. Tap anywhere to
//                  silence - un-enrolls the tag (back to ignored, name
//                  cleared) and drops back to NORMAL_LIST.
//   MOB_ACTIONED - if the alarm isn't silenced within the configurable
//                  escalation timeout, the display + LED flash
//                  bright-red/bright-blue instead, as bright as this
//                  hardware can manage. Tap anywhere to silence (same
//                  behavior as MOB_DETECTED).
//   ENROLL_CONFIRM - reached from a NORMAL_LIST row tap; enrolls/un-enrolls
//                  that one tag, deletes it outright (frees its slot - see
//                  check_for_stale_ignored_tags() for the automatic
//                  5-minute version of the same thing for an ignored tag),
//                  or cancels back to NORMAL_LIST.
//   WIFI_WARNING - shown before actually enabling WiFi from the Settings
//                  screen: WiFi is unstable for MOB detection (see the
//                  WiFi-defaults-OFF comment further down) and doubles
//                  both timers for as long as it stays on, so this asks
//                  for explicit confirmation first rather than silently
//                  degrading detection.
//
// Touch input is debounced (see read_touch()) and dispatched on RELEASE,
// not press, with a movement threshold + cooldown (see dispatch_tap()) -
// this touchscreen's raw readings are noisy enough that press-time
// dispatch caused actions to double/triple-fire.
//
// Debug web interface (once connected to real WiFi - reuses the NVS
// credential + SoftAP config-portal flow proven in wifi_persistence_test.cpp):
//   /          - lists currently-tracked tags with a "Force trigger now"
//                button each (skips straight to the alarm, for testing the
//                alarm/escalation UI without waiting for a real tag to
//                actually go missing).
//   /settings  - adjust the DETECTED -> ACTIONED escalation timeout and the
//                missing-tag threshold.
//   /enrolled  - enroll/un-enroll any currently-tracked tag, name an
//                enrolled one (the only way to make it survive a reboot -
//                see the header note above), remove one outright, or wipe
//                every enrollment at once via "Reset ALL".
// Serial '1'..'9' force-triggers MOB for the tag at that display position.
// Serial 'r'/'R' wipes every enrollment at once (same as the web page's
// "Reset ALL" button) - see factory_reset_enrolled().

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <WiFi.h>
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

// ---- WiFi persistence (same NVS-backed flow as wifi_persistence_test.cpp -
// falls into the same SoftAP config portal here too if nothing's saved yet,
// see enable_wifi() further down) ----
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

// ---- Tracked tags - identified purely by raw EID, no crypto/name
// resolution (see header comment). Up to MAX_TAGS distinct EIDs tracked at
// once; a new EID evicts whichever tracked slot has gone longest unseen
// once the table is full. ----
static const int MAX_TAGS = 8;
struct SeenTag {
    bool used;
    uint8_t eid[20];
    int rssi;
    unsigned long last_seen_ms; // 0 only transiently, between slot creation and its first mark_tag_seen()
    String name; // "" = ignored/unnamed - only an enrolled tag can have a name (see NVS_ENROLLED_NAMESPACE)
    // millis() this slot was first freshly tracked this session - 0 for a
    // tag pre-populated at boot (see load_enrolled_tags_into_table()), which
    // deliberately makes it ineligible as a find_ignore_swap_candidate()
    // match (it was already known, not something that "just appeared").
    unsigned long first_seen_ms;
};
static SeenTag g_tags[MAX_TAGS];

// Stable identity string for NVS keying and web links - the full EID hex.
static String tag_key(int idx) {
    String s;
    for (int i = 0; i < 20; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", g_tags[idx].eid[i]);
        s += buf;
    }
    return s;
}

static String tag_short_label(int idx) {
    if (g_tags[idx].name.length() > 0) return g_tags[idx].name;
    String full = tag_key(idx);
    return full.substring(0, 3) + "..." + full.substring(full.length() - 3);
}

// Rough rule-of-thumb log-distance path-loss estimate, NOT calibrated
// against real tags - see FINDINGS.md section 6, real distance needs
// sea-trial calibration.
static float estimate_distance_m(int rssi) {
    const float RSSI_REF_AT_1M = -59.0f;
    const float PATH_LOSS_EXPONENT = 2.0f;
    return pow(10.0f, (RSSI_REF_AT_1M - rssi) / (10.0f * PATH_LOSS_EXPONENT));
}

// ---- Enrolled tags (persisted in NVS) - the ONLY thing that distinguishes
// a trusted tag from an ignored one, and the ONLY thing that survives a
// reboot. Replaces an earlier design that gated initial tracking on RSSI
// proximity and persistence on a separate "named" concept - both folded
// into this single enroll/ignore toggle (see the header comment). Stored
// as "<40-char eid hex>|<name>," pairs, where <name> may be empty (enrolled
// but not named) - names are restricted to alnum/space/-/_ (see
// handle_enrolled_name()) so '|' and ',' can safely delimit without any
// escaping. Un-enrolling always clears the name too, so "has a name"
// implies "is enrolled" everywhere in this file - naming only ever applies
// to an already-enrolled tag (see handle_enrolled_name()). ----
static const char *NVS_ENROLLED_NAMESPACE = "enrolled";
static const char *NVS_ENROLLED_KEY = "list";

// Legacy namespaces from earlier designs (RSSI-proximity auto-enroll, then
// naming-gated persistence, then a separately-persisted ignore list) -
// referenced only by the one-time v3 migration further down, to purge
// them; never written to again.
static const char *NVS_LEGACY_ENROLL_NAMESPACE = "enroll";
static const char *NVS_LEGACY_NAMED_NAMESPACE = "named";
static const char *NVS_LEGACY_IGNORE_NAMESPACE = "ignore";

// RAM-cached alongside NVS: is_enrolled() gets called every loop() iteration
// (check_for_missing_tags()) and every redraw (compute_display_order() and
// friends), and re-reading flash on every one of those calls made the
// ESP-IDF NVS layer log an ERROR line per read whenever the "list" key
// doesn't exist yet (e.g. right after a factory reset) - flooding the
// serial monitor. Loaded once, only written back to flash on actual
// mutations.
static String g_enrolled_csv_cache;
static bool g_enrolled_cache_ready = false;

static void enrolled_cache_load_if_needed() {
    if (g_enrolled_cache_ready) return;
    g_prefs.begin(NVS_ENROLLED_NAMESPACE, true);
    g_enrolled_csv_cache = g_prefs.getString(NVS_ENROLLED_KEY, "");
    g_prefs.end();
    g_enrolled_cache_ready = true;
}

static void save_enrolled_csv(const String &csv) {
    g_enrolled_csv_cache = csv;
    g_enrolled_cache_ready = true;
    g_prefs.begin(NVS_ENROLLED_NAMESPACE, false);
    g_prefs.putString(NVS_ENROLLED_KEY, csv);
    g_prefs.end();
}

// True if this key has an entry at all, regardless of whether it has a name.
static bool is_enrolled(const String &key) {
    enrolled_cache_load_if_needed();
    return g_enrolled_csv_cache.indexOf("," + key + "|") >= 0;
}

// "" if unenrolled, or enrolled with no name given.
static String get_tag_name(const String &key) {
    enrolled_cache_load_if_needed();
    String needle = "," + key + "|";
    int idx = g_enrolled_csv_cache.indexOf(needle);
    if (idx < 0) return "";
    int start = idx + needle.length();
    int end = g_enrolled_csv_cache.indexOf(',', start);
    if (end < 0) end = g_enrolled_csv_cache.length();
    return g_enrolled_csv_cache.substring(start, end);
}

// Returns the CSV with this key's entry (if any) removed, without writing
// anything back - a shared building block for enroll_tag()/unenroll_tag().
static String enrolled_csv_without(const String &key) {
    enrolled_cache_load_if_needed();
    String csv = g_enrolled_csv_cache;
    String needle = "," + key + "|";
    int idx = csv.indexOf(needle);
    if (idx < 0) return csv;
    int end = csv.indexOf(',', idx + 1);
    if (end < 0) end = csv.length();
    return csv.substring(0, idx) + csv.substring(end);
}

// Enrolls (or re-enrolls, overwriting any existing name with this one -
// pass "" to enroll without a name) a tag - persists it and updates its
// live g_tags[] slot's name to match, if it currently has one.
static void enroll_tag(const String &key, const String &name) {
    String csv = enrolled_csv_without(key);
    if (csv.length() == 0) csv = ",";
    csv += key + "|" + name + ",";
    save_enrolled_csv(csv);
    for (int i = 0; i < MAX_TAGS; i++) {
        if (g_tags[i].used && tag_key(i) == key) {
            g_tags[i].name = name;
            break;
        }
    }
    if (name.length() > 0) Serial.printf("Enrolled tag \"%s\" as \"%s\" (persisted).\n", key.c_str(), name.c_str());
    else Serial.printf("Enrolled tag \"%s\" (persisted).\n", key.c_str());
}

// Un-enrolling always clears the name too (see the invariant note above) -
// returns the tag to plain ignored, both in NVS and its live slot.
static void unenroll_tag(const String &key) {
    if (!is_enrolled(key)) return;
    save_enrolled_csv(enrolled_csv_without(key));
    for (int i = 0; i < MAX_TAGS; i++) {
        if (g_tags[i].used && tag_key(i) == key) {
            g_tags[i].name = "";
            break;
        }
    }
    Serial.printf("Un-enrolled tag \"%s\".\n", key.c_str());
}

// Wipes the enrolled NVS list outright (plus the legacy pre-rewrite
// namespaces, in case anyone's upgrading with old persisted junk in them)
// and clears the live in-RAM table too - used both by the one-time
// migration in setup() and by the on-demand serial 'r'/web reset for
// whenever this needs clearing again.
static void factory_reset_enrolled() {
    g_prefs.begin(NVS_LEGACY_ENROLL_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
    g_prefs.begin(NVS_LEGACY_NAMED_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
    g_prefs.begin(NVS_LEGACY_IGNORE_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
    g_prefs.begin(NVS_ENROLLED_NAMESPACE, false);
    g_prefs.clear();
    g_prefs.end();
    g_enrolled_csv_cache = "";
    g_enrolled_cache_ready = true;
    for (int i = 0; i < MAX_TAGS; i++) { g_tags[i].used = false; g_tags[i].name = ""; }
    Serial.println("Cleared all enrolled tags from NVS and the live table.");
}

// Builds the display order over currently-USED slots only (unused slots
// are skipped entirely, not just sorted to the bottom) - ignored tags sort
// after enrolled ones. Returns how many entries were written into order[]
// (<= MAX_TAGS); callers must use this count, not MAX_TAGS, since the table
// is rarely full.
static int compute_display_order(int order[]) {
    int w = 0;
    for (int i = 0; i < MAX_TAGS; i++)
        if (g_tags[i].used && is_enrolled(tag_key(i))) order[w++] = i;
    for (int i = 0; i < MAX_TAGS; i++)
        if (g_tags[i].used && !is_enrolled(tag_key(i))) order[w++] = i;
    return w;
}

// 1-based position in the currently-displayed list - same order the
// on-screen rows and the '1'..'9' serial force-trigger commands use - for
// tagging serial log lines so a specific physical tag is easy to pick out
// across repeated [SEEN] lines without reading the full hex label each
// time. -1 if not currently shown (shouldn't happen for a used slot, but
// keeps this defensive rather than an out-of-bounds read).
static int tag_display_position(int idx) {
    int order[MAX_TAGS];
    int count = compute_display_order(order);
    for (int i = 0; i < count; i++) {
        if (order[i] == idx) return i + 1;
    }
    return -1;
}

// ---- MOB escalation timeout + missing-tag threshold (both persisted -
// genuine tuning settings, unlike the disable timer below) ----
static const char *NVS_MOBCFG_NAMESPACE = "mobcfg";
static const char *NVS_KEY_ACTION_SECS = "actionSecs";
static const char *NVS_KEY_MISSING_SECS = "missingSecs";
static uint32_t g_action_timeout_secs = 10;  // default
static uint32_t g_missing_threshold_secs = 6; // default - how long a tag can go unseen before it's MOB

static const uint32_t SETTINGS_ACTION_STEP_SECS = 1;
static const uint32_t SETTINGS_ACTION_MIN_SECS = 5;
static const uint32_t SETTINGS_ACTION_MAX_SECS = 300;
static const uint32_t SETTINGS_MISSING_STEP_SECS = 1;
static const uint32_t SETTINGS_MISSING_MIN_SECS = 2;
static const uint32_t SETTINGS_MISSING_MAX_SECS = 120;

static void load_mob_settings() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, true);
    g_action_timeout_secs = g_prefs.getUInt(NVS_KEY_ACTION_SECS, 10);
    g_missing_threshold_secs = g_prefs.getUInt(NVS_KEY_MISSING_SECS, 6);
    g_prefs.end();
}

static void save_mob_settings() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, false);
    g_prefs.putUInt(NVS_KEY_ACTION_SECS, g_action_timeout_secs);
    g_prefs.putUInt(NVS_KEY_MISSING_SECS, g_missing_threshold_secs);
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
enum class Screen { NORMAL_LIST, SETTINGS, DISABLE_MENU, MOB_DETECTED, MOB_ACTIONED, ENROLL_CONFIRM, WIFI_WARNING };
static Screen g_screen = Screen::NORMAL_LIST;
static int g_triggering_tag_idx = -1;  // which tag caused the current alarm - always >= 0 whenever g_screen is MOB_DETECTED/MOB_ACTIONED
static int g_selected_tag_idx = -1;    // which tag ENROLL_CONFIRM is acting on
static unsigned long g_mob_detected_at_ms = 0;
static int g_scroll_offset = 0;
static bool g_needs_redraw = true;
static unsigned long g_flash_last_phase = (unsigned long)-1;
// Counts real alarms only (past the disabled/not-enrolled guards below,
// including force-triggers via serial '1'..'9' or the web "Force trigger
// now" button) - see log_trigger_stats_periodically().
static unsigned long g_trigger_count = 0;

static void trigger_mob(int tag_idx) {
    if (mob_is_disabled()) {
        Serial.printf("Ignoring MOB trigger for [%d] %s - MOB detection is currently disabled.\n",
                      tag_display_position(tag_idx), tag_short_label(tag_idx).c_str());
        return;
    }
    if (!is_enrolled(tag_key(tag_idx))) {
        Serial.printf("Ignoring MOB trigger for [%d] %s - tag is not enrolled.\n",
                      tag_display_position(tag_idx), tag_short_label(tag_idx).c_str());
        return;
    }
    g_trigger_count++;
    Serial.printf("[MOB] Triggered for [%d] %s\n", tag_display_position(tag_idx), tag_short_label(tag_idx).c_str());
    g_triggering_tag_idx = tag_idx;
    g_mob_detected_at_ms = millis();
    g_screen = Screen::MOB_DETECTED;
    g_flash_last_phase = (unsigned long)-1;
}

static void log_trigger_stats_periodically() {
    static unsigned long last_check = 0;
    unsigned long now = millis();
    if (now - last_check < 10000) return;
    last_check = now;
    Serial.printf("[STATS] MOB triggered %lu time(s) since boot.\n", g_trigger_count);
}

// Silencing an alarm means "this tag's alarm is dealt with" - it
// un-enrolls the tag outright (back to plain ignored, name cleared) and
// returns straight to normal monitoring, rather than parking on a separate
// acknowledged-but-still-missing screen. Re-enroll later via a NORMAL_LIST
// row tap.
static void silence_alarm() {
    if (g_triggering_tag_idx >= 0) {
        String label = tag_short_label(g_triggering_tag_idx);
        unenroll_tag(tag_key(g_triggering_tag_idx));
        Serial.printf("[MOB] Silenced by touch - %s now un-enrolled.\n", label.c_str());
    }
    g_screen = Screen::NORMAL_LIST;
    g_triggering_tag_idx = -1;
    g_needs_redraw = true;
}

// Only called for an UNNAMED enrolled tag that's about to be declared
// missing (see check_for_missing_tags()) - a named tag never does this
// swap (see the file header's enrolled-vs-ignored note), since there's no
// reliable way yet to identify a named tag's rotation without the crypto
// key (planned to happen off-device eventually).
//
// Searches currently-IGNORED tags for one that's been visible (tracked)
// for at most 2x the missing-tag threshold - i.e. it showed up recently,
// around the time the missing tag would have rotated. A tag that's been
// hanging around for a while is presumably just some other nearby device,
// not a fresh rotation. Only ignored tags are considered - a named tag can
// never be ignored (naming only applies to an already-enrolled tag, and
// un-enrolling always clears the name), so this can never accidentally
// pick a named tag as a candidate; no separate check needed.
static int find_ignore_swap_candidate() {
    unsigned long now = millis();
    unsigned long window_ms = 2UL * g_missing_threshold_secs * 1000UL;
    int best_idx = -1;
    unsigned long best_first_seen = 0;
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) continue;
        if (is_enrolled(tag_key(i))) continue;
        if (g_tags[i].first_seen_ms == 0) continue; // pre-existing - can't be a fresh arrival
        unsigned long visible_ms = now - g_tags[i].first_seen_ms;
        if (visible_ms > window_ms) continue;
        if (g_tags[i].first_seen_ms > best_first_seen) { // most-recently-appeared wins
            best_first_seen = g_tags[i].first_seen_ms;
            best_idx = i;
        }
    }
    return best_idx;
}

// Only auto-triggers from NORMAL_LIST (the idle view) - a genuinely missing
// tag's last_seen never advances on its own, so without this guard,
// silencing an alarm (which just changes g_screen) would see the same tag
// "still missing" on the very next loop() and immediately re-trigger it.
// Restricting to NORMAL_LIST means a silenced/actioned/menu screen
// suppresses new triggers until you're back to actually watching the list.
static void check_for_missing_tags() {
    if (g_screen != Screen::NORMAL_LIST) return;
    // Same reasoning as the is_enrolled() skip below, same fix - this runs
    // every single loop() iteration, and calling trigger_mob() unconditionally
    // while MOB detection is disabled flooded the serial log with "Ignoring
    // MOB trigger..." lines hundreds of times a second (trigger_mob() itself
    // already no-ops when disabled, but only after logging every call).
    if (mob_is_disabled()) return;
    unsigned long now = millis();
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) continue;
        String key = tag_key(i);
        // Only an enrolled tag can ever go "missing" - an ignored one was
        // never trusted for alarms in the first place. This also runs
        // every single loop() iteration, so skipping cheaply here (rather
        // than letting trigger_mob() log-then-no-op every call) avoids the
        // same kind of serial-log flood the mob_is_disabled() check above
        // exists to prevent.
        if (!is_enrolled(key)) continue;
        if (now - g_tags[i].last_seen_ms < g_missing_threshold_secs * 1000UL) continue;

        if (g_tags[i].name.length() == 0) {
            int candidate = find_ignore_swap_candidate();
            if (candidate >= 0) {
                // A different, unenrolled tag showed up recently while this
                // one was going quiet - swap their roles instead of firing
                // a false alarm: the missing tag becomes ignored (it'll get
                // auto-pruned later if it never comes back - see
                // check_for_stale_ignored_tags()), and the recent arrival
                // becomes enrolled in its place. Neither is named (an
                // ignored tag never is), so there's no name to carry.
                String candidate_key = tag_key(candidate);
                String missing_label = tag_short_label(i);
                String candidate_label = tag_short_label(candidate);
                unenroll_tag(key);
                enroll_tag(candidate_key, "");
                Serial.printf("[SWAP] %s was about to be declared missing - swapping in recently-seen "
                              "ignored tag %s instead of triggering.\n",
                              missing_label.c_str(), candidate_label.c_str());
                g_needs_redraw = true;
                return;
            }
        }
        trigger_mob(i);
        return;
    }
}

// An ignored tag that never comes back (a stranger's device, or a real tag
// that got un-enrolled and never reappeared) would otherwise sit in the
// tracked table forever, taking up a slot and cluttering NORMAL_LIST. Same
// deletion the ENROLL_CONFIRM screen's "Delete this tag" button and the
// /enrolled web page's "Remove" button do, just automatic once it's been
// both ignored AND unseen for a while - still manual (and immediate) for
// anything ignored more recently than that, or enrolled.
static const unsigned long IGNORED_STALE_DELETE_MS = 5UL * 60UL * 1000UL; // 5 minutes

static void check_for_stale_ignored_tags() {
    static unsigned long last_check = 0;
    unsigned long now = millis();
    if (now - last_check < 1000) return;
    last_check = now;

    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) continue;
        if (is_enrolled(tag_key(i))) continue;
        if (now - g_tags[i].last_seen_ms < IGNORED_STALE_DELETE_MS) continue;
        Serial.printf("[CLEANUP] Ignored tag \"%s\" hasn't reappeared in 5 minutes - deleting.\n",
                      tag_short_label(i).c_str());
        g_tags[i].used = false;
        g_tags[i].name = ""; // already "" for an ignored tag, but tidy
        g_needs_redraw = true;
    }
}

static int count_enrolled_tags() {
    int n = 0;
    for (int i = 0; i < MAX_TAGS; i++)
        if (g_tags[i].used && is_enrolled(tag_key(i))) n++;
    return n;
}

// Queried live from draw_normal_screen() and update_led() (not a one-shot
// latch/screen-transition like the full-screen alarms) - true whenever
// there's genuinely nothing enrolled to protect: no tag has ever been
// worth an actual MOB alarm, so this doubles as "you probably haven't
// finished setting this up yet". Deliberately NOT a full-screen takeover
// like a real per-tag alarm (see draw_normal_screen()'s warning bar) -
// "nothing enrolled" is a much lower-stakes situation than "a tag actually
// went missing", and unlike a real alarm there's no specific tag to act on
// or dismiss, so it just clears itself the moment something gets enrolled.
static const unsigned long NO_TAGS_WARNING_DELAY_MS = 60000UL; // 1 minute post-boot grace

static bool no_tags_warning_active() {
    if (g_screen != Screen::NORMAL_LIST) return false;
    if (mob_is_disabled()) return false;
    if (millis() < NO_TAGS_WARNING_DELAY_MS) return false;
    return count_enrolled_tags() == 0;
}

// Records a fresh real sighting of a tag (a matching BLE frame). Logs it to
// serial, and if this is the tag currently causing an active alarm (before
// the user has silenced it), auto-clears back to normal. Once silenced, a
// tag is ignored outright (see silence_alarm()), so this doesn't need to
// cover that case separately.
static void mark_tag_seen(int idx) {
    g_tags[idx].last_seen_ms = millis();
    Serial.printf("[SEEN] [%d] %s rssi=%ddBm\n", tag_display_position(idx), tag_short_label(idx).c_str(),
                  g_tags[idx].rssi);

    if (idx == g_triggering_tag_idx &&
        (g_screen == Screen::MOB_DETECTED || g_screen == Screen::MOB_ACTIONED)) {
        Serial.printf("[MOB] %s is back - clearing alarm.\n", tag_short_label(idx).c_str());
        g_screen = Screen::NORMAL_LIST;
        g_triggering_tag_idx = -1;
        g_needs_redraw = true;
    }
}

// ---- BLE scanning - no crypto, just tracks distinct raw EIDs (see header
// comment) ----
static const BLEUUID FMDN_SERVICE_UUID((uint16_t)0xFEAA);

// onResult() runs inside the BLE stack's own task (BTC_TASK), which must
// stay responsive. It only does the cheap frame-format check and hands the
// 20-byte candidate off to loop() via this queue, which runs at normal app
// priority - matching an EID against already-tracked tags there is a
// trivial byte comparison (see find_or_create_tag_slot()), not the
// expensive crypto this used to require.
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

// FMDN EIDs rotate roughly every 1024s (~17min - see FINDINGS.md section 3)
// BY DESIGN so a passive listener can't prove two EIDs belong to the same
// tag without its EIK - which is exactly the crypto this build deliberately
// doesn't do. There's no RSSI/proximity gate here at all anymore (see the
// file header) - every distinct EID heard gets a slot, starting out
// ignored; only find_ignore_swap_candidate() (called from
// check_for_missing_tags(), right as an unnamed enrolled tag is about to
// be declared missing) ever guesses that one EID might be another's
// rotation, and only at that one moment - never eagerly here. (History
// note: earlier designs tried guessing eagerly at EID-arrival time, gated
// by a fixed "how long has the old one been quiet" grace period - every
// value tried failed one way or another, either too slow to beat a real
// rotation's gap or so eager that distinct, still-present tags kept
// getting reassigned onto each other's slots. Deferring the guess to the
// one moment it's actually needed avoided that whole class of bug.)
static int find_or_create_tag_slot(const uint8_t eid[20], int rssi) {
    for (int i = 0; i < MAX_TAGS; i++) {
        if (g_tags[i].used && memcmp(g_tags[i].eid, eid, 20) == 0) return i;
    }

    int free_idx = -1;
    int oldest_ignored_idx = -1;
    unsigned long oldest_ignored_time = 0xFFFFFFFFUL;
    int oldest_any_idx = 0;
    unsigned long oldest_any_time = 0xFFFFFFFFUL;
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) {
            free_idx = i;
            break;
        }
        if (g_tags[i].last_seen_ms < oldest_any_time) {
            oldest_any_time = g_tags[i].last_seen_ms;
            oldest_any_idx = i;
        }
        if (!is_enrolled(tag_key(i)) && g_tags[i].last_seen_ms < oldest_ignored_time) {
            oldest_ignored_time = g_tags[i].last_seen_ms;
            oldest_ignored_idx = i;
        }
    }
    int idx;
    if (free_idx >= 0) {
        idx = free_idx;
    } else {
        // Table's full - prefer evicting the oldest IGNORED tag over an
        // enrolled one (only falling back to the oldest overall if every
        // slot happens to be enrolled), so passing/uninteresting devices
        // can't casually bump something actually trusted out of live
        // tracking. Evicting an enrolled tag deliberately does NOT touch
        // its NVS entry - it's still enrolled, just not live-tracked this
        // moment, and will reload at next boot (or resume immediately if
        // heard again - see the is_enrolled() restore below).
        idx = (oldest_ignored_idx >= 0) ? oldest_ignored_idx : oldest_any_idx;
        Serial.printf("[TRACK] Tag table full - evicting %s to make room.\n", tag_short_label(idx).c_str());
    }

    String key;
    for (int i = 0; i < 20; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", eid[i]);
        key += buf;
    }
    // Usually this is a genuinely new EID (unenrolled by default), but it
    // could also be an already-enrolled tag reappearing after its live
    // slot was evicted for space earlier this session - restore its
    // enrolled status/name rather than starting it over as ignored.
    bool already_enrolled = is_enrolled(key);
    g_tags[idx].used = true;
    memcpy(g_tags[idx].eid, eid, 20);
    g_tags[idx].rssi = -100;
    g_tags[idx].last_seen_ms = 0;
    g_tags[idx].name = already_enrolled ? get_tag_name(key) : "";
    g_tags[idx].first_seen_ms = millis();

    if (already_enrolled) {
        Serial.printf("[TRACK] Already-enrolled tag back in live tracking (rssi=%ddBm): %s\n",
                      rssi, tag_short_label(idx).c_str());
    } else {
        Serial.printf("[TRACK] New tag seen (rssi=%ddBm): %s - starts ignored until enrolled (tap it, "
                      "or see /enrolled).\n", rssi, tag_short_label(idx).c_str());
    }
    return idx;
}

static void process_pending_eid(const uint8_t seen_eid[20], int rssi) {
    int idx = find_or_create_tag_slot(seen_eid, rssi);
    g_tags[idx].rssi = rssi;
    mark_tag_seen(idx);
}

// ---- Debug/settings web UI ----
static void handle_debug_root() {
    unsigned long now = millis();
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                  "<meta http-equiv='refresh' content='5'>"
                  "<title>OpenBoat MOB debug</title></head><body>";
    html += "<h2>Tracked tags</h2><table border=1 cellpadding=6>";
    html += "<tr><th>Tag</th><th>RSSI</th><th>Last seen</th><th>Status</th><th>Action</th></tr>";
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) continue;
        bool enrolled = is_enrolled(tag_key(i));
        String last_seen = g_tags[i].last_seen_ms == 0 ? "not seen yet this session"
                                                        : String((now - g_tags[i].last_seen_ms) / 1000) + "s ago";
        html += "<tr><td>" + tag_short_label(i) + "</td><td>" + String(g_tags[i].rssi) + "</td><td>" + last_seen +
                "</td><td>" + (enrolled ? "enrolled" : "ignored") +
                "</td><td>"
                "<form method='POST' action='/trigger' style='display:inline'>"
                "<input type='hidden' name='tag' value='" +
                String(i) + "'>"
                            "<input type='submit' value='Force trigger now'></form></td></tr>";
    }
    html += "</table>";
    html += "<p>Every heard EID is tracked and starts out <b>ignored</b> - see /enrolled to enroll one "
            "(or tap its row on the board itself). Only an enrolled tag can trigger a real MOB alarm or "
            "survive a reboot. Identified only by EID (no crypto/name resolution against the real "
            "underlying identity - see file header) unless you've given it a custom name. 'Force trigger "
            "now' skips straight to the alarm, for testing the alarm/escalation UI without waiting for a "
            "real tag to actually go missing.</p>";
    html += "<p>MOB detection currently: <b>" + String(mob_is_disabled() ? "DISABLED" : "enabled") + "</b></p>";
    html += "<p><a href='/settings'>Settings</a> | <a href='/enrolled'>Tracked tags / enrollment</a></p>";
    html += "</body></html>";
    g_web_server.send(200, "text/html", html);
}

static void handle_debug_trigger() {
    if (!g_web_server.hasArg("tag")) {
        g_web_server.send(400, "text/plain", "missing tag");
        return;
    }
    int idx = g_web_server.arg("tag").toInt();
    if (idx < 0 || idx >= MAX_TAGS || !g_tags[idx].used) {
        g_web_server.send(400, "text/plain", "bad or untracked tag index");
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

static void handle_enrolled_root() {
    String html = "<!DOCTYPE html><html><body><h2>Tracked tags</h2>";
    html += "<p>New tags start out <b>ignored</b>. Enroll one to trust it for real MOB alerts and "
            "remember it across a reboot - un-enrolling returns it to ignored and clears its name, if "
            "it had one. An ignored tag is auto-deleted after 5 minutes with no beacon.</p>";
    html += "<table border=1 cellpadding=6><tr><th>Tag</th><th>Status</th><th>Name</th><th></th><th></th></tr>";
    for (int i = 0; i < MAX_TAGS; i++) {
        if (!g_tags[i].used) continue;
        String key = tag_key(i);
        bool enrolled = is_enrolled(key);
        html += "<tr><td>" + key.substring(0, 3) + "..." + key.substring(key.length() - 3) + "</td>";
        html += "<td>" + String(enrolled ? "enrolled" : "ignored") + "</td>";
        if (enrolled) {
            html += "<td><form method='POST' action='/enrolled/name' style='display:inline'>"
                    "<input type='hidden' name='key' value='" + key + "'>"
                    "<input type='text' name='name' maxlength='20' value='" + g_tags[i].name +
                    "'> <input type='submit' value='Save name'></form></td>";
        } else {
            html += "<td>-</td>";
        }
        html += "<td><form method='POST' action='/enrolled/toggle' style='display:inline'>"
                "<input type='hidden' name='key' value='" + key + "'>"
                "<input type='submit' value='" + String(enrolled ? "Un-enroll" : "Enroll") +
                "'></form></td>";
        html += "<td><form method='POST' action='/enrolled/remove' style='display:inline' "
                "onsubmit=\"return confirm('Stop tracking and forget this tag?');\">"
                "<input type='hidden' name='key' value='" + key +
                "'><input type='submit' value='Delete'></form></td></tr>";
    }
    html += "</table>";
    html += "<form method='POST' action='/enrolled/reset_all' onsubmit=\"return confirm('Un-enroll "
            "every tag?');\"><input type='submit' value='Reset ALL (un-enroll everything)'></form>";
    html += "<p><a href='/'>Back</a></p></body></html>";
    g_web_server.send(200, "text/html", html);
}

// Only alnum/space/-/_ survive - keeps the name safe as a CSV field (no ','
// or '|', see NVS_ENROLLED_NAMESPACE) and safe to drop straight into an
// HTML attribute without any escaping.
static String sanitize_tag_name(const String &raw) {
    String clean;
    for (size_t i = 0; i < raw.length() && clean.length() < 20; i++) {
        char c = raw[i];
        if (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') clean += c;
    }
    clean.trim();
    return clean;
}

// Naming implicitly (re-)enrolls, since a name only ever makes sense on an
// enrolled tag (see the invariant note by NVS_ENROLLED_NAMESPACE).
static void handle_enrolled_name() {
    if (g_web_server.hasArg("key") && g_web_server.hasArg("name")) {
        enroll_tag(g_web_server.arg("key"), sanitize_tag_name(g_web_server.arg("name")));
    }
    g_needs_redraw = true;
    g_web_server.sendHeader("Location", "/enrolled");
    g_web_server.send(303);
}

static void handle_enrolled_toggle() {
    if (g_web_server.hasArg("key")) {
        String key = g_web_server.arg("key");
        if (is_enrolled(key)) unenroll_tag(key);
        else enroll_tag(key, "");
    }
    g_needs_redraw = true;
    g_web_server.sendHeader("Location", "/enrolled");
    g_web_server.send(303);
}

// Deleting drops the tag immediately, not just its enrollment - clears its
// live slot too so it stops being tracked right away.
static void handle_enrolled_remove() {
    if (g_web_server.hasArg("key")) {
        String key = g_web_server.arg("key");
        unenroll_tag(key);
        for (int i = 0; i < MAX_TAGS; i++) {
            if (g_tags[i].used && tag_key(i) == key) {
                g_tags[i].used = false;
                g_tags[i].name = "";
                break;
            }
        }
    }
    g_web_server.sendHeader("Location", "/enrolled");
    g_web_server.send(303);
}

static void handle_enrolled_reset_all() {
    factory_reset_enrolled();
    g_needs_redraw = true;
    g_web_server.sendHeader("Location", "/enrolled");
    g_web_server.send(303);
}

// WiFi defaults OFF - the ESP32 classic shares a single 2.4GHz radio
// between WiFi and BLE (time-division coexistence), so having WiFi
// actively connected was starving the BLE scan of airtime and causing
// missed sightings even with tags sitting right next to the board.
// Enabled/disabled on demand from the Settings screen instead of always-on
// - only needed when you actually want the debug web UI.
static bool g_wifi_enabled = false;
static bool g_web_server_started = false;

static void register_web_routes_once() {
    if (g_web_server_started) return;
    g_web_server.on("/", HTTP_GET, handle_debug_root);
    g_web_server.on("/trigger", HTTP_POST, handle_debug_trigger);
    g_web_server.on("/settings", HTTP_GET, handle_settings_root);
    g_web_server.on("/settings/save", HTTP_POST, handle_settings_save);
    g_web_server.on("/enrolled", HTTP_GET, handle_enrolled_root);
    g_web_server.on("/enrolled/name", HTTP_POST, handle_enrolled_name);
    g_web_server.on("/enrolled/toggle", HTTP_POST, handle_enrolled_toggle);
    g_web_server.on("/enrolled/remove", HTTP_POST, handle_enrolled_remove);
    g_web_server.on("/enrolled/reset_all", HTTP_POST, handle_enrolled_reset_all);
    g_web_server.begin();
    g_web_server_started = true;
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

// No saved credentials - same SoftAP + captive portal flow as
// wifi_persistence_test.cpp. Blocks forever (until the form is submitted
// and this reboots), same as that build - a deliberate one-off setup
// action, not something that runs unattended.
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

static bool g_timers_doubled_for_wifi = false;
static uint32_t g_action_timeout_secs_before_wifi = 0;
static uint32_t g_missing_threshold_secs_before_wifi = 0;

// MOB detection is unstable with WiFi enabled (see the WiFi-defaults-OFF
// comment above - the shared radio starves BLE of scan time). Doubling
// both timers gives the alarm logic more slack to ride out the extra
// missed sightings while WiFi's on, instead of false-triggering more
// often. Deliberately NOT persisted to NVS - this only adjusts the in-RAM
// values for as long as WiFi stays enabled this session; the real
// user-configured values stay untouched in NVS, and
// restore_timers_after_wifi() puts the RAM values back exactly once WiFi
// is disabled again.
static void double_timers_for_wifi() {
    if (g_timers_doubled_for_wifi) return;
    g_action_timeout_secs_before_wifi = g_action_timeout_secs;
    g_missing_threshold_secs_before_wifi = g_missing_threshold_secs;
    g_action_timeout_secs = min(g_action_timeout_secs * 2, SETTINGS_ACTION_MAX_SECS);
    g_missing_threshold_secs = min(g_missing_threshold_secs * 2, SETTINGS_MISSING_MAX_SECS);
    g_timers_doubled_for_wifi = true;
    Serial.printf("WiFi enabling - doubling detection timers for stability: escalation %lu->%lus, missing %lu->%lus\n",
                  (unsigned long)g_action_timeout_secs_before_wifi, (unsigned long)g_action_timeout_secs,
                  (unsigned long)g_missing_threshold_secs_before_wifi, (unsigned long)g_missing_threshold_secs);
}

static void restore_timers_after_wifi() {
    if (!g_timers_doubled_for_wifi) return;
    g_action_timeout_secs = g_action_timeout_secs_before_wifi;
    g_missing_threshold_secs = g_missing_threshold_secs_before_wifi;
    g_timers_doubled_for_wifi = false;
    Serial.println("WiFi disabled - restored normal detection timers.");
}

static void enable_wifi() {
    double_timers_for_wifi();
    String ssid, pass;
    if (!load_saved_wifi(ssid, pass)) {
        run_config_portal(); // never returns
    }
    g_wifi_enabled = true;
    if (try_connect_sta(ssid, pass, 10000UL)) {
        register_web_routes_once();
        Serial.printf("Debug web UI: http://%s/\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("WiFi connect failed - staying disabled.");
        WiFi.mode(WIFI_OFF);
        g_wifi_enabled = false;
        restore_timers_after_wifi();
    }
}

static void disable_wifi() {
    Serial.println("Disabling WiFi by touch - BLE scanning gets full radio time back.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_wifi_enabled = false;
    restore_timers_after_wifi();
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
static const int NO_TAGS_BAR_H = 20; // see no_tags_warning_active()

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
    tft.print("Tag (EID)");
    tft.setCursor(COL_RSSI_X, HEADER_H + 6);
    tft.print("RSSI");
    tft.setCursor(COL_DIST_X, HEADER_H + 6);
    tft.print("Dist");
    tft.setCursor(COL_SEEN_X, HEADER_H + 6);
    tft.print("Seen");

    bool warn = no_tags_warning_active();
    int bottom_bar_y = tft.height() - BUTTON_H;
    int list_bottom = bottom_bar_y - (warn ? NO_TAGS_BAR_H : 0);
    tft.drawFastHLine(0, LIST_TOP - 1, tft.width(), TFT_WHITE);
    tft.drawFastVLine(COL_RSSI_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);
    tft.drawFastVLine(COL_DIST_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);
    tft.drawFastVLine(COL_SEEN_X - 6, HEADER_H, list_bottom - HEADER_H, TFT_WHITE);

    int order[MAX_TAGS];
    int count = compute_display_order(order);

    int max_scroll = max(0, count * ROW_H - (list_bottom - LIST_TOP));
    g_scroll_offset = constrain(g_scroll_offset, 0, max_scroll);

    unsigned long now = millis();
    int first_row = g_scroll_offset / ROW_H;
    int y = LIST_TOP - (g_scroll_offset % ROW_H);
    for (int row = first_row; row < count && y < list_bottom; row++, y += ROW_H) {
        int i = order[row];
        bool enrolled = is_enrolled(tag_key(i));
        tft.setTextColor(enrolled ? TFT_WHITE : TFT_LIGHTGREY, DARK_GREEN);

        tft.setCursor(COL_TAG_X, y + 8);
        tft.print(tag_short_label(i));
        if (!enrolled) tft.print(" (ign)");
        if (g_tags[i].last_seen_ms == 0) {
            // Enrolled in a previous session, not re-heard yet this one -
            // see load_enrolled_tags_into_table().
            tft.setCursor(COL_RSSI_X, y + 8);
            tft.print("not seen yet this session");
        } else {
            float dist = estimate_distance_m(g_tags[i].rssi);
            tft.setCursor(COL_RSSI_X, y + 8);
            tft.printf("%d", g_tags[i].rssi);
            tft.setCursor(COL_DIST_X, y + 8);
            tft.printf("~%.1f", dist);
            tft.setCursor(COL_SEEN_X, y + 8);
            tft.printf("%lus", (now - g_tags[i].last_seen_ms) / 1000UL);
        }

        tft.drawFastHLine(0, y + ROW_H - 1, tft.width(), TFT_DARKGREEN);
    }

    if (count == 0) {
        tft.setTextColor(TFT_WHITE, DARK_GREEN);
        tft.setCursor(COL_TAG_X, LIST_TOP + 8);
        tft.print("No tags heard yet...");
    }

    if (warn) {
        tft.fillRect(0, list_bottom, tft.width(), NO_TAGS_BAR_H, BRIGHT_RED);
        tft.setTextColor(TFT_WHITE, BRIGHT_RED);
        tft.setTextSize(1);
        tft.setCursor(4, list_bottom + 6);
        tft.print("No tags enrolled - nothing is protected by MOB detection");
    }

    // Bottom bar: Settings (left) | Disable MOB (right)
    tft.fillRect(0, bottom_bar_y, tft.width(), BUTTON_H, TFT_DARKGREY);
    tft.drawFastVLine(tft.width() / 2, bottom_bar_y, BUTTON_H, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(10, bottom_bar_y + 10);
    tft.print("Settings");
    tft.setCursor(tft.width() / 2 + 10, bottom_bar_y + 10);
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
static const int SETTINGS_BTN_H = 30;
static const int SETTINGS_ROW1_LABEL_Y = HEADER_H + 2;
static const int SETTINGS_ROW1_Y = SETTINGS_ROW1_LABEL_Y + 10;
static const int SETTINGS_ROW2_LABEL_Y = SETTINGS_ROW1_Y + SETTINGS_BTN_H + 8;
static const int SETTINGS_ROW2_Y = SETTINGS_ROW2_LABEL_Y + 10;
static const int SETTINGS_ROW3_LABEL_Y = SETTINGS_ROW2_Y + SETTINGS_BTN_H + 8;
static const int SETTINGS_ROW3_Y = SETTINGS_ROW3_LABEL_Y + 10; // WiFi toggle row
static const int SETTINGS_BACK_Y = SETTINGS_ROW3_Y + SETTINGS_BTN_H + 8;

static void draw_adjust_row(int label_y, int row_y, const char *label, uint32_t value) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, label_y);
    tft.print(label);

    tft.fillRect(10, row_y, SETTINGS_BTN_W, SETTINGS_BTN_H, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(35, row_y + 6);
    tft.print("-");

    char buf[8];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)value);
    tft.setCursor(tft.width() / 2 - 24, row_y + 6);
    tft.print(buf);

    tft.fillRect(tft.width() - 10 - SETTINGS_BTN_W, row_y, SETTINGS_BTN_W, SETTINGS_BTN_H, TFT_DARKGREY);
    tft.setCursor(tft.width() - 10 - SETTINGS_BTN_W + 25, row_y + 6);
    tft.print("+");
}

// Single wide button spanning the row - tap anywhere on it to toggle,
// unlike the -/+ adjust rows above. Shows the actual live WiFi.status(),
// not just g_wifi_enabled, so "still connecting" is visible instead of
// looking identical to "off".
static void draw_wifi_toggle_row(int label_y, int row_y) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, label_y);
    tft.print("WiFi (needed for web UI):");

    bool connected = WiFi.status() == WL_CONNECTED;
    tft.fillRect(10, row_y, tft.width() - 20, SETTINGS_BTN_H, connected ? TFT_DARKGREEN : TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, connected ? TFT_DARKGREEN : TFT_DARKGREY);
    tft.setCursor(16, row_y + 8);
    if (connected) {
        tft.printf("ON - %s (tap to disable)", WiFi.localIP().toString().c_str());
    } else if (g_wifi_enabled) {
        tft.print("Connecting... (tap to cancel)");
    } else {
        tft.print("OFF (tap to enable)");
    }
}

static void draw_settings_screen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print("Settings");

    draw_adjust_row(SETTINGS_ROW1_LABEL_Y, SETTINGS_ROW1_Y, "Detected -> Actioned timeout:", g_action_timeout_secs);
    draw_adjust_row(SETTINGS_ROW2_LABEL_Y, SETTINGS_ROW2_Y, "Missing-tag threshold:", g_missing_threshold_secs);
    draw_wifi_toggle_row(SETTINGS_ROW3_LABEL_Y, SETTINGS_ROW3_Y);

    tft.fillRect(10, SETTINGS_BACK_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
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
    // g_triggering_tag_idx is always valid here - this screen only ever
    // shows for a real per-tag alarm now (see the file header).
    tft.print("Missing: ");
    tft.print(tag_short_label(g_triggering_tag_idx));
    tft.setTextSize(1);
    tft.setCursor(10, tft.height() - 20);
    tft.print("Tap anywhere to silence");
}

static const int ENROLL_CONFIRM_YES_Y = 100;
static const int ENROLL_CONFIRM_DELETE_Y = 150;
static const int ENROLL_CONFIRM_CANCEL_Y = 200;

static void draw_enroll_confirm_screen() {
    bool enrolled = g_selected_tag_idx >= 0 && is_enrolled(tag_key(g_selected_tag_idx));

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print(enrolled ? "Un-enroll this tag?" : "Enroll this tag?");
    tft.setCursor(10, 50);
    if (g_selected_tag_idx >= 0) tft.print(tag_short_label(g_selected_tag_idx));

    tft.fillRect(10, ENROLL_CONFIRM_YES_Y, tft.width() - 20, BUTTON_H, enrolled ? TFT_DARKGREY : TFT_DARKGREEN);
    tft.setCursor(20, ENROLL_CONFIRM_YES_Y + 12);
    tft.print(enrolled ? "Unenroll tag" : "Enroll tag");

    tft.fillRect(10, ENROLL_CONFIRM_DELETE_Y, tft.width() - 20, BUTTON_H, TFT_MAROON);
    tft.setCursor(20, ENROLL_CONFIRM_DELETE_Y + 12);
    tft.print("Delete this tag");

    tft.fillRect(10, ENROLL_CONFIRM_CANCEL_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setCursor(20, ENROLL_CONFIRM_CANCEL_Y + 12);
    tft.print("Cancel");
}

static const int WIFI_WARNING_YES_Y = 140;
static const int WIFI_WARNING_NO_Y = 190;

static void draw_wifi_warning_screen() {
    tft.fillScreen(BRIGHT_RED);
    tft.setTextColor(TFT_WHITE, BRIGHT_RED);
    tft.setTextSize(2);
    tft.setCursor(4, 4);
    tft.print("Warning");

    tft.setTextSize(1);
    tft.setCursor(4, 40);
    tft.print("MOB detection is unstable with");
    tft.setCursor(4, 54);
    tft.print("WiFi enabled (shared radio steals");
    tft.setCursor(4, 68);
    tft.print("BLE scan time). Enabling will");
    tft.setCursor(4, 82);
    tft.print("double both detection timers");
    tft.setCursor(4, 96);
    tft.print("for as long as WiFi stays on.");
    tft.setCursor(4, 116);
    tft.print("Enable WiFi anyway?");

    tft.fillRect(10, WIFI_WARNING_YES_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setTextSize(2);
    tft.setCursor(20, WIFI_WARNING_YES_Y + 10);
    tft.print("Yes, enable WiFi");

    tft.fillRect(10, WIFI_WARNING_NO_Y, tft.width() - 20, BUTTON_H, TFT_DARKGREY);
    tft.setCursor(20, WIFI_WARNING_NO_Y + 10);
    tft.print("No, stay disabled");
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

    // The warning bar itself is static (the LED does the flashing, not the
    // screen - see update_led()), so refresh_normal_list_periodically()'s
    // once-a-second forced redraw is plenty to keep it appearing/
    // disappearing promptly as tags get enrolled/un-enrolled - no need to
    // force a redraw every single frame here too.
    if (!g_needs_redraw) return;
    g_needs_redraw = false;

    switch (g_screen) {
    case Screen::NORMAL_LIST: draw_normal_screen(); break;
    case Screen::SETTINGS: draw_settings_screen(); break;
    case Screen::DISABLE_MENU: draw_disable_menu(); break;
    case Screen::ENROLL_CONFIRM: draw_enroll_confirm_screen(); break;
    case Screen::WIFI_WARNING: draw_wifi_warning_screen(); break;
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
        } else if (p.y >= SETTINGS_ROW3_Y && p.y < SETTINGS_ROW3_Y + SETTINGS_BTN_H) {
            if (WiFi.status() == WL_CONNECTED || g_wifi_enabled) disable_wifi();
            else g_screen = Screen::WIFI_WARNING;
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
    case Screen::ENROLL_CONFIRM:
        // All three buttons drop straight back to the normal screen.
        if (p.y >= ENROLL_CONFIRM_YES_Y && p.y < ENROLL_CONFIRM_YES_Y + BUTTON_H) {
            if (g_selected_tag_idx >= 0) {
                String key = tag_key(g_selected_tag_idx);
                if (is_enrolled(key)) unenroll_tag(key);
                else enroll_tag(key, "");
            }
            g_screen = Screen::NORMAL_LIST;
            g_selected_tag_idx = -1;
            g_triggering_tag_idx = -1;
        } else if (p.y >= ENROLL_CONFIRM_DELETE_Y && p.y < ENROLL_CONFIRM_DELETE_Y + BUTTON_H) {
            // Same effect as the /enrolled web page's "Delete" button -
            // un-enrolls (forgetting the name, if any) and frees the slot.
            if (g_selected_tag_idx >= 0) {
                String key = tag_key(g_selected_tag_idx);
                unenroll_tag(key);
                g_tags[g_selected_tag_idx].used = false;
                g_tags[g_selected_tag_idx].name = "";
            }
            g_screen = Screen::NORMAL_LIST;
            g_selected_tag_idx = -1;
            g_triggering_tag_idx = -1;
        } else if (p.y >= ENROLL_CONFIRM_CANCEL_Y && p.y < ENROLL_CONFIRM_CANCEL_Y + BUTTON_H) {
            g_screen = Screen::NORMAL_LIST;
            g_selected_tag_idx = -1;
            g_triggering_tag_idx = -1;
        }
        break;
    case Screen::WIFI_WARNING:
        if (p.y >= WIFI_WARNING_YES_Y && p.y < WIFI_WARNING_YES_Y + BUTTON_H) {
            g_screen = Screen::SETTINGS;
            enable_wifi();
        } else if (p.y >= WIFI_WARNING_NO_Y && p.y < WIFI_WARNING_NO_Y + BUTTON_H) {
            g_screen = Screen::SETTINGS;
        }
        break;
    }
    g_needs_redraw = true;
}

// A short, low-movement press on a table row ignores/un-ignores that tag -
// distinguished from a scroll drag by dispatch_tap()'s move-distance
// tracking, so dragging the list never accidentally opens this. y can land
// inside the "no tags enrolled" warning bar's strip (that bar is drawn
// above list_bottom - see draw_normal_screen()) without any special
// handling: tapped_row just comes out >= count and gets ignored below,
// same as tapping any other empty space.
static void handle_row_tap(int16_t y) {
    int list_bottom = tft.height() - BUTTON_H;
    if (y < LIST_TOP || y >= list_bottom) return;
    int tapped_row = (y - LIST_TOP + g_scroll_offset) / ROW_H;
    int order[MAX_TAGS];
    int count = compute_display_order(order);
    if (tapped_row < 0 || tapped_row >= count) return;
    g_selected_tag_idx = order[tapped_row];
    g_screen = Screen::ENROLL_CONFIRM;
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
        if (no_tags_warning_active()) {
            // Slower, milder flash than a real alarm (see MOB_DETECTED
            // below) - "nothing enrolled yet" is a lower-stakes situation
            // than "a tag actually went missing", and this is meant to
            // catch the eye without being mistaken for the real thing.
            unsigned long phase = (millis() / 500) % 2;
            if (phase == 0) led_set(LED_OFF, LED_DIM, LED_OFF); // dim green
            else led_set(LED_FULL, LED_OFF, LED_OFF);           // red
        } else {
            led_set(LED_OFF, LED_DIM, LED_OFF); // dim green - all clear
        }
        break;
    case Screen::SETTINGS:
    case Screen::DISABLE_MENU:
    case Screen::ENROLL_CONFIRM:
    case Screen::WIFI_WARNING:
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

// Forces a redraw at least once a second while on NORMAL_LIST, so the
// "Seen" column's elapsed-time-since-last-sighting keeps ticking even
// during a stretch with no new BLE sightings to trigger one on their own.
static void refresh_normal_list_periodically() {
    static unsigned long last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    if (g_screen == Screen::NORMAL_LIST) g_needs_redraw = true;
}

// '1'..'9' force-triggers MOB for the tag at that display position (skips
// waiting for the real missing-tag timeout) - useful for quickly testing
// the alarm/escalation UI on the bench. Same as the debug web page's
// "Force trigger now" button. 'r'/'R' wipes all enrollments - same as
// factory_reset_enrolled()'s other caller, the one-time migration in
// setup(), but callable any time it's needed again.
static void check_serial_commands() {
    if (!Serial.available()) return;
    char c = (char)Serial.read();
    if (c >= '1' && c <= '9') {
        int order[MAX_TAGS];
        int count = compute_display_order(order);
        int row = c - '1';
        if (row < count) trigger_mob(order[row]);
    } else if (c == 'r' || c == 'R') {
        factory_reset_enrolled();
        g_needs_redraw = true;
    }
}

// Pre-populates g_tags[] from tags enrolled in a previous session, so they
// show up immediately (as "never seen" this session - see draw_normal_screen)
// instead of only reappearing once actually re-heard. first_seen_ms is left
// at its zero-initialized 0 for these (never set here) - find_ignore_swap_candidate()
// treats that as "already known, not something that just appeared", so a
// pre-populated tag can never be mistaken for some other tag's swap
// candidate. Ignored tags are never in here at all - see NVS_ENROLLED_NAMESPACE.
static void load_enrolled_tags_into_table() {
    enrolled_cache_load_if_needed();
    String csv = g_enrolled_csv_cache;
    int slot = 0;
    int pos = 1; // csv starts with a leading ','
    while (slot < MAX_TAGS) {
        int bar = csv.indexOf('|', pos);
        if (bar < 0) break;
        int comma = csv.indexOf(',', bar);
        if (comma < 0) break;
        String hex = csv.substring(pos, bar);
        String name = csv.substring(bar + 1, comma);
        if (hex.length() == 40) {
            for (int i = 0; i < 20; i++) {
                g_tags[slot].eid[i] = (uint8_t)strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
            }
            g_tags[slot].used = true;
            g_tags[slot].rssi = -100;
            g_tags[slot].last_seen_ms = 0;
            g_tags[slot].name = name;
            slot++;
        }
        pos = comma + 1;
    }
    if (slot > 0) Serial.printf("Loaded %d previously-enrolled tag(s) from NVS.\n", slot);
}

// One-time cleanup, bumped to v3 for the enroll/ignore rewrite: anything
// sitting in NVS from an earlier design (RSSI-proximity auto-enroll, or
// naming-gated persistence) is in an incompatible format anyway, which the
// parser above wouldn't read correctly even if it wanted to - so a clean
// wipe is required here, not just tidy. Runs exactly once (gated by
// NVS_KEY_RESET_V3_DONE), then never fires again even across future
// reboots - enrolling after this is expected to be a deliberate action, not
// leftover cruft from an old version. A fresh install also has this key
// unset, so it pays the same one-time no-op cost of finding nothing to clear.
static const char *NVS_KEY_RESET_V3_DONE = "resetV3";

static void perform_one_time_enrollment_reset_if_needed() {
    g_prefs.begin(NVS_MOBCFG_NAMESPACE, true);
    bool done = g_prefs.getBool(NVS_KEY_RESET_V3_DONE, false);
    g_prefs.end();
    if (done) return;

    Serial.println("First boot after the enroll/ignore rewrite - clearing any tags persisted under an "
                    "earlier scheme (incompatible format anyway). This runs only this one time; type "
                    "'r' + Enter any time later for a manual reset.");
    factory_reset_enrolled();

    g_prefs.begin(NVS_MOBCFG_NAMESPACE, false);
    g_prefs.putBool(NVS_KEY_RESET_V3_DONE, true);
    g_prefs.end();
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println();
    Serial.println("=== MOB display/LED/touch/BLE state-machine (tag-unknown mode) ===");

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

    load_mob_settings();
    perform_one_time_enrollment_reset_if_needed();
    load_enrolled_tags_into_table();

    // WiFi stays OFF by default now - see enable_wifi()'s comment for why.
    // Toggle it on from the Settings screen when you actually need the
    // debug web UI.
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi is OFF by default - enable it from the Settings screen if you need the web UI.");

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    // Continuous, non-blocking scan (duration=0) - see
    // continuous_scan_diagnostic.cpp for why the blocking start(duration,...)
    // overload is unusable here: it would stall this loop() for the whole
    // scan window, which also has to keep the display/touch responsive.
    pBLEScan->start(0, nullptr, false);

    Serial.println("Serial '1'..'9' force-triggers MOB for the tag at that display position.");

    g_needs_redraw = true;
}

void loop() {
    g_web_server.handleClient();
    check_serial_commands();

    // Drain whatever onResult() queued since the last pass - matching here
    // is a trivial byte comparison now (no crypto), so this never blocks
    // the display/touch/LED work below.
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
    // New sightings update g_tags[] in memory, but that alone never told
    // render_current_screen() to actually repaint - the simulated build's
    // jitter tick used to do this as a side effect, and got lost when this
    // switched to real BLE ingestion. Without this, the table only ever
    // refreshed when a touch happened to trigger a redraw for other reasons.
    if (count > 0 && g_screen == Screen::NORMAL_LIST) g_needs_redraw = true;

    refresh_normal_list_periodically();
    log_trigger_stats_periodically();
    check_for_missing_tags();
    check_for_stale_ignored_tags();
    update_mob_state_machine();
    update_led();
    render_current_screen();
    handle_touch_input();
}
