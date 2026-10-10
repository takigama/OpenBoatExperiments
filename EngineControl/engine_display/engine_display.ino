/**
 * Engine Panel - STEP 2 (step 1b + multi-engine / protocol v2)
 *
 * = step 1b (all display features, guarded ticks, heap instrumentation,
 *   16-line bounce buffer, baseline WiFi code UNTOUCHED)
 * + protocol v2: engine-indexed CAN IDs, capability ANNOUNCE, CAN
 *   alarm-silence command with source-node authority byte
 * + engine discovery: table of announced engines, 10s ageout,
 *   auto-select first present engine, "NO ENGINE DETECTED" screen
 * + settings cog opens a general Settings dialog (WiFi/CAN status,
 *   Factory Reset with confirmation); a separate ENGINE chip opens the
 *   engine selector (choice saved to NVS)
 * + capability-driven widget visibility (only what the selected
 *   engine supports is shown)
 * + first-boot Initial Setup wizard, 3 steps: role (primary vs
 *   secondary) -> security PIN (primary only, skippable) -> WiFi
 *   SSID/password (also primary only) - secondary finishes right after
 *   choosing its role. Shown until "setup_done" is set in NVS; Factory
 *   Reset clears it (and the PIN) and reboots.
 * + optional PIN lock, PRIMARY ONLY: if a PIN was set, a full-screen
 *   lock overlay (numeric keypad, see build_pin_pad) blocks the panel at
 *   boot until the right PIN is entered - stays unlocked for the rest of
 *   that boot session. Settings -> "Change PIN"/"Set PIN" changes it
 *   later (verifies the old PIN first if one exists). Not offered on
 *   secondary - it doesn't wake up until an engine is switched on, so
 *   there's nothing on it worth locking. Same trust model as the WiFi
 *   password already in NVS - a "keep guests off the buttons" feature,
 *   not a security boundary against physical access.
 * + secondary display role (like the planned CYD cockpit display): no
 *   ignition authority, no WiFi at all (wifi_setup() never runs - see
 *   setup()/loop()), and no power switch/engine picker/full Settings -
 *   stays on the black "off" screen (no cog, no button) until the
 *   primary's ignition command is confirmed via telemetry (eng.ign_actual),
 *   then shows the normal gauges plus working glow/start buttons and a
 *   cog that opens Factory-Reset-only. See g_display_role in ui_tick's
 *   power state machine + controls_ok, and in can_send_commands().
 * + 4 fake engines on the debug page for testing without hardware
 * + serial console: "WIFI:<ssid>,<password>" over the CH340 port at any
 *   time sets WiFi creds in NVS and reboots - NVS-only, never touches
 *   WiFi.* or LVGL directly from the loop task (see serial_console_tick)
 * + CAP_STOP: the START button becomes a STOP button (red, crossed-arrow
 *   icon_stop) ONLY when the selected engine announced CAP_STOP AND
 *   this is the primary display AND the engine is running - never a
 *   generic capability. The MD2030 (mechanical-stop-only) must never
 *   get CAP_STOP, including its fake E0 on the debug page - see the
 *   safety note in can_protocol.h and g_can_stop in ui_tick.
 *
 * REQUIRES the v2 protocol in the can_protocol.h tab (the file
 * named can_protocol.h, NOT the v1 baseline header).
 * Icon tabs required: icon_temp.c, icon_oil.c, icon_glow.c, icon_start.c,
 * icon_stop.c
 * lv_conf.h: LV_FONT_MONTSERRAT_28 = 1, LV_FONT_MONTSERRAT_48 = 1
 */

#include "board_select.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"   /* esp_wifi_set_channel() - pins the radio's channel
                          * without an actual STA join/AP broadcast, see
                          * wifi_setup()'s no-credentials path */
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "driver/twai.h"
#include "lvgl_v8_port.h"
#include "can_protocol.h"
#include <esp_now.h>
#include <esp_random.h>
#include "espnow_pairing.h"
#include "espnow_bus.h"
#include "wired_bus.h"
#include "fleet_security.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   /* manifest and releases are on GitHub (https) */
#include <HTTPUpdate.h>   /* httpUpdate global singleton - the download+flash convenience wrapper actually used */
#include <Update.h>       /* esp_ota_* lower-level API HTTPUpdate sits on top of - needed for the app-valid rollback marker */
#include <ArduinoJson.h>   /* new dependency - see CLAUDE.md's "Libraries" line.
                             * OTA-only (HELM); harmless if pulled into a CYD
                             * build too since nothing instantiates a
                             * JsonDocument outside the #if TARGET_BOARD ==
                             * BOARD_HELM_S3_800x480 OTA code below. */
#include "esp_ota_ops.h"   /* esp_ota_mark_app_valid_cancel_rollback() */

/* ==================== web-viewable serial log ====================
 * This board runs headless on the boat most of the time - no USB cable
 * attached to diagnose it with. Mirrors everything the rest of this
 * file sends to Serial into a small ring buffer, served at /serial, so
 * the debug page can show it remotely (added specifically to chase an
 * ESP-NOW pairing problem with no physical serial access to HELM).
 *
 * Mechanism: subclass HardwareSerial and override its two virtual
 * write() methods - every Print/Stream convenience call (println,
 * printf, print...) ultimately funnels through one of these two, so
 * capturing there transparently covers every existing Serial.xxx(...)
 * call site in this file with no other code changes. #define Serial to
 * an instance of it right after the includes so every later usage picks
 * it up automatically. This only affects THIS translation unit (the
 * .ino plus its directly #include'd headers, e.g. can_protocol.h) -
 * vendored library .cpp files (lvgl_v8_port.cpp etc.) are separately
 * compiled and never see this #define, so their own internal Serial
 * usage, if any, is completely untouched. Kept modest in size
 * (4KB) given this board's internal-SRAM scarcity (see the hard
 * constraints section of CLAUDE.md) - the handler below needs a second,
 * equally-sized static scratch buffer to linearize the ring, so this is
 * an 8KB addition total.
 *
 * CONCURRENCY: espnow_on_recv() (an ESP-NOW driver callback - a genuinely
 * different FreeRTOS task/core than loop(), not just a different
 * function) also calls Serial.print*() for its own diagnostic logging,
 * so writes into this ring buffer can happen from a different core than
 * handle_serial_log()'s read (called from loop()'s server.handleClient()).
 * Without synchronization that's a real, not theoretical, race - a torn
 * read of g_serial_log_pos/g_serial_log_wrap can make handle_serial_log()'s
 * unsigned tail-length subtraction underflow into a huge value and
 * memcpy() far past the end of its buffer, corrupting adjacent memory
 * and taking the whole WebServer down (confirmed on real hardware: build
 * 15 crashed HELM's debug page after this feature started getting
 * polled). g_serial_log_mux (a lightweight ESP32 spinlock, not a full
 * RTOS mutex - cheap enough to take on every single byte written)
 * protects every access to the ring buffer's state, in both the writer
 * below and handle_serial_log()'s reader. */
#define SERIAL_LOG_BUF_SIZE 4096
static char           g_serial_log_buf[SERIAL_LOG_BUF_SIZE];
static size_t         g_serial_log_pos  = 0;      /* next write position (ring) */
static bool           g_serial_log_wrap = false;  /* has the ring wrapped at least once */
static portMUX_TYPE   g_serial_log_mux  = portMUX_INITIALIZER_UNLOCKED;

class LoggingSerial : public HardwareSerial {
public:
    LoggingSerial(int uart_nr) : HardwareSerial(uart_nr) {}
    size_t write(uint8_t c) override {
        portENTER_CRITICAL(&g_serial_log_mux);
        g_serial_log_buf[g_serial_log_pos++] = c;
        if (g_serial_log_pos >= SERIAL_LOG_BUF_SIZE) {
            g_serial_log_pos = 0;
            g_serial_log_wrap = true;
        }
        portEXIT_CRITICAL(&g_serial_log_mux);
        return HardwareSerial::write(c);
    }
    size_t write(const uint8_t *buf, size_t sz) override {
        /* one lock acquisition for the whole run, not one per byte -
         * printf-style output can be dozens of bytes at once */
        portENTER_CRITICAL(&g_serial_log_mux);
        for (size_t i = 0; i < sz; i++) {
            g_serial_log_buf[g_serial_log_pos++] = buf[i];
            if (g_serial_log_pos >= SERIAL_LOG_BUF_SIZE) {
                g_serial_log_pos = 0;
                g_serial_log_wrap = true;
            }
        }
        portEXIT_CRITICAL(&g_serial_log_mux);
        return HardwareSerial::write(buf, sz);
    }
};

static LoggingSerial g_log_serial(0);   /* UART0 - same peripheral the
    * core's own global Serial object would otherwise bind to; that
    * original object is simply never begin()'d or used once this
    * #define is in effect, so there's no dual-ownership of the UART */
#define Serial g_log_serial

/* these headers print on Serial, so they must come AFTER the #define above - included earlier they would
 * write to the core's original (never begun) Serial object and their output would simply vanish */
#include "setup_web.h"
#include "serial_cli.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* set once in setup(), read by ota_start_download_cb() to blank the
 * backlight during a self-update download - see that function's comment
 * for why (RGB panel corruption during flash writes, not fixable by
 * locking LVGL alone). HELM only; CYD builds never assign this (no OTA
 * there) and never call getBacklight() on it. */
static Board *g_board = NULL;

/* OTA: bump by hand every release. Monotonic build number, not semver -
 * the manifest comparison is just `remote_build > FW_BUILD`, no version
 * string parsing/ordering needed. See check_for_update_tick(). */
#define FW_BUILD 36

/* OTA manifest: ota/manifest.json in the OpenBoat repo on GitHub, written by
 * tools/release.sh. One entry per device, then one per hardware variant:
 *   {"helm":    {"viewe7": {"build":N,"url":"...","md5":"..."}},
 *    "can_sim": {"s3zero": {"build":N,"url":"...","md5":"..."}, ...}}
 * The images are GitHub Release assets. To test against a manifest of your own
 * (e.g. a local web server), put `#define OTA_MANIFEST_URL "..."` in a
 * git-ignored local_config.h next to this sketch (see local_config.example.h). */
#if defined(__has_include)
#  if __has_include("local_config.h")
#    include "local_config.h"
#  endif
#endif
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL "https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/EngineControl/ota/manifest.json"
#endif

/* this firmware's own key in the manifest's "helm" entry (HELM board only;
 * the CYD targets do no OTA) */
#define HELM_HW_KEY "viewe7"

/* heap forensics: internal SRAM is the scarce resource (WiFi, WebServer
 * and LVGL widget structs can ONLY live there - PSRAM can't help them) */
static void heap_report(const char *stage)
{
    Serial.printf("HEAP[%s]: internal=%u largest=%u psram=%u\n", stage,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* HELM only - icon_*.c are sized for the 800x480 dashboard; create_ui_cyd()
 * uses plain text instead. Gating these out (along with create_ui() and
 * friends below) keeps them from bloating the CYD binary - referencing
 * them anywhere pulls their pixel data into the link even if the
 * function that does it is never called at runtime. */
LV_IMG_DECLARE(icon_temp);
LV_IMG_DECLARE(icon_oil);
LV_IMG_DECLARE(icon_glow);
LV_IMG_DECLARE(icon_start);
LV_IMG_DECLARE(icon_stop);
#endif

/* ==================== wifi credentials ==================== */
/* Non-empty  -> used AND saved to flash.
 * Both empty -> previously saved values are loaded from flash. */

#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""
/* Left blank now that the Setup dialog (first boot) and Settings ->
 * Factory Reset manage credentials via NVS. Put bench creds back here
 * only for throwaway testing - anything here overwrites NVS every boot. */

/* ==================== pins & constants ==================== */

/* CAN pins are board-specific - the HELM board's are load-bearing (see
 * CLAUDE.md's "Hard constraints" GPIO list), the CYD ones below are
 * placeholders picked to avoid this file's TFT/touch/backlight pins for
 * each variant (see esp_panel_board_custom_conf.h) - NOT verified against
 * real hardware, confirm free GPIOs on your actual board before flashing. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
#define PIN_CAN_TX     17
#define PIN_CAN_RX     13
#elif TARGET_BOARD == BOARD_CYD_24_RESISTIVE
#define PIN_CAN_TX     22
#define PIN_CAN_RX     35   /* input-only pin is fine: TWAI RX only receives */
#else   /* both 2.8" CYD variants */
#define PIN_CAN_TX     22
#define PIN_CAN_RX     34   /* input-only pin is fine: TWAI RX only receives */
#endif

/* Direct-wire supplementary transport (see wired_bus.h) - HELM only, 2 of
 * its 3 truly-free GPIOs (10/11/12 - see CLAUDE.md's hard-constraint GPIO
 * list; CAN already claims 17/13). Leaves GPIO 12 still spare. Not
 * offered on CYD - a different chip with its own, much smaller free-GPIO
 * set already fully accounted for by CAN/touch/backlight there. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
#define PIN_WIRED_TX   10
#define PIN_WIRED_RX   11
#endif

#define RPM_MAX        4000
#define RPM_RED_V      3600      /* MD2030 rated max ~3600 rpm */
#define RPM_RUNNING_MIN 100      /* rpm at/above this = engine running */
#define OVERREV_MS     5000      /* red-zone dwell before alarm */

/* local alarm bits */
#define ALM_TEMP       (1 << 0)
#define ALM_PRESS      (1 << 1)
#define ALM_OVERREV    (1 << 2)

/* ==================== shared engine state ==================== */

typedef struct {
    volatile int      rpm;
    volatile float    temp_c;
    volatile float    oil_bar;
    volatile bool     temp_alarm;
    volatile bool     press_alarm;
    volatile bool     ign_actual;
    volatile bool     glow_active;
    volatile bool     crank_active;
    volatile bool     stop_active;   /* CTRL confirms the stop relay is cutting */
    volatile uint32_t hours_x10;
    volatile bool     hours_seen;
    volatile uint32_t last_telem_ms;
} engine_data_t;

static engine_data_t eng = { 0, 60.0f, 3.0f, false, false,
                             false, false, false, false, 0, false, 0 };

/* ==================== engine discovery (protocol v2) ==================== */

typedef struct {
    bool     present;
    uint16_t caps;
    uint8_t  type;
    uint32_t last_seen;
    bool     ign_on;   /* ignition-actual, tracked for EVERY engine (not
                         * just sel_engine) so a secondary display can
                         * filter the engine picker to only "on" engines */
    char     name[MD_ENGINE_NAME_MAXLEN + 1];   /* from MSG_ENGINE_NAME, "" if
                                                  * not yet received - falls
                                                  * back to engtype_name() */
    uint16_t fw_build;  /* from ANNOUNCE[5..6], 0 if never received (or the
                          * remote board's firmware predates this field) -
                          * see "OTA updates" in CLAUDE.md for how this
                          * drives the remote device-list modal. Currently
                          * only can_sim boards report a real value here, so
                          * it is compared against the manifest's "can_sim"
                          * entry for this board's hw_id. */
    uint8_t  hw_id;     /* from ANNOUNCE[7] (HW_*), 0 = not reported: which
                          * manifest variant this board takes. */
} engine_info_t;

static engine_info_t engines[MD_MAX_ENGINES] = {};
static uint8_t sel_engine = 0;      /* persisted in NVS ("engine") */

/* Autostart per-engine settings (Settings -> Autostart Settings), NVS keys
 * "asG%d"/"asC%d"/"asW%d"/"asR%d" - loaded once in setup() (HELM/primary
 * only, mirrors enroll_load_from_nvs()'s per-engine-indexed pattern).
 * CYD has no Settings screen at all (see create_ui_cyd()) and just uses
 * these compiled-in defaults unmodified - a deliberate scope call, not an
 * oversight; see engine_display/CLAUDE.md's Autostart section. Crank
 * seconds should never exceed CRANK_MAX_MS/1000 (60) - the CTRL board's
 * own deadman cuts cranking there regardless of what's configured. */
static uint8_t as_glow_s[MD_MAX_ENGINES]  = { 10, 10, 10, 10 };
static uint8_t as_crank_s[MD_MAX_ENGINES] = { 30, 30, 30, 30 };
static uint8_t as_wait_s[MD_MAX_ENGINES]  = { 5, 5, 5, 5 };
static uint8_t as_retries[MD_MAX_ENGINES] = { 3, 3, 3, 3 };

static const char *engtype_name(uint8_t t)
{
    switch (t) {
    case ENGTYPE_MD2030:         return "MD2030";
    case ENGTYPE_MD2030C:        return "Volvo Penta MD2030C";
    case ENGTYPE_MD2040:         return "MD2040";
    case ENGTYPE_YANMAR_2YM15:   return "Yanmar 2YM15";
    case ENGTYPE_YANMAR_4JH80:   return "Yanmar 4JH80";
    case ENGTYPE_NANNI:          return "Nanni Diesel";
    case ENGTYPE_GENERIC_DIESEL: return "Diesel";
    case ENGTYPE_GENERIC_PETROL: return "Petrol";
    default:                     return "Unknown";
    }
}

/* prefer the name the control board sent over the wire (MSG_ENGINE_NAME,
 * e.g. "Volvo Penta MD2030C") - falls back to the type-enum lookup above
 * for engines that haven't sent one yet (or the local WiFi debug-page
 * fakes, which have no real control board to send one at all) */
static const char *engine_display_name(uint8_t i)
{
    if (i < MD_MAX_ENGINES && engines[i].name[0]) return engines[i].name;
    return engtype_name(i < MD_MAX_ENGINES ? engines[i].type : ENGTYPE_UNKNOWN);
}

static uint16_t current_caps(void)
{
    /* until a control board announces itself, show everything so the
     * WiFi debug workflow keeps working with no hardware attached */
    return engines[sel_engine].present ? engines[sel_engine].caps
                                       : CAP_ALL_DEFAULT;
}

static bool any_engine_present(void)
{
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        if (engines[i].present) return true;
    return false;
}

static void reset_telemetry(void)
{
    eng.rpm = 0; eng.temp_c = 0; eng.oil_bar = 0;
    eng.temp_alarm = eng.press_alarm = false;
    eng.ign_actual = eng.glow_active = eng.crank_active = eng.stop_active = false;
    eng.hours_x10 = 0; eng.hours_seen = false;
    eng.last_telem_ms = 0;
}

/* ---- fake engines (enabled from the debug page) ----
 * All four present the same full MD2030-style capability set for now;
 * they'll diverge when the real engines' differences matter. */
#define FULL_DIESEL_CAPS (CAP_RPM | CAP_COOLANT_TEMP | CAP_OIL_PRESS | \
                          CAP_TEMP_ALARM | CAP_PRESS_ALARM | CAP_GLOW | \
                          CAP_START | CAP_IGNITION | CAP_HOURS | CAP_AUDIO)
/* E3 (Nanni) is the one fake engine that simulates an electric stop, so
 * the feature is bench-testable without real CTRL hardware. E0
 * (MD2030C) must NEVER get CAP_STOP even in simulation - the real
 * MD2030 can only ever be stopped mechanically. */
#define FULL_DIESEL_CAPS_STOP (FULL_DIESEL_CAPS | CAP_STOP)

static volatile bool fake_en[MD_MAX_ENGINES] = { false, false, false, false };
static const struct { uint16_t caps; uint8_t type; } FAKES[MD_MAX_ENGINES] = {
    { FULL_DIESEL_CAPS,      ENGTYPE_MD2030C      },   /* E0 - mechanical stop only */
    { FULL_DIESEL_CAPS,      ENGTYPE_YANMAR_2YM15 },   /* E1 */
    { FULL_DIESEL_CAPS,      ENGTYPE_YANMAR_4JH80 },   /* E2 */
    { FULL_DIESEL_CAPS_STOP, ENGTYPE_NANNI        },   /* E3 - simulates electric stop */
};

static void fake_engines_tick(void)
{
    /* refresh enabled fakes as if their ANNOUNCE just arrived */
    static bool fake_was_en[MD_MAX_ENGINES] = {};
    static uint32_t last = 0;
    if (millis() - last < ANNOUNCE_PERIOD_MS) return;
    last = millis();
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        if (fake_en[i]) {
            engines[i].present   = true;
            engines[i].caps      = FAKES[i].caps;
            engines[i].type      = FAKES[i].type;
            engines[i].last_seen = millis();
            fake_was_en[i] = true;
        } else if (fake_was_en[i]) {
            /* WE enabled it, so we drop it - a real engine announcing
             * on this slot is never touched */
            engines[i].present = false;
            engines[i].ign_on  = false;
            fake_was_en[i] = false;
        }
    }
}

/* ==================== command state (UI -> loop -> CAN) ==================== */
/* POWER: user_power is the user's commanded state, PER ENGINE - two
 * engines behind one display must have independent power states, or
 * powering one on while viewing it would make every other engine you
 * switch to also read as "powered on" (g_power/off_overlay are driven by
 * whichever engine is currently selected, so a single shared flag leaks
 * across engines the instant you switch). Engine running LATCHES its own
 * user_power[e] true (sticky): the panel stays on after that engine
 * stops until the user explicitly turns it off. While START is held,
 * rpm is masked from the running-determination. */

static volatile bool user_power[MD_MAX_ENGINES] = { false, false, false, false };
static volatile bool ign_commanded = false;
static volatile bool glow_held     = false;
static volatile bool start_held    = false;
static volatile bool stop_held     = false;  /* dead-man, CAP_STOP engines only */
static volatile bool silence_req   = false;  /* send CAN alarm-silence once */
static bool g_engine_running       = false;
static bool g_power                = false;
static bool g_can_stop              = false;  /* caps has CAP_STOP && role==primary,
                                                * cached each tick for start_cb */
static uint8_t g_active_alarms     = 0;
static uint8_t silenced_mask       = 0;
static uint32_t glow_press_ms = 0, start_press_ms = 0, stop_press_ms = 0;

/* ==================== autostart (glow -> crank -> retry) ====================
 * Single-tap automated version of the same dead-man protocol the manual
 * glow/start buttons already drive - autostart_tick() just sets
 * glow_held/start_held (+ their _press_ms stamps) exactly as glow_cb/
 * start_cb do, so can_send_commands()'s existing HOLD_RESEND_MS resend
 * loop needs no changes at all. One state machine only (not per-engine
 * parallel runs): glow_held/start_held always target sel_engine in
 * bus_send(), so autostart_engine records which engine a run is actually
 * for, and switching sel_engine away from it mid-run cancels the whole
 * sequence rather than silently commanding the wrong engine. */
enum { AUTOSTART_IDLE = 0, AUTOSTART_GLOWING, AUTOSTART_CRANKING, AUTOSTART_WAITING };
static uint8_t  autostart_state        = AUTOSTART_IDLE;
static uint8_t  autostart_engine       = 0;
static uint8_t  autostart_retries_left = 0;
static uint32_t autostart_phase_ms     = 0;

/* CRANKING's catch check needs rpm to STAY above RPM_RUNNING_MIN for a
 * short stretch, not just clear it on one sample - real (and simulated)
 * cranking twitter genuinely spikes above RPM_RUNNING_MIN transiently on
 * compression strokes well before an actual catch, and a manual start
 * never noticed because a human doesn't release START the instant the
 * tach twitches. 0 means "not currently above threshold" - reset at
 * both CRANKING entry points (a fresh crank or a retry) so a stale
 * timestamp from an earlier attempt can never look like an instant
 * catch on the very next tick. */
#define AUTOSTART_CATCH_HOLD_MS 500
static uint32_t autostart_catch_since_ms = 0;

/* ==================== gauge geometry ==================== */
/* HELM only - create_ui_cyd() shows plain-text rpm/temp/oil instead of an
 * arc gauge (doesn't fit a 320x240 screen), so none of this geometry math
 * or its point tables are needed on CYD builds. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* Path: straight climb at GA_ALPHA from P0, tangent arc of radius GA_R,
 * then horizontal at y=GA_EY out to x=GA_ENDX. rpm maps linearly onto
 * distance along the path. offset > 0 is outward (up/left of travel). */

#define GA_ALPHA_DEG   65.0f
#define GA_P0X         70.0f
#define GA_P0Y         430.0f
#define GA_R           120.0f
#define GA_EY          70.0f     /* horizontal run near the top */
#define GA_ENDX        755.0f

#define LINE_W         8
#define BAR_W          16
#define BAR_OFFSET     (-(LINE_W / 2.0f + 2.0f + BAR_W / 2.0f))

#define GAUGE_STEP_RPM 25
#define GAUGE_PTS      (RPM_MAX / GAUGE_STEP_RPM + 1)   /* 161 */
#define RED_START_IDX  (RPM_RED_V / GAUGE_STEP_RPM)     /* 144 */

static float g_alpha, g_sa, g_ca;
static float g_L1, g_arclen, g_total;
static float g_S1x, g_S1y, g_Cx, g_Cy, g_th0;

static void gauge_geom_init(void)
{
    g_alpha  = GA_ALPHA_DEG * (float)M_PI / 180.0f;
    g_sa     = sinf(g_alpha);
    g_ca     = cosf(g_alpha);
    g_Cy     = GA_EY + GA_R;
    g_S1y    = g_Cy - GA_R * g_ca;
    g_L1     = (GA_P0Y - g_S1y) / g_sa;
    g_S1x    = GA_P0X + g_L1 * g_ca;
    g_Cx     = g_S1x + GA_R * g_sa;
    g_arclen = GA_R * g_alpha;
    g_total  = g_L1 + g_arclen + (GA_ENDX - g_Cx);
    g_th0    = atan2f(g_S1y - g_Cy, g_S1x - g_Cx);
}

static void gauge_point(float rpm, float offset, lv_point_t *out)
{
    float s = g_total * rpm / (float)RPM_MAX;
    float x, y;
    if (s <= g_L1) {
        x = GA_P0X + s * g_ca - offset * g_sa;
        y = GA_P0Y - s * g_sa - offset * g_ca;
    } else if (s <= g_L1 + g_arclen) {
        float th  = g_th0 + (s - g_L1) / GA_R;
        float rad = GA_R + offset;
        x = g_Cx + rad * cosf(th);
        y = g_Cy + rad * sinf(th);
    } else {
        x = g_Cx + (s - g_L1 - g_arclen);
        y = GA_EY - offset;
    }
    out->x = (lv_coord_t)lroundf(x);
    out->y = (lv_coord_t)lroundf(y);
}

/* static point tables, filled once at startup */
static lv_point_t scale_grn_pts[RED_START_IDX + 1];
static lv_point_t scale_red_pts[GAUGE_PTS - RED_START_IDX];
static lv_point_t bar_pts[GAUGE_PTS];       /* full-path bar polyline    */
static lv_point_t bar_draw[GAUGE_PTS];      /* truncated working copy    */
static lv_point_t tick_pts[17][2];          /* every 250 rpm             */
#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

/* ==================== UI objects ==================== */

static lv_obj_t *bar_line;
static lv_obj_t *lbl_rpm, *lbl_hours;
static lv_obj_t *lbl_temp, *lbl_press;
static lv_obj_t *img_temp, *img_oil;
static lv_obj_t *img_start;   /* swapped between icon_start/icon_stop live */
static lv_obj_t *lbl_link, *lbl_engine;
static lv_obj_t *lbl_engine_off, *lbl_engine_noeng;   /* engine chip on the
                                                        * off / no-engine screens */
static lv_obj_t *btn_engine = NULL;                   /* the chip containers
                                                        * themselves - hidden
                                                        * entirely by ui_tick
                                                        * when there's nothing
                                                        * to choose between */
static lv_obj_t *btn_engine_off = NULL, *btn_engine_noeng = NULL; /* NULL on
                                                        * a secondary display:
                                                        * those screens never
                                                        * get one at all */
static lv_obj_t *lbl_overrev;
static lv_obj_t *sw_power, *btn_glow, *btn_start, *btn_mute, *btn_auto;
static lv_obj_t *lbl_auto;                       /* btn_auto's own label -
                                                    * doubles as phase status
                                                    * text, no separate
                                                    * status widget - see
                                                    * create_ui()'s comment */
static lv_obj_t *lbl_glow_cnt, *lbl_start_cnt;   /* hold-time counters */
static lv_obj_t *off_overlay;                    /* black power-off screen */
static lv_obj_t *btn_update_available = NULL;    /* off_overlay only, primary
                                                    * only - see check_for_
                                                    * update_tick()/ui_tick() */
/* declared here (not down by check_for_update_tick() itself) because
 * ui_tick() reads g_helm_update_available and is defined earlier in the
 * file than that - only FUNCTION prototypes get auto-hoisted by Arduino,
 * never variable declarations (same rule that bit g_can_disabled/
 * g_pairing_mode earlier in this project). */
static bool     g_helm_update_available = false;
static uint32_t g_helm_update_build     = 0;
static char     g_helm_update_url[160]  = "";
static char     g_helm_update_md5[40]   = "";   /* 32 hex chars + NUL, some
    * slack - "" if the manifest had no md5 for this entry (older manifest,
    * or hand-edited without one). See ota_start_download_cb(). */

/* Phase 2 (remote devices, e.g. can_sim) - same early-declaration
 * reasoning as the g_helm_update_* trio above. g_node_update[hw] caches the
 * manifest's "can_sim" entry for each hardware variant (fetched alongside
 * "helm" in the same check_for_update_tick() call); engines[i].fw_build and
 * engines[i].hw_id (see engine_info_t) are what each board is compared with. */
typedef struct {
    uint32_t build;
    char     url[160];
    char     md5[40];
} node_update_t;
static node_update_t g_node_update[HW_COUNT];

/* the url has to fit MSG_OTA_START next to the WiFi credentials; checked
 * against OTA_URL_MAXLEN further down. */
#define NODE_URL_MAXLEN 118

/* index into g_node_update[] of the manifest entry for engine e's
 * hardware, or -1 if the manifest has none for it (a board that reports no
 * hardware id is a first-generation can_sim). Returns an index rather than
 * a pointer: Arduino generates function prototypes ahead of the typedef. */
static int node_update_slot(int e)
{
    uint8_t hw = engines[e].hw_id;
    if (hw == HW_UNKNOWN) hw = HW_CANSIM_S3ZERO;
    if (hw >= HW_COUNT) return -1;
    const node_update_t *u = &g_node_update[hw];
    return (u->build > 0 && u->url[0]) ? (int)hw : -1;
}

/* remote OTA-in-progress state (device-list modal -> MSG_OTA_START ->
 * MSG_OTA_ACK), see ota_send_remote_start()/remote_ota_tick() */
typedef enum {
    REMOTE_OTA_IDLE = 0,
    REMOTE_OTA_WAIT_ACK,
    REMOTE_OTA_ACKED,
    REMOTE_OTA_TIMEOUT,
    REMOTE_OTA_CONFIRMED,   /* the target reappeared on the bus running the
        * expected build number - treated as proof the update succeeded,
        * independent of whether its one-shot MSG_OTA_ACK ever arrived at
        * all (see can_sim.ino's ota_tick() comment on why that ACK can
        * get lost even on a genuinely successful update). See
        * remote_ota_modal_tick(). */
} remote_ota_state_t;
static remote_ota_state_t g_remote_ota_state   = REMOTE_OTA_IDLE;
static int                g_remote_ota_engine  = -1;
static uint32_t           g_remote_ota_build   = 0;    /* manifest build sent to that engine */
static uint32_t           g_remote_ota_sent_ms = 0;

/* set by btn_check_update's tap (LVGL task), consumed by check_for_
 * update_tick() (loop task) to bypass the ~24h interval and check right
 * now - same volatile-flag handoff shape as the rest of this file's
 * cross-task state. lbl_check_status is declared here too since ui_tick()
 * needs to detect the true->false transition to update it (change-guard,
 * not a raw variable-hoisting concern - it's still a function reading it -
 * but keeping every off_overlay OTA widget/flag declared together). */
static bool      g_force_update_check = false;
static lv_obj_t *lbl_check_status     = NULL;

/* g_force_update_check alone is NOT safe for ui_tick() to edge-detect -
 * confirmed on real hardware ("Checking..." stuck forever even with no
 * WiFi issue and no held button). check_for_update_tick() runs on the
 * loop() task roughly every 2-5ms, so the true->false pulse on
 * g_force_update_check can easily complete entirely between two of
 * ui_tick()'s own 200ms samples - ui_tick() then never observes it as
 * true at all, never arms its own "pending" latch, and the "Checking..."
 * label set synchronously by check_update_now_cb() is never replaced.
 * These two counters replace that edge-detection with a level-safe
 * generation check: g_update_check_request_id is bumped once per tap
 * (check_update_now_cb()), g_update_check_done_id is set equal to the
 * request id that was in effect for a given run at every one of check_
 * for_update_tick()'s *forced* exit points (see that function) - once
 * g_last_check_result is actually valid for this run, never before.
 * ui_tick() just compares them for inequality on each of its own polls;
 * unlike a boolean pulse, an integer that changed once and then holds
 * still can never be "missed" by a slower poller. */
static uint32_t  g_update_check_request_id = 0;
static uint32_t  g_update_check_done_id    = 0;

/* outcome of the last check_for_update_tick() attempt, however it ended -
 * read by ui_tick() (see the g_force_update_check transition block) to
 * give the operator real feedback every time, not just "a button showed
 * up" when there happens to be an update. Set at every exit point in
 * check_for_update_tick(), including the failure ones - previously those
 * just logged to Serial and left the UI silently looking like nothing
 * happened. */
typedef enum {
    UPDATE_CHECK_RESULT_NONE = 0,   /* never actually run yet */
    UPDATE_CHECK_RESULT_OK,          /* fetched + parsed manifest successfully */
    UPDATE_CHECK_RESULT_NO_WIFI,
    UPDATE_CHECK_RESULT_FETCH_FAILED,   /* http.begin/GET/parse/missing-key */
} update_check_result_t;
static update_check_result_t g_last_check_result = UPDATE_CHECK_RESULT_NONE;

/* same pair, mirrored onto no_engine_overlay (empty-bus screen, topmost
 * of all - see that overlay's own comment) - a bus with no engines at
 * all is trivially safe to update (any_engine_active() already returns
 * false when nothing's present), so there's no reason to make the
 * operator power everything off first just to reach this screen. Both
 * buttons share ota_update_available_cb()/check_update_now_cb() with
 * off_overlay's pair - only the widgets are duplicated, not the logic. */
static lv_obj_t *btn_update_available_ne = NULL;
static lv_obj_t *lbl_check_status_ne     = NULL;

static lv_obj_t *no_engine_overlay;              /* empty-bus screen */
static lv_obj_t *lbl_no_engine_ip;
static lv_obj_t *settings_win = NULL;
static lv_obj_t *lbl_wireless_pairing = NULL;   /* "Wireless Pairing" button's
    * own label, inside settings_win - live-updated by wireless_pairing_cb()/
    * pairing_mode_tick() rather than only refreshing on dialog reopen. NULL
    * whenever settings_win isn't showing the primary-role rows (or isn't
    * open at all) - always NULL-checked before use. */
static lv_obj_t *lbl_wifi_join_all = NULL;   /* "Join All WiFi" button's result
    * line, inside settings_win - set directly by wifi_join_all_cb() (the
    * whole operation is synchronous, just writing a few bus frames, so
    * unlike the async OTA-check flow there's no tick-based change-guard
    * needed). NULL-checked/reset the same way lbl_wireless_pairing is. */
static lv_obj_t *confirm_win  = NULL;             /* factory-reset confirm,
                                                    * nested on top of settings_win */
static lv_obj_t *autostart_settings_win = NULL;   /* separate second modal,
                                                    * opened from settings_win -
                                                    * see settings_cb()'s
                                                    * comment on why it isn't
                                                    * crammed into settings_win
                                                    * itself */
static lv_obj_t *ta_ssid = NULL, *ta_pass = NULL;  /* setup-dialog wifi fields */
static lv_obj_t *tile_primary = NULL, *tile_secondary = NULL; /* role step */
static bool flash_on = false;

static Preferences prefs;                        /* begun in setup() */
static bool prefs_ok = false;
static bool need_setup = false;   /* first boot / post-factory-reset: NVS
                                    * has no "setup_done" flag yet */

/* 0 = primary (full control: glow/start/ignition/alarm-silence authority),
 * 1 = secondary (read-only monitor: never transmits commands, MUTE only
 * silences its own local flashing). Persisted in NVS ("role"). */
static uint8_t g_display_role = 0;
#define DISPLAY_ROLE_PRIMARY   0
#define DISPLAY_ROLE_SECONDARY 1

#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* "Disable CAN" (Settings) - default off, so a bench/dev unit with no
 * transceiver wired up stops showing the alarming flashing NO LINK
 * indicator. Persisted NVS "can_dis". */
static bool     g_can_disabled          = false;
/* "Wireless Pairing" (Settings) - see the ESP-NOW pairing section further
 * down for what these drive; declared here (not there) because Arduino
 * only auto-generates prototypes for FUNCTIONS, not variables, and
 * settings_cb() above needs these before that section is reached. */
static bool     g_pairing_mode          = false;
static uint32_t g_pairing_mode_start_ms = 0;
#endif

/* setup wizard: step 0 = display role, step 1 = security PIN (both
 * roles), step 2 = WiFi (primary only). Text/values entered on any step
 * are carried here since the widgets themselves get torn down on
 * Next/Back. */
static int setup_step = 0;
static char pending_ssid[64] = "";
static char pending_pass[64] = "";
static uint8_t pending_role = DISPLAY_ROLE_PRIMARY;
static char pending_pin[9] = "";        /* empty = no PIN wanted */
static char wifi_ip_text[64] = "WiFi starting...";

/* ==================== PIN lock ==================== */
/* stored_pin: loaded from NVS at boot ("pin" key, empty = no lock).
 * Same trust model as the WiFi password already stored in NVS - this is
 * a "keep guests from mashing buttons" feature, not a security boundary
 * against a determined attacker with physical access to the board. */
static char stored_pin[9] = "";

/* the numeric pad is a single shared widget reused by 3 flows (setup
 * wizard's PIN step, the boot lock screen, and Settings -> Change PIN) -
 * only one can ever be showing at a time, same single-modal assumption
 * used everywhere else in this file. */
static char pinpad_buf[9] = "";
static lv_obj_t *pinpad_display_lbl = NULL;
static lv_obj_t *pinpad_error_lbl = NULL;
/* raw function-pointer syntax, not a typedef - Arduino's auto-generated
 * prototypes get hoisted above ALL user code (even above typedefs
 * defined earlier in this same file), so a function taking a typedef'd
 * parameter type fails to compile ("has not been declared") the moment
 * the auto-prototype is inserted before the typedef. Raw syntax needs
 * nothing predeclared, so it can't hit that ordering problem. */
static void (*pinpad_on_submit)(const char *entered) = NULL;

/* setup wizard's PIN step: enter, then confirm */
static int  pin_setup_phase = 0;   /* 0 = enter, 1 = confirm */
static char pin_setup_first[9] = "";
static lv_obj_t *pin_setup_sub_lbl = NULL;

/* Settings -> Change PIN: verify old (skipped if none set), enter new,
 * confirm new */
static int  pin_change_phase = 0;
static char pin_change_first[9] = "";
static lv_obj_t *pin_change_sub_lbl = NULL;

static bool link_up(void)
{
    return (millis() - eng.last_telem_ms) < LINK_TIMEOUT_MS &&
           eng.last_telem_ms != 0;
}

/* Set HIDDEN only on CHANGE. LVGL v8's clear_flag(HIDDEN) invalidates
 * the object even when it was already visible - doing that every tick
 * on a full-screen overlay redraws the whole panel 5x/second, and that
 * PSRAM churn destabilizes WiFi under load. */
static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== UI event callbacks ==================== */
/* HELM only up to glow_cb/start_cb/mute_cb below (power switch + settings/
 * engine-picker modals only exist on the 800x480 dashboard) - the gate
 * resumes after those 3, which CYD's buttons reuse directly. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
static void power_sw_cb(lv_event_t *e)
{
    (void)e;
    /* switch is disabled while the engine runs, so this only fires
     * when the user is genuinely allowed to change their mind */
    bool checked = lv_obj_has_state(sw_power, LV_STATE_CHECKED);
    user_power[sel_engine] = checked;
    if (!checked) {
        glow_held  = false;
        start_held = false;
    }
}

static void power_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        user_power[sel_engine] = true;    /* ui_tick hides the overlay + syncs switch */
    }
}

#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

/* glow_cb/start_cb/mute_cb: pure logic, no widget references - reused
 * as-is by both create_ui() (HELM) and create_ui_cyd() (CYD). Both guard
 * on autostart_state == AUTOSTART_IDLE (not just the PRESSED branch) -
 * while autostart owns glow_held/start_held, a stray manual touch on the
 * physical buttons must not be able to release them out from under it
 * (autostart_tick() manages its own transitions and has no way to know
 * a manual RELEASED event just silently cleared the flag it's holding). */
static void glow_cb(lv_event_t *e)
{
    if (autostart_state != AUTOSTART_IDLE) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && g_power && !g_engine_running) {
        glow_held = true;
        glow_press_ms = millis();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        glow_held = false;
    }
}

/* same physical button serves two mutually-exclusive purposes depending
 * on whether the engine is already running: crank (dead-man, masks rpm
 * from the running-determination) while stopped, or - ONLY when the
 * selected engine announced CAP_STOP and this is the primary display -
 * a dead-man stop while running. g_can_stop is cached from ui_tick. */
static void start_cb(lv_event_t *e)
{
    if (autostart_state != AUTOSTART_IDLE) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && g_power) {
        if (!g_engine_running) {
            start_held = true;    /* masks rpm from 'running' until release */
            start_press_ms = millis();
        } else if (g_can_stop) {
            stop_held = true;
            stop_press_ms = millis();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        start_held = false;
        stop_held = false;
    }
}

/* Single-tap toggle: IDLE -> start a fresh run for sel_engine, anything
 * else -> cancel immediately (release both flags, back to IDLE). Mirrors
 * glow_cb/start_cb's own g_power/!g_engine_running gate for starting,
 * since it's bypassing those callbacks entirely and driving the flags
 * directly. */
static void autostart_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (autostart_state != AUTOSTART_IDLE) {
        glow_held  = false;
        start_held = false;
        autostart_state = AUTOSTART_IDLE;
        return;
    }

    if (!g_power || g_engine_running) return;   /* same gate glow_cb/start_cb enforce */

    autostart_engine       = sel_engine;
    autostart_retries_left = as_retries[sel_engine];
    autostart_phase_ms     = millis();
    glow_held      = true;
    glow_press_ms  = millis();
    autostart_state = AUTOSTART_GLOWING;
}

static void mute_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        silenced_mask |= g_active_alarms;   /* ack what's ringing NOW */
        silence_req = true;                 /* loop sends CAN silence */
    }
}

/* HELM only again from here - settings/engine-picker modals, both only
 * ever opened from create_ui()'s cog/engine chip. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480

/* ---- modals: general settings + engine picker (separate controls,
 * separate dialogs; both share the single settings_win modal slot so
 * only one can be open at a time) ---- */

static void settings_close_cb(lv_event_t *e)
{
    (void)e;
    if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
    lbl_wireless_pairing = NULL;   /* deleted along with settings_win above */
    lbl_wifi_join_all    = NULL;   /* ditto */
}

static void engine_pick_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx != sel_engine) {
        sel_engine = idx;
        if (prefs_ok) prefs.putUChar("engine", sel_engine);
        reset_telemetry();
        silenced_mask = 0;
    }
    settings_close_cb(NULL);
}

/* shared by the tap handler (defense in depth) and ui_tick (which drives
 * the chip's actual visibility): how many engines could sel_engine
 * switch to right now. Secondary is further filtered to "on" engines
 * only - it has no ignition authority, so an off engine isn't a valid
 * destination anyway (see engine_picker_cb's per-row filtering below). */
static int count_selectable_engines(void)
{
    bool secondary = (g_display_role == DISPLAY_ROLE_SECONDARY);
    int n = 0;
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        if (engines[i].present && (!secondary || engines[i].ign_on)) n++;
    return n;
}

/* triggered by the ENGINE chip (not the cog) */
static void engine_picker_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || settings_win) return;

    /* defense in depth: the chip itself is hidden by ui_tick whenever
     * this is < 2 (see count_selectable_engines), so this shouldn't
     * normally fire - but don't build a pointless dialog if it does */
    if (count_selectable_engines() < 2) return;
    bool secondary = (g_display_role == DISPLAY_ROLE_SECONDARY);

    settings_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(settings_win, 460, 420);
    lv_obj_center(settings_win);
    lv_obj_set_style_bg_color(settings_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(settings_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(settings_win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(settings_win);
    lv_label_set_text(title, "Select Engine");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        /* secondary can only switch INTO engines that are actually on -
         * it has no ignition authority, so an off engine isn't a valid
         * destination (primary can still browse any present engine) */
        bool selectable = engines[i].present && (!secondary || engines[i].ign_on);

        lv_obj_t *b = lv_btn_create(settings_win);
        lv_obj_set_size(b, 400, 56);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 50 + i * 66);
        lv_obj_set_style_bg_color(b,
            (i == sel_engine) ? lv_color_hex(0x1c5c40)
                              : lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(b, engine_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        if (!selectable && i != sel_engine)
            lv_obj_add_state(b, LV_STATE_DISABLED);

        lv_obj_t *l = lv_label_create(b);
        if (!engines[i].present) {
            lv_label_set_text_fmt(l, "Engine %d: not detected%s", i,
                (i == sel_engine) ? "  (selected)" : "");
        } else if (secondary && !engines[i].ign_on) {
            lv_label_set_text_fmt(l, "Engine %d: %s  (off)%s", i,
                engine_display_name(i),
                (i == sel_engine) ? "  (selected)" : "");
        } else {
            lv_label_set_text_fmt(l, "Engine %d: %s%s", i,
                engine_display_name(i),
                (i == sel_engine) ? "  (selected)" : "");
        }
        lv_obj_center(l);
    }

    lv_obj_t *cb = lv_btn_create(settings_win);
    lv_obj_set_size(cb, 400, 50);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(cb, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
}

/* one-shot: fires ~700ms after being armed, then reboots. Used after any
 * NVS change that wifi_setup()/setup() need to re-read from scratch,
 * since we never touch WiFi APIs ourselves - a clean reboot re-runs the
 * exact same proven boot path instead. */
static void restart_now_cb(lv_timer_t *t)
{
    (void)t;
    ESP.restart();
}

static void arm_restart(void)
{
    lv_timer_t *rt = lv_timer_create(restart_now_cb, 700, NULL);
    lv_timer_set_repeat_count(rt, 1);
}

/* ---- factory reset: nested confirm dialog on top of settings_win ---- */

static void factory_reset_confirm_close_cb(lv_event_t *e)
{
    (void)e;
    if (confirm_win) { lv_obj_del(confirm_win); confirm_win = NULL; }
}

static void factory_reset_do_cb(lv_event_t *e)
{
    (void)e;
    if (prefs_ok) prefs.clear();   /* wipes wifi creds, engine, setup_done */

    if (confirm_win) { lv_obj_del(confirm_win); confirm_win = NULL; }
    if (settings_win) lv_obj_clean(settings_win);
    lv_obj_t *msg = lv_label_create(settings_win);
    lv_label_set_text(msg, "Factory reset.\nRestarting...");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
    lv_obj_center(msg);
    arm_restart();
}

static void factory_reset_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || confirm_win) return;

    confirm_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(confirm_win, 380, 220);
    lv_obj_center(confirm_win);
    lv_obj_set_style_bg_color(confirm_win, lv_color_hex(0x241010), 0);
    lv_obj_set_style_border_color(confirm_win, lv_color_hex(0x662222), 0);
    lv_obj_clear_flag(confirm_win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(confirm_win);
    lv_label_set_text(title, "Factory Reset?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xff6666), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *msg = lv_label_create(confirm_win);
    lv_label_set_text(msg, "Clears WiFi credentials, engine selection,\nand PIN, then restarts the panel.");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xd8b8b8), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *cancel = lv_btn_create(confirm_win);
    lv_obj_set_size(cancel, 160, 50);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(cancel, factory_reset_confirm_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "Cancel");
    lv_obj_center(cancel_l);

    lv_obj_t *reset = lv_btn_create(confirm_win);
    lv_obj_set_size(reset, 160, 50);
    lv_obj_align(reset, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0xaa2222), 0);
    lv_obj_add_event_cb(reset, factory_reset_do_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reset_l = lv_label_create(reset);
    lv_label_set_text(reset_l, "Reset");
    lv_obj_center(reset_l);
}

static void pin_change_cb(lv_event_t *e);   /* defined further down, with the rest of the PIN flows */

static void can_disable_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    /* switch is "CAN Enabled" (positive sense) - g_can_disabled stays
     * the inverted internal flag everything else reads */
    g_can_disabled = !lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (prefs_ok) prefs.putBool("can_dis", g_can_disabled);
    Serial.printf("Settings: CAN %s (bus_send will now use %s for the broadcast bucket)\n",
        g_can_disabled ? "disabled" : "enabled",
        g_can_disabled ? "ESP-NOW only" : "CAN when available");
}

/* opens the pairing window - see pairing_mode_tick()/espnow_on_recv()
 * further down (ESP-NOW pairing section). Re-arming pairing mode is the
 * only "specially told to" trigger; an already-paired peer never
 * re-broadcasts on its own. */
/* shown on the Settings button that used to open a pairing window. There is no window now: a board joins
 * by itself whenever it holds the same shared secret (fleet_security.h). */
static const char *espnow_key_label(void)
{
    return g_fsec_have_key ? "ESP-NOW: key set" : "ESP-NOW: NO KEY (set it over serial)";
}

static void wireless_pairing_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    Serial.printf("ESP-NOW: %s\n", g_fsec_have_key ? "key is set; boards join on their own" : "no key set - type KEY <passphrase> over serial");
    /* label itself is updated from ui_tick()'s g_pairing_mode change-guard,
     * not here - g_pairing_mode can also flip true from the web debug
     * page's /esppair handler, so a single change-guarded spot on the
     * LVGL task covers every trigger source instead of duplicating this
     * update per-trigger */
}

/* "Join All WiFi" (Settings) - touchscreen equivalent of the web debug
 * page's identical button (handle_wifi_join_all()) - shares the actual
 * work via wifi_join_all_devices(), defined further down alongside
 * wifi_join_send(). Reaches every present engine over WHICHEVER
 * transport it's actually on (CAN, ESP-NOW, or the direct-wire link) -
 * bus_send()'s existing per-engine routing handles that automatically,
 * nothing here needs to know which transport won. Synchronous (just
 * writing a handful of bus frames), so the result can be shown
 * immediately rather than needing a tick-based change-guard the way the
 * async OTA-check flow does. */
static void wifi_join_all_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!lbl_wifi_join_all) return;
    char buf[64];
    wifi_join_all_devices(buf, sizeof(buf));
    lv_label_set_text(lbl_wifi_join_all, buf);
}

/* ==================== Autostart Settings (per-engine stepper rows) ====================
 * No slider/spinbox/stepper widget exists anywhere else in this file -
 * every other Settings control is a switch or a full-width button. This
 * is the one new small reusable widget this feature needs: a label
 * showing "<name>: <value><unit>" plus "-"/"+" buttons that adjust the
 * value in place, clamped to [vmin,vmax], with an immediate NVS write on
 * every tap (matches can_disable_sw_cb's immediate-persist convention). */
/* Tagged struct (NOT `typedef struct {...} stepper_ctx_t;`) - Arduino's
 * auto-generated prototypes get hoisted above ALL user code including
 * typedefs defined earlier in this same file, so a bare-typedef parameter
 * type breaks ("not declared in this scope") unless every signature
 * spells out the elaborated `struct stepper_ctx_t` form instead. Same
 * fix already used for sim_engine_t/enroll_slot_t elsewhere in this
 * project - see those comments for the full explanation. */
struct stepper_ctx_t {
    uint8_t  *val;
    uint8_t   vmin, vmax;
    lv_obj_t *value_lbl;
    const char *label_text;
    const char *unit;
    uint8_t   engine;
    void (*persist)(uint8_t engine, uint8_t v);
};

/* fixed 4-slot pool, reused every time the sub-modal is (re)opened - the
 * widgets themselves are torn down with the modal, but the context
 * structs backing their callbacks need storage that outlives one
 * settings_cb() call, so static rather than stack-local. */
static struct stepper_ctx_t g_stepper_ctx[4];

static void stepper_refresh_label(struct stepper_ctx_t *ctx)
{
    lv_label_set_text_fmt(ctx->value_lbl, "%s: %d%s", ctx->label_text, *ctx->val, ctx->unit);
}

static void stepper_minus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    struct stepper_ctx_t *ctx = (struct stepper_ctx_t *)lv_event_get_user_data(e);
    if (*ctx->val > ctx->vmin) (*ctx->val)--;
    stepper_refresh_label(ctx);
    if (ctx->persist) ctx->persist(ctx->engine, *ctx->val);
}

static void stepper_plus_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    struct stepper_ctx_t *ctx = (struct stepper_ctx_t *)lv_event_get_user_data(e);
    if (*ctx->val < ctx->vmax) (*ctx->val)++;
    stepper_refresh_label(ctx);
    if (ctx->persist) ctx->persist(ctx->engine, *ctx->val);
}

static void build_stepper_row(lv_obj_t *parent, lv_coord_t y, int ctx_idx,
                               const char *label_text, uint8_t *val, uint8_t engine,
                               uint8_t vmin, uint8_t vmax, const char *unit,
                               void (*persist)(uint8_t, uint8_t))
{
    struct stepper_ctx_t *ctx = &g_stepper_ctx[ctx_idx];
    ctx->val = val; ctx->vmin = vmin; ctx->vmax = vmax;
    ctx->label_text = label_text; ctx->unit = unit;
    ctx->engine = engine; ctx->persist = persist;

    ctx->value_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(ctx->value_lbl, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_pos(ctx->value_lbl, 16, y + 12);

    lv_obj_t *minus = lv_btn_create(parent);
    lv_obj_set_size(minus, 50, 40);
    lv_obj_set_pos(minus, 230, y);
    lv_obj_set_style_bg_color(minus, lv_color_hex(0x1c3450), 0);
    lv_obj_add_event_cb(minus, stepper_minus_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *ml = lv_label_create(minus);
    lv_label_set_text(ml, "-");
    lv_obj_center(ml);

    lv_obj_t *plus = lv_btn_create(parent);
    lv_obj_set_size(plus, 50, 40);
    lv_obj_set_pos(plus, 290, y);
    lv_obj_set_style_bg_color(plus, lv_color_hex(0x1c3450), 0);
    lv_obj_add_event_cb(plus, stepper_plus_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_t *pl = lv_label_create(plus);
    lv_label_set_text(pl, "+");
    lv_obj_center(pl);

    stepper_refresh_label(ctx);
}

static void as_persist_glow(uint8_t engine, uint8_t v)
{ char k[8]; snprintf(k, sizeof(k), "asG%d", engine); if (prefs_ok) prefs.putUChar(k, v); }
static void as_persist_crank(uint8_t engine, uint8_t v)
{ char k[8]; snprintf(k, sizeof(k), "asC%d", engine); if (prefs_ok) prefs.putUChar(k, v); }
static void as_persist_wait(uint8_t engine, uint8_t v)
{ char k[8]; snprintf(k, sizeof(k), "asW%d", engine); if (prefs_ok) prefs.putUChar(k, v); }
static void as_persist_retries(uint8_t engine, uint8_t v)
{ char k[8]; snprintf(k, sizeof(k), "asR%d", engine); if (prefs_ok) prefs.putUChar(k, v); }

static void autostart_settings_close_cb(lv_event_t *e)
{
    (void)e;
    if (autostart_settings_win) { lv_obj_del(autostart_settings_win); autostart_settings_win = NULL; }
}

/* opens as a second, separate modal (not folded into settings_win itself -
 * that window is already 420x456 on a 480-tall screen, no room left for 4
 * more rows). Always reflects sel_engine at the moment it's opened - if
 * the user switches engines while this is open the values shown go stale
 * until they close/reopen it, a minor accepted rough edge. */
static void open_autostart_settings_cb(lv_event_t *e)
{
    (void)e;
    if (autostart_settings_win) return;

    uint8_t engine = sel_engine;

    autostart_settings_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(autostart_settings_win, 440, 340);
    lv_obj_center(autostart_settings_win);
    lv_obj_set_style_bg_color(autostart_settings_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(autostart_settings_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(autostart_settings_win, LV_OBJ_FLAG_SCROLLABLE);
    /* same default-theme-padding fix as settings_win - see its comment */
    lv_obj_set_style_pad_all(autostart_settings_win, 0, 0);

    /* "Autostart: <engine name>" was wrapping to a second line at
     * montserrat_28/350px - "(unnamed engine)" alone is 17 chars, a real
     * typed name (up to MD_ENGINE_NAME_MAXLEN=24) plus the "Autostart: "
     * prefix can run to 35 - no single font/width combo at 28pt was
     * ever going to fit that comfortably. Dropped to montserrat_20 and
     * widened both the window and the label (the stepper rows' +/-
     * buttons are absolute-positioned well inside 380px already, so
     * widening just adds slack on the right, nothing to reflow). LONG_DOT
     * stays on as a safety net for anything longer still. */
    lv_obj_t *title = lv_label_create(autostart_settings_win);
    lv_label_set_text_fmt(title, "Autostart: %s",
        engines[engine].name[0] ? engines[engine].name : "(unnamed engine)");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 410);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    build_stepper_row(autostart_settings_win, 50, 0, "Glow", &as_glow_s[engine], engine,
                       0, 60, "s", as_persist_glow);
    build_stepper_row(autostart_settings_win, 105, 1, "Crank", &as_crank_s[engine], engine,
                       1, CRANK_MAX_MS / 1000, "s", as_persist_crank);
    build_stepper_row(autostart_settings_win, 160, 2, "Wait between retries", &as_wait_s[engine], engine,
                       0, 60, "s", as_persist_wait);
    build_stepper_row(autostart_settings_win, 215, 3, "Retries", &as_retries[engine], engine,
                       0, 10, "", as_persist_retries);

    lv_obj_t *cb = lv_btn_create(autostart_settings_win);
    lv_obj_set_size(cb, 400, 50);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(cb, autostart_settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
}

/* triggered by the cog. Primary gets status + Change PIN + Factory
 * Reset (this is where the WiFi credentials page (roadmap) will live).
 * Secondary is stripped down to Factory Reset only - no WiFi/engine/PIN
 * settings of its own (it doesn't wake up until an engine is switched
 * on, so there's nothing on it for a PIN to protect either), and this
 * cog only exists once an engine is on. */
static void settings_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || settings_win) return;

    bool primary = (g_display_role == DISPLAY_ROLE_PRIMARY);

    /* button height 42 (was 50) + tighter row spacing - the previous
     * primary-role layout was 472px tall against a 480px-tall screen,
     * leaving only ~4px margin top/bottom on paper; on real hardware
     * that was cutting Factory Reset/Close off almost entirely. This
     * layout tops out at 400px, leaving real breathing room. */
    const lv_coord_t SETTINGS_BTN_H = 42;
    settings_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(settings_win, 420, primary ? 450 : 170);
    lv_obj_center(settings_win);
    lv_obj_set_style_bg_color(settings_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(settings_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(settings_win, LV_OBJ_FLAG_SCROLLABLE);
    /* default-theme-padding fix - same one off_overlay/no_engine_overlay
     * already needed (see their own comments). This window never had it,
     * masked by the old layout's generous slack; the tighter row spacing
     * above finally pushed it into visibly overlapping Factory Reset
     * with Close (FR reduced to a barely-visible sliver behind Close,
     * which renders on top since it's created later). */
    lv_obj_set_style_pad_all(settings_win, 0, 0);

    lv_obj_t *title = lv_label_create(settings_win);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_coord_t fr_y = 36;
    if (primary) {
        lv_obj_t *wifi_l = lv_label_create(settings_win);
        lv_label_set_text(wifi_l, wifi_ip_text);
        lv_obj_set_style_text_color(wifi_l, lv_color_hex(0x8fa3b8), 0);
        lv_obj_align(wifi_l, LV_ALIGN_TOP_LEFT, 16, 44);

        lv_obj_t *can_l = lv_label_create(settings_win);
        if (g_can_disabled) {
            lv_label_set_text(can_l, "CAN bus: disabled");
            lv_obj_set_style_text_color(can_l, lv_color_hex(0x8fa3b8), 0);
        } else {
            lv_label_set_text_fmt(can_l, "CAN bus: %s", link_up() ? "LINK" : "NO LINK");
            lv_obj_set_style_text_color(can_l,
                link_up() ? lv_color_hex(0x00cc66) : lv_color_hex(0xff2222), 0);
        }
        lv_obj_align(can_l, LV_ALIGN_TOP_LEFT, 16, 66);

        lv_obj_t *pin = lv_btn_create(settings_win);
        lv_obj_set_size(pin, 380, SETTINGS_BTN_H);
        lv_obj_align(pin, LV_ALIGN_TOP_MID, 0, 96);
        lv_obj_set_style_bg_color(pin, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(pin, pin_change_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *pin_l = lv_label_create(pin);
        lv_label_set_text(pin_l, stored_pin[0] ? "Change PIN" : "Set PIN");
        lv_obj_center(pin_l);

        /* "CAN Enabled" (positive sense, checked = normal operation) -
         * internally still g_can_disabled (inverted) - for a bench/dev
         * unit with no transceiver wired up, unchecking stops the
         * alarming flashing NO LINK indicator above and skips TWAI init
         * entirely (see setup()). Default checked (CAN enabled). */
        lv_obj_t *can_dis_l = lv_label_create(settings_win);
        lv_label_set_text(can_dis_l, "CAN Enabled");
        lv_obj_set_style_text_color(can_dis_l, lv_color_hex(0xeeeeee), 0);
        lv_obj_align(can_dis_l, LV_ALIGN_TOP_LEFT, 16, 150);

        lv_obj_t *can_dis_sw = lv_switch_create(settings_win);
        lv_obj_set_size(can_dis_sw, 90, 38);
        lv_obj_align(can_dis_sw, LV_ALIGN_TOP_RIGHT, -16, 144);
        if (!g_can_disabled) lv_obj_add_state(can_dis_sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(can_dis_sw, can_disable_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

        /* Wireless Pairing: user-facing name for the ESP-NOW pairing
         * handshake - opens a 60s window during which a new (never
         * before paired) module can be accepted. See espnow_on_recv()/
         * pairing_mode_tick() further down. */
        lv_obj_t *pair = lv_btn_create(settings_win);
        lv_obj_set_size(pair, 380, SETTINGS_BTN_H);
        lv_obj_align(pair, LV_ALIGN_TOP_MID, 0, 194);
        lv_obj_set_style_bg_color(pair, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(pair, wireless_pairing_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *pair_l = lv_label_create(pair);
        lv_label_set_text(pair_l, espnow_key_label());
        lv_obj_center(pair_l);
        lbl_wireless_pairing = pair_l;

        /* opens the separate Autostart Settings modal (see its own
         * open_autostart_settings_cb() comment for why it's a second
         * modal rather than more rows crammed in here). */
        lv_obj_t *as_btn = lv_btn_create(settings_win);
        lv_obj_set_size(as_btn, 380, SETTINGS_BTN_H);
        lv_obj_align(as_btn, LV_ALIGN_TOP_MID, 0, 244);
        lv_obj_set_style_bg_color(as_btn, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(as_btn, open_autostart_settings_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *as_l = lv_label_create(as_btn);
        lv_label_set_text(as_l, "Autostart Settings");
        lv_obj_center(as_l);

        /* "Join All WiFi": tells every currently-present engine (over
         * whichever transport it's actually on - CAN, ESP-NOW, or the
         * direct-wire link) to join HELM's own WiFi network and persist
         * that join. Touchscreen equivalent of the web debug page's
         * identical button - see wifi_join_all_cb()/wifi_join_all_
         * devices()'s comments for the shared implementation. Button +
         * result share one row (button left half, result label right
         * half) rather than stacking, to keep this dialog's total height
         * from creeping back toward the ~472px that used to cut Factory
         * Reset/Close off on real hardware (see this function's own
         * comment on SETTINGS_BTN_H) - a short result string ("Sent to
         * N device(s)") fits the narrower half fine. */
        lv_obj_t *wja = lv_btn_create(settings_win);
        lv_obj_set_size(wja, 180, SETTINGS_BTN_H);
        lv_obj_align(wja, LV_ALIGN_TOP_LEFT, 16, 294);
        lv_obj_set_style_bg_color(wja, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(wja, wifi_join_all_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *wja_l = lv_label_create(wja);
        lv_label_set_text(wja_l, "Join All WiFi");
        lv_obj_center(wja_l);

        lv_obj_t *wja_result = lv_label_create(settings_win);
        lv_label_set_text(wja_result, "");
        lv_obj_set_width(wja_result, 184);
        lv_label_set_long_mode(wja_result, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(wja_result, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(wja_result, lv_color_hex(0x8fa3b8), 0);
        lv_obj_align(wja_result, LV_ALIGN_TOP_RIGHT, -16, 296);
        lbl_wifi_join_all = wja_result;

        fr_y = 344;
    }

    lv_obj_t *fr = lv_btn_create(settings_win);
    lv_obj_set_size(fr, 380, SETTINGS_BTN_H);
    lv_obj_align(fr, LV_ALIGN_TOP_MID, 0, fr_y);
    lv_obj_set_style_bg_color(fr, lv_color_hex(0x4a1c1c), 0);
    lv_obj_add_event_cb(fr, factory_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fr_l = lv_label_create(fr);
    lv_label_set_text(fr_l, "Factory Reset");
    lv_obj_set_style_text_color(fr_l, lv_color_hex(0xff9999), 0);
    lv_obj_center(fr_l);

    lv_obj_t *cb = lv_btn_create(settings_win);
    lv_obj_set_size(cb, 380, SETTINGS_BTN_H);
    lv_obj_align(cb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(cb, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(cb, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
}
#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

/* ==================== numeric PIN pad (shared widget) ====================
 * HELM only from here through build_setup_dialog() below - the PIN pad,
 * boot lock screen, Settings -> Change PIN, and setup wizard are all
 * 800x480-hardcoded and CYD is unconditionally DISPLAY_ROLE_SECONDARY
 * with need_setup=false (see setup()), so none of this is ever reached
 * on a CYD build - gated out here so it doesn't bloat the binary either. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* Reused by 3 flows: the setup wizard's PIN step, the boot lock screen,
 * and Settings -> Change PIN. Caller builds its own title/subtitle/nav
 * buttons in `parent`; this just adds the digit display, an (initially
 * hidden) error label, and a 3x4 keypad, and calls back via
 * pinpad_on_submit(pinpad_buf) when OK is pressed. */

#define PINPAD_MAXLEN   (sizeof(pinpad_buf) - 1)

static void pinpad_refresh_display(void)
{
    size_t n = strlen(pinpad_buf);
    static char masked[PINPAD_MAXLEN + 1];
    for (size_t i = 0; i < n; i++) masked[i] = '*';
    masked[n] = 0;
    lv_label_set_text(pinpad_display_lbl, n ? masked : "");
}

static void pinpad_reset(void)
{
    pinpad_buf[0] = 0;
    pinpad_refresh_display();
}

static void pinpad_show_error(const char *msg)
{
    lv_label_set_text(pinpad_error_lbl, msg);
}

static void pinpad_digit_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char c = (char)(intptr_t)lv_event_get_user_data(e);
    size_t n = strlen(pinpad_buf);
    if (n < PINPAD_MAXLEN) {
        pinpad_buf[n] = c;
        pinpad_buf[n + 1] = 0;
        pinpad_refresh_display();
    }
}

static void pinpad_backspace_cb(lv_event_t *e)
{
    (void)e;
    size_t n = strlen(pinpad_buf);
    if (n) pinpad_buf[n - 1] = 0;
    pinpad_refresh_display();
}

static void pinpad_enter_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text(pinpad_error_lbl, "");
    if (pinpad_on_submit) pinpad_on_submit(pinpad_buf);
}

#define PINPAD_BTN_W   140
#define PINPAD_BTN_H   56
#define PINPAD_GAP     12
#define PINPAD_GRID_W  (3 * PINPAD_BTN_W + 2 * PINPAD_GAP)
#define PINPAD_GRID_X0 ((800 - PINPAD_GRID_W) / 2)

/* y0: top of the digit display. Grid/error label follow at fixed
 * offsets below it, ending well clear of the screen bottom. */
static void build_pin_pad(lv_obj_t *parent, lv_coord_t y0, void (*on_submit)(const char *entered))
{
    pinpad_buf[0] = 0;
    pinpad_on_submit = on_submit;

    pinpad_display_lbl = lv_label_create(parent);
    lv_label_set_text(pinpad_display_lbl, "");
    lv_obj_set_style_text_color(pinpad_display_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(pinpad_display_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(pinpad_display_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pinpad_display_lbl, 400);
    lv_obj_align(pinpad_display_lbl, LV_ALIGN_TOP_MID, 0, y0);

    pinpad_error_lbl = lv_label_create(parent);
    lv_label_set_text(pinpad_error_lbl, "");
    lv_obj_set_style_text_color(pinpad_error_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_style_text_align(pinpad_error_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(pinpad_error_lbl, 500);
    lv_obj_align(pinpad_error_lbl, LV_ALIGN_TOP_MID, 0, y0 + 48);

    static const char keys[12] = { '1','2','3','4','5','6','7','8','9','\b','0','\n' };
    lv_coord_t grid_y0 = y0 + 80;   /* leaves room below for Back/Skip/Cancel at y~424 */
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, PINPAD_BTN_W, PINPAD_BTN_H);
        lv_obj_set_pos(b, PINPAD_GRID_X0 + col * (PINPAD_BTN_W + PINPAD_GAP),
                          grid_y0 + row * (PINPAD_BTN_H + PINPAD_GAP));

        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_center(l);

        char k = keys[i];
        if (k == '\b') {
            lv_label_set_text(l, LV_SYMBOL_BACKSPACE);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x33475c), 0);
            lv_obj_add_event_cb(b, pinpad_backspace_cb, LV_EVENT_CLICKED, NULL);
        } else if (k == '\n') {
            lv_label_set_text(l, LV_SYMBOL_OK);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x1c5c40), 0);
            lv_obj_add_event_cb(b, pinpad_enter_cb, LV_EVENT_CLICKED, NULL);
        } else {
            lv_label_set_text_fmt(l, "%c", k);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x1c3450), 0);
            lv_obj_add_event_cb(b, pinpad_digit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        }
    }
}

/* ==================== boot lock screen ====================
 * Built once at boot (from setup(), same spot need_setup's wizard is
 * built) if stored_pin[0] is set. Full-screen, on lv_layer_top() like
 * every other modal here, so it sits above the main dashboard AND
 * off_overlay/no_engine_overlay regardless of which of those would
 * otherwise be showing. Unlocks for the rest of this boot session only -
 * it does not re-lock until the next power cycle. */

static void lock_submit(const char *entered)
{
    if (strcmp(entered, stored_pin) == 0) {
        if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
        return;
    }
    pinpad_show_error("Incorrect PIN");
    pinpad_reset();
}

static void build_lock_overlay(void)
{
    settings_win = setup_screen_base("Panel Locked");

    lv_obj_t *sub = lv_label_create(settings_win);
    lv_label_set_text(sub, "Enter PIN to continue");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7d92a6), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 42);

    build_pin_pad(settings_win, 70, lock_submit);
}

/* ==================== Settings -> Change PIN ==================== */
/* Primary only - see settings_cb, which doesn't even show this button on
 * a secondary's (reduced) Settings dialog. If no PIN is currently set,
 * skips straight to "enter new" (nothing to verify). */

static void pin_change_submit(const char *entered)
{
    if (pin_change_phase == 0) {
        if (strcmp(entered, stored_pin) != 0) {
            pinpad_show_error("Incorrect PIN");
            pinpad_reset();
            return;
        }
        pin_change_phase = 1;
        lv_label_set_text(pin_change_sub_lbl, "Enter new PIN (blank to remove)");
        pinpad_reset();
        return;
    }

    if (pin_change_phase == 1) {
        if (entered[0] == 0) {
            stored_pin[0] = 0;
            if (prefs_ok) prefs.putString("pin", "");
            if (settings_win) lv_obj_clean(settings_win);
            lv_obj_t *msg = lv_label_create(settings_win);
            lv_label_set_text(msg, "PIN removed.");
            lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
            lv_obj_align(msg, LV_ALIGN_CENTER, 0, -30);
            lv_obj_t *cb = lv_btn_create(settings_win);
            lv_obj_set_size(cb, 200, 50);
            lv_obj_align(cb, LV_ALIGN_CENTER, 0, 40);
            lv_obj_set_style_bg_color(cb, lv_color_hex(0x33475c), 0);
            lv_obj_add_event_cb(cb, settings_close_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *cl = lv_label_create(cb);
            lv_label_set_text(cl, "Close");
            lv_obj_center(cl);
            return;
        }
        if (strlen(entered) < 4) {
            pinpad_show_error("PIN must be at least 4 digits");
            pinpad_reset();
            return;
        }
        strncpy(pin_change_first, entered, sizeof(pin_change_first) - 1);
        pin_change_phase = 2;
        lv_label_set_text(pin_change_sub_lbl, "Confirm new PIN");
        pinpad_reset();
        return;
    }

    /* phase 2: confirm */
    if (strcmp(entered, pin_change_first) == 0) {
        strncpy(stored_pin, entered, sizeof(stored_pin) - 1);
        if (prefs_ok) prefs.putString("pin", stored_pin);
        if (settings_win) lv_obj_clean(settings_win);
        lv_obj_t *msg = lv_label_create(settings_win);
        lv_label_set_text(msg, "PIN updated.");
        lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, -30);
        lv_obj_t *cb = lv_btn_create(settings_win);
        lv_obj_set_size(cb, 200, 50);
        lv_obj_align(cb, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_bg_color(cb, lv_color_hex(0x33475c), 0);
        lv_obj_add_event_cb(cb, settings_close_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cl = lv_label_create(cb);
        lv_label_set_text(cl, "Close");
        lv_obj_center(cl);
    } else {
        pinpad_show_error("PINs didn't match");
        pin_change_phase = 1;
        pin_change_first[0] = 0;
        lv_label_set_text(pin_change_sub_lbl, "Enter new PIN (blank to remove)");
        pinpad_reset();
    }
}

static void build_pin_change_screen(void)
{
    settings_win = setup_screen_base(stored_pin[0] ? "Change PIN" : "Set PIN");
    pin_change_phase = stored_pin[0] ? 0 : 1;
    pin_change_first[0] = 0;

    pin_change_sub_lbl = lv_label_create(settings_win);
    lv_label_set_text(pin_change_sub_lbl,
        pin_change_phase == 0 ? "Enter current PIN" : "Enter new PIN (blank to remove)");
    lv_obj_set_style_text_color(pin_change_sub_lbl, lv_color_hex(0x7d92a6), 0);
    lv_obj_align(pin_change_sub_lbl, LV_ALIGN_TOP_MID, 0, 42);

    build_pin_pad(settings_win, 70, pin_change_submit);

    lv_obj_t *cancel = lv_btn_create(settings_win);
    lv_obj_set_size(cancel, 140, 44);
    lv_obj_set_pos(cancel, 40, 424);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(cancel, settings_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "Cancel");
    lv_obj_center(cancel_l);
}

/* triggered by the "Change PIN"/"Set PIN" button inside the Settings
 * dialog - replaces settings_win's content rather than guarding on it
 * already being open (it's ALWAYS already open when this fires). */
static void pin_change_cb(lv_event_t *e)
{
    (void)e;
    if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
    build_pin_change_screen();
}

/* ---- initial setup wizard: shown automatically on first boot (or after
 * a factory reset) until "setup_done" is set in NVS. Only ever persists
 * strings + flags to NVS and reboots - never touches WiFi.* itself, so
 * the proven wifi_setup() boot path in setup() is what actually connects.
 * Step 0: primary vs secondary display role, asked FIRST because it
 * decides whether steps 1-2 even happen - a secondary finishes the
 * wizard immediately after choosing its role. Step 1: security PIN,
 * PRIMARY ONLY (see the note on build_lock_overlay's call site for why -
 * short version: a secondary panel doesn't wake up until an engine is
 * switched on, so there's nothing on it for a PIN to protect). Step 2:
 * WiFi, also primary only. Typed values are carried in
 * pending_ssid/pending_pass/pending_role/pending_pin across steps since
 * the widgets get torn down on Next/Back. */

static void build_setup_dialog(void);   /* dispatches on setup_step */

static void setup_ta_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) return;
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, ta);
}

static void role_tile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    pending_role = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_set_style_bg_color(tile_primary,
        pending_role == DISPLAY_ROLE_PRIMARY ? lv_color_hex(0x1c5c40)
                                              : lv_color_hex(0x1c3450), 0);
    lv_obj_set_style_bg_color(tile_secondary,
        pending_role == DISPLAY_ROLE_SECONDARY ? lv_color_hex(0x1c5c40)
                                                : lv_color_hex(0x1c3450), 0);
}

/* writes NVS + reboots. Reused directly as the click handler for both
 * "Skip WiFi" (fields untouched) and the role step's secondary path -
 * and via setup_wifi_finish_cb below once fields have been captured. */
static void setup_finish_cb(lv_event_t *e)
{
    (void)e;
    if (prefs_ok) {
        prefs.putString("ssid", pending_ssid);
        prefs.putString("pass", pending_pass);
        prefs.putUChar("role", pending_role);
        prefs.putString("pin", pending_pin);
        prefs.putBool("setup_done", true);
    }
    if (settings_win) lv_obj_clean(settings_win);
    lv_obj_t *msg = lv_label_create(settings_win);
    lv_label_set_text(msg, "Saved.\nRestarting...");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xffffff), 0);
    lv_obj_center(msg);
    arm_restart();
}

/* role step's Next: secondary finishes right here - the PIN step is
 * primary-only too (secondary displays don't even wake up until an
 * engine is switched on, so there's nothing for a PIN to protect).
 * Primary goes on to the PIN step. */
static void role_next_cb(lv_event_t *e)
{
    if (pending_role == DISPLAY_ROLE_SECONDARY) {
        setup_finish_cb(e);
        return;
    }
    setup_step = 1;
    if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
    build_setup_dialog();
}

/* PIN step's Next/Skip: only ever reached by primary (see role_next_cb),
 * so this always goes on to the WiFi step */
static void pin_step_advance(void)
{
    setup_step = 2;
    if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
    build_setup_dialog();
}

/* generic Back: target step comes in via user_data (0=role, 1=pin) */
static void setup_back_cb(lv_event_t *e)
{
    setup_step = (int)(intptr_t)lv_event_get_user_data(e);
    if (settings_win) { lv_obj_del(settings_win); settings_win = NULL; }
    build_setup_dialog();
}

/* wifi step's Save & Restart: captures the typed fields, then finishes
 * exactly like setup_finish_cb (only reachable when role == primary) */
static void setup_wifi_finish_cb(lv_event_t *e)
{
    strncpy(pending_ssid, lv_textarea_get_text(ta_ssid), sizeof(pending_ssid) - 1);
    pending_ssid[sizeof(pending_ssid) - 1] = 0;
    strncpy(pending_pass, lv_textarea_get_text(ta_pass), sizeof(pending_pass) - 1);
    pending_pass[sizeof(pending_pass) - 1] = 0;
    setup_finish_cb(e);
}

static lv_obj_t *setup_screen_base(const char *title_text)
{
    lv_obj_t *scr = lv_obj_create(lv_layer_top());
    lv_obj_set_size(scr, 800, 480);
    lv_obj_set_pos(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a1929), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    return scr;
}

static void build_setup_step_wifi(void)
{
    settings_win = setup_screen_base("Initial Setup - Step 3: WiFi (primary only)");

    lv_obj_t *sub = lv_label_create(settings_win);
    lv_label_set_text(sub, "Connect the panel to WiFi (used for the debug page).");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7d92a6), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 42);

    /* pending_ssid/pass are the source of truth once the user has typed
     * anything; seed them from NVS only the first time (buffer empty) */
    if (!pending_ssid[0] && prefs_ok) {
        String cur = prefs.getString("ssid", "");
        strncpy(pending_ssid, cur.c_str(), sizeof(pending_ssid) - 1);
    }
    if (!pending_pass[0] && prefs_ok) {
        String cur = prefs.getString("pass", "");
        strncpy(pending_pass, cur.c_str(), sizeof(pending_pass) - 1);
    }

    lv_obj_t *lbl_ssid = lv_label_create(settings_win);
    lv_label_set_text(lbl_ssid, "Network (SSID)");
    lv_obj_set_style_text_color(lbl_ssid, lv_color_hex(0x8fa3b8), 0);
    lv_obj_set_pos(lbl_ssid, 150, 70);

    ta_ssid = lv_textarea_create(settings_win);
    lv_textarea_set_one_line(ta_ssid, true);
    lv_textarea_set_placeholder_text(ta_ssid, "network name");
    lv_obj_set_size(ta_ssid, 440, 44);
    lv_obj_set_pos(ta_ssid, 150, 92);
    if (pending_ssid[0]) lv_textarea_set_text(ta_ssid, pending_ssid);

    /* Save sits beside the SSID field, not under it - the on-screen
     * keyboard docks at the bottom and would otherwise cover it */
    lv_obj_t *save = lv_btn_create(settings_win);
    lv_obj_set_size(save, 170, 44);
    lv_obj_set_pos(save, 610, 92);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x1c5c40), 0);
    lv_obj_add_event_cb(save, setup_wifi_finish_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_l = lv_label_create(save);
    lv_label_set_text(save_l, "Save & Restart");
    lv_obj_center(save_l);

    lv_obj_t *lbl_pass = lv_label_create(settings_win);
    lv_label_set_text(lbl_pass, "Password");
    lv_obj_set_style_text_color(lbl_pass, lv_color_hex(0x8fa3b8), 0);
    lv_obj_set_pos(lbl_pass, 150, 144);

    ta_pass = lv_textarea_create(settings_win);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_set_size(ta_pass, 440, 44);
    lv_obj_set_pos(ta_pass, 150, 166);
    if (pending_pass[0]) lv_textarea_set_text(ta_pass, pending_pass);

    /* skip: finishes immediately without touching whatever's in the
     * fields (setup_finish_cb just persists pending_ssid/pass as-is) */
    lv_obj_t *skip = lv_btn_create(settings_win);
    lv_obj_set_size(skip, 170, 44);
    lv_obj_set_pos(skip, 610, 166);
    lv_obj_set_style_bg_color(skip, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(skip, setup_finish_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *skip_l = lv_label_create(skip);
    lv_label_set_text(skip_l, "Skip WiFi");
    lv_obj_center(skip_l);

    /* NOT bottom-left - the keyboard docks at the bottom (y=274..480)
     * and would cover it, same mistake as the Save/Skip buttons earlier */
    lv_obj_t *back = lv_btn_create(settings_win);
    lv_obj_set_size(back, 130, 40);
    lv_obj_set_pos(back, 150, 222);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(back, setup_back_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *back_l = lv_label_create(back);
    lv_label_set_text(back_l, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_l);

    lv_obj_t *kb = lv_keyboard_create(settings_win);
    lv_obj_set_size(kb, 800, 206);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, ta_ssid);
    lv_obj_add_event_cb(ta_ssid, setup_ta_event_cb, LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(ta_pass, setup_ta_event_cb, LV_EVENT_FOCUSED, kb);
}

static void build_setup_step_role(void)
{
    settings_win = setup_screen_base("Initial Setup - Step 1: Display Role");

    lv_obj_t *sub = lv_label_create(settings_win);
    lv_label_set_text(sub, "Is this the main helm display, or a secondary (CAN-only, no WiFi) monitor?");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7d92a6), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 42);

    tile_primary = lv_btn_create(settings_win);
    lv_obj_set_size(tile_primary, 680, 84);
    lv_obj_align(tile_primary, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(tile_primary,
        pending_role == DISPLAY_ROLE_PRIMARY ? lv_color_hex(0x1c5c40)
                                              : lv_color_hex(0x1c3450), 0);
    lv_obj_add_event_cb(tile_primary, role_tile_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)DISPLAY_ROLE_PRIMARY);
    lv_obj_t *tp_t = lv_label_create(tile_primary);
    lv_label_set_text(tp_t, "Primary Display");
    lv_obj_set_style_text_font(tp_t, &lv_font_montserrat_28, 0);
    lv_obj_align(tp_t, LV_ALIGN_TOP_LEFT, 12, 6);
    lv_obj_t *tp_s = lv_label_create(tile_primary);
    lv_label_set_text(tp_s, "Full control - ignition, glow, start, alarm silence");
    lv_obj_set_style_text_color(tp_s, lv_color_hex(0xbfd0e0), 0);
    lv_obj_align(tp_s, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    tile_secondary = lv_btn_create(settings_win);
    lv_obj_set_size(tile_secondary, 680, 84);
    lv_obj_align(tile_secondary, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_bg_color(tile_secondary,
        pending_role == DISPLAY_ROLE_SECONDARY ? lv_color_hex(0x1c5c40)
                                                : lv_color_hex(0x1c3450), 0);
    lv_obj_add_event_cb(tile_secondary, role_tile_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)DISPLAY_ROLE_SECONDARY);
    lv_obj_t *ts_t = lv_label_create(tile_secondary);
    lv_label_set_text(ts_t, "Secondary Display");
    lv_obj_set_style_text_font(ts_t, &lv_font_montserrat_28, 0);
    lv_obj_align(ts_t, LV_ALIGN_TOP_LEFT, 12, 6);
    lv_obj_t *ts_s = lv_label_create(tile_secondary);
    lv_label_set_text(ts_s, "Glow/start only, no WiFi - wakes when primary turns engine on");
    lv_obj_set_style_text_color(ts_s, lv_color_hex(0xbfd0e0), 0);
    lv_obj_align(ts_s, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    /* single Next: secondary finishes right here (setup_finish_cb, no
     * WiFi step at all); primary goes on to the WiFi step */
    lv_obj_t *next = lv_btn_create(settings_win);
    lv_obj_set_size(next, 240, 50);
    lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, -60, -30);
    lv_obj_set_style_bg_color(next, lv_color_hex(0x1c5c40), 0);
    lv_obj_add_event_cb(next, role_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_l = lv_label_create(next);
    lv_label_set_text(next_l, "Next " LV_SYMBOL_RIGHT);
    lv_obj_center(next_l);
}

/* PIN step: enter, then confirm. Blank + OK (or the Skip button) means
 * no PIN wanted - pending_pin stays empty and setup_finish_cb persists
 * that as-is, same "don't touch what wasn't typed" reasoning as WiFi's
 * Skip. */
static void pin_setup_submit(const char *entered)
{
    if (pin_setup_phase == 0) {
        if (entered[0] == 0) {
            pending_pin[0] = 0;
            pin_step_advance();
            return;
        }
        if (strlen(entered) < 4) {
            pinpad_show_error("PIN must be at least 4 digits");
            pinpad_reset();
            return;
        }
        strncpy(pin_setup_first, entered, sizeof(pin_setup_first) - 1);
        pin_setup_phase = 1;
        lv_label_set_text(pin_setup_sub_lbl, "Confirm PIN");
        pinpad_reset();
        return;
    }

    /* phase 1: confirm */
    if (strcmp(entered, pin_setup_first) == 0) {
        strncpy(pending_pin, entered, sizeof(pending_pin) - 1);
        pin_step_advance();
    } else {
        pinpad_show_error("PINs didn't match - try again");
        pin_setup_phase = 0;
        pin_setup_first[0] = 0;
        lv_label_set_text(pin_setup_sub_lbl, "Enter PIN (4+ digits)");
        pinpad_reset();
    }
}

static void pin_setup_skip_cb(lv_event_t *e)
{
    (void)e;
    pending_pin[0] = 0;
    pin_step_advance();
}

static void build_setup_step_pin(void)
{
    settings_win = setup_screen_base("Initial Setup - Step 2: Security PIN");
    pin_setup_phase = 0;
    pin_setup_first[0] = 0;

    pin_setup_sub_lbl = lv_label_create(settings_win);
    lv_label_set_text(pin_setup_sub_lbl, "Enter PIN (4+ digits)");
    lv_obj_set_style_text_color(pin_setup_sub_lbl, lv_color_hex(0x7d92a6), 0);
    lv_obj_align(pin_setup_sub_lbl, LV_ALIGN_TOP_MID, 0, 42);

    build_pin_pad(settings_win, 70, pin_setup_submit);

    lv_obj_t *back = lv_btn_create(settings_win);
    lv_obj_set_size(back, 130, 44);
    lv_obj_set_pos(back, 40, 424);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(back, setup_back_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_t *back_l = lv_label_create(back);
    lv_label_set_text(back_l, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_l);

    lv_obj_t *skip = lv_btn_create(settings_win);
    lv_obj_set_size(skip, 130, 44);
    lv_obj_set_pos(skip, 630, 424);
    lv_obj_set_style_bg_color(skip, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(skip, pin_setup_skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *skip_l = lv_label_create(skip);
    lv_label_set_text(skip_l, "Skip");
    lv_obj_center(skip_l);
}

static void build_setup_dialog(void)
{
    if (settings_win) return;
    if      (setup_step == 0) build_setup_step_role();
    else if (setup_step == 1) build_setup_step_pin();
    else                      build_setup_step_wifi();
}
#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

/* ==================== periodic UI refresh (5 Hz) ==================== */

/* Called from update_engine_state() right after running_raw is computed -
 * takes it as a parameter rather than recomputing, since g_engine_running
 * itself is masked false by start_held (see below) and this needs the
 * UNMASKED signal: autostart is the one holding start_held true during
 * CRANKING, so g_engine_running would never read true while it's actively
 * cranking otherwise - checking running_raw directly is what lets it
 * detect a catch and release start_held itself, on the same tick. */
static void autostart_tick(bool running_raw)
{
    uint32_t now = millis();

    if (autostart_state != AUTOSTART_IDLE &&
        (autostart_engine != sel_engine || !g_power)) {
        /* switched engines mid-run, or lost power - a stale sequence
         * must never keep commanding whichever engine is selected now */
        glow_held  = false;
        start_held = false;
        autostart_state = AUTOSTART_IDLE;
        return;
    }

    switch (autostart_state) {
    case AUTOSTART_GLOWING:
        if (now - autostart_phase_ms >= (uint32_t)as_glow_s[autostart_engine] * 1000UL) {
            glow_held  = false;
            start_held = true;
            start_press_ms = now;
            autostart_phase_ms = now;
            autostart_catch_since_ms = 0;
            autostart_state = AUTOSTART_CRANKING;
        }
        break;

    case AUTOSTART_CRANKING:
        if (running_raw) {
            if (autostart_catch_since_ms == 0) autostart_catch_since_ms = now;
            if (now - autostart_catch_since_ms >= AUTOSTART_CATCH_HOLD_MS) {
                /* held above threshold long enough - trust it as a real
                 * catch, not a crank-twitter spike. Release immediately,
                 * sequence done. */
                start_held = false;
                autostart_state = AUTOSTART_IDLE;
            }
        } else {
            autostart_catch_since_ms = 0;
            if (now - autostart_phase_ms >= (uint32_t)as_crank_s[autostart_engine] * 1000UL) {
                start_held = false;
                if (autostart_retries_left > 0) {
                    autostart_retries_left--;
                    autostart_phase_ms = now;
                    autostart_state = AUTOSTART_WAITING;
                } else {
                    autostart_state = AUTOSTART_IDLE;   /* retries exhausted, gave up */
                }
            }
        }
        break;

    case AUTOSTART_WAITING:
        if (now - autostart_phase_ms >= (uint32_t)as_wait_s[autostart_engine] * 1000UL) {
            start_held = true;   /* re-crank only, no re-glow */
            start_press_ms = now;
            autostart_phase_ms = now;
            autostart_catch_since_ms = 0;
            autostart_state = AUTOSTART_CRANKING;
        }
        break;

    case AUTOSTART_IDLE:
    default:
        break;
    }
}

/* Non-widget engine/alarm/power state machine, shared by ui_tick() (HELM)
 * and ui_tick_cyd() (CYD) - kept free of any lv_obj_* calls so both tick
 * functions can call it before going on to update their own, completely
 * separate widget trees. */
static void update_engine_state(void)
{
    /* --- power / running state machine --- */
    int rpm = eng.rpm;
    if (rpm < 0) rpm = 0;

    bool running_raw = (rpm >= RPM_RUNNING_MIN);
    autostart_tick(running_raw);
    g_engine_running = running_raw && !start_held;   /* mask while cranking */
    if (g_display_role == DISPLAY_ROLE_SECONDARY) {
        /* no local ignition authority: pure follower. Wakes the instant
         * the primary's ignition command is confirmed by CTRL (ign_actual),
         * NOT when rpm actually rises - glow/start need to work *before*
         * the engine catches, or a secondary station could never crank it. */
        g_power = eng.ign_actual;
    } else {
        if (g_engine_running) user_power[sel_engine] = true;   /* sticky: latch, don't override */
        g_power = user_power[sel_engine];
    }
    ign_commanded = g_power;   /* harmless on secondary: never transmitted */

    /* --- overrev tracking: >5 s continuously in the red --- */
    static uint32_t red_since = 0;
    bool overrev = false;
    if (rpm >= RPM_RED_V) {
        if (red_since == 0) red_since = millis();
        overrev = (millis() - red_since) >= OVERREV_MS;
    } else {
        red_since = 0;
    }

    /* --- alarm bookkeeping --- */
    g_active_alarms = (eng.temp_alarm ? ALM_TEMP : 0) |
                      (eng.press_alarm ? ALM_PRESS : 0) |
                      (overrev ? ALM_OVERREV : 0);
    silenced_mask &= g_active_alarms;   /* cleared alarms re-arm */

    /* --- engine presence: auto-select --- */
    /* Skipped while an autostart run is actively targeting the selected
     * engine (autostart_engine == sel_engine) - confirmed on real
     * hardware as the actual root cause of "autostart gives up too
     * early" whenever a brief ESP-NOW dropout happened to coincide with
     * a run: autostart_tick() (above) treats sel_engine changing out from
     * under it as "the user switched engines mid-run" and correctly (by
     * its own logic) cancels the whole sequence - but a transient
     * ageout/reselect is not the user switching anything, it's this
     * block silently doing it on their behalf the instant the target
     * engine's ANNOUNCE gap crosses ENGINE_LOST_MS while a different
     * engine is still broadcasting. autostart_tick() already handles a
     * temporarily-vanished target fine on its own (engine_ageout_tick()
     * zeroes eng.rpm via reset_telemetry() when sel_engine itself ages
     * out, which just reads as "not caught yet" mid-crank, or doesn't
     * matter at all during GLOWING/WAITING, which are pure timers) - it
     * just needs sel_engine and g_power left alone so it can keep running
     * and either catch a real reconnect or time out on its own configured
     * schedule, not an unrelated bus hiccup. */
    bool autostart_owns_selection =
        (autostart_state != AUTOSTART_IDLE && autostart_engine == sel_engine);
    if (!engines[sel_engine].present && any_engine_present() && !autostart_owns_selection) {
        /* selected engine vanished but another is broadcasting:
         * switch to the first present one rather than going dark */
        for (int i = 0; i < MD_MAX_ENGINES; i++) {
            if (engines[i].present) {
                sel_engine = i;
                if (prefs_ok) prefs.putUChar("engine", sel_engine);
                reset_telemetry();
                silenced_mask = 0;
                break;
            }
        }
    }
}

/* HELM only from here through create_ui() below - ui_tick() drives the
 * 800x480 dashboard's widget globals, which only exist when create_ui()
 * (not create_ui_cyd()) built them. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480
static void ui_tick(lv_timer_t *t)
{
    (void)t;
    flash_on = !flash_on;
    update_engine_state();

    /* re-derive locals update_engine_state() computed internally - it
     * doesn't return them, cheap to recompute from the globals it published */
    int rpm = eng.rpm;
    if (rpm < 0) rpm = 0;
    bool overrev = g_active_alarms & ALM_OVERREV;

    uint16_t caps = current_caps();

    set_hidden(no_engine_overlay, any_engine_present());

    /* IP text prepared by wifi_setup; label updated only on change */
    static char last_ip_text[sizeof(wifi_ip_text)] = "";
    if (strcmp(last_ip_text, wifi_ip_text) != 0) {
        strncpy(last_ip_text, wifi_ip_text, sizeof(last_ip_text));
        lv_label_set_text(lbl_no_engine_ip, wifi_ip_text);
    }

    /* --- power-off overlay (guarded: no-op unless state changed) --- */
    set_hidden(off_overlay, g_power);
    bool update_showing = g_helm_update_available || any_remote_update_available();
    if (btn_update_available) set_hidden(btn_update_available, !update_showing);
    if (btn_update_available_ne) set_hidden(btn_update_available_ne, !update_showing);
    remote_ota_modal_tick();
    update_all_tick();

    /* change-guarded on g_update_check_done_id (set by check_for_update_
     * tick() once a forced check finishes one way or another) rather than
     * edge-detecting g_force_update_check itself - see g_update_check_
     * done_id's own comment on why a boolean pulse can get missed
     * entirely by this 200ms poll. Updates both off_overlay's and
     * no_engine_overlay's status labels together - only one screen is
     * ever visible at a time, so this is simpler than tracking which one
     * triggered the check. */
    static uint32_t last_seen_done_id = 0;
    if (g_update_check_done_id != last_seen_done_id) {
        last_seen_done_id = g_update_check_done_id;
        /* always give a real answer - previously a found update just
         * hid this label and relied on the "Update Available" button
         * silently appearing, which didn't read as feedback from the
         * tap at all. Every outcome (including both failure modes) now
         * gets an explicit message instead of going quiet. */
        const char *result_text;
        switch (g_last_check_result) {
        case UPDATE_CHECK_RESULT_NO_WIFI:      result_text = "No WiFi connection"; break;
        case UPDATE_CHECK_RESULT_FETCH_FAILED: result_text = "Update check failed"; break;
        case UPDATE_CHECK_RESULT_OK:
            result_text = update_showing ? "Update available!" : "No updates available";
            break;
        default: result_text = "Update check failed"; break;
        }
        if (lbl_check_status) {
            lv_obj_clear_flag(lbl_check_status, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_check_status, result_text);
        }
        if (lbl_check_status_ne) {
            lv_obj_clear_flag(lbl_check_status_ne, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_check_status_ne, result_text);
        }
        /* Confirmed on real hardware (same root cause as remote_ota_
         * modal_tick()'s identical fix): this update can silently fail
         * to reach the physical screen under ESP-NOW/WiFi load, leaving
         * "Checking..." stuck forever even though the label's text was
         * genuinely updated at the LVGL-object level. Force the flush
         * rather than trusting the next automatic cycle. */
        lv_refr_now(NULL);
    }

    /* Wireless Pairing button label - change-guarded on g_pairing_mode
     * itself rather than updated inline wherever it gets set, since it
     * can flip true from three different places (this button,
     * wireless_pairing_cb(); the web debug page's /esppair handler; and
     * back to false from pairing_mode_tick()'s auto-close after
     * PAIR_WINDOW_MS) - one spot here covers all of them instead of
     * duplicating the lv_label_set_text call three times. Previously
     * this only ever refreshed when the dialog was closed and reopened,
     * which read as the tap having done nothing. */
    static bool last_pairing_mode_ui = false;
    if (g_pairing_mode != last_pairing_mode_ui) {
        last_pairing_mode_ui = g_pairing_mode;
        if (lbl_wireless_pairing)
            lv_label_set_text(lbl_wireless_pairing, espnow_key_label());
    }

    /* --- tachometer bar (only redrawn when rpm changes) --- */
    if (rpm > RPM_MAX) rpm = RPM_MAX;
    static int last_bar_rpm = -1;
    if (rpm != last_bar_rpm) {
        last_bar_rpm = rpm;
        set_hidden(bar_line, rpm == 0);
        if (rpm > 0) {
            int   idx  = rpm / GAUGE_STEP_RPM;
            float frac = (rpm % GAUGE_STEP_RPM) / (float)GAUGE_STEP_RPM;
            if (idx >= GAUGE_PTS - 1) { idx = GAUGE_PTS - 2; frac = 1.0f; }

            int n = idx + 1;
            memcpy(bar_draw, bar_pts, n * sizeof(lv_point_t));
            bar_draw[n].x = bar_pts[idx].x +
                (lv_coord_t)lroundf(frac * (bar_pts[idx + 1].x - bar_pts[idx].x));
            bar_draw[n].y = bar_pts[idx].y +
                (lv_coord_t)lroundf(frac * (bar_pts[idx + 1].y - bar_pts[idx].y));
            lv_line_set_points(bar_line, bar_draw, n + 1);

            lv_obj_set_style_line_color(bar_line,
                (rpm >= RPM_RED_V) ? lv_color_hex(0xe24b4a)
                                   : lv_color_hex(0x00cc66), 0);
        }
    }

    /* --- digital rpm + overrev alarm --- */
    static int last_rpm_shown = INT32_MIN;
    if ((int)eng.rpm != last_rpm_shown) {
        last_rpm_shown = (int)eng.rpm;
        lv_label_set_text_fmt(lbl_rpm, "%d", last_rpm_shown);
    }
    static int last_rpm_state = -1;   /* 0 normal, 1 red, 2/3 overrev flash */
    int rpm_state = overrev ? (((silenced_mask & ALM_OVERREV) || flash_on) ? 2 : 3)
                            : ((rpm >= RPM_RED_V) ? 1 : 0);
    if (rpm_state != last_rpm_state) {
        last_rpm_state = rpm_state;
        lv_color_t c = (rpm_state == 0) ? lv_color_hex(0xffffff)
                     : (rpm_state == 3) ? lv_color_hex(0x441111)
                                        : lv_color_hex(0xe24b4a);
        if (rpm_state == 2) c = lv_color_hex(0xff2222);
        lv_obj_set_style_text_color(lbl_rpm, c, 0);
        if (overrev)
            lv_obj_set_style_text_color(lbl_overrev, c, 0);
    }
    set_hidden(lbl_overrev, !overrev);

    /* --- engine hours (text only on change) --- */
    static uint32_t last_hours = 0xFFFFFFFF;
    uint32_t hours_now = eng.hours_seen ? eng.hours_x10 : 0xFFFFFFFE;
    if (hours_now != last_hours) {
        last_hours = hours_now;
        if (eng.hours_seen) {
            lv_label_set_text_fmt(lbl_hours, "%lu.%lu h",
                                  (unsigned long)(eng.hours_x10 / 10),
                                  (unsigned long)(eng.hours_x10 % 10));
        } else {
            lv_label_set_text(lbl_hours, "-- h");
        }
    }

    /* --- coolant temperature --- */
    static int last_temp = INT32_MIN;
    if ((int)eng.temp_c != last_temp) {
        last_temp = (int)eng.temp_c;
        lv_label_set_text_fmt(lbl_temp, "%d\xC2\xB0""C", last_temp);
    }
    static int last_temp_state = -1;  /* 0 ok, 1 flash-on, 2 flash-off, 3 silenced */
    int temp_state = !eng.temp_alarm ? 0
                   : (silenced_mask & ALM_TEMP) ? 3
                   : (flash_on ? 1 : 2);
    if (temp_state != last_temp_state) {
        last_temp_state = temp_state;
        lv_obj_set_style_text_color(lbl_temp,
            (temp_state == 0) ? lv_color_hex(0x00cc66)
          : (temp_state == 2) ? lv_color_hex(0x441111)
                              : lv_color_hex(0xff2222), 0);
    }

    /* --- oil pressure --- */
    static int last_oil10 = INT32_MIN;
    int oil10 = (int)lroundf(eng.oil_bar * 10.0f);
    if (oil10 != last_oil10) {
        last_oil10 = oil10;
        lv_label_set_text_fmt(lbl_press, "%d.%d bar", oil10 / 10, oil10 % 10);
    }
    static int last_press_state = -1;
    int press_state = !eng.press_alarm ? 0
                    : (silenced_mask & ALM_PRESS) ? 3
                    : (flash_on ? 1 : 2);
    if (press_state != last_press_state) {
        last_press_state = press_state;
        lv_obj_set_style_text_color(lbl_press,
            (press_state == 0) ? lv_color_hex(0x00cc66)
          : (press_state == 2) ? lv_color_hex(0x441111)
                               : lv_color_hex(0xff2222), 0);
    }

    /* --- MUTE button: visible while any alarm is active --- */
    set_hidden(btn_mute, !g_active_alarms);
    if (g_active_alarms) {
        static int last_mute_state = -1;  /* 0 acked, 1 flash-on, 2 flash-off */
        bool unacked = g_active_alarms & ~silenced_mask;
        int mute_state = !unacked ? 0 : (flash_on ? 1 : 2);
        if (mute_state != last_mute_state) {
            last_mute_state = mute_state;
            lv_obj_set_style_bg_color(btn_mute,
                (mute_state == 0) ? lv_color_hex(0x33475c)
              : (mute_state == 1) ? lv_color_hex(0xcc8800)
                                  : lv_color_hex(0x664400), 0);
        }
    }

    /* --- CAN link indicator --- */
    if (g_can_disabled) {
        /* user said "no CAN hardware, don't warn me" - stay hidden,
         * don't run the flashing state machine at all */
        set_hidden(lbl_link, true);
    } else {
        bool lnk = link_up();
        static int last_lnk_state = -1;  /* 0 up, 1 down-flash-on, 2 down-flash-off */
        int lnk_state = lnk ? 0 : (flash_on ? 1 : 2);
        set_hidden(lbl_link, false);
        if (lnk_state != last_lnk_state) {
            bool text_changed = (last_lnk_state == 0) != (lnk_state == 0) ||
                                last_lnk_state == -1;
            last_lnk_state = lnk_state;
            if (text_changed)
                lv_label_set_text(lbl_link, lnk ? "LINK" : "NO LINK");
            lv_obj_set_style_text_color(lbl_link,
                (lnk_state == 0) ? lv_color_hex(0x00cc66)
              : (lnk_state == 1) ? lv_color_hex(0xff2222)
                                 : lv_color_hex(0x662222), 0);
        }
    }

    /* --- selected engine indicator (text only on change) --- */
    static uint8_t last_eng_sel = 0xFF, last_eng_type = 0xFF;
    static int last_eng_present = -1;
    static char last_eng_name[MD_ENGINE_NAME_MAXLEN + 1] = "";
    if (sel_engine != last_eng_sel ||
        engines[sel_engine].type != last_eng_type ||
        (int)engines[sel_engine].present != last_eng_present ||
        strcmp(engines[sel_engine].name, last_eng_name) != 0) {
        last_eng_sel     = sel_engine;
        last_eng_type    = engines[sel_engine].type;
        last_eng_present = (int)engines[sel_engine].present;
        strncpy(last_eng_name, engines[sel_engine].name, sizeof(last_eng_name) - 1);
        char eng_buf[40];
        snprintf(eng_buf, sizeof(eng_buf), "E%d %s", sel_engine,
            engines[sel_engine].present
                ? engine_display_name(sel_engine) : "--");
        lv_label_set_text(lbl_engine, eng_buf);
        lv_label_set_text(lbl_engine_off, eng_buf);
        lv_label_set_text(lbl_engine_noeng, eng_buf);
    }

    /* --- ENGINE chip: hidden entirely (not just un-tappable) when
     * there's nothing to choose between - a control that reacts to a
     * tap but visibly does nothing is worse than no control at all --- */
    static int last_selectable = -1;
    int selectable = count_selectable_engines();
    if (selectable != last_selectable) {
        last_selectable = selectable;
        bool show = selectable >= 2;
        set_hidden(btn_engine, !show);
        if (btn_engine_off)   set_hidden(btn_engine_off, !show);
        if (btn_engine_noeng) set_hidden(btn_engine_noeng, !show);
    }

    /* --- capability-driven visibility (guarded no-ops) --- */
    set_hidden(img_temp,  !(caps & CAP_COOLANT_TEMP));
    set_hidden(lbl_temp,  !(caps & CAP_COOLANT_TEMP));
    set_hidden(img_oil,   !(caps & CAP_OIL_PRESS));
    set_hidden(lbl_press, !(caps & CAP_OIL_PRESS));
    set_hidden(lbl_hours, !(caps & CAP_HOURS));
    set_hidden(btn_glow,  !(caps & CAP_GLOW));
    set_hidden(btn_start, !(caps & CAP_START));

    /* --- controls: reflect + lock (state calls no-op when unchanged) --- */
    if (g_power) lv_obj_add_state(sw_power, LV_STATE_CHECKED);
    else         lv_obj_clear_state(sw_power, LV_STATE_CHECKED);

    if (g_engine_running) lv_obj_add_state(sw_power, LV_STATE_DISABLED);
    else                  lv_obj_clear_state(sw_power, LV_STATE_DISABLED);

    /* glow/start are available to BOTH roles (matches the CYD cockpit
     * display note in CLAUDE.md: glow/start only, no ignition authority).
     * START doubles as STOP once the engine is running, but ONLY when
     * the selected engine's ANNOUNCE included CAP_STOP and this is the
     * primary display - see can_protocol.h's safety note. Most
     * engines (the MD2030 specifically) can only be stopped
     * mechanically, so this must never appear as a generic,
     * always-available control - g_can_stop is what enforces that. */
    g_can_stop = (caps & CAP_STOP) && (g_display_role == DISPLAY_ROLE_PRIMARY);
    bool crank_ok = g_power && !g_engine_running;
    bool stop_ok  = g_power && g_engine_running && g_can_stop;
    if (crank_ok) lv_obj_clear_state(btn_glow, LV_STATE_DISABLED);
    else          lv_obj_add_state(btn_glow, LV_STATE_DISABLED);
    if (crank_ok || stop_ok) lv_obj_clear_state(btn_start, LV_STATE_DISABLED);
    else                     lv_obj_add_state(btn_start, LV_STATE_DISABLED);
    /* stays clickable through the whole sequence (autostart_state !=
     * IDLE) even if crank_ok momentarily reads false, so cancel is
     * always reachable - see autostart_cb()/autostart_tick()'s comments
     * on why crank_ok is actually g_power-equivalent throughout a run. */
    if (crank_ok || autostart_state != AUTOSTART_IDLE) lv_obj_clear_state(btn_auto, LV_STATE_DISABLED);
    else                                                lv_obj_add_state(btn_auto, LV_STATE_DISABLED);

    /* --- glow: red while held, seconds counter beside --- */
    set_hidden(lbl_glow_cnt, !glow_held);
    static int last_glow_state = -1;  /* 0 idle, 1 relay-active, 2 held */
    int glow_state = glow_held ? 2 : (eng.glow_active ? 1 : 0);
    if (glow_state != last_glow_state) {
        last_glow_state = glow_state;
        lv_obj_set_style_bg_color(btn_glow,
            (glow_state == 2) ? lv_color_hex(0xcc2222)
          : (glow_state == 1) ? lv_color_hex(0xff9922)
                              : lv_color_hex(0x995500), 0);
    }
    if (glow_held) {
        static uint32_t last_gsec = 0xFFFFFFFF;
        uint32_t gsec = (millis() - glow_press_ms) / 1000;
        if (gsec != last_gsec) {
            last_gsec = gsec;
            lv_label_set_text_fmt(lbl_glow_cnt, "%lus", (unsigned long)gsec);
        }
    }

    /* --- start/stop: same physical button, two personalities.
     * crank:  dark green idle / bright green cranking / red held
     * stop:   dark red armed-idle / bright red CTRL-confirmed / red held
     * Running with no CAP_STOP (the MD2030, or any secondary display)
     * just stays disabled showing the ordinary crank icon - identical
     * to the button's behavior before this feature existed. */
    bool stop_mode = g_engine_running && g_can_stop;
    static int last_stop_mode = -1;
    if ((int)stop_mode != last_stop_mode) {
        last_stop_mode = (int)stop_mode;
        lv_img_set_src(img_start, stop_mode ? &icon_stop : &icon_start);
    }

    /* --- autostart button: idle "AUTO" vs active "CANCEL", amber while
     * running. lbl_glow_cnt/lbl_start_cnt above already show a live
     * seconds count during the GLOWING/CRANKING phases for free, since
     * autostart drives the exact same glow_held/start_held flags - no
     * separate countdown needed on this button (WAITING between retries
     * has no visible countdown of its own, a known minor gap given how
     * little screen room this button has). */
    bool autostart_active = (autostart_state != AUTOSTART_IDLE);
    static int last_autostart_active = -1;
    if ((int)autostart_active != last_autostart_active) {
        last_autostart_active = (int)autostart_active;
        lv_label_set_text(lbl_auto, autostart_active ? "CANCEL" : "AUTO");
        lv_obj_set_style_bg_color(btn_auto,
            autostart_active ? lv_color_hex(0xb35900) : lv_color_hex(0x1c3450), 0);
    }

    set_hidden(lbl_start_cnt, !(start_held || stop_held));
    static int last_start_state = -1;
    int start_state;
    if (stop_mode) start_state = stop_held ? 12 : (eng.stop_active ? 14 : 13);
    else           start_state = start_held ? 2  : (eng.crank_active ? 1 : 0);
    if (start_state != last_start_state) {
        last_start_state = start_state;
        lv_color_t c;
        switch (start_state) {
        case 1:  c = lv_color_hex(0x22cc66); break;   /* crank active */
        case 2:  c = lv_color_hex(0xcc2222); break;   /* crank held */
        case 12: c = lv_color_hex(0xcc2222); break;   /* stop held */
        case 13: c = lv_color_hex(0x662222); break;   /* stop armed, idle */
        case 14: c = lv_color_hex(0xff2222); break;   /* stop confirmed active */
        default: c = lv_color_hex(0x116633); break;   /* crank idle */
        }
        lv_obj_set_style_bg_color(btn_start, c, 0);
    }
    if (start_held) {
        static uint32_t last_ssec = 0xFFFFFFFF;
        uint32_t ssec = (millis() - start_press_ms) / 1000;
        if (ssec != last_ssec) {
            last_ssec = ssec;
            lv_label_set_text_fmt(lbl_start_cnt, "%lus", (unsigned long)ssec);
        }
    } else if (stop_held) {
        static uint32_t last_tsec = 0xFFFFFFFF;
        uint32_t tsec = (millis() - stop_press_ms) / 1000;
        if (tsec != last_tsec) {
            last_tsec = tsec;
            lv_label_set_text_fmt(lbl_start_cnt, "%lus", (unsigned long)tsec);
        }
    }
}

/* ==================== UI construction ==================== */

static lv_obj_t *make_polyline(lv_obj_t *parent, const lv_point_t *pts,
                               uint16_t n, lv_color_t color,
                               lv_coord_t width)
{
    lv_obj_t *ln = lv_line_create(parent);
    lv_line_set_points(ln, pts, n);
    lv_obj_set_style_line_color(ln, color, 0);
    lv_obj_set_style_line_width(ln, width, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_set_pos(ln, 0, 0);
    return ln;
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a1929), 0);

    gauge_geom_init();

    /* ---- fill the static point tables ---- */
    for (int i = 0; i <= RED_START_IDX; i++)
        gauge_point(i * GAUGE_STEP_RPM, 0.0f, &scale_grn_pts[i]);
    for (int i = RED_START_IDX; i < GAUGE_PTS; i++)
        gauge_point(i * GAUGE_STEP_RPM, 0.0f,
                    &scale_red_pts[i - RED_START_IDX]);
    for (int i = 0; i < GAUGE_PTS; i++)
        gauge_point(i * GAUGE_STEP_RPM, BAR_OFFSET, &bar_pts[i]);

    /* ---- scale line: green with fixed red tip ---- */
    make_polyline(scr, scale_grn_pts, RED_START_IDX + 1,
                  lv_color_hex(0x00aa55), LINE_W);
    make_polyline(scr, scale_red_pts, GAUGE_PTS - RED_START_IDX,
                  lv_color_hex(0xe24b4a), LINE_W);

    /* ---- ticks: every 250, majors on the thousands ---- */
    for (int i = 0; i <= 16; i++) {
        int rpm    = i * 250;
        bool major = (rpm % 1000) == 0;
        bool mid   = (rpm % 500) == 0;
        float o1 = LINE_W / 2.0f + 2.0f;
        float o2 = o1 + (major ? 22.0f : (mid ? 14.0f : 9.0f));
        gauge_point(rpm, o1, &tick_pts[i][0]);
        gauge_point(rpm, o2, &tick_pts[i][1]);
        make_polyline(scr, tick_pts[i], 2,
                      major ? lv_color_hex(0x8fa3b8) : lv_color_hex(0x4d6377),
                      major ? 4 : 2);
    }

    /* ---- labels 1..4 (rpm x 1000) ---- */
    lv_obj_t *lbl_tick1 = NULL;   /* the "1" label - caption anchors to it */
    for (int k = 1; k <= 4; k++) {
        lv_point_t p;
        gauge_point(k * 1000.0f, 46.0f, &p);
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text_fmt(l, "%d", k);
        lv_obj_set_style_text_color(l, lv_color_hex(0xebf0f5), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(l, p.x - 8, p.y - 9);
        if (k == 1) lbl_tick1 = l;
    }

    lv_obj_t *cap = lv_label_create(scr);
    lv_label_set_text(cap, "rpm x 1000");
    lv_obj_set_style_text_color(cap, lv_color_hex(0x7d92a6), 0);
    lv_obj_align_to(cap, lbl_tick1, LV_ALIGN_OUT_TOP_MID, 0, -4);

    /* ---- travelling bar (hidden until rpm > 0) ---- */
    bar_line = make_polyline(scr, bar_pts, 2, lv_color_hex(0x00cc66), BAR_W);
    lv_obj_add_flag(bar_line, LV_OBJ_FLAG_HIDDEN);

    /* ---- digital rpm in the pocket under the stick ---- */
    lbl_rpm = lv_label_create(scr);
    lv_label_set_text(lbl_rpm, "0");
    lv_obj_set_style_text_color(lbl_rpm, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_rpm, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(lbl_rpm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_rpm, 220);
    lv_obj_set_pos(lbl_rpm, 220, 245);

    lv_obj_t *rpm_cap = lv_label_create(scr);
    lv_label_set_text(rpm_cap, "rpm");
    lv_obj_set_style_text_color(rpm_cap, lv_color_hex(0x7d92a6), 0);
    lv_obj_set_style_text_align(rpm_cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(rpm_cap, 220);
    lv_obj_set_pos(rpm_cap, 220, 305);

    lbl_hours = lv_label_create(scr);
    lv_label_set_text(lbl_hours, "-- h");
    lv_obj_set_style_text_color(lbl_hours, lv_color_hex(0x8fa3b8), 0);
    lv_obj_set_style_text_font(lbl_hours, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(lbl_hours, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_hours, 220);
    lv_obj_set_pos(lbl_hours, 220, 335);

    /* ---- overrev warning (hidden) ---- */
    lbl_overrev = lv_label_create(scr);
    lv_label_set_text(lbl_overrev, "OVER REV");
    lv_obj_set_style_text_font(lbl_overrev, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_overrev, lv_color_hex(0xff2222), 0);
    lv_obj_set_style_text_align(lbl_overrev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_overrev, 220);
    lv_obj_set_pos(lbl_overrev, 220, 205);
    lv_obj_add_flag(lbl_overrev, LV_OBJ_FLAG_HIDDEN);

    /* ---- MUTE (hidden until an alarm is active). Parked just above the
     * digital rpm readout (which starts at y=245) - verified clear of
     * the gauge scale line down to y=149 in this x column, and the
     * OVER REV warning label starts at y=205 (5px gap below). ---- */
    btn_mute = lv_btn_create(scr);
    lv_obj_set_size(btn_mute, 150, 50);
    lv_obj_set_pos(btn_mute, 235, 150);
    lv_obj_set_style_bg_color(btn_mute, lv_color_hex(0x664400), 0);
    lv_obj_add_event_cb(btn_mute, mute_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(btn_mute);
    lv_label_set_text(ml, LV_SYMBOL_MUTE " MUTE");
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_28, 0);
    lv_obj_center(ml);
    lv_obj_add_flag(btn_mute, LV_OBJ_FLAG_HIDDEN);

    /* ---- icon + value blocks, right side ---- */
    img_temp = lv_img_create(scr);
    lv_img_set_src(img_temp, &icon_temp);
    lv_obj_set_pos(img_temp, 528, 162);

    lbl_temp = lv_label_create(scr);
    lv_label_set_text(lbl_temp, "--");
    lv_obj_set_style_text_color(lbl_temp, lv_color_hex(0x00cc66), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(lbl_temp, 585, 170);

    img_oil = lv_img_create(scr);
    lv_img_set_src(img_oil, &icon_oil);
    lv_obj_set_pos(img_oil, 530, 254);

    lbl_press = lv_label_create(scr);
    lv_label_set_text(lbl_press, "--");
    lv_obj_set_style_text_color(lbl_press, lv_color_hex(0x00cc66), 0);
    lv_obj_set_style_text_font(lbl_press, &lv_font_montserrat_48, 0);
    lv_obj_set_pos(lbl_press, 585, 258);

    /* ---- settings cog + link status, top-left ---- */
    lv_obj_t *btn_cog = lv_btn_create(scr);
    lv_obj_set_size(btn_cog, 48, 48);
    lv_obj_align(btn_cog, LV_ALIGN_TOP_LEFT, 8, 11);
    /* no box: transparent background, no shadow - just the glyph */
    lv_obj_set_style_bg_opa(btn_cog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_cog, 0, 0);
    lv_obj_add_event_cb(btn_cog, settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cog = lv_label_create(btn_cog);
    lv_label_set_text(cog, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(cog, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(cog, lv_color_hex(0x7d92a6), 0);
    lv_obj_center(cog);

    lbl_link = lv_label_create(scr);
    lv_label_set_text(lbl_link, "NO LINK");
    lv_obj_set_style_text_color(lbl_link, lv_color_hex(0x662222), 0);
    lv_obj_set_pos(lbl_link, 66, 19);

    /* ENGINE chip: separate control from the cog - opens the engine
     * picker, not general settings. y=416 (20px lower than before) puts
     * its bottom edge at 450, past the gauge's 0-point (GA_P0Y=430) -
     * harmless since the scale line doesn't extend below that anyway.
     * At this height the scale line only reaches out to x~110, verified
     * clear from x=120 on; x=130 leaves a bit of margin. */
    btn_engine = lv_btn_create(scr);
    lv_obj_set_size(btn_engine, 200, 34);
    lv_obj_set_pos(btn_engine, 130, 416);
    /* filled chip + border + a dropdown chevron - a plain transparent
     * label here just reads as static text, not something you can tap */
    lv_obj_set_style_bg_opa(btn_engine, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn_engine, lv_color_hex(0x142838), 0);
    lv_obj_set_style_border_width(btn_engine, 1, 0);
    lv_obj_set_style_border_color(btn_engine, lv_color_hex(0x33475c), 0);
    lv_obj_set_style_radius(btn_engine, 8, 0);
    lv_obj_set_style_shadow_width(btn_engine, 0, 0);
    lv_obj_add_event_cb(btn_engine, engine_picker_cb, LV_EVENT_CLICKED, NULL);
    lbl_engine = lv_label_create(btn_engine);
    lv_label_set_text(lbl_engine, "E0 --");
    lv_obj_set_style_text_color(lbl_engine, lv_color_hex(0x7d92a6), 0);
    /* long engine names ("E0 Volvo Penta MD2030C") truncate with an
     * ellipsis instead of overflowing past the chip's visible border */
    lv_label_set_long_mode(lbl_engine, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_engine, 160);
    lv_obj_align(lbl_engine, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *chev_engine = lv_label_create(btn_engine);
    lv_label_set_text(chev_engine, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(chev_engine, lv_color_hex(0x556677), 0);
    lv_obj_align(chev_engine, LV_ALIGN_RIGHT_MID, -8, 0);
    /* visibility (shown only when there's 2+ engines to choose between,
     * for whichever role) is driven live by ui_tick, not fixed here */

    /* ---- control cluster, bottom-right ---- */
    lv_obj_t *pwr_cap = lv_label_create(scr);
    lv_label_set_text(pwr_cap, "POWER");
    lv_obj_set_style_text_color(pwr_cap, lv_color_hex(0x7d92a6), 0);
    lv_obj_set_pos(pwr_cap, 530, 352);

    sw_power = lv_switch_create(scr);
    lv_obj_set_size(sw_power, 90, 38);
    lv_obj_set_pos(sw_power, 610, 342);
    lv_obj_add_event_cb(sw_power, power_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* secondary has no ignition authority - it only follows the bus,
     * so there's nothing for a power switch to command here */
    if (g_display_role == DISPLAY_ROLE_SECONDARY) {
        lv_obj_add_flag(pwr_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sw_power, LV_OBJ_FLAG_HIDDEN);
    }

    /* AUTOSTART - squeezed into the only gap left on this row (engine
     * chip ends x330, lbl_glow_cnt starts x392 - NOT independently
     * verified clear the way the MUTE button's placement comment
     * documents, check visually after first flash and nudge if anything
     * overlaps). Deliberately narrow (58px) - its own label text doubles
     * as the phase status readout (AUTO/CANCEL/GLOW/CRANK n//WAIT),
     * updated in ui_tick(), rather than a separate status widget there's
     * no room for. */
    btn_auto = lv_btn_create(scr);
    lv_obj_set_size(btn_auto, 58, 74);
    lv_obj_set_pos(btn_auto, 332, 396);
    /* the default theme's button padding was eating a further ~10px off
     * this already-narrow 58px button before the label ever got a
     * chance at it - reclaim it, the label needs every pixel available */
    lv_obj_set_style_pad_all(btn_auto, 2, 0);
    lv_obj_add_event_cb(btn_auto, autostart_cb, LV_EVENT_CLICKED, NULL);
    lbl_auto = lv_label_create(btn_auto);
    lv_label_set_text(lbl_auto, "AUTO");
    lv_obj_set_style_text_align(lbl_auto, LV_TEXT_ALIGN_CENTER, 0);
    /* "CANCEL" (active state, see ui_tick()) was still wrapping to a
     * second line at montserrat_12/50px - dropped further to
     * montserrat_10 and widened close to the button's real interior
     * width (54 of 58, now that btn_auto's own padding above is
     * minimal) rather than guessing again. There's no room to widen the
     * button itself (see the tight-gap comment above). */
    lv_obj_set_style_text_font(lbl_auto, &lv_font_montserrat_10, 0);
    lv_obj_set_width(lbl_auto, 54);
    lv_obj_center(lbl_auto);

    btn_glow = lv_btn_create(scr);
    lv_obj_set_size(btn_glow, 135, 74);
    lv_obj_set_pos(btn_glow, 455, 396);
    lv_obj_add_event_cb(btn_glow, glow_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *gi = lv_img_create(btn_glow);
    lv_img_set_src(gi, &icon_glow);       /* diesel pre-heat coil */
    lv_obj_center(gi);

    /* hold counter, left-beside GLOW (hidden until pressed) */
    lbl_glow_cnt = lv_label_create(scr);
    lv_label_set_text(lbl_glow_cnt, "0s");
    lv_obj_set_style_text_font(lbl_glow_cnt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_glow_cnt, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(lbl_glow_cnt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(lbl_glow_cnt, 58);
    lv_obj_set_pos(lbl_glow_cnt, 392, 418);
    lv_obj_add_flag(lbl_glow_cnt, LV_OBJ_FLAG_HIDDEN);

    btn_start = lv_btn_create(scr);
    lv_obj_set_size(btn_start, 135, 74);
    lv_obj_set_pos(btn_start, 605, 396);
    lv_obj_add_event_cb(btn_start, start_cb, LV_EVENT_ALL, NULL);
    img_start = lv_img_create(btn_start);
    lv_img_set_src(img_start, &icon_start);      /* circular crank arrow;
                                                   * swapped to icon_stop
                                                   * live by ui_tick */
    lv_obj_center(img_start);

    /* hold counter, right-beside START (hidden until pressed) */
    lbl_start_cnt = lv_label_create(scr);
    lv_label_set_text(lbl_start_cnt, "0s");
    lv_obj_set_style_text_font(lbl_start_cnt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_start_cnt, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(lbl_start_cnt, 55);
    lv_obj_set_pos(lbl_start_cnt, 745, 418);
    lv_obj_add_flag(lbl_start_cnt, LV_OBJ_FLAG_HIDDEN);

    /* ---- power-off overlay: black screen, power button + cog ----
     * created LAST so it sits above everything. Shown at boot
     * (user_power starts false); ui_tick shows/hides it. */
    off_overlay = lv_obj_create(scr);
    lv_obj_set_size(off_overlay, 800, 480);
    lv_obj_set_pos(off_overlay, 0, 0);
    lv_obj_set_style_bg_color(off_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(off_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(off_overlay, 0, 0);
    lv_obj_set_style_radius(off_overlay, 0, 0);
    /* unlike scr (the root screen), a plain lv_obj_create() container
     * picks up the theme's default padding - left un-zeroed, every
     * absolutely-positioned child (cog2, btn_engine_off, ...) would sit
     * shifted from where the identical coordinates land on scr */
    lv_obj_set_style_pad_all(off_overlay, 0, 0);
    lv_obj_clear_flag(off_overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (g_display_role == DISPLAY_ROLE_PRIMARY) {
        lv_obj_t *btn_power = lv_btn_create(off_overlay);
        lv_obj_set_size(btn_power, 140, 140);
        lv_obj_center(btn_power);
        lv_obj_set_style_radius(btn_power, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn_power, lv_color_hex(0x142838), 0);
        lv_obj_set_style_border_color(btn_power, lv_color_hex(0x33475c), 0);
        lv_obj_set_style_border_width(btn_power, 2, 0);
        lv_obj_add_event_cb(btn_power, power_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *pglyph = lv_label_create(btn_power);
        lv_label_set_text(pglyph, LV_SYMBOL_POWER);
        lv_obj_set_style_text_font(pglyph, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(pglyph, lv_color_hex(0x8fa3b8), 0);
        lv_obj_center(pglyph);

        lv_obj_t *cog2 = lv_btn_create(off_overlay);
        lv_obj_set_size(cog2, 48, 48);
        lv_obj_align(cog2, LV_ALIGN_TOP_LEFT, 8, 11);
        lv_obj_set_style_bg_opa(cog2, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(cog2, 0, 0);
        lv_obj_add_event_cb(cog2, settings_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cog2g = lv_label_create(cog2);
        lv_label_set_text(cog2g, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(cog2g, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(cog2g, lv_color_hex(0x556677), 0);
        lv_obj_center(cog2g);

        btn_engine_off = lv_btn_create(off_overlay);
        lv_obj_set_size(btn_engine_off, 200, 34);
        lv_obj_set_pos(btn_engine_off, 130, 416);
        lv_obj_set_style_bg_opa(btn_engine_off, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn_engine_off, lv_color_hex(0x142838), 0);
        lv_obj_set_style_border_width(btn_engine_off, 1, 0);
        lv_obj_set_style_border_color(btn_engine_off, lv_color_hex(0x33475c), 0);
        lv_obj_set_style_radius(btn_engine_off, 8, 0);
        lv_obj_set_style_shadow_width(btn_engine_off, 0, 0);
        lv_obj_add_event_cb(btn_engine_off, engine_picker_cb, LV_EVENT_CLICKED, NULL);
        lbl_engine_off = lv_label_create(btn_engine_off);
        lv_label_set_text(lbl_engine_off, "E0 --");
        lv_obj_set_style_text_color(lbl_engine_off, lv_color_hex(0x556677), 0);
        lv_label_set_long_mode(lbl_engine_off, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_engine_off, 160);
        lv_obj_align(lbl_engine_off, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *chev_engine_off = lv_label_create(btn_engine_off);
        lv_label_set_text(chev_engine_off, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_color(chev_engine_off, lv_color_hex(0x556677), 0);
        lv_obj_align(chev_engine_off, LV_ALIGN_RIGHT_MID, -8, 0);

        /* OTA: low-key, off_overlay only (never over the live dashboard -
         * see check_for_update_tick()'s comment on why this matters).
         * Hidden by default, shown by ui_tick() when
         * g_helm_update_available. Tapping it does NOT start anything
         * directly - ota_update_available_cb() re-checks every engine is
         * off and shows the safety warning first. */
        btn_update_available = lv_btn_create(off_overlay);
        lv_obj_set_size(btn_update_available, 220, 50);
        lv_obj_align(btn_update_available, LV_ALIGN_TOP_RIGHT, -8, 8);
        lv_obj_set_style_bg_color(btn_update_available, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(btn_update_available, ota_update_available_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *upd_l = lv_label_create(btn_update_available);
        lv_label_set_text(upd_l, "Update Available");
        lv_obj_center(upd_l);
        lv_obj_add_flag(btn_update_available, LV_OBJ_FLAG_HIDDEN);

        /* manual "check now" - independent of the ~24h auto-check in
         * check_for_update_tick(). Always visible (not gated on anything),
         * sits just under btn_update_available whether or not that one is
         * currently shown. See check_update_now_cb()/g_force_update_check. */
        lv_obj_t *btn_check_update = lv_btn_create(off_overlay);
        lv_obj_set_size(btn_check_update, 220, 36);
        lv_obj_align(btn_check_update, LV_ALIGN_TOP_RIGHT, -8, 64);
        lv_obj_set_style_bg_color(btn_check_update, lv_color_hex(0x16283c), 0);
        lv_obj_add_event_cb(btn_check_update, check_update_now_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *chk_l = lv_label_create(btn_check_update);
        lv_label_set_text(chk_l, "Check for Updates");
        lv_obj_center(chk_l);

        lbl_check_status = lv_label_create(off_overlay);
        lv_label_set_text(lbl_check_status, "");
        lv_obj_set_style_text_color(lbl_check_status, lv_color_hex(0x8fa3b8), 0);
        lv_obj_align(lbl_check_status, LV_ALIGN_TOP_RIGHT, -8, 104);
        lv_obj_add_flag(lbl_check_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* secondary: no power button (no ignition authority to wake
         * itself), no cog (factory reset only shows up once an engine
         * is actually on), no engine picker - purely passive until the
         * primary turns something on. lbl_engine_off/lbl_no_engine_ip
         * still need real objects for ui_tick's text updates, just kept
         * invisible. */
        lv_obj_t *wait_l = lv_label_create(off_overlay);
        lv_label_set_text(wait_l, "Secondary display\nwaiting for engine...");
        lv_obj_set_style_text_align(wait_l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(wait_l, lv_color_hex(0x556677), 0);
        lv_obj_center(wait_l);

        lbl_engine_off = lv_label_create(off_overlay);
        lv_label_set_text(lbl_engine_off, "E0 --");
        lv_obj_add_flag(lbl_engine_off, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- no-engine overlay: topmost of all ---- */
    no_engine_overlay = lv_obj_create(scr);
    lv_obj_set_size(no_engine_overlay, 800, 480);
    lv_obj_set_pos(no_engine_overlay, 0, 0);
    lv_obj_set_style_bg_color(no_engine_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(no_engine_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(no_engine_overlay, 0, 0);
    lv_obj_set_style_radius(no_engine_overlay, 0, 0);
    /* see off_overlay above - same default-theme-padding fix */
    lv_obj_set_style_pad_all(no_engine_overlay, 0, 0);
    lv_obj_clear_flag(no_engine_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ne = lv_label_create(no_engine_overlay);
    lv_label_set_text(ne, "NO ENGINE DETECTED");
    lv_obj_set_style_text_font(ne, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ne, lv_color_hex(0x8fa3b8), 0);
    lv_obj_align(ne, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *ne2 = lv_label_create(no_engine_overlay);
    lv_label_set_text(ne2, "Listening on CAN bus...");
    lv_obj_set_style_text_color(ne2, lv_color_hex(0x556677), 0);
    lv_obj_align(ne2, LV_ALIGN_CENTER, 0, 20);

    lbl_no_engine_ip = lv_label_create(no_engine_overlay);
    lv_label_set_text(lbl_no_engine_ip, "");
    lv_obj_set_style_text_color(lbl_no_engine_ip, lv_color_hex(0x8fa3b8), 0);
    lv_obj_align(lbl_no_engine_ip, LV_ALIGN_BOTTOM_MID, 0, -20);

    if (g_display_role == DISPLAY_ROLE_PRIMARY) {
        lv_obj_t *cog3 = lv_btn_create(no_engine_overlay);
        lv_obj_set_size(cog3, 48, 48);
        lv_obj_align(cog3, LV_ALIGN_TOP_LEFT, 8, 11);
        lv_obj_set_style_bg_opa(cog3, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(cog3, 0, 0);
        lv_obj_add_event_cb(cog3, settings_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cog3g = lv_label_create(cog3);
        lv_label_set_text(cog3g, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(cog3g, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(cog3g, lv_color_hex(0x556677), 0);
        lv_obj_center(cog3g);

        btn_engine_noeng = lv_btn_create(no_engine_overlay);
        lv_obj_set_size(btn_engine_noeng, 200, 34);
        lv_obj_set_pos(btn_engine_noeng, 130, 416);
        lv_obj_set_style_bg_opa(btn_engine_noeng, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn_engine_noeng, lv_color_hex(0x142838), 0);
        lv_obj_set_style_border_width(btn_engine_noeng, 1, 0);
        lv_obj_set_style_border_color(btn_engine_noeng, lv_color_hex(0x33475c), 0);
        lv_obj_set_style_radius(btn_engine_noeng, 8, 0);
        lv_obj_set_style_shadow_width(btn_engine_noeng, 0, 0);
        lv_obj_add_event_cb(btn_engine_noeng, engine_picker_cb, LV_EVENT_CLICKED, NULL);
        lbl_engine_noeng = lv_label_create(btn_engine_noeng);
        lv_label_set_text(lbl_engine_noeng, "E0 --");
        lv_obj_set_style_text_color(lbl_engine_noeng, lv_color_hex(0x556677), 0);
        lv_label_set_long_mode(lbl_engine_noeng, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl_engine_noeng, 160);
        lv_obj_align(lbl_engine_noeng, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *chev_engine_noeng = lv_label_create(btn_engine_noeng);
        lv_label_set_text(chev_engine_noeng, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_color(chev_engine_noeng, lv_color_hex(0x556677), 0);
        lv_obj_align(chev_engine_noeng, LV_ALIGN_RIGHT_MID, -8, 0);

        /* OTA - same pair/behavior as off_overlay's, see btn_update_
         * available_ne's declaration comment. */
        btn_update_available_ne = lv_btn_create(no_engine_overlay);
        lv_obj_set_size(btn_update_available_ne, 220, 50);
        lv_obj_align(btn_update_available_ne, LV_ALIGN_TOP_RIGHT, -8, 8);
        lv_obj_set_style_bg_color(btn_update_available_ne, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(btn_update_available_ne, ota_update_available_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *upd_l_ne = lv_label_create(btn_update_available_ne);
        lv_label_set_text(upd_l_ne, "Update Available");
        lv_obj_center(upd_l_ne);
        lv_obj_add_flag(btn_update_available_ne, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *btn_check_update_ne = lv_btn_create(no_engine_overlay);
        lv_obj_set_size(btn_check_update_ne, 220, 36);
        lv_obj_align(btn_check_update_ne, LV_ALIGN_TOP_RIGHT, -8, 64);
        lv_obj_set_style_bg_color(btn_check_update_ne, lv_color_hex(0x16283c), 0);
        lv_obj_add_event_cb(btn_check_update_ne, check_update_now_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *chk_l_ne = lv_label_create(btn_check_update_ne);
        lv_label_set_text(chk_l_ne, "Check for Updates");
        lv_obj_center(chk_l_ne);

        lbl_check_status_ne = lv_label_create(no_engine_overlay);
        lv_label_set_text(lbl_check_status_ne, "");
        lv_obj_set_style_text_color(lbl_check_status_ne, lv_color_hex(0x8fa3b8), 0);
        lv_obj_align(lbl_check_status_ne, LV_ALIGN_TOP_RIGHT, -8, 104);
        lv_obj_add_flag(lbl_check_status_ne, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* secondary: no cog here either - stays passive until an engine
         * shows up on the bus at all, same reasoning as the off overlay */
        lbl_engine_noeng = lv_label_create(no_engine_overlay);
        lv_label_set_text(lbl_engine_noeng, "E0 --");
        lv_obj_add_flag(lbl_engine_noeng, LV_OBJ_FLAG_HIDDEN);
    }

    lv_timer_create(ui_tick, 200, NULL);   /* 5 Hz, alarm flash 2.5 Hz */
}
#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

/* ==================== CYD compact secondary screen ====================
 * Small 320x240 glow/start/silence-only screen for the CYD targets (see
 * board_select.h) - CYD is unconditionally DISPLAY_ROLE_SECONDARY (forced
 * in setup()), so it never needs the wizard, PIN lock, ignition control,
 * or create_ui()'s 800x480 arc gauge. Own widget globals (cyd_* prefix)
 * and own tick function below, kept fully separate from create_ui()/
 * ui_tick() rather than merged via null-checks, per this file's "ui_tick
 * must stay change-guarded" discipline (see CLAUDE.md). Shares the same
 * update_engine_state()/glow_cb/start_cb/mute_cb/current_caps()/
 * any_engine_present() the HELM screen uses - only the drawing differs. */

static lv_obj_t *cyd_lbl_engine, *cyd_lbl_rpm, *cyd_lbl_temp, *cyd_lbl_oil;
static lv_obj_t *cyd_btn_glow, *cyd_btn_start, *cyd_btn_mute, *cyd_btn_auto;
static lv_obj_t *cyd_lbl_auto;   /* doubles as phase status, same reasoning
                                   * as lbl_auto in create_ui() */
static lv_obj_t *cyd_off_overlay, *cyd_no_engine_overlay;

static void ui_tick_cyd(lv_timer_t *t)
{
    (void)t;
    flash_on = !flash_on;
    update_engine_state();

    uint16_t caps = current_caps();

    set_hidden(cyd_no_engine_overlay, any_engine_present());
    set_hidden(cyd_off_overlay, g_power);

    const char *name = engines[sel_engine].name[0] ? engines[sel_engine].name
                                                    : engtype_name(engines[sel_engine].type);
    static char last_name[MD_ENGINE_NAME_MAXLEN + 1] = "";
    if (strncmp(last_name, name, sizeof(last_name)) != 0) {
        strncpy(last_name, name, sizeof(last_name) - 1);
        last_name[sizeof(last_name) - 1] = 0;
        lv_label_set_text(cyd_lbl_engine, name);
    }

    int rpm = eng.rpm;
    if (rpm < 0) rpm = 0;
    static int last_rpm = INT32_MIN;
    if (rpm != last_rpm) {
        last_rpm = rpm;
        lv_label_set_text_fmt(cyd_lbl_rpm, "%d", rpm);
    }
    bool overrev = g_active_alarms & ALM_OVERREV;
    static int last_rpm_state = -1;   /* 0 normal, 1 red, 2/3 overrev flash */
    int rpm_state = overrev ? (((silenced_mask & ALM_OVERREV) || flash_on) ? 2 : 3)
                            : ((rpm >= RPM_RED_V) ? 1 : 0);
    if (rpm_state != last_rpm_state) {
        last_rpm_state = rpm_state;
        lv_color_t c = (rpm_state == 0) ? lv_color_hex(0xffffff)
                     : (rpm_state == 3) ? lv_color_hex(0x441111)
                                        : lv_color_hex(0xe24b4a);
        if (rpm_state == 2) c = lv_color_hex(0xff2222);
        lv_obj_set_style_text_color(cyd_lbl_rpm, c, 0);
    }

    static int last_temp = INT32_MIN;
    if ((int)eng.temp_c != last_temp) {
        last_temp = (int)eng.temp_c;
        lv_label_set_text_fmt(cyd_lbl_temp, "%d\xC2\xB0""C", last_temp);
    }
    static int last_temp_state = -1;  /* 0 ok, 1 flash-on, 2 flash-off, 3 silenced */
    int temp_state = !eng.temp_alarm ? 0
                   : (silenced_mask & ALM_TEMP) ? 3
                   : (flash_on ? 1 : 2);
    if (temp_state != last_temp_state) {
        last_temp_state = temp_state;
        lv_obj_set_style_text_color(cyd_lbl_temp,
            (temp_state == 0) ? lv_color_hex(0x00cc66)
          : (temp_state == 2) ? lv_color_hex(0x441111)
                              : lv_color_hex(0xff2222), 0);
    }

    static int last_oil10 = INT32_MIN;
    int oil10 = (int)lroundf(eng.oil_bar * 10.0f);
    if (oil10 != last_oil10) {
        last_oil10 = oil10;
        lv_label_set_text_fmt(cyd_lbl_oil, "%d.%d bar", oil10 / 10, oil10 % 10);
    }
    static int last_press_state = -1;
    int press_state = !eng.press_alarm ? 0
                    : (silenced_mask & ALM_PRESS) ? 3
                    : (flash_on ? 1 : 2);
    if (press_state != last_press_state) {
        last_press_state = press_state;
        lv_obj_set_style_text_color(cyd_lbl_oil,
            (press_state == 0) ? lv_color_hex(0x00cc66)
          : (press_state == 2) ? lv_color_hex(0x441111)
                               : lv_color_hex(0xff2222), 0);
    }

    set_hidden(cyd_btn_glow,  !(caps & CAP_GLOW));
    set_hidden(cyd_btn_start, !(caps & CAP_START));
    set_hidden(cyd_btn_auto,  !(caps & CAP_START));

    bool cyd_autostart_active = (autostart_state != AUTOSTART_IDLE);
    static int last_cyd_autostart_active = -1;
    if ((int)cyd_autostart_active != last_cyd_autostart_active) {
        last_cyd_autostart_active = (int)cyd_autostart_active;
        lv_label_set_text(cyd_lbl_auto, cyd_autostart_active ? "CANCEL AUTOSTART" : "AUTOSTART");
        lv_obj_set_style_bg_color(cyd_btn_auto,
            cyd_autostart_active ? lv_color_hex(0xb35900) : lv_color_hex(0x33475c), 0);
    }

    set_hidden(cyd_btn_mute, !g_active_alarms);
    if (g_active_alarms) {
        static int last_mute_state = -1;  /* 0 acked, 1 flash-on, 2 flash-off */
        bool unacked = g_active_alarms & ~silenced_mask;
        int mute_state = !unacked ? 0 : (flash_on ? 1 : 2);
        if (mute_state != last_mute_state) {
            last_mute_state = mute_state;
            lv_obj_set_style_bg_color(cyd_btn_mute,
                (mute_state == 0) ? lv_color_hex(0x33475c)
              : (mute_state == 1) ? lv_color_hex(0xcc8800)
                                  : lv_color_hex(0x664400), 0);
        }
    }
}

static lv_obj_t *cyd_make_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 96, 64);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_ALL, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

static void create_ui_cyd(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a1929), 0);

    cyd_lbl_engine = lv_label_create(scr);
    lv_label_set_text(cyd_lbl_engine, "--");
    lv_obj_set_style_text_color(cyd_lbl_engine, lv_color_hex(0x8fa3b8), 0);
    lv_obj_align(cyd_lbl_engine, LV_ALIGN_TOP_MID, 0, 4);

    cyd_lbl_rpm = lv_label_create(scr);
    lv_obj_set_style_text_font(cyd_lbl_rpm, &lv_font_montserrat_48, 0);
    lv_label_set_text(cyd_lbl_rpm, "0");
    lv_obj_set_style_text_color(cyd_lbl_rpm, lv_color_hex(0xffffff), 0);
    lv_obj_align(cyd_lbl_rpm, LV_ALIGN_TOP_MID, 0, 24);

    cyd_lbl_temp = lv_label_create(scr);
    lv_label_set_text(cyd_lbl_temp, "-- C");
    lv_obj_align(cyd_lbl_temp, LV_ALIGN_TOP_LEFT, 8, 84);

    cyd_lbl_oil = lv_label_create(scr);
    lv_label_set_text(cyd_lbl_oil, "-- bar");
    lv_obj_align(cyd_lbl_oil, LV_ALIGN_TOP_RIGHT, -8, 84);

    /* AUTOSTART - the 3 existing bottom buttons already use ~94% of the
     * 320px width, no room for a 4th same-size button there. Uses the
     * empty strip between the temp/oil readouts (end ~y104) and the
     * button row (starts ~y170) instead - a thin full-width bar. */
    cyd_btn_auto = lv_btn_create(scr);
    lv_obj_set_size(cyd_btn_auto, 300, 44);
    lv_obj_align(cyd_btn_auto, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_add_event_cb(cyd_btn_auto, autostart_cb, LV_EVENT_CLICKED, NULL);
    cyd_lbl_auto = lv_label_create(cyd_btn_auto);
    lv_label_set_text(cyd_lbl_auto, "AUTOSTART");
    lv_obj_center(cyd_lbl_auto);

    cyd_btn_glow  = cyd_make_btn(scr, "GLOW",  glow_cb);
    cyd_btn_start = cyd_make_btn(scr, "START", start_cb);
    cyd_btn_mute  = cyd_make_btn(scr, "MUTE",  mute_cb);
    lv_obj_align(cyd_btn_glow,  LV_ALIGN_BOTTOM_LEFT,   6, -6);
    lv_obj_align(cyd_btn_start, LV_ALIGN_BOTTOM_MID,    0, -6);
    lv_obj_align(cyd_btn_mute,  LV_ALIGN_BOTTOM_RIGHT, -6, -6);

    /* power-off overlay: black screen, topmost until the primary display
     * turns an engine on - no power/settings/engine-picker controls,
     * secondary has no ignition authority to wake itself with (matches
     * create_ui()'s secondary-role off_overlay reasoning). */
    cyd_off_overlay = lv_obj_create(scr);
    lv_obj_set_size(cyd_off_overlay, 320, 240);
    lv_obj_set_pos(cyd_off_overlay, 0, 0);
    lv_obj_set_style_bg_color(cyd_off_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cyd_off_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cyd_off_overlay, 0, 0);
    lv_obj_set_style_radius(cyd_off_overlay, 0, 0);
    lv_obj_set_style_pad_all(cyd_off_overlay, 0, 0);
    lv_obj_clear_flag(cyd_off_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *wait_l = lv_label_create(cyd_off_overlay);
    lv_label_set_text(wait_l, "waiting for engine...");
    lv_obj_set_style_text_color(wait_l, lv_color_hex(0x556677), 0);
    lv_obj_center(wait_l);

    /* no-engine overlay: topmost of all */
    cyd_no_engine_overlay = lv_obj_create(scr);
    lv_obj_set_size(cyd_no_engine_overlay, 320, 240);
    lv_obj_set_pos(cyd_no_engine_overlay, 0, 0);
    lv_obj_set_style_bg_color(cyd_no_engine_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cyd_no_engine_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cyd_no_engine_overlay, 0, 0);
    lv_obj_set_style_radius(cyd_no_engine_overlay, 0, 0);
    lv_obj_set_style_pad_all(cyd_no_engine_overlay, 0, 0);
    lv_obj_clear_flag(cyd_no_engine_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ne = lv_label_create(cyd_no_engine_overlay);
    lv_label_set_text(ne, "NO ENGINE\nDETECTED");
    lv_obj_set_style_text_align(ne, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ne, lv_color_hex(0x8fa3b8), 0);
    lv_obj_center(ne);

    lv_timer_create(ui_tick_cyd, 200, NULL);   /* 5 Hz, alarm flash 2.5 Hz */
}

/* ==================== CAN (TWAI) ==================== */

static bool can_ok = false;
static uint8_t can_busoff_strikes = 0;

static void can_setup(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    g.tx_queue_len = 8;
    g.rx_queue_len = 16;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
        /* pull RX high (recessive) so a missing transceiver reads as an
         * idle bus instead of noise that storms the error interrupts */
        if (PIN_CAN_RX < 34)   /* GPIO 34-39 are input-only pads with no internal pull-up */
            gpio_set_pull_mode((gpio_num_t)PIN_CAN_RX, GPIO_PULLUP_ONLY);
        can_ok = true;
        Serial.printf("CAN: started @250k (TX=%d, RX=%d)\n", PIN_CAN_TX, PIN_CAN_RX);
    } else {
        Serial.println("CAN: driver failed to start - running without CAN");
    }
}

#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* ==================== direct-wire supplementary transport (see wired_bus.h) ====================
 * UART1 - a completely separate hardware peripheral from UART0 (the
 * CH340 debug port this file's LoggingSerial/`#define Serial` machinery
 * wraps), so this never interacts with the serial-log capture above.
 * Always started, matching can_setup()'s own "always try, note whether
 * it worked" shape - see wired_bus.h's header comment for why this is
 * safe to leave always-on rather than gating behind a toggle. */
static HardwareSerial WiredSerial(1);
static bool           g_wired_ok = false;

static void wired_setup(void)
{
    WiredSerial.begin(WIRED_BAUD, SERIAL_8N1, PIN_WIRED_RX, PIN_WIRED_TX);
    g_wired_ok = true;
    Serial.printf("Wired: link started @%d baud (TX=%d, RX=%d)\n",
        WIRED_BAUD, PIN_WIRED_TX, PIN_WIRED_RX);
}
#endif

/* bus_send() - the actual transport-routing implementation - is defined
 * later in this file (after the ESP-NOW pairing section, which it
 * depends on for HELM builds) but used starting here; Arduino
 * auto-generates a prototype for every top-level function, so the
 * physical ordering doesn't matter for that - only the enroll_slot_t
 * type/engine_slots variable it also needs are required to already be
 * declared by the time bus_send() is defined, which they are (right
 * below). See bus_send()'s own comment for the transport logic. */

/* ==================== bus enrollment (master-side, PRIMARY only) ====================
 * HELM is always the bus master: every other node (engine CTRL boards,
 * the alarmer) gets its working index assigned here rather than
 * hardcoded on that board. Keyed by MAC so a replaced/rebooted board
 * gets the SAME index back - persisted in NVS so this survives a HELM
 * reboot too, not just the slave's. See can_protocol.h's ENROLLMENT
 * doc comment for the full handshake. A secondary display never runs
 * any of this - it's a bus listener, same as it has no ignition/stop
 * authority elsewhere. */

/* which wire this slot's traffic actually arrives on - learned from
 * inbound frames (see bus_espnow_stamp_transport() near bus_send()
 * below), not fixed at enrollment time, so it self-heals if HELM alone
 * reboots while a remote node keeps broadcasting without re-enrolling. */
enum { BUS_TRANSPORT_UNKNOWN = 0, BUS_TRANSPORT_CAN, BUS_TRANSPORT_ESPNOW, BUS_TRANSPORT_WIRED };

/* Tagged struct (NOT a bare `typedef struct {...} enroll_slot_t;`) -
 * enroll_slot_for_mac() below returns a pointer to this type, and
 * Arduino's auto-generated prototypes get hoisted above ALL user code,
 * including this typedef - a bare-name return type in that hoisted
 * prototype fails to compile ("does not name a type") since the alias
 * isn't visible yet at that point. The elaborated form `struct
 * enroll_slot_t` doesn't have that problem (same fix already used for
 * sim_engine_t in can_sim.ino - see that file's comment). */
typedef struct enroll_slot_t {
    bool    has_mac;
    uint8_t mac[6];             /* CAN-protocol enrollment identity - for a can_sim-style
                                 * board this is a SYNTHETIC per-role MAC (derive_sim_mac()),
                                 * not the same value as espnow_peer_mac below */
    uint8_t transport;          /* BUS_TRANSPORT_* */
    uint8_t espnow_peer_mac[6]; /* the sender's real radio MAC, only meaningful when
                                 * transport==BUS_TRANSPORT_ESPNOW - multiple slots can
                                 * legitimately share the same value (one physical board
                                 * emulating several synthetic CAN identities) */
} enroll_slot_t;

static enroll_slot_t engine_slots[MD_MAX_ENGINES];
static enroll_slot_t alarmer_slots[MD_MAX_ALARMERS];

static void enroll_mac_to_hex(const uint8_t *mac, char *out /* >=13 bytes */)
{
    for (int j = 0; j < 6; j++) snprintf(&out[j * 2], 3, "%02X", mac[j]);
}

static void enroll_hex_to_mac(const char *hex, uint8_t *mac)
{
    char byte_str[3] = {0};
    for (int j = 0; j < 6; j++) {
        byte_str[0] = hex[j * 2];
        byte_str[1] = hex[j * 2 + 1];
        mac[j] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
}

static void enroll_load_from_nvs(void)
{
    if (!prefs_ok) return;
    char key[8], hex[13];
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        snprintf(key, sizeof(key), "encM%d", i);
        String h = prefs.getString(key, "");
        if (h.length() == 12) {
            strncpy(hex, h.c_str(), 12); hex[12] = 0;
            enroll_hex_to_mac(hex, engine_slots[i].mac);
            engine_slots[i].has_mac = true;
        }
    }
    for (int i = 0; i < MD_MAX_ALARMERS; i++) {
        snprintf(key, sizeof(key), "almM%d", i);
        String h = prefs.getString(key, "");
        if (h.length() == 12) {
            strncpy(hex, h.c_str(), 12); hex[12] = 0;
            enroll_hex_to_mac(hex, alarmer_slots[i].mac);
            alarmer_slots[i].has_mac = true;
        }
    }
}

/* HELM/primary only - CYD has no Settings screen, keeps the compiled-in
 * as_*[] defaults untouched (see those arrays' declaration comment). */
static void autostart_load_from_nvs(void)
{
    if (!prefs_ok) return;
    char key[8];
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        snprintf(key, sizeof(key), "asG%d", i);
        as_glow_s[i]  = prefs.getUChar(key, as_glow_s[i]);
        snprintf(key, sizeof(key), "asC%d", i);
        as_crank_s[i] = prefs.getUChar(key, as_crank_s[i]);
        snprintf(key, sizeof(key), "asW%d", i);
        as_wait_s[i]  = prefs.getUChar(key, as_wait_s[i]);
        snprintf(key, sizeof(key), "asR%d", i);
        as_retries[i] = prefs.getUChar(key, as_retries[i]);
    }
}

/* find this MAC's existing slot, or claim the first free one and persist
 * it; returns ASSIGNED_ID_NONE (still a valid response - it's what gets
 * wired onto the bus) if the pool for this node_type is full */
static uint8_t enroll_find_or_assign(const uint8_t *mac, uint8_t node_type)
{
    enroll_slot_t *slots;
    int n;
    const char *prefix;

    if (node_type == NODE_TYPE_ENGINE_CTRL) {
        slots = engine_slots;  n = MD_MAX_ENGINES;  prefix = "encM";
    } else if (node_type == NODE_TYPE_ALARMER) {
        slots = alarmer_slots; n = MD_MAX_ALARMERS; prefix = "almM";
    } else {
        return ASSIGNED_ID_NONE;   /* unrecognized node type */
    }

    for (int i = 0; i < n; i++)
        if (slots[i].has_mac && md_mac_eq(slots[i].mac, mac)) return (uint8_t)i;

    for (int i = 0; i < n; i++) {
        if (!slots[i].has_mac) {
            slots[i].has_mac = true;
            memcpy(slots[i].mac, mac, 6);
            if (prefs_ok) {
                char key[8], hex[13];
                snprintf(key, sizeof(key), "%s%d", prefix, i);
                enroll_mac_to_hex(mac, hex);
                prefs.putString(key, hex);
            }
            return (uint8_t)i;
        }
    }
    return ASSIGNED_ID_NONE;   /* pool full */
}

/* read-only counterpart to enroll_find_or_assign() - the same linear
 * scan without the mutating assign-if-not-found branch, searching BOTH
 * pools since the caller (ESP-NOW transport stamping) doesn't know
 * node_type up front. Returns NULL if this MAC hasn't been enrolled
 * (shouldn't happen for a caller invoked right after
 * enroll_find_or_assign() succeeded on the same MAC, but the enrollment
 * pool could theoretically have been full). */
static struct enroll_slot_t *enroll_slot_for_mac(const uint8_t *mac)
{
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        if (engine_slots[i].has_mac && md_mac_eq(engine_slots[i].mac, mac)) return &engine_slots[i];
    for (int i = 0; i < MD_MAX_ALARMERS; i++)
        if (alarmer_slots[i].has_mac && md_mac_eq(alarmer_slots[i].mac, mac)) return &alarmer_slots[i];
    return NULL;
}

static void send_enroll_assign(const uint8_t *mac, uint8_t assigned_id)
{
    uint8_t d[8] = {0};
    memcpy(d, mac, 6);
    d[6] = assigned_id;
    bus_send(MSG_ENROLL_ASSIGN, d, 8);
}

static void can_handle_rx(const twai_message_t *m)
{
    int e;

#if TARGET_BOARD == BOARD_HELM_S3_800x480
    espnow_relay_to_displays(m->identifier, m->data, m->data_length_code);
#endif

    if (m->identifier == MSG_ENROLL_REQUEST) {
        if (g_display_role == DISPLAY_ROLE_PRIMARY && m->data_length_code >= 7) {
            uint8_t assigned = enroll_find_or_assign(&m->data[0], m->data[6]);
            send_enroll_assign(&m->data[0], assigned);
        }
        return;
    }

    if (m->identifier >= MSG_OTA_ACK(0) && m->identifier < MSG_OTA_ACK(0) + MD_MAX_ENGINES) {
        /* only ever flips the state enum - never touches an LVGL widget
         * from here, this can run off the LVGL task (loop()'s can_poll()
         * or an ESP-NOW receive callback). remote_ota_modal_tick(), which
         * DOES run on the LVGL task (called from ui_tick()), notices the
         * state change and repaints. Same handoff shape as glow_held/
         * start_held. */
        int ae = (int)(m->identifier - MSG_OTA_ACK(0));
        bool accepted = (g_remote_ota_state == REMOTE_OTA_WAIT_ACK && ae == g_remote_ota_engine &&
            m->data_length_code >= 1 && m->data[0] == OTA_ACK_OK);
        Serial.printf("OTA: MSG_OTA_ACK received for engine %d (dlc=%d status=%d) - "
            "waiting_state=%d waiting_engine=%d - %s\n",
            ae, m->data_length_code, m->data_length_code >= 1 ? m->data[0] : -1,
            (int)g_remote_ota_state, g_remote_ota_engine, accepted ? "ACCEPTED" : "ignored");
        if (accepted) g_remote_ota_state = REMOTE_OTA_ACKED;
        return;
    }

    if ((e = md_engine_from_name(m->identifier)) >= 0) {
        if (e < MD_MAX_ENGINES && m->data_length_code >= 3) {
            uint8_t total_len = m->data[0];
            uint8_t chunk_idx = m->data[1];
            if (total_len > MD_ENGINE_NAME_MAXLEN) total_len = MD_ENGINE_NAME_MAXLEN;
            int off   = chunk_idx * NAME_CHUNK_BYTES;
            int avail = m->data_length_code - 2;
            for (int j = 0; j < avail && off + j < total_len; j++)
                engines[e].name[off + j] = (char)m->data[2 + j];
            engines[e].name[total_len] = 0;
        }
        return;
    }

    if ((e = md_engine_from_announce(m->identifier)) >= 0) {
        if (m->data_length_code >= 5 && e < MD_MAX_ENGINES) {
            engines[e].present   = true;
            engines[e].caps      = md_unpack_u16(&m->data[1]);
            engines[e].type      = m->data[3];
            engines[e].last_seen = millis();
            /* fw_build (OTA) is new - older firmware on the wire simply
             * won't fill bytes 5..6, dlc stays what it always was (5+),
             * so guard length separately rather than bumping the dlc>=5
             * check above and breaking pre-OTA senders */
            if (m->data_length_code >= 7)
                engines[e].fw_build = md_unpack_u16(&m->data[5]);
            if (m->data_length_code >= 8)
                engines[e].hw_id = m->data[7];
        }
        return;
    }

    if ((e = md_engine_from_telem(m->identifier)) >= 0) {
        /* ign_on tracked for every engine, regardless of selection - the
         * secondary display's engine picker needs to know this for
         * engines the panel doesn't currently have selected */
        if (e < MD_MAX_ENGINES && m->data_length_code >= 7)
            engines[e].ign_on = (m->data[6] & TFLAG_IGNITION_ON) != 0;

        if (e == sel_engine && m->data_length_code >= 7) {
            eng.rpm         = md_unpack_u16(&m->data[0]);
            eng.temp_c      = (int16_t)md_unpack_u16(&m->data[2]) / 10.0f;
            eng.oil_bar     = md_unpack_u16(&m->data[4]) / 100.0f;
            eng.temp_alarm  = m->data[6] & TFLAG_TEMP_ALARM;
            eng.press_alarm = m->data[6] & TFLAG_PRESS_ALARM;
            eng.ign_actual  = m->data[6] & TFLAG_IGNITION_ON;
            eng.glow_active = m->data[6] & TFLAG_GLOW_ACTIVE;
            eng.crank_active= m->data[6] & TFLAG_CRANK_ACTIVE;
            eng.stop_active = m->data[6] & TFLAG_STOP_ACTIVE;
            eng.last_telem_ms = millis();
        }
        return;
    }

    if ((e = md_engine_from_hours(m->identifier)) >= 0) {
        if (e == sel_engine && m->data_length_code >= 4) {
            eng.hours_x10  = md_unpack_u32(&m->data[0]);
            eng.hours_seen = true;
        }
        return;
    }
}

/* age out engines that stopped announcing - deliberately NOT inside
 * can_poll() (which no-ops entirely when !can_ok, e.g. a bench unit
 * running ESP-NOW-only with no CAN transceiver wired): ANNOUNCE/TELEM
 * over ESP-NOW arrive via espnow_on_recv()'s callback regardless of
 * can_ok and correctly set present=true/last_seen, but nothing ever
 * cleared present back to false if that lived only in can_poll() - a
 * disconnected/powered-off ESP-NOW-only node would look "present"
 * forever. Called unconditionally from loop(), both transports share it. */
static void engine_ageout_tick(void)
{
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        if (engines[i].present &&
            millis() - engines[i].last_seen > ENGINE_LOST_MS) {
            engines[i].present = false;
            engines[i].ign_on  = false;
            engines[i].name[0] = 0;
            /* if this was the SELECTED engine, wipe its live telemetry
             * too - the auto-select block below only calls
             * reset_telemetry() when switching to a DIFFERENT engine, so
             * without this, a stale non-zero eng.rpm (e.g. mid-crank-
             * twitter when the link dropped) survives the disconnect and
             * can still read as "running" once the SAME engine reconnects,
             * silently breaking the start button (start_cb() treats an
             * apparently-already-running engine as a stop attempt, which
             * does nothing for an engine with no CAP_STOP). */
            if (i == sel_engine) {
                reset_telemetry();
                silenced_mask = 0;
            }
        }
    }
}

static void can_poll(void)
{
    if (!can_ok) return;

    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        if (!m.rtr) can_handle_rx(&m);
    }

    /* bus health: recover from bus-off; auto-disable if hopeless */
    static uint32_t last_check = 0;
    if (millis() - last_check > 1000) {
        last_check = millis();
        twai_status_info_t s;
        if (twai_get_status_info(&s) == ESP_OK) {
            if (s.state == TWAI_STATE_BUS_OFF) {
                if (++can_busoff_strikes >= 5) {
                    /* no working bus (transceiver missing/unpowered?):
                     * shut CAN down so it can't eat CPU / starve WiFi */
                    twai_stop();
                    twai_driver_uninstall();
                    can_ok = false;
                    Serial.println("CAN: repeated bus-off - no transceiver? "
                                   "CAN disabled until reboot");
                } else {
                    twai_initiate_recovery();
                }
            } else if (s.state == TWAI_STATE_STOPPED) {
                twai_start();
            } else if (s.state == TWAI_STATE_RUNNING &&
                       s.tx_error_counter == 0 && s.rx_error_counter == 0) {
                can_busoff_strikes = 0;   /* healthy bus: forgive the past */
            }
        }
    }
}

/* NOTE: no can_ok guard here (deliberately removed) - bus_send() already
 * makes its own per-message CAN-vs-ESP-NOW routing decision internally,
 * so an early return here whenever CAN happens to be unhealthy would
 * silently drop glow/start/ignition/stop/heartbeat entirely, even with a
 * fully working ESP-NOW link - the same class of bug fixed in can_sim's
 * bus_send() (see its comment). */
static void can_send_commands(void)
{
    uint32_t now = millis();
    uint8_t d[2];
    d[1] = NODE_ID_HELM;   /* source: authority enforced by CTRL board */

    /* dead-man held messages every HOLD_RESEND_MS while pressed - BOTH
     * roles may command glow/start (CLAUDE.md's CYD note: glow/start
     * only, no ignition authority - a secondary is the same deal) */
    static uint32_t last_hold = 0;
    if (now - last_hold >= HOLD_RESEND_MS) {
        last_hold = now;
        if (glow_held)  { d[0] = 1; bus_send(MSG_CMD_GLOW_HELD(sel_engine),  d, 2); }
        if (start_held) { d[0] = 1; bus_send(MSG_CMD_START_HELD(sel_engine), d, 2); }
    }

    /* latched ignition desired-state: primary-only authority. On
     * change + slow refresh. */
    if (g_display_role == DISPLAY_ROLE_PRIMARY) {
        static bool     last_sent_ign = false;
        static uint32_t last_ign_tx   = 0;
        static bool     ign_ever_sent = false;
        if (ign_commanded != last_sent_ign || !ign_ever_sent ||
            now - last_ign_tx >= IGNITION_RESEND_MS) {
            d[0] = ign_commanded ? 1 : 0;
            bus_send(MSG_CMD_IGNITION(sel_engine), d, 2);
            last_sent_ign = ign_commanded;
            last_ign_tx   = now;
            ign_ever_sent = true;
        }

        /* STOP: primary-only authority, same dead-man cadence as
         * glow/start. g_can_stop (cached in ui_tick) already keeps
         * stop_held from ever being set unless the selected engine
         * announced CAP_STOP - this is just the transmit side. */
        static uint32_t last_stop_hold = 0;
        if (stop_held && now - last_stop_hold >= HOLD_RESEND_MS) {
            last_stop_hold = now;
            d[0] = 1;
            bus_send(MSG_CMD_STOP(sel_engine), d, 2);
        }
    }

    /* one-shot alarm silence (from MUTE): always consumed so the local
     * flash silences either way, only broadcast from the primary so a
     * secondary can't silence the bus-wide (audio) alarm on its own say */
    if (silence_req) {
        silence_req = false;
        if (g_display_role == DISPLAY_ROLE_PRIMARY) {
            d[0] = 1;
            bus_send(MSG_CMD_ALARM_SILENCE(sel_engine), d, 2);
        }
    }

    /* heartbeat: every display announces itself regardless of role */
    static uint32_t last_hb = 0;
    if (now - last_hb >= HEARTBEAT_MS) {
        last_hb = now;
        uint8_t hb[2] = { NODE_ID_HELM, (uint8_t)((now / 1000) & 0xFF) };
        bus_send(MSG_HB_HELM, hb, 2);
    }
}

/* ==================== ESP-NOW pairing (infrastructure only) ====================
 * HELM only for now (see espnow_pairing.h) - HELM is always the pairing
 * ACCEPTOR: normally ignores every ESP-NOW pairing request, and only
 * considers new (never-before-seen) MACs while g_pairing_mode is open
 * (armed by the "Wireless Pairing" button in Settings). A MAC already on
 * the allowlist is implicitly still paired forever, independent of
 * pairing mode - this handshake only decides who's on the allowlist, it
 * doesn't yet route any engine command/telemetry traffic. */
#if TARGET_BOARD == BOARD_HELM_S3_800x480

/* ESP_NOW_MAX_ENCRYPT_PEER_NUM (esp_now.h) is a hard radio-level cap of 6
 * simultaneously-encrypted peers - the allowlist can't exceed that once
 * every peer is encrypted (see below), so size it to match exactly
 * rather than allowing a pairing that could never actually work. */
#define ESPNOW_MAX_PAIRED 6

typedef struct {
    bool    has_mac;
    uint8_t mac[6];
    uint8_t lmk[ESPNOW_LMK_LEN];
    uint8_t node_type;   /* NODE_TYPE_* the peer announced when it paired (0 = unknown / paired
                          * before this field existed). NODE_TYPE_CYD peers get engine data relayed
                          * to them, see espnow_relay_to_displays(). */
} espnow_peer_slot_t;
static espnow_peer_slot_t espnow_peers[ESPNOW_MAX_PAIRED];

static void espnow_lmk_to_hex(const uint8_t *lmk, char *out /* >=33 bytes */)
{
    for (int j = 0; j < ESPNOW_LMK_LEN; j++) snprintf(&out[j * 2], 3, "%02X", lmk[j]);
}

static void espnow_hex_to_lmk(const char *hex, uint8_t *lmk)
{
    char byte_str[3] = {0};
    for (int j = 0; j < ESPNOW_LMK_LEN; j++) {
        byte_str[0] = hex[j * 2];
        byte_str[1] = hex[j * 2 + 1];
        lmk[j] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
}

/* pure NVS -> memory load, no esp_now_* calls - safe to call before
 * esp_now_init(). Registering these as actual encrypted esp_now peers
 * happens separately in espnow_setup(), after init. */
static void espnow_pairing_load_from_nvs(void)
{
    if (!prefs_ok || !g_fsec_have_key) return;
    uint8_t me[6];
    fsec_my_mac(me);
    char key[8], hex[33];
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
        snprintf(key, sizeof(key), "espM%d", i);
        String hm = prefs.getString(key, "");
        if (hm.length() == 12) {
            strncpy(hex, hm.c_str(), 12); hex[12] = 0;
            enroll_hex_to_mac(hex, espnow_peers[i].mac);
            fsec_lmk(me, espnow_peers[i].mac, espnow_peers[i].lmk);   /* derived, never stored */
            espnow_peers[i].has_mac = true;
            snprintf(key, sizeof(key), "espT%d", i);
            espnow_peers[i].node_type = prefs.getUChar(key, 0);
        }
    }
}

static int espnow_peer_find(const uint8_t *mac)
{
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++)
        if (espnow_peers[i].has_mac && md_mac_eq(espnow_peers[i].mac, mac)) return i;
    return -1;
}

static bool espnow_peer_known(const uint8_t *mac)
{
    return espnow_peer_find(mac) >= 0;
}

static void espnow_random_lmk(uint8_t *lmk)
{
    for (int i = 0; i < ESPNOW_LMK_LEN; i += 4) {
        uint32_t r = esp_random();
        memcpy(&lmk[i], &r, 4);
    }
}

/* claims the first free allowlist slot for a new MAC, generates a fresh
 * random LMK for it, and persists both. A MAC already known reuses its
 * existing LMK (no-op success, not a new key) - this matters for a
 * requester that lost its own pairing state and had to re-broadcast;
 * it needs the SAME key back, not a new one it'll never learn.
 *
 * Registers the peer UNENCRYPTED for now, not encrypted - the ACK that's
 * about to carry this key to the far end has to go out in the clear
 * (the far end doesn't have the key to decrypt an encrypted one yet, or
 * on a resync may have lost it). espnow_upgrade_tick() switches it over
 * afterwards, once that plaintext send has actually gone out - see
 * espnow_on_recv().
 *
 * Returns false if the allowlist is full or the radio's peer table
 * rejects the add (also capped at ESPNOW_MAX_PAIRED). lmk_out is filled
 * on success either way, for the caller to embed in the PAIR_MSG_ACK. */
static bool espnow_pairing_accept_mac(const uint8_t *mac, uint8_t node_type, uint8_t *lmk_out)
{
    int idx = espnow_peer_find(mac);
    if (idx < 0) {
        for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
            if (!espnow_peers[i].has_mac) { idx = i; break; }
        }
        if (idx < 0) return false;   /* allowlist full */
        espnow_peers[idx].has_mac = true;
        memcpy(espnow_peers[idx].mac, mac, 6);
        uint8_t me[6];
        fsec_my_mac(me);
        fsec_lmk(me, mac, espnow_peers[idx].lmk);   /* derived from the shared secret + both addresses */
    }
    espnow_peers[idx].node_type = node_type;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;   /* use whatever channel WiFi is already on */
    peer.encrypt = false;
    esp_err_t err = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        espnow_peers[idx].has_mac = false;   /* radio rejected it - don't persist a peer we can't use */
        return false;
    }

    if (prefs_ok) {
        char key[8], hex[33];
        snprintf(key, sizeof(key), "espM%d", idx);
        enroll_mac_to_hex(mac, hex);
        prefs.putString(key, hex);
        snprintf(key, sizeof(key), "espT%d", idx);
        prefs.putUChar(key, node_type);
    }

    memcpy(lmk_out, espnow_peers[idx].lmk, ESPNOW_LMK_LEN);
    return true;
}

/* deferred upgrade to encrypted - see espnow_pairing_accept_mac()'s
 * comment for why this can't happen immediately after esp_now_send().
 * espnow_on_recv() arms this; espnow_upgrade_tick() (called from
 * loop()) fires it PAIR_UPGRADE_DELAY_MS later, comfortably after the
 * plaintext ACK has actually gone out over the air. */
#define PAIR_UPGRADE_DELAY_MS 200
static uint8_t  g_pending_upgrade_mac[6];
static uint32_t g_pending_upgrade_at_ms = 0;   /* 0 = nothing pending */

static void espnow_upgrade_tick(void)
{
    if (!g_pending_upgrade_at_ms || millis() < g_pending_upgrade_at_ms) return;
    g_pending_upgrade_at_ms = 0;

    int idx = espnow_peer_find(g_pending_upgrade_mac);
    if (idx < 0) return;   /* cleared in the meantime */

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, espnow_peers[idx].mac, 6);
    peer.channel = 0;
    peer.encrypt = true;
    memcpy(peer.lmk, espnow_peers[idx].lmk, ESPNOW_LMK_LEN);
    if (esp_now_mod_peer(&peer) == ESP_OK)
        Serial.println("ESP-NOW: peer upgraded to encrypted");
    else
        Serial.println("ESP-NOW: failed to upgrade peer to encrypted - stays plaintext");
}

static int espnow_peer_count(void)
{
    int n = 0;
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++)
        if (espnow_peers[i].has_mac) n++;
    return n;
}

/* wipes the allowlist (in-memory + NVS) and the ESP-NOW peer table -
 * does NOT touch g_pairing_mode. A cleared peer that's still running
 * will notice on its own next attempt to reach HELM and, if it also
 * gets REPAIR'd (or was never paired to begin with), start broadcasting
 * PAIR_MSG_REQUEST again - this side alone doesn't reach out and tell it. */
static void espnow_pairing_clear_all(void)
{
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
        if (espnow_peers[i].has_mac) {
            esp_now_del_peer(espnow_peers[i].mac);
            espnow_peers[i].has_mac = false;
        }
        if (prefs_ok) {
            char key[8];
            snprintf(key, sizeof(key), "espM%d", i);
            prefs.remove(key);
            snprintf(key, sizeof(key), "espL%d", i);
            prefs.remove(key);
        }
    }
    Serial.println("ESP-NOW: all pairings cleared");
}

/* dispatches a validated bus frame into the exact same handler CAN uses
 * (can_handle_rx() is untouched by any of this ESP-NOW work), then
 * learns/refreshes which engine slot (if any) this frame belongs to, so
 * bus_send() knows to route that engine's future commands back over
 * ESP-NOW. Re-stamped on every inbound frame, not just at enrollment -
 * see enroll_slot_t's comment for why. */
static void espnow_handle_bus_frame(const uint8_t *peer_mac, const espnow_bus_frame_t *f)
{
    twai_message_t m = {};
    m.identifier = f->can_id;
    m.data_length_code = f->dlc;
    if (f->dlc) memcpy(m.data, f->data, f->dlc);
    can_handle_rx(&m);

    int e = md_engine_from_telem(f->can_id);
    if (e < 0) e = md_engine_from_hours(f->can_id);
    if (e < 0) e = md_engine_from_announce(f->can_id);
    if (e < 0) e = md_engine_from_name(f->can_id);
    if (e >= 0 && e < MD_MAX_ENGINES) {
        if (engine_slots[e].transport != BUS_TRANSPORT_ESPNOW)
            Serial.printf("ESP-NOW: engine %d transport -> ESPNOW (via id=0x%03lX from %02X:%02X:%02X:%02X:%02X:%02X)\n",
                e, (unsigned long)f->can_id, peer_mac[0], peer_mac[1], peer_mac[2],
                peer_mac[3], peer_mac[4], peer_mac[5]);
        engine_slots[e].transport = BUS_TRANSPORT_ESPNOW;
        memcpy(engine_slots[e].espnow_peer_mac, peer_mac, 6);
    } else if (f->can_id == MSG_ENROLL_REQUEST) {
        /* e isn't known yet - can_handle_rx() just ran
         * enroll_find_or_assign()/send_enroll_assign() using the
         * payload's (synthetic, can_sim-style) MAC; look up which slot
         * now owns that MAC and stamp it with the REAL radio MAC this
         * frame actually arrived from - those are two different values,
         * see enroll_slot_t's comment. */
        enroll_slot_t *slot = enroll_slot_for_mac(&f->data[0]);
        if (slot) {
            if (slot->transport != BUS_TRANSPORT_ESPNOW)
                Serial.printf("ESP-NOW: engine slot transport -> ESPNOW (via ENROLL_REQUEST from %02X:%02X:%02X:%02X:%02X:%02X)\n",
                    peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
            slot->transport = BUS_TRANSPORT_ESPNOW;
            memcpy(slot->espnow_peer_mac, peer_mac, 6);
        } else {
            Serial.printf("ESP-NOW: ENROLL_REQUEST from %02X:%02X:%02X:%02X:%02X:%02X - "
                          "no enroll_slot_for_mac() match yet (enroll_find_or_assign() may not have run)\n",
                peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        }
    }
}

/* Sends one frame over the direct-wire link (see wired_bus.h for the
 * framing layout) - point-to-point, so unlike espnow_send_bus_frame()
 * there's no peer MAC to address, just write the bytes. */
static void wired_send_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    uint8_t f[WIRED_FRAME_LEN] = {0};
    f[0] = WIRED_SYNC_BYTE;
    f[1] = (uint8_t)(id & 0xFF);
    f[2] = (uint8_t)((id >> 8) & 0xFF);
    f[3] = len;
    if (len) memcpy(&f[4], data, len);
    uint8_t chk = 0;
    for (int i = 1; i < 12; i++) chk ^= f[i];
    f[12] = chk;
    WiredSerial.write(f, WIRED_FRAME_LEN);
}

/* Same shape as espnow_handle_bus_frame() above, minus the peer-MAC
 * plumbing - point-to-point means there's only ever one possible remote
 * device on this link, so "transport==WIRED" alone is enough to route a
 * future unicast command back down it, no address needed. */
static void wired_handle_bus_frame(const twai_message_t *m)
{
    can_handle_rx(m);

    int e = md_engine_from_telem(m->identifier);
    if (e < 0) e = md_engine_from_hours(m->identifier);
    if (e < 0) e = md_engine_from_announce(m->identifier);
    if (e < 0) e = md_engine_from_name(m->identifier);
    if (e >= 0 && e < MD_MAX_ENGINES) {
        if (engine_slots[e].transport != BUS_TRANSPORT_WIRED)
            Serial.printf("Wired: engine %d transport -> WIRED (via id=0x%03lX)\n",
                e, (unsigned long)m->identifier);
        engine_slots[e].transport = BUS_TRANSPORT_WIRED;
    } else if (m->identifier == MSG_ENROLL_REQUEST) {
        enroll_slot_t *slot = enroll_slot_for_mac(&m->data[0]);
        if (slot) {
            if (slot->transport != BUS_TRANSPORT_WIRED)
                Serial.println("Wired: engine slot transport -> WIRED (via ENROLL_REQUEST)");
            slot->transport = BUS_TRANSPORT_WIRED;
        }
    }
}

/* Called from loop() - reads whatever bytes have arrived on the wired
 * UART and reassembles WIRED_FRAME_LEN-byte frames. Fixed-length-after-
 * sync framing (see wired_bus.h): once WIRED_SYNC_BYTE is seen, the next
 * WIRED_FRAME_LEN-1 bytes are taken as-is and checksummed - a bad
 * checksum just drops that one frame and goes back to hunting for the
 * next sync byte, so a single corrupted/dropped byte can't wedge the
 * parser, it just costs one lost frame. */
static void wired_bus_tick(void)
{
    static uint8_t buf[WIRED_FRAME_LEN];
    static uint8_t pos = 0;

    while (WiredSerial.available()) {
        uint8_t b = (uint8_t)WiredSerial.read();
        if (pos == 0) {
            if (b != WIRED_SYNC_BYTE) continue;   /* hunting for sync */
            buf[pos++] = b;
            continue;
        }
        buf[pos++] = b;
        if (pos < WIRED_FRAME_LEN) continue;

        pos = 0;   /* frame complete either way - reset for the next one */
        uint8_t chk = 0;
        for (int i = 1; i < 12; i++) chk ^= buf[i];
        if (chk != buf[12]) {
            Serial.println("Wired: frame checksum mismatch, dropped");
            continue;
        }

        twai_message_t m = {};
        m.identifier = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8);
        m.data_length_code = buf[3];
        if (m.data_length_code > 8) continue;   /* malformed, ignore */
        if (m.data_length_code) memcpy(m.data, &buf[4], m.data_length_code);

        wired_handle_bus_frame(&m);
    }
}

static void espnow_on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* deliberately NOT logging every rx: telemetry alone is ~20/sec and drowned out the lines that matter */
    if (!g_fsec_have_key) return;

    /* a board asking to join: its HELLO must carry a tag only a holder of the shared secret can make */
    if (len == (int)sizeof(espnow_hello_t) && data[0] == ESPNOW_MSG_HELLO) {
        espnow_hello_t hello;
        memcpy(&hello, data, sizeof(hello));
        uint8_t want[8];
        fsec_hello_tag(ESPNOW_MSG_HELLO, hello.mac, hello.node_type, NULL, want);
        if (!fsec_tag_equal(want, hello.tag, 8) || memcmp(hello.mac, info->src_addr, 6) != 0) {
            static uint32_t last_log = 0;
            if (millis() - last_log > 10000) {
                last_log = millis();
                Serial.printf("ESP-NOW: HELLO from %02X:%02X:%02X:%02X:%02X:%02X rejected - not made with our key\n",
                    info->src_addr[0], info->src_addr[1], info->src_addr[2],
                    info->src_addr[3], info->src_addr[4], info->src_addr[5]);
            }
            return;
        }
        uint8_t lmk[ESPNOW_LMK_LEN];
        bool known = espnow_peer_known(info->src_addr);
        if (!espnow_pairing_accept_mac(info->src_addr, hello.node_type, lmk)) {
            Serial.println("ESP-NOW: HELLO ignored - the peer table is full");
            return;
        }

        /* answer with our own tag so the board knows we are genuine too. Broadcast (not addressed): the board
         * has not set up the encrypted peer yet, and we may already hold an encrypted one for it. */
        espnow_hello_ack_t ack = {};
        ack.type = ESPNOW_MSG_HELLO_ACK;
        fsec_my_mac(ack.mac);
        ack.node_type = NODE_TYPE_HELM;
        memcpy(ack.peer_mac, hello.mac, 6);
        fsec_hello_tag(ESPNOW_MSG_HELLO_ACK, ack.mac, ack.node_type, ack.peer_mac, ack.tag);
        esp_now_send(ESPNOW_BROADCAST_MAC, (uint8_t *)&ack, sizeof(ack));

        memcpy(g_pending_upgrade_mac, info->src_addr, 6);
        g_pending_upgrade_at_ms = millis() + PAIR_UPGRADE_DELAY_MS;
        if (!known)
            Serial.printf("ESP-NOW: %02X:%02X:%02X:%02X:%02X:%02X joined type=%d (upgrading to encrypted)\n",
                info->src_addr[0], info->src_addr[1], info->src_addr[2],
                info->src_addr[3], info->src_addr[4], info->src_addr[5], (int)hello.node_type);
        return;
    }

    if (len == (int)sizeof(espnow_bus_frame_t)) {
        espnow_bus_frame_t f;
        memcpy(&f, data, sizeof(f));
        if (!espnow_peer_known(info->src_addr)) return;
        if (!fsec_frame_verify(&f, info->src_addr)) {
            static uint32_t last_log = 0;
            if (millis() - last_log > 10000) {
                last_log = millis();
                Serial.println("ESP-NOW: a bus frame failed its signature check (wrong key, or a replay) - dropped");
            }
            return;
        }
        espnow_handle_bus_frame(info->src_addr, &f);
    }
}

static void espnow_setup(void)
{
    if (!g_fsec_have_key) {
        Serial.println("ESP-NOW: OFF - no key set. Type  KEY <passphrase>  (12+ characters, the same on every board).");
        return;
    }
    espnow_pairing_load_from_nvs();

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW: init failed");
        return;
    }
    esp_now_register_recv_cb(espnow_on_recv);
    esp_now_register_send_cb(espnow_on_sent);
    uint8_t pmk[ESPNOW_LMK_LEN];
    fsec_pmk(pmk);
    esp_now_set_pmk(pmk);

    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    bcast.channel = 0;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    /* re-register every already-paired peer as an encrypted esp_now peer
     * - the NVS allowlist survives reboot, but esp_now's own in-memory
     * peer table does not, so this has to happen every boot. */
    int restored = 0;
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
        if (!espnow_peers[i].has_mac) continue;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, espnow_peers[i].mac, 6);
        peer.channel = 0;
        peer.encrypt = true;
        memcpy(peer.lmk, espnow_peers[i].lmk, ESPNOW_LMK_LEN);
        if (esp_now_add_peer(&peer) == ESP_OK) restored++;
        else Serial.println("ESP-NOW: failed to restore an encrypted peer on boot");
    }

    Serial.printf("ESP-NOW: ready (key fingerprint %08lX) - %d peer(s) restored\n",
        (unsigned long)fsec_fingerprint(), restored);
}

/* called from loop(): auto-closes the pairing window after PAIR_WINDOW_MS */
static void pairing_mode_tick(void)
{
    /* nothing to time out any more - there is no pairing window (see espnow_key_label()) */
}

/* ==================== OTA: update checking ====================
 * Primary only, and only when actually WiFi-joined (a bench unit running
 * ESP-NOW-only has no route to the manifest URL at all - checked via
 * WiFi.status() directly). g_helm_update_* are declared early in the
 * file (near btn_update_available) - ui_tick() reads them and is defined
 * long before this point, and only FUNCTION prototypes get auto-hoisted
 * by Arduino, never variable declarations (same rule that bit
 * g_can_disabled/g_pairing_mode earlier in this project). */
#define UPDATE_CHECK_INTERVAL_MS (24UL * 60 * 60 * 1000)   /* ~24h */

/* "Check for Updates" button (off_overlay AND no_engine_overlay, primary
 * only - shared by both screens' buttons) - just sets the force flag and
 * gives immediate feedback; the actual (blocking) HTTP fetch happens on
 * the next loop()/check_for_update_tick() pass, not here (this runs on
 * the LVGL task). ui_tick() below watches g_update_check_done_id for a
 * change to know a forced check finished and updates both status labels
 * with the result - only one of the two screens is ever visible at a
 * time, so updating both unconditionally is simpler than tracking which
 * one triggered it, and harmless (the hidden one's text just isn't seen
 * until that screen is shown). */
static void check_update_now_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    g_force_update_check = true;
    g_update_check_request_id++;
    if (lbl_check_status) {
        lv_label_set_text(lbl_check_status, "Checking...");
        lv_obj_clear_flag(lbl_check_status, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_check_status_ne) {
        lv_label_set_text(lbl_check_status_ne, "Checking...");
        lv_obj_clear_flag(lbl_check_status_ne, LV_OBJ_FLAG_HIDDEN);
    }
}

/* GitHub release URLs answer with a redirect to a signed URL on another host. Following it inside the
 * HTTP update library means a second TLS connection set up while the first is still being torn down, and on
 * the HELM (little contiguous internal RAM left once the UI is up) that failed with "connection refused".
 * So follow the redirect here by hand, one TLS connection at a time - each is freed before the next opens -
 * and hand the update library the final URL. Logs each hop with the free internal RAM. */
static String ota_resolve_url(const String &start_url)
{
    String url = start_url;
    for (int hop = 0; hop < 4; hop++) {
        if (!url.startsWith("https://")) return url;   /* plain http (a local test server): nothing to follow */
        WiFiClientSecure c;
        c.setInsecure();
        HTTPClient h;
        h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        const char *keys[] = {"Location"};
        h.collectHeaders(keys, 1);
        if (!h.begin(c, url)) {
            Serial.println("OTA: redirect resolve - http.begin() failed");
            return url;
        }
        int code = h.GET();
        String loc = h.header("Location");
        Serial.printf("OTA: hop %d -> HTTP %d (internal RAM %u, largest block %u, next url %u chars)\n",
            hop, code, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)loc.length());
        h.end();
        c.stop();
        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && loc.length()) {
            url = loc;
            continue;
        }
        return url;   /* 200, or an error the update call will report itself */
    }
    return url;
}

/* called from loop(): fetches OTA_MANIFEST_URL once per boot (a few
 * seconds after WiFi settles is fine - the interval check below just
 * naturally lets the first call through immediately) and every
 * UPDATE_CHECK_INTERVAL_MS after that. Deliberately skipped whenever a
 * dead-man hold is active (glow_held/start_held) - this does a blocking
 * HTTP GET on the same loop() task that's responsible for resending
 * MSG_CMD_GLOW_HELD/START_HELD every HOLD_RESEND_MS, and stalling that
 * mid-crank would abort a real start attempt. Deferring one tick and
 * trying again next time is harmless; this isn't time-critical. */
static void check_for_update_tick(void)
{
    if (g_display_role != DISPLAY_ROLE_PRIMARY) { g_force_update_check = false; return; }

    /* Captured up front (not just before the network call, as this used
     * to be) so EVERY exit path below - including the fast ones that
     * finish in under a millisecond, like "no WiFi" - can stamp g_update_
     * check_done_id. See that variable's own comment: a done_id bump is a
     * permanent state change ui_tick() can never miss, unlike the old
     * design of clearing g_force_update_check and hoping ui_tick() polls
     * at the right moment to catch the pulse. */
    bool     forced = g_force_update_check;
    uint32_t req_id = g_update_check_request_id;

    /* WiFi down means there's no route to the manifest at all - clear a
     * pending force-check here (rather than leaving it set) so the next
     * automatic interval check isn't skipped too; ui_tick() reads WiFi.
     * status() itself to show "No WiFi connection" for this specific
     * case. A glow/start hold, below, is different - that's a "not right
     * now" defer, not a "can't", so it leaves the force flag (and req_id)
     * set and retries next tick without stamping done_id. */
    if (WiFi.status() != WL_CONNECTED) {
        g_force_update_check = false;
        g_last_check_result = UPDATE_CHECK_RESULT_NO_WIFI;
        if (forced) g_update_check_done_id = req_id;
        return;
    }
    /* Deferred for the ENTIRE autostart sequence (autostart_state !=
     * IDLE), not just the instantaneous glow_held/start_held flags.
     * Confirmed on real hardware as the actual cause of "autostart gives
     * up too early" and occasional full reboots: autostart's WAITING
     * phase (between a failed crank and the next retry) has BOTH held
     * flags false, so a manifest check - automatic (once per boot, right
     * after WiFi settles) or forced - could start there. HTTPClient's
     * http.begin()/GET() is a synchronous blocking call with no way to
     * abort once started; if it's still in flight when autostart_tick()
     * (a completely separate task/timer) flips start_held back to true
     * for the retry, loop()'s can_send_commands() - the thing that
     * actually resends MSG_CMD_START_HELD every HOLD_RESEND_MS - stays
     * stalled behind the fetch, so the retry's first command frame goes
     * out late or not at all. can_sim/CTRL's own HOLD_TIMEOUT_MS (300ms)
     * dead-man logic then sees silence and drops the relay almost
     * immediately - "gives up too early" from the operator's point of
     * view. A long enough stall (slow/unreachable manifest host) can also
     * trip the loop task's watchdog and reboot the board outright, which
     * separately explains the intermittent full-reboot symptom. Checking
     * glow_held/start_held alone only prevented a check from *starting*
     * during an active hold - it did nothing to protect the gap between
     * autostart phases, which is exactly the window this closes. */
    if (glow_held || start_held || autostart_state != AUTOSTART_IDLE) return;

    static bool     ever_checked = false;
    static uint32_t last_check_ms = 0;
    uint32_t now = millis();
    if (!forced && ever_checked && now - last_check_ms < UPDATE_CHECK_INTERVAL_MS) return;
    ever_checked = true;
    last_check_ms = now;
    g_force_update_check = false;

    HTTPClient http;
    WiFiClient       plain_client;
    WiFiClientSecure tls_client;
    tls_client.setInsecure();   /* chain not verified - see ota_start_download_cb() */
    NetworkClient &client = (strncmp(OTA_MANIFEST_URL, "https://", 8) == 0)
        ? (NetworkClient &)tls_client : (NetworkClient &)plain_client;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, OTA_MANIFEST_URL)) {
        Serial.println("OTA: manifest http.begin() failed");
        g_last_check_result = UPDATE_CHECK_RESULT_FETCH_FAILED;
        if (forced) g_update_check_done_id = req_id;
        return;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OTA: manifest fetch failed, HTTP %d\n", code);
        http.end();
        g_last_check_result = UPDATE_CHECK_RESULT_FETCH_FAILED;
        if (forced) g_update_check_done_id = req_id;
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;   /* ArduinoJson 7.x unified API - no manual sizing */
    DeserializationError jerr = deserializeJson(doc, body);
    if (jerr) {
        Serial.printf("OTA: manifest JSON parse failed: %s\n", jerr.c_str());
        g_last_check_result = UPDATE_CHECK_RESULT_FETCH_FAILED;
        if (forced) g_update_check_done_id = req_id;
        return;
    }

    JsonObject helm = doc["helm"][HELM_HW_KEY];
    if (helm.isNull()) {
        Serial.println("OTA: manifest has no \"helm\" entry for this hardware (" HELM_HW_KEY ")");
        g_last_check_result = UPDATE_CHECK_RESULT_FETCH_FAILED;
        if (forced) g_update_check_done_id = req_id;
        return;
    }
    uint32_t remote_build = helm["build"] | 0;
    const char *url = helm["url"] | "";
    const char *md5 = helm["md5"] | "";

    if (remote_build > FW_BUILD && url[0]) {
        g_helm_update_available = true;
        g_helm_update_build = remote_build;
        strncpy(g_helm_update_url, url, sizeof(g_helm_update_url) - 1);
        g_helm_update_url[sizeof(g_helm_update_url) - 1] = 0;
        strncpy(g_helm_update_md5, md5, sizeof(g_helm_update_md5) - 1);
        g_helm_update_md5[sizeof(g_helm_update_md5) - 1] = 0;
        Serial.printf("OTA: update available - build %lu (currently running %d)\n",
            (unsigned long)remote_build, FW_BUILD);
    } else {
        g_helm_update_available = false;
        Serial.printf("OTA: up to date (running build %d, manifest has %lu)\n",
            FW_BUILD, (unsigned long)remote_build);
    }

    /* "can_sim" entries, one per hardware variant - cached for comparison
     * against each present engine's engines[i].fw_build/hw_id (see
     * engine_info_t) when the device-list modal is built. No local FW_BUILD
     * to compare against here - remote devices have their own independent
     * build numbers. */
    JsonObject cansim = doc["can_sim"];
    for (int hw = 1; hw < HW_COUNT; hw++) {
        node_update_t *nu = &g_node_update[hw];
        nu->build = 0;
        nu->url[0] = 0;
        nu->md5[0] = 0;
        const char *key = md_hw_key((uint8_t)hw);
        JsonObject v = key ? cansim[key] : JsonObject();
        if (v.isNull()) continue;
        const char *cs_url = v["url"] | "";
        const char *cs_md5 = v["md5"] | "";
        if (strlen(cs_url) > NODE_URL_MAXLEN) {
            Serial.printf("OTA: can_sim/%s url is %u chars, over the %d that fit in MSG_OTA_START - ignored\n",
                key, (unsigned)strlen(cs_url), NODE_URL_MAXLEN);
            continue;
        }
        nu->build = v["build"] | 0;
        strncpy(nu->url, cs_url, sizeof(nu->url) - 1);
        nu->url[sizeof(nu->url) - 1] = 0;
        strncpy(nu->md5, cs_md5, sizeof(nu->md5) - 1);
        nu->md5[sizeof(nu->md5) - 1] = 0;
    }

    g_last_check_result = UPDATE_CHECK_RESULT_OK;
    if (forced) g_update_check_done_id = req_id;
}

/* true if any currently-present engine is reporting a fw_build behind the
 * manifest's "can_sim" entry for its hardware - drives btn_update_available's visibility
 * alongside g_helm_update_available (see ui_tick()). engines[i].fw_build
 * of 0 means "never reported" (older firmware, or not really an OTA-
 * capable remote board at all) and is deliberately excluded, not treated
 * as "build 0 needs updating". */
static bool any_remote_update_available(void)
{
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        int slot = node_update_slot(i);
        if (slot >= 0 && engines[i].present && engines[i].fw_build > 0 &&
            engines[i].fw_build < g_node_update[slot].build)
            return true;
    }
    return false;
}

/* stricter than check_for_update_tick()'s notification gate: this is the
 * actual "safe to update" check, run at the moment the button is tapped.
 * engines[] doesn't track live rpm for anything but sel_engine (only
 * ign_on is tracked fleet-wide, see engine_info_t's comment) - ign_on is
 * the best available proxy for "is this engine active" on a
 * non-selected engine, erring conservative rather than trying to be
 * precise about exact running state for engines we're not watching
 * closely right now. */
static bool any_engine_active(void)
{
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        if (!engines[i].present) continue;
        if (i == sel_engine) {
            if (g_engine_running || glow_held || start_held) return true;
        } else if (engines[i].ign_on) {
            return true;
        }
    }
    return false;
}

/* "Update All": one button updates every out-of-date board at the same time, then HELM itself last.
 * g_ua_items tracks each engine that needs an update (an ESP-NOW board that hosts several engines is
 * sent ONE command but tracked per engine); update_all_tick() watches them come back. */
#define UA_TIMEOUT_MS (6UL * 60 * 1000)
typedef struct {
    int      engine;
    uint32_t target_build;
    bool     done;
    bool     failed;
} ua_item_t;
static ua_item_t g_ua_items[MD_MAX_ENGINES];
static int       g_ua_count = 0;
static bool      g_ua_active = false;
static bool      g_ua_helm_after = false;
static uint32_t  g_ua_started_ms = 0;
static uint32_t  g_ua_helm_at_ms = 0;   /* when to start HELM's own update, 0 = not scheduled */
static lv_obj_t *g_ua_label = NULL;
/* set by the serial UPDATE command (loop task), acted on by update_all_tick() (LVGL task) once the forced
 * manifest check has finished - LVGL objects may only be created on the LVGL task */
static volatile uint32_t g_serial_update_wait_id = 0;

static lv_obj_t *ota_win = NULL;

static void ota_close_cb(lv_event_t *e)
{
    (void)e;
    g_ua_active = false;      /* closing the window stops tracking, so nothing touches the deleted label */
    g_ua_label = NULL;
    g_ua_helm_at_ms = 0;
    if (ota_win) { lv_obj_del(ota_win); ota_win = NULL; }
    /* harmless no-op if this window wasn't a remote-OTA one - closing
     * always cancels any wait-for-ACK in flight so a stray late ACK/
     * timeout can't reopen or repaint a window the user already dismissed */
    g_remote_ota_state  = REMOTE_OTA_IDLE;
    g_remote_ota_engine = -1;
}

/* Settings cog / off_overlay button -> here. Never opens anything over
 * the live dashboard (btn_update_available only exists on off_overlay -
 * see create_ui()) - this is the SECOND, stricter check: a running
 * engine anywhere blocks the flow outright rather than just hiding a
 * notification. One shared safety warning covers every target in the
 * list below (HELM itself and/or any out-of-date remote engine) - each
 * row's own "Update" button starts that specific device's update
 * immediately, no second per-device confirm screen. */
static void ota_update_available_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || ota_win) return;

    bool blocked = any_engine_active();

    ota_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ota_win, 460, blocked ? 260 : 380);
    lv_obj_center(ota_win);
    lv_obj_set_style_bg_color(ota_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(ota_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(ota_win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ota_win);
    lv_label_set_text(title, blocked ? "Cannot Update" : "Firmware Update");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *msg = lv_label_create(ota_win);
    lv_obj_set_width(msg, 420);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 44);
    if (blocked) {
        lv_label_set_text(msg,
            "An engine is still running or being started. "
            "Power everything off before updating.");
    } else {
        lv_label_set_text(msg,
            "Only update in a secure environment where a failed update "
            "can be addressed calmly - this is not something to do "
            "underway or in an emergency. Update All updates every board "
            "listed at the same time, then this display last.");
    }

    if (!blocked) {
        lv_obj_t *list = lv_obj_create(ota_win);
        lv_obj_set_size(list, 420, 130);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 130);
        lv_obj_set_style_bg_color(list, lv_color_hex(0x0a1620), 0);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 6, 0);

        /* what Update All will do, one row per board: "name   build X -> Y" */
        if (g_helm_update_available) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), 40);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x16283c), 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = lv_label_create(row);
            lv_label_set_text_fmt(l, "HELM display (last):  build %d -> %lu", FW_BUILD,
                (unsigned long)g_helm_update_build);
            lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        }
        for (int i = 0; i < MD_MAX_ENGINES; i++) {
            int slot = node_update_slot(i);
            if (slot < 0 || !engines[i].present || engines[i].fw_build == 0 ||
                engines[i].fw_build >= g_node_update[slot].build)
                continue;
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), 40);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x16283c), 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = lv_label_create(row);
            lv_label_set_text_fmt(l, "%s:  build %u -> %lu", engine_display_name(i),
                (unsigned)engines[i].fw_build, (unsigned long)g_node_update[slot].build);
            lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        }
    }

    lv_obj_t *close = lv_btn_create(ota_win);
    lv_obj_set_size(close, blocked ? 420 : 200, 50);
    lv_obj_align(close, blocked ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_BOTTOM_LEFT, blocked ? 0 : 8, -12);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(close, ota_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, blocked ? "Close" : "Cancel");
    lv_obj_center(close_l);

    if (!blocked) {
        lv_obj_t *all = lv_btn_create(ota_win);
        lv_obj_set_size(all, 200, 50);
        lv_obj_align(all, LV_ALIGN_BOTTOM_RIGHT, -8, -12);
        lv_obj_set_style_bg_color(all, lv_color_hex(0x4a1c1c), 0);
        lv_obj_add_event_cb(all, ota_update_all_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *all_l = lv_label_create(all);
        lv_label_set_text(all_l, "Update All");
        lv_obj_set_style_text_color(all_l, lv_color_hex(0xff9999), 0);
        lv_obj_center(all_l);
    }
}

/* The actual download+flash. rebootOnUpdate(false) so a successful
 * download shows a confirmation before restarting, rather than an
 * unexplained reboot.
 *
 * Holding lvgl_port_lock() for the entire blocking httpUpdate.update()
 * call (instead of releasing it beforehand, the original assumption -
 * "LVGL renders on its own FreeRTOS task, so the display keeps working
 * during the download") stops LVGL's own render task from touching
 * flash-mapped font/icon data while Update.h is writing flash, but on
 * real hardware this alone wasn't enough - the RGB panel still visibly
 * corrupted. The panel scans out continuously from a bounce buffer (see
 * setup()'s RGB_BOUNCE_LINES comment) that has to be refilled by the
 * display driver on a tight schedule; ESP32's flash write/erase calls
 * (which Update.h makes repeatedly, once per chunk) briefly halt
 * whichever core isn't doing the write and disable the flash cache,
 * which starves that refill regardless of whether LVGL's own task is
 * locked out - a lower-level DMA problem the LVGL lock can't reach.
 * There's no clean fix for that without touching the RGB driver
 * directly, so instead: turn the backlight off for the duration (still
 * corrupting internally, just invisible) and back on once
 * httpUpdate.update() returns and flash writes have stopped. */
static void ota_helm_self_update(void)
{
    g_ua_active = false;
    g_ua_label = NULL;
    if (ota_win) { lv_obj_del(ota_win); ota_win = NULL; }

    lvgl_port_lock(-1);
    ota_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ota_win, 460, 180);
    lv_obj_center(ota_win);
    lv_obj_set_style_bg_color(ota_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(ota_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(ota_win, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *msg = lv_label_create(ota_win);
    lv_obj_set_width(msg, 420);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, "Performing update...\nscreen will go blank until it restarts");
    lv_obj_center(msg);
    lv_refr_now(NULL);

    Backlight *bl = g_board ? g_board->getBacklight() : NULL;   /* may be
        * null if backlight support isn't enabled/initialized - see
        * Board::getBacklight()'s own doc comment */
    if (bl) bl->off();

    /* https (GitHub releases) needs a TLS client, plain http must not use
     * one. The certificate chain is not verified - same trade as the other
     * OpenBoat firmwares; the md5 below guards against a corrupted download.
     * GitHub redirects the asset URL to its download host, so follow it. */
    WiFiClient       plain_client;
    WiFiClientSecure tls_client;
    tls_client.setInsecure();
    String real_url = ota_resolve_url(String(g_helm_update_url));
    NetworkClient &client = real_url.startsWith("https://")
        ? (NetworkClient &)tls_client : (NetworkClient &)plain_client;
    httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);   /* ota_resolve_url() already did */
    httpUpdate.rebootOnUpdate(false);
    /* checksum verification (if the manifest had one - see check_for_
     * update_tick()): HTTPUpdate's own setMD5sum() makes Update.end()
     * compare the actually-written image against this before accepting
     * it, failing the update outright on a mismatch rather than flashing
     * a corrupted/tampered download. Skipped, not failed, if empty -
     * an older or hand-edited manifest without a checksum still updates,
     * just without this extra guarantee. */
    if (g_helm_update_md5[0]) httpUpdate.setMD5sum(g_helm_update_md5);
    Serial.printf("OTA: downloading from %s\n", g_helm_update_url);
    t_httpUpdate_return ret = httpUpdate.update(client, real_url);

    if (bl) bl->on();

    lv_obj_clean(ota_win);
    lv_obj_t *result = lv_label_create(ota_win);
    lv_obj_set_width(result, 420);
    lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);

    if (ret == HTTP_UPDATE_OK) {
        lv_label_set_text(result, "Update downloaded successfully.\nRestarting now...");
        lv_obj_center(result);
        lvgl_port_unlock();
        Serial.println("OTA: update OK, restarting");
        delay(1500);
        ESP.restart();
        return;   /* never reached */
    }

    if (ret == HTTP_UPDATE_NO_UPDATES) {
        lv_label_set_text_fmt(result, "No update found at that URL.\n%s",
            httpUpdate.getLastErrorString().c_str());
        Serial.printf("OTA: HTTP_UPDATE_NO_UPDATES - %s\n", httpUpdate.getLastErrorString().c_str());
    } else {
        lv_label_set_text_fmt(result, "Update failed: %s\nNothing was changed - still running build %d.",
            httpUpdate.getLastErrorString().c_str(), FW_BUILD);
        Serial.printf("OTA: HTTP_UPDATE_FAILED - %s\n", httpUpdate.getLastErrorString().c_str());
    }
    lv_obj_center(result);

    lv_obj_t *close = lv_btn_create(ota_win);
    lv_obj_set_size(close, 200, 50);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(close, ota_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, "Close");
    lv_obj_center(close_l);
    lvgl_port_unlock();
}

static void ota_start_download_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ota_helm_self_update();
}

/* ==================== OTA: remote device trigger (Phase 2) ====================
 * HELM sends its own WiFi credentials + that device-type's manifest URL to
 * a selected engine over bus_send() (CAN or ESP-NOW, whichever transport
 * that engine is actually on - already handled transparently by
 * bus_send() itself). The target reassembles, ACKs, joins WiFi, downloads
 * and flashes itself - see can_sim.ino's ota_handle_start_chunk()/
 * ota_tick(). No local FW_BUILD comparison here (unlike HELM's own
 * self-update): a remote device's build number is independent of HELM's. */
/* 8s was too tight in practice: an OTA_ACK is sent by the target BEFORE
 * it touches WiFi (see can_sim.ino's ota_tick()), so the ACK itself
 * should normally be near-instant - but if the target's ESP-NOW link is
 * mid-channel-hunt (see CACHED_CHANNEL_RETRIES in can_sim.ino) or the
 * bus is otherwise flaky, the MSG_OTA_START burst itself (~40 chunked
 * frames) can take a real chunk-loss retry or two to fully reassemble
 * before any ACK goes out at all. Confirmed on real hardware: the
 * update had genuinely succeeded on the target by the time HELM's UI
 * gave up and showed Retry. */
#define OTA_ACK_TIMEOUT_MS 30000

/* mirrors can_sim.ino's sim_engine_send_name() chunking exactly (same
 * total_len/chunk_idx framing, see can_protocol.h's MSG_OTA_START
 * comment) - only the payload shape differs: four NUL-terminated
 * strings back to back instead of one (ssid/pass/url/md5). Field caps
 * sum to exactly OTA_START_MAX_LEN (250) including their 4 NUL
 * terminators, so total_len can never overflow buf regardless of how
 * long ssid/pass/url/md5 actually are. md5 is always exactly 32 hex
 * chars in practice (or empty, see check_for_update_tick()) - the cap
 * here is just defensive, matching the pattern of the other fields. */
#define OTA_SSID_MAXLEN 32
#define OTA_PASS_MAXLEN 64
#define OTA_MD5_MAXLEN  32
#define OTA_URL_MAXLEN  (OTA_START_MAX_LEN - OTA_SSID_MAXLEN - OTA_PASS_MAXLEN - OTA_MD5_MAXLEN - 4)
static_assert(OTA_URL_MAXLEN == NODE_URL_MAXLEN, "NODE_URL_MAXLEN (manifest parsing) must match OTA_URL_MAXLEN");

static void ota_send_start(int e, const char *ssid, const char *pass, const char *url, const char *md5)
{
    char buf[OTA_START_MAX_LEN] = {0};
    int pos = 0;
    int n;

    n = snprintf(buf + pos, OTA_SSID_MAXLEN + 1, "%s", ssid);
    if (n > OTA_SSID_MAXLEN) n = OTA_SSID_MAXLEN;
    pos += n; buf[pos++] = 0;

    n = snprintf(buf + pos, OTA_PASS_MAXLEN + 1, "%s", pass);
    if (n > OTA_PASS_MAXLEN) n = OTA_PASS_MAXLEN;
    pos += n; buf[pos++] = 0;

    n = snprintf(buf + pos, OTA_URL_MAXLEN + 1, "%s", url);
    if (n > OTA_URL_MAXLEN) n = OTA_URL_MAXLEN;
    pos += n; buf[pos++] = 0;

    n = snprintf(buf + pos, OTA_MD5_MAXLEN + 1, "%s", md5);
    if (n > OTA_MD5_MAXLEN) n = OTA_MD5_MAXLEN;
    pos += n; buf[pos++] = 0;

    uint8_t total_len = (uint8_t)pos;
    uint8_t chunks = (total_len + OTA_START_CHUNK_BYTES - 1) / OTA_START_CHUNK_BYTES;
    for (uint8_t c = 0; c < chunks; c++) {
        uint8_t d[8] = {0};
        d[0] = total_len;
        d[1] = c;
        int off = c * OTA_START_CHUNK_BYTES;
        for (int j = 0; j < OTA_START_CHUNK_BYTES && off + j < total_len; j++)
            d[2 + j] = (uint8_t)buf[off + j];
        bus_send(MSG_OTA_START(e), d, 8);
    }
    Serial.printf("OTA: sent MSG_OTA_START to engine %d (%u chunks, %u bytes)\n",
        e, chunks, total_len);
}

/* Debug-page "join & stay on WiFi" trigger - see handle_wifi_join_all()
 * below and can_protocol.h's MSG_WIFI_JOIN comment. Same chunking shape
 * as ota_send_start() above, just the "<ssid>\0<password>\0" payload
 * (no url/md5) - reuses OTA_SSID_MAXLEN/OTA_PASS_MAXLEN since they
 * already sum, with 2 NULs, to exactly WIFI_JOIN_MAX_LEN. */
static void wifi_join_send(int e, const char *ssid, const char *pass)
{
    char buf[WIFI_JOIN_MAX_LEN] = {0};
    int pos = 0;
    int n;

    n = snprintf(buf + pos, OTA_SSID_MAXLEN + 1, "%s", ssid);
    if (n > OTA_SSID_MAXLEN) n = OTA_SSID_MAXLEN;
    pos += n; buf[pos++] = 0;

    n = snprintf(buf + pos, OTA_PASS_MAXLEN + 1, "%s", pass);
    if (n > OTA_PASS_MAXLEN) n = OTA_PASS_MAXLEN;
    pos += n; buf[pos++] = 0;

    uint8_t total_len = (uint8_t)pos;
    uint8_t chunks = (total_len + WIFI_JOIN_CHUNK_BYTES - 1) / WIFI_JOIN_CHUNK_BYTES;
    for (uint8_t c = 0; c < chunks; c++) {
        uint8_t d[8] = {0};
        d[0] = total_len;
        d[1] = c;
        int off = c * WIFI_JOIN_CHUNK_BYTES;
        for (int j = 0; j < WIFI_JOIN_CHUNK_BYTES && off + j < total_len; j++)
            d[2 + j] = (uint8_t)buf[off + j];
        bus_send(MSG_WIFI_JOIN(e), d, 8);
    }
    Serial.printf("WiFi: sent MSG_WIFI_JOIN to engine %d (%u chunks, %u bytes)\n",
        e, chunks, total_len);
}

/* Builds the "waiting for ACK" window and kicks off the send. Also used
 * by ota_retry_remote_cb() below - safe to call again for the same
 * engine, it just re-sends and resets the timeout clock. */
static void ota_send_remote_start(int e)
{
    lvgl_port_lock(-1);
    ota_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ota_win, 460, 200);
    lv_obj_center(ota_win);
    lv_obj_set_style_bg_color(ota_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(ota_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(ota_win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg = lv_label_create(ota_win);
    lv_obj_set_width(msg, 420);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);

    /* HELM must itself be on working WiFi to have any credentials to hand
     * off at all - can't happen via the button in practice (WiFi status
     * isn't otherwise gated here), but a bench unit's WiFi could drop
     * between opening the device list and tapping Update, so check again
     * right before sending rather than trusting an earlier state. */
    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(msg,
            "HELM has no active WiFi connection right now - "
            "cannot hand off credentials to the target device.");
        lv_obj_center(msg);
        lv_obj_t *close = lv_btn_create(ota_win);
        lv_obj_set_size(close, 200, 50);
        lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_bg_color(close, lv_color_hex(0x33475c), 0);
        lv_obj_add_event_cb(close, ota_close_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *close_l = lv_label_create(close);
        lv_label_set_text(close_l, "Close");
        lv_obj_center(close_l);
        lvgl_port_unlock();
        return;
    }

    lv_label_set_text_fmt(msg, "Sending update to %s...\nwaiting for acknowledgment",
        engine_display_name(e));
    lv_obj_center(msg);
    lvgl_port_unlock();

    String ssid = prefs_ok ? prefs.getString("ssid", "") : String("");
    String pass = prefs_ok ? prefs.getString("pass", "") : String("");
    int slot = node_update_slot(e);
    if (slot < 0) return;   /* the device list only offers boards that have an image */
    const node_update_t *nu = &g_node_update[slot];
    ota_send_start(e, ssid.c_str(), pass.c_str(), nu->url, nu->md5);

    g_remote_ota_engine  = e;
    g_remote_ota_build   = nu->build;
    g_remote_ota_sent_ms = millis();
    g_remote_ota_state   = REMOTE_OTA_WAIT_ACK;
}

static void ota_retry_remote_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int target = g_remote_ota_engine;
    if (ota_win) { lv_obj_del(ota_win); ota_win = NULL; }
    if (target >= 0 && target < MD_MAX_ENGINES) ota_send_remote_start(target);
}

/* "Wait Longer" - deliberately does NOT re-send MSG_OTA_START (that's
 * what Retry is for). The original burst may well have arrived and the
 * target could already be mid-update (joining WiFi, downloading,
 * flashing) by the time OTA_ACK_TIMEOUT_MS elapses - re-sending in that
 * case is redundant at best and, worse, would land on a device that's
 * no longer listening the same way mid-flash. Just pushes the deadline
 * out another OTA_ACK_TIMEOUT_MS and goes back to waiting, in case the
 * ACK itself is only delayed (channel-hunt retry, bus contention) rather
 * than genuinely lost. */
static void ota_wait_longer_remote_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_remote_ota_engine < 0 || g_remote_ota_engine >= MD_MAX_ENGINES || !ota_win) return;

    lvgl_port_lock(-1);
    lv_obj_clean(ota_win);
    lv_obj_t *msg = lv_label_create(ota_win);
    lv_obj_set_width(msg, 420);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);
    lv_label_set_text_fmt(msg, "Still waiting for %s...\n(not re-sent - just giving it more time)",
        engine_display_name(g_remote_ota_engine));
    lv_obj_center(msg);
    lvgl_port_unlock();

    g_remote_ota_sent_ms = millis();
    g_remote_ota_state   = REMOTE_OTA_WAIT_ACK;
}

/* row "Update" button handler from ota_update_available_cb()'s device
 * list - user_data is -1 for the HELM row, 0..MD_MAX_ENGINES-1 for a
 * remote engine row. Reuses Phase 1's ota_start_download_cb() verbatim
 * for the HELM case (same event, still LV_EVENT_CLICKED). */
/* The "Update All" button: send every out-of-date board its update command now (all at once - they
 * each leave the network, join WiFi, download and restart on their own), then show progress until each
 * is back on the new build, and finally update HELM itself. */
static void ota_update_all_start(void)
{
    if (ota_win) { lv_obj_del(ota_win); ota_win = NULL; }

    lvgl_port_lock(-1);
    ota_win = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ota_win, 460, 340);
    lv_obj_center(ota_win);
    lv_obj_set_style_bg_color(ota_win, lv_color_hex(0x102438), 0);
    lv_obj_set_style_border_color(ota_win, lv_color_hex(0x33475c), 0);
    lv_obj_clear_flag(ota_win, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ota_win);
    lv_label_set_text(title, "Updating");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    g_ua_label = lv_label_create(ota_win);
    lv_obj_set_width(g_ua_label, 420);
    lv_label_set_long_mode(g_ua_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_ua_label, lv_color_hex(0xcccccc), 0);
    lv_obj_align(g_ua_label, LV_ALIGN_TOP_LEFT, 8, 50);

    lv_obj_t *close = lv_btn_create(ota_win);
    lv_obj_set_size(close, 420, 50);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(close, ota_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, "Close (updates carry on)");
    lv_obj_center(close_l);

    g_ua_count = 0;
    g_ua_active = false;
    g_ua_helm_after = g_helm_update_available;
    g_ua_helm_at_ms = 0;

    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(g_ua_label,
            "HELM has no active WiFi connection right now - cannot hand the network details to the other "
            "boards or download anything.");
        lvgl_port_unlock();
        return;
    }

    String ssid = prefs_ok ? prefs.getString("ssid", "") : String("");
    String pass = prefs_ok ? prefs.getString("pass", "") : String("");
    uint8_t sent_mac[MD_MAX_ENGINES][6];
    int sent_n = 0;
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        int slot = node_update_slot(i);
        if (slot < 0 || !engines[i].present || engines[i].fw_build == 0 ||
            engines[i].fw_build >= g_node_update[slot].build)
            continue;
        const node_update_t *nu = &g_node_update[slot];
        g_ua_items[g_ua_count++] = { i, nu->build, false, false };

        /* one command per physical radio: several engines on one ESP-NOW board are one board */
        bool duplicate = false;
        if (engine_slots[i].transport == BUS_TRANSPORT_ESPNOW) {
            for (int k = 0; k < sent_n; k++)
                if (md_mac_eq(sent_mac[k], engine_slots[i].espnow_peer_mac)) duplicate = true;
            if (!duplicate) memcpy(sent_mac[sent_n++], engine_slots[i].espnow_peer_mac, 6);
        }
        if (!duplicate) {
            ota_send_start(i, ssid.c_str(), pass.c_str(), nu->url, nu->md5);
            Serial.printf("OTA: update all - command sent to engine %d (target build %lu)\n",
                i, (unsigned long)nu->build);
        }
    }

    g_ua_started_ms = millis();
    g_ua_active = true;
    if (g_ua_count == 0) g_ua_helm_at_ms = millis() + 1500;   /* only HELM itself to do */
    lvgl_port_unlock();
}

static void ota_update_all_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ota_update_all_start();
}

/* every ui_tick(): watch the boards come back on their new build, show progress, then do HELM last */
static void update_all_tick(void)
{
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    /* serial UPDATE: the forced manifest check has finished - start the update here, on the LVGL task */
    if (g_serial_update_wait_id && g_update_check_done_id == g_serial_update_wait_id) {
        g_serial_update_wait_id = 0;
        if (g_last_check_result != UPDATE_CHECK_RESULT_OK) {
            Serial.println("Serial: the update check failed - nothing was changed");
        } else if (!g_helm_update_available && !any_remote_update_available()) {
            Serial.println("Serial: everything is up to date");
        } else if (any_engine_active()) {
            Serial.println("Serial: not updating - an engine is running or being started");
        } else if (ota_win) {
            Serial.println("Serial: not updating - an update screen is already open");
        } else {
            Serial.println("Serial: starting Update All");
            ota_update_all_start();
        }
    }
#endif
    if (!g_ua_active || !g_ua_label) return;
    uint32_t now = millis();

    bool unresolved = false, any_failed = false;
    for (int k = 0; k < g_ua_count; k++) {
        ua_item_t &it = g_ua_items[k];
        if (!it.done && !it.failed) {
            if (engines[it.engine].present && engines[it.engine].fw_build >= it.target_build)
                it.done = true;
            else if (now - g_ua_started_ms > UA_TIMEOUT_MS)
                it.failed = true;
        }
        if (!it.done && !it.failed) unresolved = true;
        if (it.failed) any_failed = true;
    }

    if (!unresolved && g_ua_helm_at_ms == 0) {
        if (any_failed) {
            g_ua_active = false;   /* leave HELM alone - something needs looking at first */
        } else if (g_ua_helm_after) {
            g_ua_helm_at_ms = now + 3000;
        } else {
            g_ua_active = false;
        }
    }

    char buf[400];
    int n = 0;
    for (int k = 0; k < g_ua_count; k++) {
        ua_item_t &it = g_ua_items[k];
        n += snprintf(buf + n, sizeof(buf) - n, "%s: %s\n", engine_display_name(it.engine),
            it.done ? "updated" : it.failed ? "NO RESPONSE" : "updating...");
    }
    if (g_ua_helm_after) {
        n += snprintf(buf + n, sizeof(buf) - n, "HELM display: %s\n",
            g_ua_helm_at_ms ? "updating now..." : "waiting for the boards");
    }
    if (!unresolved && any_failed)
        n += snprintf(buf + n, sizeof(buf) - n,
            "\nSome boards did not come back. The HELM display was NOT updated - check them first.");
    else if (!unresolved && !g_ua_helm_after && g_ua_count)
        n += snprintf(buf + n, sizeof(buf) - n, "\nAll boards updated.");
    else if (g_ua_active)
        n += snprintf(buf + n, sizeof(buf) - n, "\nThis takes a minute or two. Do not power anything off.");

    static char last[400];
    if (strcmp(buf, last) != 0) {
        strncpy(last, buf, sizeof(last) - 1);
        last[sizeof(last) - 1] = 0;
        lv_label_set_text(g_ua_label, buf);
        lv_refr_now(NULL);
    }

    if (g_ua_active && g_ua_helm_at_ms && now >= g_ua_helm_at_ms) {
        g_ua_helm_at_ms = 0;
        ota_helm_self_update();   /* ends in a restart on success; a failure shows its own result screen */
    }
}

static void ota_device_update_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int target = (int)(intptr_t)lv_event_get_user_data(e);

    if (ota_win) { lv_obj_del(ota_win); ota_win = NULL; }

    if (target < 0) {
        ota_start_download_cb(e);
        return;
    }
    if (target < MD_MAX_ENGINES) ota_send_remote_start(target);
}

/* Called every ui_tick() cycle (200ms), for as long as a remote OTA is
 * being actively tracked (WAIT_ACK or TIMEOUT - ACKED/CONFIRMED/IDLE are
 * all terminal, nothing left to monitor). g_remote_ota_last_drawn tracks
 * what was last actually shown on screen, separate from g_remote_ota_
 * state itself, since state can now transition TWICE in one tracked run
 * (WAIT_ACK -> TIMEOUT -> CONFIRMED, if a late build-number bump shows
 * up after the timeout screen is already up) - a single "!= WAIT_ACK"
 * guard (the original design) only ever handled one transition and
 * would silently stop redrawing on the second one. */
static void remote_ota_modal_tick(void)
{
    if (g_remote_ota_state != REMOTE_OTA_WAIT_ACK && g_remote_ota_state != REMOTE_OTA_TIMEOUT)
        return;

    /* Build-number confirmation: the target reappearing on the bus
     * already running the expected build is treated as proof the update
     * succeeded, independent of whether its one-shot MSG_OTA_ACK ever
     * arrived. Confirmed on real hardware that it can genuinely finish
     * (visible via a fresh ENROLL_REQUEST after its own reboot) while
     * HELM's UI still times out waiting on that ACK - see can_sim.ino's
     * ota_tick() comment on why that specific packet is the weak link.
     * Checked on every tick regardless of WAIT_ACK/TIMEOUT, since the
     * full WiFi-join+download+flash+reboot cycle can easily run past
     * OTA_ACK_TIMEOUT_MS even on a healthy link - this needs to catch a
     * late success after the Retry/Wait Longer screen is already up,
     * not just during the initial wait. */
    if (g_remote_ota_engine >= 0 && g_remote_ota_engine < MD_MAX_ENGINES &&
        engines[g_remote_ota_engine].present && g_remote_ota_build > 0 &&
        engines[g_remote_ota_engine].fw_build >= g_remote_ota_build) {
        g_remote_ota_state = REMOTE_OTA_CONFIRMED;
    } else if (g_remote_ota_state == REMOTE_OTA_WAIT_ACK &&
               millis() - g_remote_ota_sent_ms > OTA_ACK_TIMEOUT_MS) {
        Serial.printf("OTA: no MSG_OTA_ACK from engine %d after %lums, giving up\n",
            g_remote_ota_engine, (unsigned long)(millis() - g_remote_ota_sent_ms));
        g_remote_ota_state = REMOTE_OTA_TIMEOUT;
    }

    static remote_ota_state_t last_drawn = REMOTE_OTA_IDLE;
    if (g_remote_ota_state == last_drawn) return;   /* nothing new to draw */
    last_drawn = g_remote_ota_state;

    if (g_remote_ota_state == REMOTE_OTA_WAIT_ACK) return;   /* still actively waiting */
    if (!ota_win) return;   /* user already closed the window */

    lv_obj_clean(ota_win);

    lv_obj_t *msg = lv_label_create(ota_win);
    lv_obj_set_width(msg, 420);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 30);

    if (g_remote_ota_state == REMOTE_OTA_ACKED) {
        lv_label_set_text(msg,
            "Device acknowledged - updating now.\n"
            "It will rejoin the bus automatically once done.");
    } else if (g_remote_ota_state == REMOTE_OTA_CONFIRMED) {
        lv_label_set_text_fmt(msg,
            "%s is back online running build %lu -\nthe update succeeded.",
            engine_display_name(g_remote_ota_engine), (unsigned long)g_remote_ota_build);
    } else {
        lv_label_set_text_fmt(msg,
            "No response from %s.\nCheck it is powered and in range.",
            engine_display_name(g_remote_ota_engine));
    }

    lv_obj_t *close = lv_btn_create(ota_win);
    lv_obj_set_size(close, g_remote_ota_state == REMOTE_OTA_TIMEOUT ? 140 : 420, 50);
    lv_obj_align(close, g_remote_ota_state == REMOTE_OTA_TIMEOUT ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_MID,
                 g_remote_ota_state == REMOTE_OTA_TIMEOUT ? 8 : 0, -12);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x33475c), 0);
    lv_obj_add_event_cb(close, ota_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, "Close");
    lv_obj_center(close_l);

    if (g_remote_ota_state == REMOTE_OTA_TIMEOUT) {
        /* three buttons across a 460px-wide window: Close (left) / Wait
         * Longer (middle) / Retry (right), 140px each with small gaps -
         * see ota_wait_longer_remote_cb()'s comment for why "wait" and
         * "retry" are deliberately different actions, not the same
         * thing worded differently. Neither applies once CONFIRMED (or
         * ACKED) - nothing left to wait for or retry. */
        lv_obj_t *wait = lv_btn_create(ota_win);
        lv_obj_set_size(wait, 140, 50);
        lv_obj_align(wait, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_bg_color(wait, lv_color_hex(0x1c3450), 0);
        lv_obj_add_event_cb(wait, ota_wait_longer_remote_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *wait_l = lv_label_create(wait);
        lv_label_set_text(wait_l, "Wait Longer");
        lv_obj_center(wait_l);

        lv_obj_t *retry = lv_btn_create(ota_win);
        lv_obj_set_size(retry, 140, 50);
        lv_obj_align(retry, LV_ALIGN_BOTTOM_RIGHT, -8, -12);
        lv_obj_set_style_bg_color(retry, lv_color_hex(0x4a1c1c), 0);
        lv_obj_add_event_cb(retry, ota_retry_remote_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *retry_l = lv_label_create(retry);
        lv_label_set_text(retry_l, "Retry");
        lv_obj_set_style_text_color(retry_l, lv_color_hex(0xff9999), 0);
        lv_obj_center(retry_l);
    }

    /* same lv_refr_now() reasoning as check_for_update_tick()'s status
     * label - confirmed on real hardware that this update can otherwise
     * silently fail to reach the physical screen under ESP-NOW/WiFi
     * load. */
    Serial.println("OTA: remote_ota_modal_tick() redrew the result screen");
    lv_refr_now(NULL);
}

static esp_err_t espnow_send_bus_frame(const uint8_t *dest_mac, uint32_t id, const uint8_t *data, uint8_t len)
{
    if (!g_fsec_have_key) return ESP_ERR_INVALID_STATE;
    espnow_bus_frame_t f;
    fsec_frame_build(&f, id, data, len);   /* signed with the shared secret */
    return esp_now_send(dest_mac, (uint8_t *)&f, sizeof(f));
}

/* CYD secondary displays have no ESP-NOW link to the engine units - they pair with HELM only (see
 * the CYD requester below). HELM is the hub: every engine-state frame it receives, over any
 * transport, is passed on to each paired NODE_TYPE_CYD peer as an encrypted unicast, so the CYD
 * sees what a display on the CAN bus would. Only the display-relevant frames go: ANNOUNCE,
 * telemetry, hours and names. Called from can_handle_rx() for every received frame. */
/* radio-level outcome of each send: did the receiver acknowledge it? (broadcasts always "succeed") */
static volatile uint32_t g_espnow_tx_acked = 0, g_espnow_tx_noack = 0;
static void espnow_on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status == ESP_NOW_SEND_SUCCESS) g_espnow_tx_acked++;
    else g_espnow_tx_noack++;
}

/* send one frame, as an encrypted unicast, to every joined board */
static esp_err_t espnow_unicast_to_peers(uint32_t id, const uint8_t *data, uint8_t len)
{
    esp_err_t worst = ESP_OK;
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
        if (espnow_peers[i].has_mac) {
            esp_err_t err = espnow_send_bus_frame(espnow_peers[i].mac, id, data, len);
            if (err != ESP_OK) worst = err;
        }
    }
    return worst;
}

static void espnow_relay_to_displays(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (md_engine_from_telem(id) < 0 && md_engine_from_hours(id) < 0 &&
        md_engine_from_announce(id) < 0 && md_engine_from_name(id) < 0)
        return;
    static uint32_t ok_count = 0, fail_count = 0, last_report_ms = 0;
    static esp_err_t last_err = ESP_OK;
    for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
        if (espnow_peers[i].has_mac && espnow_peers[i].node_type == NODE_TYPE_CYD) {
            esp_err_t err = espnow_send_bus_frame(espnow_peers[i].mac, id, data, len);
            if (err == ESP_OK) ok_count++;
            else { fail_count++; last_err = err; }
        }
    }
    if (millis() - last_report_ms >= 5000) {
        if (ok_count || fail_count)
            CLI_LOG("ESP-NOW: relay to displays - %lu sent, %lu refused by the radio%s%s; radio acked %lu, no ack %lu\n",
                (unsigned long)ok_count, (unsigned long)fail_count,
                fail_count ? ", last error " : "", fail_count ? esp_err_to_name(last_err) : "",
                (unsigned long)g_espnow_tx_acked, (unsigned long)g_espnow_tx_noack);
        g_espnow_tx_acked = g_espnow_tx_noack = 0;
        ok_count = fail_count = 0;
        last_report_ms = millis();
    }
}

#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

#if TARGET_BOARD != BOARD_HELM_S3_800x480
/* ==================== ESP-NOW requester (CYD secondary display) ====================
 * The CYD joins the HELM only and is a listener: HELM relays the engine data to it (see
 * espnow_relay_to_displays()). It joins with the shared secret (fleet_security.h) - no pairing window:
 * while it has no link it hops channels looking for HELM and broadcasts a HELLO that only a holder of the
 * secret can make; HELM's signed reply lets both sides derive the pair key. ESP-NOW is OFF until a key is
 * set (serial: KEY <passphrase>). Stage 1 is receive-only: the CYD sends nothing over ESP-NOW but the
 * HELLO (its mute stays local by design, and glow/start over ESP-NOW is a later stage). */
#define CYD_CHANNEL_SCAN_MAX        13
#define CYD_CHANNEL_DWELL_MS        2000UL
#define CYD_LOST_CONTACT_MS         6000UL
#define CYD_CACHED_CHANNEL_RETRIES  5

static bool     g_cyd_paired = false;
static uint8_t  g_cyd_helm_mac[6];
static uint32_t g_cyd_last_helm_rx_ms = 0;
static bool     g_cyd_hunting = false;
static uint8_t  g_cyd_channel = 1;
static uint8_t  g_cyd_cached_tries_left = 0;
static uint32_t g_cyd_hunt_last_hop_ms = 0;
static uint32_t g_cyd_rx_frames = 0;   /* bus frames accepted from HELM, for the status line */

static void cyd_bytes_to_hex(const uint8_t *buf, int len, char *out)
{
    for (int j = 0; j < len; j++) snprintf(&out[j * 2], 3, "%02X", buf[j]);
}

static void cyd_hex_to_bytes(const char *hex, uint8_t *buf, int len)
{
    char b[3] = {0};
    for (int j = 0; j < len; j++) {
        b[0] = hex[j * 2];
        b[1] = hex[j * 2 + 1];
        buf[j] = (uint8_t)strtoul(b, NULL, 16);
    }
}

/* register HELM as an encrypted peer; the key is worked out from the shared secret and both addresses */
static bool cyd_add_helm_peer(void)
{
    uint8_t me[6];
    fsec_my_mac(me);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, g_cyd_helm_mac, 6);
    peer.channel = 0;
    peer.encrypt = true;
    fsec_lmk(me, g_cyd_helm_mac, peer.lmk);
    return (esp_now_is_peer_exist(g_cyd_helm_mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer)) == ESP_OK;
}

static void cyd_espnow_on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!g_fsec_have_key) return;

    if (!g_cyd_paired && len == (int)sizeof(espnow_hello_ack_t) && data[0] == ESPNOW_MSG_HELLO_ACK) {
        espnow_hello_ack_t ack;
        memcpy(&ack, data, sizeof(ack));
        uint8_t me[6];
        fsec_my_mac(me);
        if (memcmp(ack.peer_mac, me, 6) != 0) return;   /* an answer to some other board */
        uint8_t want[8];
        fsec_hello_tag(ESPNOW_MSG_HELLO_ACK, ack.mac, ack.node_type, ack.peer_mac, want);
        if (!fsec_tag_equal(want, ack.tag, 8) || ack.node_type != NODE_TYPE_HELM) {
            Serial.println("ESP-NOW: reply rejected - it was not made with our key");
            return;
        }
        memcpy(g_cyd_helm_mac, ack.mac, 6);
        if (!cyd_add_helm_peer()) {
            Serial.println("ESP-NOW: got a valid reply but could not register HELM as an encrypted peer");
            return;
        }
        g_cyd_paired = true;
        g_cyd_last_helm_rx_ms = millis();
        if (prefs_ok) {
            char hex[13];
            cyd_bytes_to_hex(g_cyd_helm_mac, 6, hex); hex[12] = 0;
            prefs.putString("helm_mac", hex);
            prefs.putBool("paired", true);
        }
        Serial.println("ESP-NOW: joined HELM (encrypted)");
        return;
    }

    if (g_cyd_paired && len == (int)sizeof(espnow_bus_frame_t) && md_mac_eq(info->src_addr, g_cyd_helm_mac)) {
        espnow_bus_frame_t f;
        memcpy(&f, data, sizeof(f));
        if (!fsec_frame_verify(&f, info->src_addr)) return;   /* bad signature or a replay */
        /* only a frame addressed to us proves we are on HELM's channel; broadcasts can leak in from a
         * neighbouring channel, so they are used but don't count as contact */
        if (memcmp(info->des_addr, ESPNOW_BROADCAST_MAC, 6) != 0)
            g_cyd_last_helm_rx_ms = millis();
        g_cyd_rx_frames++;
        twai_message_t m = {};
        m.identifier = f.can_id;
        m.data_length_code = f.dlc;
        if (f.dlc) memcpy(m.data, f.data, f.dlc);
        can_handle_rx(&m);   /* the same dispatch a CAN frame goes through */
    }
}

static void cyd_espnow_setup(void)
{
    if (!g_fsec_have_key) {
        Serial.println("ESP-NOW: OFF - no key set. Type  KEY <passphrase>  (12+ characters, the same on every board).");
        return;
    }
    WiFi.mode(WIFI_STA);   /* radio on, no network join */
    /* start on the channel that last worked, not channel 1 - a board that powers up on the wrong channel
     * can still hear a faint trickle of HELM's broadcasts and never go looking */
    uint8_t first_ch = prefs_ok ? prefs.getUChar("lastch", 1) : 1;
    if (first_ch < 1 || first_ch > CYD_CHANNEL_SCAN_MAX) first_ch = 1;
    g_cyd_channel = first_ch;
    esp_wifi_set_channel(first_ch, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW: init failed");
        return;
    }
    esp_now_register_recv_cb(cyd_espnow_on_recv);
    uint8_t pmk[ESPNOW_LMK_LEN];
    fsec_pmk(pmk);
    esp_now_set_pmk(pmk);

    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    bcast.channel = 0;
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    g_cyd_paired = prefs_ok && prefs.getBool("paired", false);
    if (g_cyd_paired) {
        String hm = prefs.getString("helm_mac", "");
        if (hm.length() == 12) {
            cyd_hex_to_bytes(hm.c_str(), g_cyd_helm_mac, 6);
            if (!cyd_add_helm_peer()) Serial.println("ESP-NOW: failed to restore HELM as an encrypted peer");
        } else {
            g_cyd_paired = false;
        }
    }
    g_cyd_last_helm_rx_ms = millis();
    Serial.printf("ESP-NOW: ready (key fingerprint %08lX, %s)\n", (unsigned long)fsec_fingerprint(),
        g_cyd_paired ? "joined before, looking for HELM" : "not joined yet, will look for HELM");
}

/* forget HELM so the next tick starts a fresh hunt + HELLO (serial REPAIR) */
static void cyd_espnow_forget_helm(void)
{
    if (g_cyd_paired) esp_now_del_peer(g_cyd_helm_mac);
    g_cyd_paired = false;
    if (prefs_ok) {
        prefs.putBool("paired", false);
        prefs.remove("helm_mac");
    }
}

/* called from loop(): while we have no link or HELM has gone quiet, hop channels (the last good one first)
 * and announce ourselves with a HELLO on each. */
static void cyd_pairing_tick(void)
{
    if (!g_fsec_have_key) return;   /* ESP-NOW is off until a key is set */

    static uint32_t last_status_ms = 0;
    static uint32_t last_status_frames = 0;
    if (g_cyd_paired && millis() - last_status_ms >= 5000) {
        uint8_t cur_ch = 0;
        wifi_second_chan_t cur_second = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&cur_ch, &cur_second);
        CLI_LOG("ESP-NOW: %lu frames from HELM in the last 5 s (radio on channel %d, hunting=%d)\n",
            (unsigned long)(g_cyd_rx_frames - last_status_frames), (int)cur_ch, (int)g_cyd_hunting);
        last_status_frames = g_cyd_rx_frames;
        last_status_ms = millis();
    }

    bool lost = g_cyd_paired && (millis() - g_cyd_last_helm_rx_ms > CYD_LOST_CONTACT_MS);
    bool need_hunt = !g_cyd_paired || lost;

    if (!need_hunt) {
        if (g_cyd_hunting) {
            g_cyd_hunting = false;
            if (prefs_ok) prefs.putUChar("lastch", g_cyd_channel);
            Serial.printf("ESP-NOW: contact confirmed on channel %d\n", g_cyd_channel);
        }
        return;
    }

    if (!g_cyd_hunting) {
        g_cyd_hunting = true;
        uint8_t start = prefs_ok ? prefs.getUChar("lastch", 1) : 1;
        if (start < 1 || start > CYD_CHANNEL_SCAN_MAX) start = 1;
        g_cyd_channel = start;
        g_cyd_cached_tries_left = CYD_CACHED_CHANNEL_RETRIES;
        g_cyd_hunt_last_hop_ms = millis() - CYD_CHANNEL_DWELL_MS;   /* probe immediately */
        Serial.printf("ESP-NOW: %s - channel hunt from %d\n", lost ? "lost contact with HELM" : "not joined", g_cyd_channel);
    } else if (millis() - g_cyd_hunt_last_hop_ms >= CYD_CHANNEL_DWELL_MS) {
        if (g_cyd_cached_tries_left > 0) g_cyd_cached_tries_left--;
        else g_cyd_channel = (g_cyd_channel % CYD_CHANNEL_SCAN_MAX) + 1;
        g_cyd_hunt_last_hop_ms = millis();
    } else {
        return;   /* still dwelling on this channel */
    }

    esp_wifi_set_channel(g_cyd_channel, WIFI_SECOND_CHAN_NONE);
    espnow_hello_t hello = {};
    hello.type = ESPNOW_MSG_HELLO;
    fsec_my_mac(hello.mac);
    hello.node_type = NODE_TYPE_CYD;
    fsec_hello_tag(ESPNOW_MSG_HELLO, hello.mac, hello.node_type, NULL, hello.tag);
    esp_now_send(ESPNOW_BROADCAST_MAC, (uint8_t *)&hello, sizeof(hello));
    Serial.printf("ESP-NOW: HELLO on channel %d\n", g_cyd_channel);
}

/* serial WEBMODE: the CYD restarts into a mode that serves the setup page (ESP-NOW key + WiFi details) and
 * says where to find it on its own screen - see setup_web.h */
static void cyd_webmode_key_changed(void)
{
    if (prefs_ok) prefs.putBool("paired", false);   /* any old link used the old key */
}

static void cyd_webmode_show(const char *where)
{
    lvgl_port_lock(-1);
    lv_obj_t *bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, 320, 240);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(bg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 296);
    lv_label_set_text_fmt(l, "SETUP MODE\n\nOpen %s\n\nSet the ESP-NOW key and WiFi there. Ends by itself in %d minutes.",
        where, SW_MODE_MINUTES);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    lv_refr_now(NULL);
    lvgl_port_unlock();
}

static void cyd_webmode_run(void)
{
    SetupWebCtx ctx = { &prefs, prefs_ok, "CYD display", FW_BUILD, cyd_webmode_key_changed };
    sw_run_setup_mode(ctx, NULL, cyd_webmode_show);   /* never returns */
}
#endif /* TARGET_BOARD != BOARD_HELM_S3_800x480 */

/* ==================== bus_send(): transport-routing wrapper ====================
 * Same signature/call sites as the old CAN-only can_send() it replaces.
 * HELM routes per message: the 4 unicast dead-man/ignition commands go
 * ONLY to their specific engine's already-paired-and-encrypted ESP-NOW
 * peer if that engine's transport is ESP-NOW (never dual-emit - a
 * command for one engine must not broadcast to everyone); everything
 * else (ANNOUNCE/TELEM/HOURS/NAME/ALARM_SILENCE/ENROLL_REQUEST/ENROLL_ASSIGN/heartbeats)
 * dual-emits on every live transport, matching CAN's own bus-wide
 * topology - ESP-NOW broadcast frames can't be encrypted (a real ESP-NOW
 * constraint), so that bucket's authenticity rests on the sender being
 * on the pairing allowlist, weaker than the unicast bucket's AES check
 * (see can_protocol.h's AUTHORITY comment). CYD (no ESP-NOW pairing
 * infrastructure in this codebase yet) and can_sim-without-CAN-disabled
 * both just get plain CAN, unchanged from before this file had ESP-NOW
 * at all. The CAN fallback below checks `!g_can_disabled && can_ok`, NOT
 * can_ok alone - toggling "CAN Enabled" off in Settings/serial must
 * actually stop CAN transmission at runtime, not just skip can_setup()
 * on the NEXT boot (can_ok, once true, doesn't get cleared just because
 * the user unchecks the setting - it only reflects actual bus health). */
static void bus_send(uint32_t id, const uint8_t *data, uint8_t len)
{
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    int e;
    bool is_unicast = md_is_unicast_command(id, &e);

    if (is_unicast) {
        if (e >= 0 && e < MD_MAX_ENGINES && engine_slots[e].transport == BUS_TRANSPORT_ESPNOW) {
            CLI_LOG("bus_send: unicast id=0x%03lX engine %d -> ESP-NOW %02X:%02X:%02X:%02X:%02X:%02X\n",
                (unsigned long)id, e,
                engine_slots[e].espnow_peer_mac[0], engine_slots[e].espnow_peer_mac[1],
                engine_slots[e].espnow_peer_mac[2], engine_slots[e].espnow_peer_mac[3],
                engine_slots[e].espnow_peer_mac[4], engine_slots[e].espnow_peer_mac[5]);
            espnow_send_bus_frame(engine_slots[e].espnow_peer_mac, id, data, len);
            return;
        }
        /* point-to-point, no address needed - "transport==WIRED" alone
         * means there's exactly one possible destination for this frame */
        if (e >= 0 && e < MD_MAX_ENGINES && engine_slots[e].transport == BUS_TRANSPORT_WIRED) {
            CLI_LOG("bus_send: unicast id=0x%03lX engine %d -> WIRED\n", (unsigned long)id, e);
            wired_send_frame(id, data, len);
            return;
        }
        bool will_use_can = !g_can_disabled && can_ok;
        CLI_LOG("bus_send: unicast id=0x%03lX engine %d - transport=%d (not ESPNOW/WIRED), "
                      "%s (can_ok=%d, g_can_disabled=%d)\n",
            (unsigned long)id, e,
            (e >= 0 && e < MD_MAX_ENGINES) ? engine_slots[e].transport : -1,
            will_use_can ? "falling through to CAN" : "DROPPED - no usable transport",
            can_ok, g_can_disabled);
    }

    if (!g_can_disabled && can_ok) {
        twai_message_t m = {};
        m.identifier = id;
        m.data_length_code = len;
        m.ss = 1;   /* single shot: no endless retries when nobody ACKs */
        if (len) memcpy(m.data, data, len);
        twai_transmit(&m, 0);
    }

    /* broadcast bucket only - a unicast command already returned above,
     * whether it went out over ESP-NOW, WIRED, or fell through to CAN.
     * Wired is dual-emitted unconditionally alongside CAN/ESP-NOW
     * (always-attempted, see wired_bus.h) - harmless if nothing's
     * actually connected on the other end. */
    if (!is_unicast) {
        if (espnow_peer_count() > 0)
            espnow_send_bus_frame(ESPNOW_BROADCAST_MAC, id, data, len);
        /* every joined board also gets the heartbeat addressed to it: a broadcast can be heard (faintly)
         * from the wrong channel, an addressed encrypted frame only on the right one, so that is what
         * tells a board it is really in contact (cyd_espnow_on_recv(), can_sim's espnow_on_recv()) */
        if (id == MSG_HB_HELM)
            espnow_unicast_to_peers(id, data, len);
        if (g_wired_ok)
            wired_send_frame(id, data, len);
    }
#else
    if (!can_ok) return;
    twai_message_t m = {};
    m.identifier = id;
    m.data_length_code = len;
    m.ss = 1;
    if (len) memcpy(m.data, data, len);
    twai_transmit(&m, 0);
#endif
}

/* ==================== serial console (WiFi creds, any time) ====================
 * Bench convenience: set WiFi creds without touching the touchscreen -
 * handy when the panel isn't in reach, or the on-screen keyboard is
 * being fussy. Send over the CH340 serial port at any time (not just
 * during first-boot setup):
 *   WIFI:<ssid>,<password>\n     (password may be empty for open APs)
 * Only ever writes NVS + calls ESP.restart() - deliberately never
 * touches WiFi.* or any LVGL object from here: this runs on the Arduino
 * loop task, not the LVGL task, and CLAUDE.md's hard-won rule is that
 * WiFi APIs may only be touched from the one proven place (wifi_setup()
 * in setup()). A clean reboot re-runs that exact path instead. */

/* the serial menu: one table feeds Tab completion, the ? list and MENU */
static const CliCmd kCli[] = {
    { "MENU",     "",                  "show this list (also HELP or ?)" },
    { "STATUS",   "",                  "firmware build, ESP-NOW key and joined boards, WiFi" },
    { "KEY",      "<passphrase>",      "set the shared ESP-NOW secret (12+ characters, same on every board), then restart" },
    { "KEYSHOW",  "",                  "is a key set? its fingerprint (same on every board with the same key)" },
    { "KEYCLEAR", "",                  "forget the key (ESP-NOW goes off), then restart" },
    { "WIFI:",    "<ssid>,<password>", "save WiFi details and restart (password may be empty)" },
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    { "WEBMODE",  "",                  "where this display's setup web page is (its normal web page)" },
    { "UPDATE",   "",                  "check for updates, then update every board that needs it (this display last)" },
    { "CLEARPEERS", "",                "forget every joined board (they rejoin by themselves)" },
    { "CANON",    "",                  "enable CAN for the broadcast bucket" },
    { "CANOFF",   "",                  "disable CAN (ESP-NOW only)" },
#else
    { "WEBMODE",  "",                  "restart into setup mode: a web page to set the key and WiFi (10 minutes)" },
    { "REPAIR",   "",                  "forget HELM and look for it again" },
#endif
    { "REBOOT",   "",                  "restart" },
};
static const int kCliN = sizeof(kCli) / sizeof(kCli[0]);
#if TARGET_BOARD == BOARD_HELM_S3_800x480
#define CLI_PROMPT "HELM> "
#else
#define CLI_PROMPT "CYD> "
#endif

static void print_serial_help(void)
{
    Serial.println("Serial menu - Tab completes a command, ? lists them (case-insensitive):");
    for (int i = 0; i < kCliN; i++)
        Serial.printf("  %-11s %-18s %s\n", kCli[i].name, kCli[i].args, kCli[i].help);
}

static void handle_serial_line(char *line)
{
    if (strcasecmp(line, "HELP") == 0 || strcasecmp(line, "MENU") == 0 || strcmp(line, "?") == 0) {
        print_serial_help();
        return;
    }
    if (strncasecmp(line, "KEY ", 4) == 0) {
        const char *phrase = line + 4;
        while (*phrase == ' ') phrase++;
        if (strlen(phrase) < FSEC_MIN_PASSPHRASE) {
            Serial.printf("Serial: the passphrase must be at least %d characters\n", FSEC_MIN_PASSPHRASE);
            return;
        }
        Serial.println("Serial: working out the key (a second or two)...");
        if (!fsec_set_passphrase(prefs, prefs_ok, phrase)) {
            Serial.println("Serial: could not save the key (flash unavailable)");
            return;
        }
#if TARGET_BOARD == BOARD_HELM_S3_800x480
        espnow_pairing_clear_all();   /* boards joined under the old key are of no use */
#else
        if (prefs_ok) prefs.putBool("paired", false);
#endif
        Serial.printf("Serial: key saved, fingerprint %08lX (it must read the same on every board) - restarting\n",
            (unsigned long)fsec_fingerprint());
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "KEYSHOW") == 0 || strcasecmp(line, "KEY?") == 0 || strcasecmp(line, "KEY") == 0) {
        if (g_fsec_have_key) Serial.printf("Serial: key is set, fingerprint %08lX\n", (unsigned long)fsec_fingerprint());
        else Serial.println("Serial: no key set - ESP-NOW is off. Type  KEY <passphrase>");
        return;
    }
    if (strcasecmp(line, "KEYCLEAR") == 0) {
        fsec_clear(prefs, prefs_ok);
#if TARGET_BOARD == BOARD_HELM_S3_800x480
        espnow_pairing_clear_all();
#else
        if (prefs_ok) prefs.putBool("paired", false);
#endif
        Serial.println("Serial: key cleared - restarting with ESP-NOW off");
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
#if TARGET_BOARD != BOARD_HELM_S3_800x480
    if (strcasecmp(line, "WEBMODE") == 0) {
        if (prefs_ok) prefs.putBool("webmode", true);
        Serial.println("Serial: restarting into setup mode");
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
#else
    if (strcasecmp(line, "WEBMODE") == 0) {
        Serial.println(WiFi.status() == WL_CONNECTED
            ? "Serial: this display's setup page is its normal web page - open http://<its IP>/ (see STATUS)"
            : "Serial: connect it to WiFi first (WIFI:<ssid>,<password>); its web page is then at its IP address");
        return;
    }
#endif
    if (strcasecmp(line, "REBOOT") == 0) {
        Serial.println("Serial: restarting");
        Serial.flush();
        delay(200);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "STATUS") == 0) {
        Serial.printf("Firmware build %d, uptime %lus\n", FW_BUILD, (unsigned long)(millis() / 1000));
        Serial.printf("ESP-NOW key: %s", g_fsec_have_key ? "set" : "NOT SET");
        if (g_fsec_have_key) Serial.printf(", fingerprint %08lX", (unsigned long)fsec_fingerprint());
        Serial.println();
#if TARGET_BOARD == BOARD_HELM_S3_800x480
        Serial.printf("Joined boards: %d\n", espnow_peer_count());
        for (int i = 0; i < ESPNOW_MAX_PAIRED; i++) {
            if (!espnow_peers[i].has_mac) continue;
            Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X  type %d\n",
                espnow_peers[i].mac[0], espnow_peers[i].mac[1], espnow_peers[i].mac[2],
                espnow_peers[i].mac[3], espnow_peers[i].mac[4], espnow_peers[i].mac[5], espnow_peers[i].node_type);
        }
        Serial.printf("WiFi: %s", WiFi.status() == WL_CONNECTED ? "connected, " : "not connected");
        if (WiFi.status() == WL_CONNECTED) Serial.printf("%s, channel %d", WiFi.localIP().toString().c_str(), (int)WiFi.channel());
        Serial.println();
        Serial.printf("CAN: %s\n", g_can_disabled ? "disabled by setting" : (can_ok ? "running" : "not healthy"));
#else
        Serial.printf("Link to HELM: %s\n", g_cyd_paired ? "joined" : "not joined");
#endif
        return;
    }
#if TARGET_BOARD != BOARD_HELM_S3_800x480
    if (strcasecmp(line, "REPAIR") == 0) {
        cyd_espnow_forget_helm();
        Serial.println("ESP-NOW: forgot HELM, looking for it again");
        return;
    }
#endif
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    if (strcasecmp(line, "CLEARPEERS") == 0 || strcasecmp(line, "CLEARPAIRS") == 0) {
        espnow_pairing_clear_all();
        return;
    }
    if (strcasecmp(line, "UPDATE") == 0) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("Serial: no WiFi connection - set it with WIFI:<ssid>,<password>");
            return;
        }
        g_force_update_check = true;
        g_update_check_request_id++;
        g_serial_update_wait_id = g_update_check_request_id;
        Serial.println("Serial: checking for updates...");
        return;
    }
    if (strcasecmp(line, "CANON") == 0 || strcasecmp(line, "CANOFF") == 0) {
        g_can_disabled = (strcasecmp(line, "CANOFF") == 0);
        if (prefs_ok) prefs.putBool("can_dis", g_can_disabled);
        Serial.printf("Serial: CAN %s (bus_send will now use %s for the broadcast bucket)\n",
            g_can_disabled ? "disabled" : "enabled",
            g_can_disabled ? "ESP-NOW only" : "CAN when available");
        return;
    }
#endif

    if (strncasecmp(line, "WIFI:", 5) != 0) {
        Serial.println("Serial: unrecognized command. Type HELP for the list.");
        return;
    }
    if (g_display_role == DISPLAY_ROLE_SECONDARY) {
        Serial.println("Serial: this is a secondary display - it never joins WiFi, ignored");
        return;
    }
    char *rest = line + 5;
    char *comma = strchr(rest, ',');
    const char *ssid, *pass;
    if (comma) {
        *comma = 0;
        ssid = rest;
        pass = comma + 1;
    } else {
        ssid = rest;
        pass = "";
    }
    if (!ssid[0]) {
        Serial.println("Serial: empty SSID, ignored");
        return;
    }
    if (!prefs_ok) {
        Serial.println("Serial: NVS unavailable, can't save WiFi creds");
        return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    Serial.printf("Serial: saved WiFi ssid=\"%s\" - restarting to connect...\n", ssid);
    Serial.flush();
    delay(200);
    ESP.restart();
}

static void serial_console_tick(void)
{
    static char line[CLI_LINE_MAX];
    static size_t len = 0;
    static bool greeted = false;
    if (!greeted && millis() > 3000) {   /* once, after the boot messages */
        greeted = true;
        Serial.print("\r\nType ? for the command list; Tab completes.\r\n" CLI_PROMPT);
    }
    while (Serial.available()) {
        if (cli_feed((char)Serial.read(), kCli, kCliN, CLI_PROMPT, line, sizeof(line), &len)) {
            if (len > 0) handle_serial_line(line);
            len = 0;
            Serial.print(CLI_PROMPT);
        }
    }
}

/* ==================== WiFi debug server ==================== */

static WebServer server(80);

static const char DEBUG_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width">
<title>MD2030 Debug</title><style>
body{font-family:sans-serif;background:#0a1929;color:#eee;padding:20px;max-width:480px;margin:auto}
label{display:block;margin-top:18px}
input[type=range]{width:100%}
input[type=number]{width:8em;background:#142838;color:#eee;border:1px solid #33475c;padding:4px}
.al{margin-top:18px;font-size:1.1em}
span{color:#6cf}
h2{color:#fff}
em{color:#888;font-size:0.85em}
</style></head><body>
<h2>MD2030 Panel &mdash; Debug</h2>
<em>Fudged values write the same struct CAN telemetry writes.
With the control board online, telemetry will overwrite these
within 100 ms - disconnect CAN to test from here.</em>
<label>RPM: <span id="vr">0</span>
<input type="range" min="0" max="4000" value="0" id="rpm"></label>
<label>Coolant &deg;C: <span id="vt">60</span>
<input type="range" min="0" max="130" value="60" id="temp"></label>
<label>Oil pressure (bar): <span id="vp">3.0</span>
<input type="range" min="0" max="6" step="0.1" value="3" id="oil"></label>
<label>Engine hours:
<input type="number" min="0" max="99999" step="0.1" value="0" id="hours">
<button type="button" id="hoursSet">Set</button></label>
<div class="al"><input type="checkbox" id="ta"> Temperature alarm</div>
<div class="al"><input type="checkbox" id="pa"> Oil pressure alarm</div>
<h2>Fake engines</h2>
<em>Enable simulated engines on the bus. Sliders above feed whichever
engine the panel has selected. "ignition on" is tracked per-engine even
when not selected - it's what a secondary display's engine picker
filters on.</em>
<div class="al"><input type="checkbox" id="f0"> E0: Volvo Penta MD2030C
  <input type="checkbox" id="g0"> ignition on</div>
<div class="al"><input type="checkbox" id="f1"> E1: Yanmar 2YM15
  <input type="checkbox" id="g1"> ignition on</div>
<div class="al"><input type="checkbox" id="f2"> E2: Yanmar 4JH80
  <input type="checkbox" id="g2"> ignition on</div>
<div class="al"><input type="checkbox" id="f3"> E3: Nanni Diesel
  <input type="checkbox" id="g3"> ignition on</div>
<h2>ESP-NOW</h2>
<em>There is no pairing window. A board joins by itself, at any time, if it holds the same secret
passphrase as this display. The secret is kept only in each board's own flash.</em>
<div class="al">Status: <span id="espstat">--</span></div>
<form method="POST" action="/key" class="al">Passphrase (12 or more characters, the same on every board)<br>
<input type="password" name="phrase" autocomplete="off" style="width:100%;background:#142838;color:#eee;border:1px solid #33475c;padding:6px">
<button type="submit">Save key and restart</button></form>
<div class="al">
<button type="button" id="espClearBtn">Forget joined boards</button>
</div>
<h2>Remote WiFi Join</h2>
<em>Hands every currently-present engine's board HELM's own saved WiFi
credentials and tells it to join and stay on that network (saves to its
own NVS and reboots, same as its serial WIFI:&lt;ssid&gt;,&lt;password&gt;
command) - handy for reaching a board's own debug page without a USB
cable. Every targeted board reboots to connect, briefly dropping off the
bus.</em>
<div class="al">
<button type="button" id="wifiJoinAllBtn">Join &amp; Stay on WiFi (All Devices)</button>
<span id="wifiJoinStat"></span>
</div>
<h2>Serial Log</h2>
<em>Mirrors this board's own USB/CH340 serial output - useful when
there's no cable attached to actually watch it. Newest lines at the
bottom; auto-scrolls unless paused. <b>Tap Pause before trying to select
text</b> - a box that keeps refreshing out from under you makes
selecting anything basically impossible.</em>
<div class="al">
<button type="button" id="serialPauseBtn">Pause</button>
</div>
<textarea id="seriallog" readonly style="background:#000;color:#0f0;padding:8px;width:100%;height:240px;font-size:0.8em;border:1px solid #33475c;box-sizing:border-box;resize:vertical"></textarea>
<script>
var serialPaused = false;
function serialPoll(){
  if (serialPaused) return;
  fetch('/serial').then(function(r){ return r.text(); }).then(function(t){
    var el = document.getElementById('seriallog');
    var atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 4;
    el.value = t;
    if (atBottom) el.scrollTop = el.scrollHeight;
  }).catch(function(){});
}
document.getElementById('serialPauseBtn').addEventListener('click', function(){
  serialPaused = !serialPaused;
  this.textContent = serialPaused ? 'Resume' : 'Pause';
});
setInterval(serialPoll, 1000);
serialPoll();
</script>
<script>
function espPoll(){
  fetch('/espstatus').then(function(r){ return r.json(); }).then(function(j){
    var s = (j.key_set ? 'key set' : 'NO KEY - ESP-NOW is off') + ', ' + j.peers + ' board' + (j.peers==1?'':'s') + ' joined';
    document.getElementById('espstat').textContent = s;
  }).catch(function(){});
}
document.getElementById('espClearBtn').addEventListener('click', function(){
  fetch('/espclear').then(espPoll).catch(function(){});
});
setInterval(espPoll, 1000);
espPoll();
document.getElementById('wifiJoinAllBtn').addEventListener('click', function(){
  var btn = this, stat = document.getElementById('wifiJoinStat');
  btn.disabled = true;
  stat.textContent = 'sending...';
  fetch('/wifijoinall').then(function(r){ return r.text(); }).then(function(t){
    stat.textContent = t;
    btn.disabled = false;
  }).catch(function(){ stat.textContent = 'request failed'; btn.disabled = false; });
});
</script>
<script>
var busy=false, dirty=false;
function push(){
  busy=true; dirty=false;
  fetch('/set?rpm='+document.getElementById('rpm').value
    +'&temp='+document.getElementById('temp').value
    +'&oil='+document.getElementById('oil').value
    +'&hours='+document.getElementById('hours').value
    +'&ta='+(document.getElementById('ta').checked?1:0)
    +'&pa='+(document.getElementById('pa').checked?1:0)
    +'&f0='+(document.getElementById('f0').checked?1:0)
    +'&f1='+(document.getElementById('f1').checked?1:0)
    +'&f2='+(document.getElementById('f2').checked?1:0)
    +'&f3='+(document.getElementById('f3').checked?1:0)
    +'&g0='+(document.getElementById('g0').checked?1:0)
    +'&g1='+(document.getElementById('g1').checked?1:0)
    +'&g2='+(document.getElementById('g2').checked?1:0)
    +'&g3='+(document.getElementById('g3').checked?1:0))
  .catch(function(){})
  .finally(function(){ busy=false; if(dirty) push(); });
}
function send(){
  document.getElementById('vr').textContent=document.getElementById('rpm').value;
  document.getElementById('vt').textContent=document.getElementById('temp').value;
  document.getElementById('vp').textContent=document.getElementById('oil').value;
  if(busy){ dirty=true; } else { push(); }
}
['rpm','temp','oil','ta','pa','f0','f1','f2','f3','g0','g1','g2','g3'].forEach(function(id){
  document.getElementById(id).addEventListener('input',send);
});
/* hours is a typed number, not a dragged slider - only send it when
 * the Set button is pressed, not on every keystroke */
document.getElementById('hoursSet').addEventListener('click', function(){
  fetch('/set?hours='+document.getElementById('hours').value+'&hset=1').catch(function(){});
});
</script></body></html>
)HTML";

static void handle_root(void)
{
    Serial.println("HTTP: serving debug page");
    server.send_P(200, "text/html", DEBUG_PAGE);
}

/* linearizes g_serial_log_buf's ring into chronological order and serves
 * it as plain text - see the LoggingSerial class up near the top of the
 * file for how the ring gets filled. Static scratch buffer (not a stack
 * array - SERIAL_LOG_BUF_SIZE+1 is too big to put on this task's stack,
 * same reasoning as other big one-shot buffers elsewhere in this file);
 * safe because WebServer handles one request at a time. Reads the ring
 * under g_serial_log_mux, same as every write does - see that variable's
 * comment for why an unsynchronized read here previously corrupted
 * memory and took the whole server down. */
static void handle_serial_log(void)
{
    static char out[SERIAL_LOG_BUF_SIZE + 1];
    size_t n;
    portENTER_CRITICAL(&g_serial_log_mux);
    if (g_serial_log_wrap) {
        size_t tail = SERIAL_LOG_BUF_SIZE - g_serial_log_pos;
        memcpy(out, g_serial_log_buf + g_serial_log_pos, tail);
        memcpy(out + tail, g_serial_log_buf, g_serial_log_pos);
        n = SERIAL_LOG_BUF_SIZE;
    } else {
        memcpy(out, g_serial_log_buf, g_serial_log_pos);
        n = g_serial_log_pos;
    }
    portEXIT_CRITICAL(&g_serial_log_mux);
    out[n] = 0;
    server.send(200, "text/plain", out);
}

static void handle_set(void)
{
    if (server.hasArg("rpm"))  eng.rpm         = server.arg("rpm").toInt();
    if (server.hasArg("temp")) eng.temp_c      = server.arg("temp").toFloat();
    if (server.hasArg("oil"))  eng.oil_bar     = server.arg("oil").toFloat();
    if (server.hasArg("ta"))   eng.temp_alarm  = (server.arg("ta") == "1");
    if (server.hasArg("pa"))   eng.press_alarm = (server.arg("pa") == "1");
    /* hset marks a deliberate press of the debug page's Set button, as
     * opposed to "hours" just riding along on every slider's push() -
     * without it, an explicit 0 could never be set (h > 0 would skip it)
     * and every rpm/temp/oil drag would keep re-applying a stale value */
    if (server.hasArg("hours") && server.hasArg("hset")) {
        float h = server.arg("hours").toFloat();
        eng.hours_x10  = (uint32_t)lroundf(h * 10.0f);
        eng.hours_seen = true;
    }
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        char key[4] = { 'f', (char)('0' + i), 0, 0 };
        if (server.hasArg(key)) fake_en[i] = (server.arg(key) == "1");

        /* "ignition on" per fake engine - only meaningful for engines
         * WE'RE simulating, same reasoning as fake_engines_tick's
         * "we set it, we own it" rule for present/ign_on */
        char gkey[4] = { 'g', (char)('0' + i), 0, 0 };
        if (server.hasArg(gkey) && fake_en[i])
            engines[i].ign_on = (server.arg(gkey) == "1");
    }
    server.send(200, "text/plain", "ok");
}

#if TARGET_BOARD == BOARD_HELM_S3_800x480
/* a new key makes every joined board's link useless - forget them, they rejoin under the new key */
static void espnow_pairing_clear_all_cb(void)
{
    espnow_pairing_clear_all();
}

static void handle_espnow_status(void)
{
    char buf[120];
    snprintf(buf, sizeof(buf), "{\"pairing_mode\":false,\"remaining_ms\":0,\"key_set\":%s,\"peers\":%d}",
        g_fsec_have_key ? "true" : "false", espnow_peer_count());
    server.send(200, "application/json", buf);
}

static void handle_espnow_pair(void)
{
    g_pairing_mode = true;
    g_pairing_mode_start_ms = millis();
    Serial.println("ESP-NOW: pairing window opened (60s) [via debug page]");
    server.send(200, "text/plain", "ok");
}

static void handle_espnow_clear(void)
{
    espnow_pairing_clear_all();
    server.send(200, "text/plain", "ok");
}

/* Shared by the debug-page button (handle_wifi_join_all() below) and the
 * touchscreen Settings dialog's "Join All WiFi" button (wifi_join_all_
 * cb()) - hands every currently-present engine HELM's own saved WiFi
 * credentials via MSG_WIFI_JOIN (see can_protocol.h and wifi_join_send()
 * above), over whichever transport that engine is actually on right now
 * (CAN, ESP-NOW, or the direct-wire link - bus_send()'s existing per-
 * engine routing handles all three identically, no special-casing needed
 * here). The receiving board saves the credentials to its own NVS and
 * reboots to connect - same effect as its serial WIFI:<ssid>,<password>
 * command, just triggered remotely instead of over USB. Reaches every
 * physical board that has AT LEAST ONE engine slot enrolled and present
 * - a board enrolled ONLY as an alarmer (no engine slots at all) isn't
 * reachable this way; not a concern for can_sim today (always enrolls at
 * least one engine) or any other board that exists yet, but noted as a
 * known gap, same style as the "no per-device-type field yet" gap in the
 * OTA section. Deliberately unicast+encrypted when the target is on
 * ESP-NOW (MSG_WIFI_JOIN is in md_is_unicast_command()'s bucket) since it
 * carries HELM's own WiFi password - see can_protocol.h's comment.
 * Writes a human-readable result into result_buf (both callers just show
 * it verbatim). */
static void wifi_join_all_devices(char *result_buf, size_t result_buf_size)
{
    /* kept short deliberately - shown both inline on the debug page next
     * to the button, and in a narrow ~184px column in the touchscreen
     * Settings dialog (see settings_cb()) */
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(result_buf, result_buf_size, "No WiFi connection - nothing sent");
        return;
    }
    String ssid = prefs_ok ? prefs.getString("ssid", "") : String("");
    String pass = prefs_ok ? prefs.getString("pass", "") : String("");
    if (!ssid.length()) {
        snprintf(result_buf, result_buf_size, "No saved WiFi credentials");
        return;
    }
    int sent = 0;
    for (int e = 0; e < MD_MAX_ENGINES; e++) {
        if (!engines[e].present) continue;
        wifi_join_send(e, ssid.c_str(), pass.c_str());
        sent++;
    }
    if (sent > 0)
        snprintf(result_buf, result_buf_size, "Sent to %d device(s), rebooting", sent);
    else
        snprintf(result_buf, result_buf_size, "No engines present - nothing sent");
}

static void handle_wifi_join_all(void)
{
    char buf[64];
    wifi_join_all_devices(buf, sizeof(buf));
    server.send(200, "text/plain", buf);
}
#endif /* TARGET_BOARD == BOARD_HELM_S3_800x480 */

static void wifi_setup(void)
{
    /* credential resolution: compiled-in values win and are persisted;
     * empty compiled-in values fall back to what's saved in flash.
     * prefs is begun in setup(). */
    String ssid = WIFI_SSID;
    String pass = WIFI_PASSWORD;

    if (ssid.length() > 0) {
        if (prefs_ok) {
            prefs.putString("ssid", ssid);
            prefs.putString("pass", pass);
        }
        Serial.println("WiFi: using compiled-in credentials (saved to flash)");
    } else if (prefs_ok) {
        ssid = prefs.getString("ssid", "");
        pass = prefs.getString("pass", "");
        if (ssid.length() > 0) {
            Serial.println("WiFi: using credentials saved in flash");
        }
    }

    /* no credentials = never asked for WiFi - stay off it entirely.
     * No AP broadcast, no STA join attempt, no web server: just park the
     * radio in STA mode on a fixed channel so ESP-NOW still works (it
     * only needs WiFi.mode() to have run, not an actual connection).
     * WIFI:<ssid>,<password> over serial (works with no network at all)
     * or Settings/the wizard is the only way this board starts using
     * WiFi - see CLAUDE.md's ESP-NOW section on why this matters for a
     * bench setup that's often deliberately run with no WiFi at all. */
    if (ssid.length() == 0) {
        WiFi.mode(WIFI_STA);
        esp_wifi_set_channel(ESPNOW_AP_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
        Serial.printf("WiFi: no credentials - staying off WiFi (ESP-NOW only, channel %d). "
                      "Send WIFI:<ssid>,<password> over serial to enable networking.\n",
                      ESPNOW_AP_FALLBACK_CHANNEL);
        snprintf(wifi_ip_text, sizeof(wifi_ip_text), "WiFi off (ESP-NOW only)");
        return;
    }

    Serial.printf("Connecting to WiFi \"%s\"", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   /* no modem power-save: web UI stays snappy */
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        /* credentials were explicitly given but didn't work - still don't
         * fall back to broadcasting our own AP, that's unrequested WiFi
         * activity too. Stay off network like the no-credentials case;
         * WIFI:<ssid>,<password> retries, or move back in range + reboot. */
        esp_wifi_set_channel(ESPNOW_AP_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
        Serial.printf("WiFi: join failed - staying off WiFi (ESP-NOW only, channel %d), "
                      "debug page unreachable until WIFI:<ssid>,<password> succeeds\n",
                      ESPNOW_AP_FALLBACK_CHANNEL);
        snprintf(wifi_ip_text, sizeof(wifi_ip_text), "WiFi join failed (ESP-NOW only)");
        return;
    }

    Serial.print("Connected. Debug page: http://");
    Serial.println(WiFi.localIP());
    snprintf(wifi_ip_text, sizeof(wifi_ip_text),
             "Debug: http://%s", WiFi.localIP().toString().c_str());
    Serial.printf("WiFi: channel %d (ESP-NOW will use this)\n", WiFi.channel());

    server.on("/", handle_root);
    server.on("/set", handle_set);
    server.on("/serial", handle_serial_log);
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    server.on("/espstatus", handle_espnow_status);
    {
        SetupWebCtx ctx = { &prefs, prefs_ok, "HELM display", FW_BUILD, espnow_pairing_clear_all_cb };
        sw_register(server, ctx, false);   /* /key, /wifi and /reboot; "/" stays the debug page */
    }
    server.on("/espclear", handle_espnow_clear);
    server.on("/wifijoinall", handle_wifi_join_all);
#endif
    server.begin();
}

/* ==================== arduino entry points ==================== */

void setup()
{
    Serial.begin(115200);
    Serial.println("MD2030 helm panel starting (step 2 - multi-engine)");

    /* NVS first: settings selector and engine persistence need it */
    prefs_ok = prefs.begin("md2030", false);
    fsec_begin(prefs, prefs_ok);   /* the shared ESP-NOW secret, if one was set */
    if (!prefs_ok) Serial.println("NVS: prefs.begin failed - running without");
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    if (prefs_ok) {
        sel_engine = prefs.getUChar("engine", 0);
        if (sel_engine >= MD_MAX_ENGINES) sel_engine = 0;
        g_display_role = prefs.getUChar("role", DISPLAY_ROLE_PRIMARY);
        need_setup = !prefs.getBool("setup_done", false);
        String cur_pin = prefs.getString("pin", "");
        strncpy(stored_pin, cur_pin.c_str(), sizeof(stored_pin) - 1);
        g_can_disabled = prefs.getBool("can_dis", false);
    }
    autostart_load_from_nvs();   /* HELM/primary only - see as_*[] declaration comment */
#else
    /* CYD is unconditionally secondary - nothing to configure, no wizard,
     * no PIN (see create_ui_cyd()'s comment) - set unconditionally
     * (not gated on prefs_ok) so an NVS failure can never fall back to
     * the DISPLAY_ROLE_PRIMARY/need_setup=false compile-time defaults,
     * which would be wrong for this board. */
    g_display_role = DISPLAY_ROLE_SECONDARY;
    need_setup = false;
    if (prefs_ok) {
        sel_engine = prefs.getUChar("engine", 0);
        if (sel_engine >= MD_MAX_ENGINES) sel_engine = 0;
    }
#endif
    enroll_load_from_nvs();   /* bus master: MAC->id table (PRIMARY only sends assigns) */

    Board *board = new Board();
    g_board = board;   /* see g_board's declaration - OTA backlight blanking */
    board->init();

#if LVGL_PORT_AVOID_TEARING_MODE
    board->getLCD()->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#endif

    /* Bounce buffer + reduced pixel clock: keeps the RGB panel stable
     * while WiFi is active (PSRAM bandwidth contention).
     * SIZE MATTERS: the driver allocates TWO bounce buffers in
     * DMA-capable INTERNAL SRAM. 48 lines = 2 x 76.8KB = ~154KB, which
     * starved the heap to ~3.5KB and broke WiFi joins, page serving,
     * everything. 16 lines = 2 x 25.6KB = ~51KB: frees ~100KB. */
#define RGB_PCLK_HZ (14 * 1000 * 1000)
#define RGB_BOUNCE_LINES 16
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    {
        auto lcd = board->getLCD();
        auto lcd_bus = lcd->getBus();
        if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
            auto rgb_bus = static_cast<BusRGB *>(lcd_bus);
            rgb_bus->configRGB_BounceBufferSize(
                lcd->getFrameWidth() * RGB_BOUNCE_LINES);
            rgb_bus->configRgbTimingFreqHz(RGB_PCLK_HZ);
            Serial.printf("RGB: bounce buffer (%d lines) + PCLK 14MHz configured\n",
                          RGB_BOUNCE_LINES);
        }
    }
#else
    Serial.println("RGB: WARNING - bounce buffer block COMPILED OUT");
#endif

    heap_report("pre-begin");
    assert(board->begin());
    heap_report("board");

    lvgl_port_init(board->getLCD(), board->getTouch());
    heap_report("lvgl");

    lvgl_port_lock(-1);
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    create_ui();
    if (need_setup) build_setup_dialog();
    /* PIN only ever applies to primary - a secondary display doesn't
     * wake up until an engine is switched on (see ui_tick's power state
     * machine), so there's nothing on it worth locking. The wizard never
     * lets a secondary set one, but this guard is the actual enforcement
     * point in case that ever drifts out of sync. */
    else if (stored_pin[0] && g_display_role == DISPLAY_ROLE_PRIMARY) build_lock_overlay();
#else
    create_ui_cyd();   /* need_setup is always false, no wizard/lock screen to reach */
#endif
    lvgl_port_unlock();
    heap_report("ui");

#if TARGET_BOARD != BOARD_HELM_S3_800x480
    if (prefs_ok && prefs.getBool("webmode", false)) {   /* serial WEBMODE asked for the setup page */
        prefs.putBool("webmode", false);
        cyd_webmode_run();
    }
#endif

#if TARGET_BOARD == BOARD_HELM_S3_800x480
    if (!g_can_disabled) can_setup();
    else Serial.println("CAN: disabled by user setting - skipping TWAI init");
    wired_setup();   /* always-on supplementary transport, see wired_bus.h */
#else
    can_setup();
    cyd_espnow_setup();   /* CYD: join the ESP-NOW network through HELM */
#endif
    heap_report("can");
    if (g_display_role == DISPLAY_ROLE_PRIMARY) {
        wifi_setup();
        heap_report("wifi");
#if TARGET_BOARD == BOARD_HELM_S3_800x480
        espnow_setup();
        heap_report("espnow");
#endif
        Serial.println("Serial: send HELP at any time for the full command list");
    } else {
        Serial.println("WiFi: skipped - secondary display role never joins/serves");
    }

    /* OTA rollback safety: an image flashed via Update.h/HTTPUpdate boots
     * into a PENDING_VERIFY state on a rollback-enabled partition table
     * (HELM's app3M_fat9M_16MB has the required ota_0/ota_1 pair - see
     * engine_display/CLAUDE.md's OTA section) and gets automatically
     * rolled back to the previous image if it's never explicitly marked
     * valid before the next reboot. Marking it here, after board/display/
     * WiFi/ESP-NOW init have all already run without crashing, is a
     * reasonable "this image actually works" bar - a genuinely broken
     * update won't reach this line at all, and falls back on its own.
     * Harmless no-op on a table without rollback enabled. */
    esp_ota_mark_app_valid_cancel_rollback();

    Serial.println("setup() complete");
}

void loop()
{
    static bool first = true;
    if (first) { first = false; Serial.println("loop() running"); }
    static uint32_t last_hb = 0;
    if (millis() - last_hb > 2000) {
        last_hb = millis();
        CLI_LOG("loop alive: internal=%u largest=%u uptime=%lus\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned long)(millis() / 1000));
    }

    /* server was never .begin()'d on a secondary display - see setup() */
    if (g_display_role == DISPLAY_ROLE_PRIMARY) server.handleClient();
#if TARGET_BOARD == BOARD_HELM_S3_800x480
    pairing_mode_tick();
    espnow_upgrade_tick();
    check_for_update_tick();
    wired_bus_tick();
#else
    cyd_pairing_tick();
#endif
    serial_console_tick();
    fake_engines_tick();
    can_poll();
    engine_ageout_tick();
    can_send_commands();
    delay(2);
}
