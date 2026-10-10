/**
 * can_sim.ino - CAN bus node simulator
 *
 * Pretends to be THREE nodes on the bus at once, so the HELM/secondary
 * displays can be bench-tested against real CAN traffic instead of the
 * HELM's own local (software-only) fake engines:
 *
 *   - 2 fake engine CTRL boards (ANNOUNCE + telemetry + hours; honors
 *     glow/start-held dead-man commands, latched ignition, and - for
 *     SIM_B only - CAP_STOP/stop-held, exactly like a real CTRL board
 *     would per can_protocol.h's authority/dead-man rules)
 *   - 1 "alarmer" unit: listens to EVERY engine's alarm flags on the
 *     bus (not just its own two - a real standalone alarmer box would
 *     hear whatever's actually out there), drives a real buzzer, and
 *     honors per-engine CMD_ALARM_SILENCE the same way the display does
 *
 * SIM_A has no CAP_STOP (mirrors the real MD2030 - mechanical-stop-only).
 * SIM_B has CAP_STOP, so the HELM's electric-stop button has something
 * real to talk to over an actual bus, not just the local debug-page fake.
 *
 * WiFi debug page (joins WiFi the same shape as the HELM panel - blocking
 * join, NVS persistence, AP fallback - but no setup wizard or PIN lock,
 * since there's no screen to show one on): enable/disable each engine
 * independently (vanishes off the bus when disabled, like the real thing
 * being powered down), toggle "random rev" per engine (ramps between
 * idle and just-over-redline instead of sitting still), and a live
 * rpm/temp/oil/hours readout for both. No screen also means WiFi creds
 * are configured over serial - "WIFI:<ssid>,<password>" at any time (see
 * handle_serial_line) - since that's the only input this board has.
 *
 * Board: ESP32-S3-Zero (Waveshare, ESP32-S3FH4R2 - 4MB flash, 2MB quad
 * PSRAM, NOT the octal PSRAM the HELM board uses). See CLAUDE.md in this
 * folder for board settings and pin choices.
 *
 * REQUIRES can_protocol.h in this same folder, copied by hand from
 * ../engine_display/can_protocol.h (the canonical copy) - Arduino sketches
 * can't share files across folders. Keep them in sync; see the note at
 * the top of that file.
 */

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"   /* esp_wifi_set_channel() - pins the radio's channel
                          * without an actual STA join/AP broadcast, see
                          * wifi_setup()'s no-credentials path */
#include <WebServer.h>
#include <Preferences.h>
#include "driver/twai.h"
#include "can_protocol.h"
#include <esp_now.h>
#include "espnow_pairing.h"
#include "espnow_bus.h"
#include "fleet_security.h"
#include "setup_web.h"
#include "wired_bus.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   /* GitHub release downloads are https */
#include <HTTPClient.h>
#include <HTTPUpdate.h>   /* httpUpdate global singleton - download+flash convenience wrapper */
#include <Update.h>       /* esp_ota_* lower-level API, needed for the app-valid rollback marker */
#include "esp_ota_ops.h"  /* esp_ota_mark_app_valid_cancel_rollback() */

/* OTA: bump by hand every release - see engine_display.ino's identical
 * FW_BUILD for the full reasoning (monotonic build number, not semver).
 * Reported in this board's own ANNOUNCE (sim_engine_send_announce()) so
 * HELM's device list knows whether it's out of date. */
#define FW_BUILD 27

/* which hardware this image is built for (reported in ANNOUNCE[7], so HELM
 * offers this board the matching firmware from the OTA manifest) */
#if CONFIG_IDF_TARGET_ESP32S3
#define HW_ID  HW_CANSIM_S3ZERO
#elif CONFIG_IDF_TARGET_ESP32C3
#define HW_ID  HW_CANSIM_C3
#else
#error "can_sim: no HW_ID for this chip - add one in can_protocol.h (HW_*, md_hw_key()) and here, plus a variant in tools/devices.json"
#endif

/* This user's ESP32-C3 modules cannot transmit at the default (maximum, 19.5 dBm)
 * power: the radio goes completely silent - no beacons, no association, no
 * ESP-NOW - while receiving still works (also found on the FishFinderProBluetooth
 * C3, see its wifi_manager.cpp, which uses 8.5). Measured on this board with a
 * receiver 30 cm away, stepping the power in software: 19.5 dBm never transmits;
 * 19 and 18.5 work when approached from below but not after starting at 19.5;
 * 17 dBm and below always work, and the received level stops rising above 17.
 * 15 dBm keeps a safe margin below that cliff and about twice the range of 8.5.
 * Call after every WiFi.mode(). The S3 boards are unaffected. */
static void wifi_tx_cap(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
    WiFi.setTxPower(WIFI_POWER_15dBm);
#endif
}

#if CONFIG_IDF_TARGET_ESP32C3
/* ESP32-C3: only GPIO 0-10 and 18-21 exist. Avoid GPIO 2/8/9 (strapping),
 * 18/19 (native USB, used for Serial here) and 20/21 (UART0). CAN is on 7/10
 * to match the Esp32RaymarineSeatalk board's CAN pins; GPIO 4-6 double as
 * JTAG pins, harmless unless JTAG is used. */
#define PIN_CAN_TX     7
#define PIN_CAN_RX     10
#define PIN_BUZZER     6
#define PIN_WIRED_TX   4
#define PIN_WIRED_RX   5
#else
/* ==================== pins ====================
 * ESP32-S3-Zero: GPIO 1-13 are the easily-solderable header pins.
 * GPIO 0 is the BOOT button - avoid. GPIO 21 drives the onboard WS2812.
 * GPIO 43/44 are default UART0 - left free for Serial debug output. */
#define PIN_CAN_TX     4
#define PIN_CAN_RX     5
#define PIN_BUZZER     6

/* Direct-wire supplementary transport (see wired_bus.h) - 2 of this
 * board's free header pins, cross-wired to the display's PIN_WIRED_TX/RX
 * (see engine_display/engine_display.ino): this board's TX(1) -> display
 * RX, this board's RX(2) <- display TX, plus a common GND. */
#define PIN_WIRED_TX   1
#define PIN_WIRED_RX   2
#endif

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

/* ==================== wifi ====================
 * No screen on this board, so there's no setup wizard/PIN lock like the
 * HELM panel has - but it still needs SOME way to get WiFi creds onto a
 * headless device. Same resolution rule as the panel: non-empty
 * compiled-in values here win and get saved to NVS; if both are blank,
 * whatever's already saved in NVS is used instead. Creds can also be set
 * at any time over serial - see handle_serial_line() - which is really
 * the intended way to configure this board day-to-day, since editing
 * and reflashing the sketch just to change WiFi is annoying for a
 * headless bench tool. Falls back to AP "CAN-SIM"/"cansimboat" if
 * nothing resolves or the join fails. */
#define WIFI_SSID      ""
#define WIFI_PASSWORD  ""

static Preferences prefs;
static bool prefs_ok = false;

/* "Disable CAN" - bench convenience for exercising ESP-NOW pairing with
 * no CAN transceiver wired up at all. Default off. Persisted "can_dis". */
static bool     g_can_disabled = false;
/* ESP-NOW pairing (requester role - this board is never the acceptor,
 * see espnow_pairing.h). Once paired, never re-broadcasts on its own -
 * "REPAIR" over serial or the debug page's Clear Pairing button are the only
 * "specially told to" triggers. Persisted "paired". */
static bool     g_paired  = false;

/* true ONLY once wifi_setup() actually confirms WL_CONNECTED to a real
 * network - NOT just "has credentials configured", since a credentialed
 * join that fails lands on the same fixed fallback channel as having no
 * credentials at all, and needs the same channel-hunting treatment (see
 * pairing_requester_tick()). A board that genuinely joined a router owns
 * whatever channel that router assigned and has no reason to hunt. */
static bool     g_wifi_joined = false;

/* channel-hunting (pairing_requester_tick()): last time ANY frame from
 * g_helm_mac was actually received - espnow_on_recv() updates this on
 * every bus frame from the paired peer, and pairing succeeding sets it
 * immediately too (avoids one spurious hunt cycle right after a fresh
 * pair, before HELM's first heartbeat has had a chance to arrive). */
static uint32_t g_last_helm_rx_ms = 0;

/* channel-hunting tuning (see pairing_requester_tick()). ESP32 supports
 * 2.4GHz channels 1-13; HB_HELM at 1Hz means a 2s dwell gives ~2 chances
 * to hear it while parked on the right channel - long enough to be
 * reliable, short enough that a full 13-channel sweep (~26s worst case)
 * isn't painfully slow. LOST_CONTACT_MS is a few missed heartbeats'
 * worth of margin (not 1-2, to avoid re-hunting over a single dropped
 * packet), not so long that a real channel change goes unnoticed for
 * ages. */
#define CHANNEL_SCAN_MAX     13
#define CHANNEL_DWELL_MS     2000UL
#define LOST_CONTACT_MS      6000UL
/* extra dwells to spend on the cached last-known-good channel (NVS
 * "lastch") before falling back to the full 1..13 sweep - a reboot
 * (including right after an OTA update finishes) is very likely to
 * reconnect on the SAME channel HELM was already on, since HELM's own
 * channel rarely changes between boots. A single CHANNEL_DWELL_MS is
 * long enough to catch HELM's 1Hz heartbeat in the common case, but a
 * single missed/collided packet would previously send this board
 * straight into a full sweep instead of just trying the same good
 * channel again. */
#define CACHED_CHANNEL_RETRIES 5
static uint8_t  g_cached_channel_tries_left = 0;
static bool     g_hunting          = false;
static uint8_t  g_channel          = 1;   /* current candidate while hunting,
                                            * last confirmed-good channel once
                                            * settled - dual purpose, see
                                            * pairing_requester_tick() */
static uint32_t g_hunt_last_hop_ms = 0;

/* OTA (MSG_OTA_START reassembly + handoff to ota_tick()) - see that
 * function's comment for why execution is deferred to loop() rather
 * than happening inline where a chunk completes. g_ota_ready is
 * volatile: set from bus_handle_rx()'s context (an ESP-NOW receive
 * callback, or inline from can_poll()), read+cleared from loop(), same
 * cross-context pattern already used for glow_held/start_held. */
#define OTA_MAX_CHUNKS ((OTA_START_MAX_LEN + OTA_START_CHUNK_BYTES - 1) / OTA_START_CHUNK_BYTES)
static uint8_t        g_ota_buf[OTA_START_MAX_LEN];
static uint8_t        g_ota_total_len = 0;
static bool           g_ota_chunk_seen[OTA_MAX_CHUNKS];
static bool           g_ota_in_progress = false;
static char           g_ota_ssid[33];
static char           g_ota_pass[65];
static char           g_ota_url[160];
static char           g_ota_md5[33];   /* 32 hex chars + NUL, "" if the
    * manifest had none for this entry - see ota_handle_start_chunk()/
    * ota_tick() and can_protocol.h's MSG_OTA_START comment */
static int            g_ota_engine = -1;
static volatile bool  g_ota_ready = false;

/* WiFi join (MSG_WIFI_JOIN reassembly + handoff to wifi_join_tick()) -
 * same cross-context shape as the OTA block above: reassembled from
 * bus_handle_rx()'s context (possibly an ESP-NOW receive callback),
 * acted on from loop() since that's the one safe place to touch WiFi/
 * NVS-then-reboot on this board. */
#define WIFI_JOIN_MAX_CHUNKS ((WIFI_JOIN_MAX_LEN + WIFI_JOIN_CHUNK_BYTES - 1) / WIFI_JOIN_CHUNK_BYTES)
static uint8_t        g_wifi_join_buf[WIFI_JOIN_MAX_LEN];
static uint8_t        g_wifi_join_total_len = 0;
static bool           g_wifi_join_chunk_seen[WIFI_JOIN_MAX_CHUNKS];
static bool           g_wifi_join_in_progress = false;
static char           g_wifi_join_ssid[33];
static char           g_wifi_join_pass[65];
static volatile bool  g_wifi_join_ready = false;

/* ==================== which engines this board pretends to be ====================
 * Up to MD_MAX_ENGINES (4) fully user-defined fake engines, configured
 * from the debug page: name, capability bits, throttle behavior, and
 * force-alarm test toggles - see the "sim_engine_t" struct below. Engine
 * index (0..3) is NOT set here - HELM is the bus master and assigns it
 * dynamically via the ENROLL_REQUEST/ENROLL_ASSIGN handshake in
 * can_protocol.h, keyed by MAC address, same as a real CTRL board would
 * get. See derive_sim_mac() below for how one physical board fakes
 * distinct MAC identities for its several pretend nodes.
 *
 * All 4 are ENGTYPE_GENERIC_DIESEL - deliberately NOT user-selectable as
 * ENGTYPE_MD2030/MD2030C, because CAP_STOP is freely checkable on these
 * debug-defined engines and the real MD2030 must NEVER be paired with
 * CAP_STOP, not even in simulation (see ../engine_display/CLAUDE.md's
 * safety model). The display-visible name comes from the free-text
 * `name` field now, so the type enum isn't needed for that anymore. */

#define SIM_DEFAULT_CAPS  (CAP_RPM | CAP_COOLANT_TEMP | CAP_OIL_PRESS | \
                           CAP_TEMP_ALARM | CAP_PRESS_ALARM | CAP_GLOW | \
                           CAP_START | CAP_IGNITION | CAP_HOURS | CAP_AUDIO)

/* OFF is numerically 0 so struct zero-init (sims[] at file scope) gives
 * every engine this as its default throttle mode without an explicit
 * assignment in setup() - matters because a freshly-added engine should
 * read as "off" on the debug page, not silently look like it's already
 * idling before anyone has touched anything. OFF and IDLE behave
 * identically once s->running is true (see sim_engine_physics_tick()) -
 * this is purely a debug-page default/labeling distinction, not a
 * separate physics state; whether the engine is actually running at all
 * is controlled entirely by the crank/dead-man sequence, not this field. */
#define THROTTLE_OFF    0
#define THROTTLE_IDLE   1
#define THROTTLE_RANDOM 2

/* simulated physics tuning. All 4 engines share these for now (no real
 * spec data to justify making per-engine profiles actually differ) -
 * see CLAUDE.md's "framework, not finished" note. */
#define SIM_CRANK_CATCH_MS      5000    /* start-held -> engine actually fires */
#define SIM_CATCH_RPM_FRAC      0.4f    /* rough catch, then ramps up to idle */
#define SIM_CRANK_RPM_BASE      180.0f  /* starter motor turning the engine over */
#define SIM_CRANK_RPM_JITTER    120.0f  /* +/- twitter from compression-stroke kicks */
#define SIM_CRANK_JITTER_MS     120     /* how often the twitter target re-picks */
#define SIM_IDLE_RPM            800.0f
#define SIM_IDLE_SETTLE_PER_S   80.0f
#define SIM_SPINDOWN_PER_S      900.0f  /* natural decel after stop/fuel-cut */
#define SIM_REDLINE_RPM         3600.0f /* matches the display's shared RPM_RED_V */
#define SIM_RANDOM_REV_MIN      1500.0f
#define SIM_RANDOM_REV_MAX      3700.0f /* "just over redline" */
#define SIM_RANDOM_REV_RAMP_PER_S    300.0f
#define SIM_RANDOM_RETARGET_MIN_MS  4000
#define SIM_RANDOM_RETARGET_MAX_MS  12000
#define SIM_OPERATING_TEMP_C    82.0f
#define SIM_AMBIENT_TEMP_C      20.0f
#define SIM_TEMP_ALARM_C        95.0f
#define SIM_OIL_ALARM_BAR       1.0f
#define SIM_OIL_RAMP_FRAC       0.05f   /* pressure builds over ~a couple
                                          * seconds after firing, not instantly */
#define SIM_HOURS_SAVE_MS       5000

/* ==================== per-engine simulated state ==================== */
/* Tagged struct (NOT `typedef struct {...} sim_engine_t;`) used as a
 * parameter type in several functions below. Arduino's auto-generated
 * prototypes get hoisted above ALL user code, including anything
 * defined earlier in this same file - a typedef alias isn't visible yet
 * at that point, so the hoisted copy fails to parse ("not declared in
 * this scope"). A TAGGED struct fixes it, but only if every use of the
 * type - definition AND every function signature - spells out the
 * elaborated form `struct sim_engine_t`, not the bare name. Bare
 * `sim_engine_t` doesn't trigger C++'s implicit-forward-declare-by-
 * mention rule; only the `struct Tag` form does, and Arduino's hoisted
 * prototype is a verbatim copy of whatever the signature actually says.
 * Hit this same bug twice now (once with a function-pointer typedef in
 * engine_display.ino, once here) - see that project's memory note. */
struct sim_engine_t {
    uint8_t  idx;       /* engine index - only meaningful once enrolled */
    uint8_t  type;      /* always ENGTYPE_GENERIC_DIESEL - see comment above */
    uint16_t caps;      /* user-configurable via the debug page's checkboxes */
    bool     enabled;   /* debug-page toggle: off the bus entirely when false,
                         * like the real CTRL board being powered down */
    char     name[MD_ENGINE_NAME_MAXLEN + 1];   /* free text, debug page;
                                                  * sent via MSG_ENGINE_NAME */

    /* bus enrollment (see can_protocol.h ENROLLMENT doc + derive_sim_mac
     * below): unenrolled at boot, sends ENROLL_REQUEST until HELM (the
     * bus master) assigns `idx` via ENROLL_ASSIGN matching our `mac` */
    uint8_t  mac[6];
    bool     enrolled;
    bool     last_assign_none;   /* HELM said "no free slot" - retry slower */
    uint32_t last_enroll_req_ms;
    uint32_t last_name_ms;

    /* commanded/latched state, driven by received CAN commands - each
     * gated on the matching CAP_* bit, same discipline CAP_STOP always
     * had: a control board with no glow relay wired up wouldn't react to
     * CMD_GLOW_HELD either, whether or not a display ever sends it */
    volatile bool     ign_commanded;      /* latched, from CMD_IGNITION */
    volatile bool     glow_held;          /* dead-man, from CMD_GLOW_HELD */
    volatile bool     start_held;         /* dead-man, from CMD_START_HELD */
    volatile bool     stop_held;          /* dead-man, from CMD_STOP (CAP_STOP only) */
    volatile uint32_t glow_last_rx;
    volatile uint32_t start_last_rx;
    volatile uint32_t stop_last_rx;
    uint32_t crank_start_ms;              /* 0 = not currently cranking */
    float    crank_jitter_rpm;            /* starter-motor twitter target, re-picked
                                            * every SIM_CRANK_JITTER_MS while cranking */
    uint32_t crank_jitter_retarget_ms;

    /* simulated physical values */
    float    rpm;
    float    temp_c;
    float    oil_bar;
    bool     running;
    uint32_t hours_x10;
    float    hours_accum_x10;   /* fractional remainder not yet committed -
                                  * one loop tick's worth of runtime is way
                                  * below 0.1h, so this can't just truncate */

    /* debug-page throttle behavior: off (default), sit at idle, or wander
     * between idle and just-over-redline (THROTTLE_OFF / THROTTLE_IDLE /
     * THROTTLE_RANDOM) */
    uint8_t  throttle_mode;
    float    random_target_rpm;
    uint32_t random_retarget_ms;

    /* debug-page "force alarm" test toggles - OR'd with the natural
     * physics-derived condition below, and (like every alarm) gated on
     * the matching CAP_* bit: unchecking the capability means there's
     * no sensor to have tripped, so the force toggle can't matter either.
     * charge/water have NO natural physics model at all (no simulated
     * alternator or fuel-water sensor) - the checkbox is the ONLY way to
     * ever raise those two, which is fine: they exist to bench-test the
     * display/alarmer reacting to CAP_CHARGE_ALARM/CAP_WATER_ALARM. */
    bool     force_temp_alarm;
    bool     force_press_alarm;
    bool     force_charge_alarm;
    bool     force_water_alarm;

    /* debug-page "force fail to start" test toggle - when set, cranking
     * (start_held) never reaches SIM_CRANK_CATCH_MS's catch transition;
     * see sim_engine_physics_tick(). Not an alarm, just bench-tests
     * HELM's autostart retry logic against an engine that never catches. */
    bool     force_fail_start;

    /* per-engine alarm silence, mirrors the display's silenced_mask */
    bool     temp_silenced;
    bool     press_silenced;
    bool     charge_silenced;
    bool     water_silenced;

    uint32_t last_announce_ms;
    uint32_t last_telem_ms;
    uint32_t last_hours_ms;
};

static struct sim_engine_t sims[MD_MAX_ENGINES] = {};

static bool can_ok = false;

/* declared up here (not down by wired_setup()) since bus_send() -
 * defined earlier in the file than wired_setup() - needs g_wired_ok
 * visible at that point; unlike functions, Arduino doesn't hoist global
 * variable declarations, only prototypes. See wired_bus.h. */
static HardwareSerial WiredSerial(1);
static bool           g_wired_ok = false;
static uint32_t       g_wired_last_rx_ms = 0;   /* 0 = never - see handle_status()'s "wired_active" */

/* This ONE physical board pretends to be several separate bus nodes (up
 * to 4 fake engines + the alarmer), but an ESP32 only has ONE real MAC
 * address - a real, separate CTRL board would just use its own real MAC
 * directly. Here we derive distinct-but-stable synthetic MACs from the
 * real one by XORing the last byte with a small per-role tag, purely so
 * the enrollment handshake (which identifies nodes by MAC) has that many
 * different identities to work with in this simulator. */
static void derive_sim_mac(uint8_t *out, uint8_t role_tag)
{
    WiFi.macAddress(out);
    out[5] ^= role_tag;
}

/* ==================== ESP-NOW pairing + channel-hunting ====================
 * This board is always the pairing REQUESTER (see espnow_pairing.h) -
 * mirror of HELM's acceptor role. Broadcasts PAIR_MSG_REQUEST while
 * hunting for HELM's actual channel (see pairing_requester_tick()) -
 * HELM is the only node expected to ever join real WiFi, so its channel
 * isn't knowable in advance. Once paired and in contact, never
 * broadcasts a pairing request again on its own; only REPAIR (serial or
 * debug page) re-arms it. Real engine command/telemetry/enrollment
 * traffic flows over this same paired link whenever CAN isn't usable -
 * see bus_send(). */

/* HELM's MAC + the LMK it generated for us, learned from a PAIR_MSG_ACK
 * and persisted so a reboot can re-establish the encrypted peer without
 * re-pairing. Only meaningful once g_paired is true. */
static uint8_t g_helm_mac[6];
/* (the pair key is derived from the shared secret and both radio addresses when needed - fleet_security.h) */

static void bytes_to_hex(const uint8_t *buf, int len, char *out)
{
    for (int j = 0; j < len; j++) snprintf(&out[j * 2], 3, "%02X", buf[j]);
}

static void hex_to_bytes(const char *hex, uint8_t *buf, int len)
{
    char byte_str[3] = {0};
    for (int j = 0; j < len; j++) {
        byte_str[0] = hex[j * 2];
        byte_str[1] = hex[j * 2 + 1];
        buf[j] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
}

/* register the HELM as an encrypted peer; the key is worked out from the shared secret and both addresses */
static bool espnow_add_helm_peer(void)
{
    uint8_t me[6];
    fsec_my_mac(me);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, g_helm_mac, 6);
    peer.channel = 0;
    peer.encrypt = true;
    fsec_lmk(me, g_helm_mac, peer.lmk);
    return (esp_now_is_peer_exist(g_helm_mac) ? esp_now_mod_peer(&peer) : esp_now_add_peer(&peer)) == ESP_OK;
}

static void espnow_on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* not logged per frame: this runs in the radio's own task, and a Serial write that blocks there
     * (the C3's USB serial does) stalls the receive path and the main loop with it */
    if (!g_fsec_have_key) return;

    if (!g_paired && len == (int)sizeof(espnow_hello_ack_t) && data[0] == ESPNOW_MSG_HELLO_ACK) {
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
        memcpy(g_helm_mac, ack.mac, 6);
        if (!espnow_add_helm_peer()) {
            Serial.println("ESP-NOW: got a valid reply but could not register HELM as an encrypted peer");
            return;
        }
        g_paired = true;
        g_last_helm_rx_ms = millis();
        if (prefs_ok) {
            char hex[13];
            bytes_to_hex(g_helm_mac, 6, hex); hex[12] = 0;
            prefs.putString("helm_mac", hex);
            prefs.putBool("paired", true);
        }
        Serial.println("ESP-NOW: joined HELM (encrypted)");
        return;
    }

    if (g_paired && len == (int)sizeof(espnow_bus_frame_t) && md_mac_eq(info->src_addr, g_helm_mac)) {
        espnow_bus_frame_t f;
        memcpy(&f, data, sizeof(f));
        if (!fsec_frame_verify(&f, info->src_addr)) return;   /* bad signature or a replay */

        /* only a frame ADDRESSED to us proves we are on HELM's channel - broadcasts can be heard
         * (faintly) from a neighbouring channel, so they don't count as contact */
        if (memcmp(info->des_addr, ESPNOW_BROADCAST_MAC, 6) != 0)
            g_last_helm_rx_ms = millis();
        twai_message_t m = {};
        m.identifier = f.can_id;
        m.data_length_code = f.dlc;
        if (f.dlc) memcpy(m.data, f.data, f.dlc);
        bus_handle_rx(&m);
    }
}

static void espnow_setup(void)
{
    if (!g_fsec_have_key) {
        Serial.println("ESP-NOW: OFF - no key set. Type  KEY <passphrase>  (12+ characters, the same on every board).");
        return;
    }
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW: init failed");
        return;
    }
    esp_now_register_recv_cb(espnow_on_recv);
    uint8_t pmk[ESPNOW_LMK_LEN];
    fsec_pmk(pmk);
    esp_now_set_pmk(pmk);

    esp_now_peer_info_t bcast = {};
    memcpy(bcast.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    bcast.channel = 0;   /* use whatever channel WiFi is already on */
    bcast.encrypt = false;
    esp_now_add_peer(&bcast);

    /* already joined from a previous boot - re-register HELM as an encrypted peer (esp_now's own table
     * doesn't survive reboot, only our NVS copy of HELM's address does; the key is re-derived) */
    g_paired = prefs_ok && prefs.getBool("paired", false);
    if (g_paired) {
        String hm = prefs.getString("helm_mac", "");
        if (hm.length() == 12) {
            hex_to_bytes(hm.c_str(), g_helm_mac, 6);
            if (!espnow_add_helm_peer()) Serial.println("ESP-NOW: failed to restore HELM as an encrypted peer");
        } else {
            g_paired = false;
            prefs.putBool("paired", false);
        }
    }

    Serial.printf("ESP-NOW: ready (key fingerprint %08lX)\n", (unsigned long)fsec_fingerprint());
}

/* shared by REPAIR (serial) and the debug page's Clear Pairing button -
 * forgets HELM as an encrypted peer (locally and in NVS) and clears the
 * paired flag. pairing_requester_tick()'s own need_hunt check (!g_paired)
 * picks this up on the very next call and starts a fresh channel hunt
 * with no other trigger needed. */
static void espnow_forget_helm(void)
{
    if (g_paired) esp_now_del_peer(g_helm_mac);
    g_paired = false;
    if (prefs_ok) {
        prefs.putBool("paired", false);
        prefs.remove("helm_mac");
    }
}

/* called from loop(): channel-hunting state machine, replaces the old
 * fixed-channel fast/slow retry cadence. HELM is the only node expected
 * to ever join real WiFi (its own router-assigned channel, unknowable in
 * advance) - every other node (this board) normally has no WiFi
 * credentials of its own and just needs to FIND whatever channel HELM
 * currently occupies, not assume a fixed one.
 *
 * Active whenever this board hasn't joined real WiFi itself (!g_wifi_joined
 * - a credentialed join that failed lands on the same fixed fallback
 * channel as having no credentials at all, so it needs hunting too) AND
 * either isn't paired yet, or hasn't heard from its already-paired HELM
 * in LOST_CONTACT_MS (HELM's own channel changed out from under it -
 * e.g. it rebooted and joined a different network). Sweeps 1..CHANNEL_
 * SCAN_MAX, dwelling CHANNEL_DWELL_MS on each candidate - long enough to
 * catch HELM's 1Hz heartbeat if it's there. Broadcasts PAIR_MSG_REQUEST
 * on each candidate while unpaired; once paired, no separate probe is
 * needed - bus_send()'s own regular ANNOUNCE/TELEM broadcasts double as
 * the probe and HELM will respond normally the moment the right channel
 * is found. The last channel that actually worked is cached in NVS
 * ("lastch") and tried first on the next hunt - CACHED_CHANNEL_RETRIES
 * extra dwells on it before falling back to the full sweep, since a
 * reboot is very likely to reconnect on the exact same channel and a
 * single missed packet shouldn't send this board straight into a full
 * 1..13 sweep. */
static void pairing_requester_tick(void)
{
    if (!g_fsec_have_key) return;   /* ESP-NOW is off until a key is set */
    /* A directly-wired HELM needs no ESP-NOW pairing/hunting at all -
     * the two boards already found each other over the wire (HELM's own
     * heartbeat and this board's ENROLL_REQUEST retries reach each other
     * within ~1s of both boards being up, since bus_send() dual/triple-
     * emits broadcasts onto every live transport - see wired_bus.h).
     * Spending airtime hunting for a wireless peer that isn't needed
     * would be pure waste. ESP-NOW itself stays initialized (espnow_
     * setup() already ran in setup(), unconditionally) so this board can
     * seamlessly fall back to normal hunting the moment the wire is
     * unplugged (g_wired_last_rx_ms ages out past WIRED_ACTIVE_TIMEOUT_MS)
     * - no reboot needed either way. */
    static bool wired_suppressed = false;
    bool wired_active = (g_wired_last_rx_ms != 0 &&
        millis() - g_wired_last_rx_ms < WIRED_ACTIVE_TIMEOUT_MS);
    if (wired_active) {
        if (!wired_suppressed) {
            wired_suppressed = true;
            Serial.println("ESP-NOW: direct-wire link detected - pairing/hunting suspended");
        }
        return;
    }
    if (wired_suppressed) {
        wired_suppressed = false;
        Serial.println("ESP-NOW: direct-wire link lost - resuming normal pairing/hunting");
    }

    if (g_wifi_joined) return;   /* real network join owns the channel instead */

    bool lost = g_paired && (millis() - g_last_helm_rx_ms > LOST_CONTACT_MS);
    bool need_hunt = !g_paired || lost;

    if (!need_hunt) {
        if (g_hunting) {
            g_hunting = false;
            if (prefs_ok) prefs.putUChar("lastch", g_channel);
            Serial.printf("ESP-NOW: contact confirmed on channel %d\n", g_channel);
        }
        return;
    }

    if (!g_hunting) {
        g_hunting = true;
        uint8_t start = prefs_ok ? prefs.getUChar("lastch", 1) : 1;
        if (start < 1 || start > CHANNEL_SCAN_MAX) start = 1;
        g_channel = start;
        g_cached_channel_tries_left = CACHED_CHANNEL_RETRIES;
        g_hunt_last_hop_ms = millis() - CHANNEL_DWELL_MS;   /* force an immediate probe below */
        Serial.printf("ESP-NOW: %s - starting channel hunt at %d (%d retries before sweeping)\n",
            lost ? "lost contact with HELM" : "not yet paired", g_channel, CACHED_CHANNEL_RETRIES);
    } else if (millis() - g_hunt_last_hop_ms >= CHANNEL_DWELL_MS) {
        if (g_cached_channel_tries_left > 0) {
            g_cached_channel_tries_left--;   /* stay on the same candidate for another dwell */
        } else {
            g_channel = (g_channel % CHANNEL_SCAN_MAX) + 1;
        }
        g_hunt_last_hop_ms = millis();
    } else {
        return;   /* still dwelling on the current candidate channel */
    }

    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);

    /* announce ourselves: a HELLO only a holder of the shared secret can make. Sent while looking for HELM and
     * also while re-looking after contact is lost, so HELM can pick us up again if it forgot us. */
    espnow_hello_t hello = {};
    hello.type = ESPNOW_MSG_HELLO;
    fsec_my_mac(hello.mac);
    hello.node_type = NODE_TYPE_ENGINE_CTRL;
    fsec_hello_tag(ESPNOW_MSG_HELLO, hello.mac, hello.node_type, NULL, hello.tag);
    esp_err_t err = esp_now_send(ESPNOW_BROADCAST_MAC, (uint8_t *)&hello, sizeof(hello));
    Serial.printf("ESP-NOW: %s on channel %d - HELLO %s\n",
        g_paired ? "looking for HELM again" : "looking for HELM", g_channel, err == ESP_OK ? "sent" : "FAILED");
}

/* ==================== bus_send(): transport-routing wrapper ====================
 * Same signature/call sites as the old CAN-only can_send() it replaces.
 * One physical board, one uplink. Routes on !g_can_disabled (the user's
 * preference, still honored so "CAN Enabled" can force ESP-NOW-only even
 * with working CAN hardware attached) AND can_ok (actual CAN health) -
 * requires BOTH, not g_can_disabled alone. This matters because
 * can_bus_health_tick() can flip can_ok to false on its own (repeated
 * bus-off with no transceiver wired) without ever touching
 * g_can_disabled - gating on the user flag alone meant a bench unit that
 * booted with CAN enabled but no transceiver would silently blackhole
 * every message once bus-off auto-disabled CAN, instead of falling back
 * to an already-paired ESP-NOW peer. This board never SENDS the 4
 * unicast commands (only receives them, via bus_handle_rx()), so
 * everything here is broadcast - no per-engine peer routing needed. */
static void bus_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    /* always attempted, independent of the CAN-vs-ESP-NOW choice below -
     * a supplementary transport, not an alternative in the same sense
     * those two are (see wired_bus.h). Harmless if nothing's actually
     * wired up on the other end. */
    if (g_wired_ok) wired_send_frame(id, data, len);

    if (!g_can_disabled && can_ok) {
        twai_message_t m = {};
        m.identifier = id;
        m.data_length_code = len;
        m.ss = 1;   /* single shot: no endless retries when nobody ACKs */
        if (len) memcpy(m.data, data, len);
        twai_transmit(&m, 0);
        return;
    }

    if (!g_paired || !g_fsec_have_key) return;   /* no transceiver AND no ESP-NOW peer yet - nothing to send on */

    espnow_bus_frame_t f;
    fsec_frame_build(&f, id, data, len);   /* signed with the shared secret */
    esp_err_t err = esp_now_send(ESPNOW_BROADCAST_MAC, (uint8_t *)&f, sizeof(f));

    /* how many bus frames the radio accepted vs refused, every 5 s - ESP-NOW has a small send
     * queue, and this board bursts several frames per loop pass */
    static uint32_t ok_count = 0, fail_count = 0, last_report_ms = 0;
    static esp_err_t last_err = ESP_OK;
    if (err == ESP_OK) ok_count++;
    else { fail_count++; last_err = err; }
    if (millis() - last_report_ms >= 5000) {
        Serial.printf("ESP-NOW: sent %lu bus frames, %lu refused by the radio%s%s\n",
            (unsigned long)ok_count, (unsigned long)fail_count,
            fail_count ? ", last error " : "", fail_count ? esp_err_to_name(last_err) : "");
        ok_count = fail_count = 0;
        last_report_ms = millis();
    }
}

/* ==================== alarmer: hears the WHOLE bus, not just sim_a/b ==================== */

typedef struct {
    bool     present;
    bool     temp_alarm, press_alarm, charge_alarm, water_alarm;
    bool     temp_silenced, press_silenced, charge_silenced, water_silenced;
    uint32_t last_seen;
} alarmer_engine_t;

static alarmer_engine_t alarmer_state[MD_MAX_ENGINES];

/* the alarmer's own bus enrollment - it doesn't key any behavior off
 * its assigned id (it already listens to the whole bus regardless), but
 * it still enrolls so HELM's MAC->id table has a real record of it,
 * same as the two fake engines */
static uint8_t  alarmer_mac[6];
static bool     alarmer_enrolled = false;
static bool     alarmer_last_assign_none = false;
static uint8_t  alarmer_id = 0;
static uint32_t alarmer_last_enroll_req_ms = 0;

static void alarmer_handle_telem(int e, const twai_message_t *m)
{
    if (e < 0 || e >= MD_MAX_ENGINES || m->data_length_code < 7) return;
    uint8_t flags = m->data[6];
    bool temp_alarm   = flags & TFLAG_TEMP_ALARM;
    bool press_alarm  = flags & TFLAG_PRESS_ALARM;
    bool charge_alarm = flags & TFLAG_CHARGE_ALARM;
    bool water_alarm  = flags & TFLAG_WATER_ALARM;

    alarmer_state[e].present   = true;
    alarmer_state[e].last_seen = millis();
    if (!temp_alarm)   alarmer_state[e].temp_silenced   = false;
    if (!press_alarm)  alarmer_state[e].press_silenced  = false;
    if (!charge_alarm) alarmer_state[e].charge_silenced = false;
    if (!water_alarm)  alarmer_state[e].water_silenced  = false;
    alarmer_state[e].temp_alarm   = temp_alarm;
    alarmer_state[e].press_alarm  = press_alarm;
    alarmer_state[e].charge_alarm = charge_alarm;
    alarmer_state[e].water_alarm  = water_alarm;
}

static void alarmer_handle_silence(int e, const twai_message_t *m)
{
    if (e < 0 || e >= MD_MAX_ENGINES || m->data_length_code < 2) return;
    if (m->data[1] != NODE_ID_HELM && m->data[1] != NODE_ID_CYD) return;
    alarmer_state[e].temp_silenced   |= alarmer_state[e].temp_alarm;
    alarmer_state[e].press_silenced  |= alarmer_state[e].press_alarm;
    alarmer_state[e].charge_silenced |= alarmer_state[e].charge_alarm;
    alarmer_state[e].water_silenced  |= alarmer_state[e].water_alarm;
}

static bool alarmer_should_sound(void)
{
    uint32_t now = millis();
    for (int e = 0; e < MD_MAX_ENGINES; e++) {
        if (!alarmer_state[e].present) continue;
        if (now - alarmer_state[e].last_seen > ENGINE_LOST_MS) continue;
        if (alarmer_state[e].temp_alarm   && !alarmer_state[e].temp_silenced)   return true;
        if (alarmer_state[e].press_alarm  && !alarmer_state[e].press_silenced)  return true;
        if (alarmer_state[e].charge_alarm && !alarmer_state[e].charge_silenced) return true;
        if (alarmer_state[e].water_alarm  && !alarmer_state[e].water_silenced)  return true;
    }
    return false;
}

/* Simple on/off beeper - works with an active buzzer as-is. If you're
 * driving a passive speaker and want an actual tone/pitch, swap this
 * for LEDC PWM (ledcAttach/ledcWrite on newer cores, ledcSetup/
 * ledcAttachPin on older ones - API differs by arduino-esp32 version,
 * which is why this framework doesn't guess and just toggles the pin). */
static void buzzer_tick(void)
{
    static uint32_t last_toggle = 0;
    static bool     beep_on = false;

    if (!alarmer_should_sound()) {
        if (beep_on) { digitalWrite(PIN_BUZZER, LOW); beep_on = false; }
        return;
    }
    if (millis() - last_toggle >= 500) {
        last_toggle = millis();
        beep_on = !beep_on;
        digitalWrite(PIN_BUZZER, beep_on ? HIGH : LOW);
    }
}

/* ==================== CAN (TWAI) ==================== */

static void can_setup(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    g.tx_queue_len = 16;
    g.rx_queue_len = 16;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
        gpio_set_pull_mode((gpio_num_t)PIN_CAN_RX, GPIO_PULLUP_ONLY);
        can_ok = true;
        Serial.printf("CAN: started @250k (TX=%d, RX=%d)\n", PIN_CAN_TX, PIN_CAN_RX);
    } else {
        Serial.println("CAN: driver failed to start - check transceiver wiring");
    }
}

/* ==================== direct-wire supplementary transport (see wired_bus.h) ====================
 * UART1 - separate hardware peripheral from UART0 (this board's native-
 * USB Serial console, see "USB CDC On Boot" in CLAUDE.md), so no
 * interaction with the debug/serial-command path. Always started,
 * matching can_setup()'s own "always try, note whether it worked" shape
 * - see wired_bus.h's header comment for why this is safe to leave
 * always-on. This board never tracks per-engine transport the way
 * engine_display.ino's engine_slots[] does (it's the slave side, one
 * physical board/one uplink - see bus_send()'s own comment below), so
 * unlike that file's wired_handle_bus_frame() there's no stamping here,
 * just a direct hand-off to bus_handle_rx(). WiredSerial/g_wired_ok
 * themselves are declared up near can_ok (bus_send() needs g_wired_ok
 * visible earlier in the file than this point). */
static void wired_setup(void)
{
    WiredSerial.begin(WIRED_BAUD, SERIAL_8N1, PIN_WIRED_RX, PIN_WIRED_TX);
    g_wired_ok = true;
    Serial.printf("Wired: link started @%d baud (TX=%d, RX=%d)\n",
        WIRED_BAUD, PIN_WIRED_TX, PIN_WIRED_RX);
}

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

/* Called from loop() - same fixed-length-after-sync framing/checksum
 * discipline as engine_display.ino's wired_bus_tick(), see that file's
 * comment for the reasoning. Feeds a good frame straight to
 * bus_handle_rx() (already shared by the real CAN loop and the ESP-NOW
 * bus-frame path - see that function's own comment), no per-engine
 * bookkeeping needed on this side. */
static void wired_bus_tick(void)
{
    static uint8_t buf[WIRED_FRAME_LEN];
    static uint8_t pos = 0;

    while (WiredSerial.available()) {
        uint8_t b = (uint8_t)WiredSerial.read();
        if (pos == 0) {
            if (b != WIRED_SYNC_BYTE) continue;
            buf[pos++] = b;
            continue;
        }
        buf[pos++] = b;
        if (pos < WIRED_FRAME_LEN) continue;

        pos = 0;
        uint8_t chk = 0;
        for (int i = 1; i < 12; i++) chk ^= buf[i];
        if (chk != buf[12]) {
            Serial.println("Wired: frame checksum mismatch, dropped");
            continue;
        }

        twai_message_t m = {};
        m.identifier = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8);
        m.data_length_code = buf[3];
        if (m.data_length_code > 8) continue;
        if (m.data_length_code) memcpy(m.data, &buf[4], m.data_length_code);

        g_wired_last_rx_ms = millis();   /* see handle_status()'s "wired_active" */
        bus_handle_rx(&m);
    }
}

/* bus_send() - the real transport-routing implementation - is defined
 * later in this file (needs espnow_send_bus_frame(), added alongside the
 * ESP-NOW pairing code) but used starting here; Arduino auto-generates a
 * prototype for every top-level function, so physical ordering doesn't
 * matter for that. */

static uint8_t can_busoff_strikes = 0;

static void can_bus_health_tick(void)
{
    static uint32_t last_check = 0;
    if (!can_ok || millis() - last_check < 1000) return;
    last_check = millis();

    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) return;

    if (s.state == TWAI_STATE_BUS_OFF) {
        if (++can_busoff_strikes >= 5) {
            twai_stop();
            twai_driver_uninstall();
            can_ok = false;
            Serial.println("CAN: repeated bus-off - check wiring/termination, CAN disabled until reboot");
        } else {
            twai_initiate_recovery();
        }
    } else if (s.state == TWAI_STATE_STOPPED) {
        twai_start();
    } else if (s.state == TWAI_STATE_RUNNING &&
               s.tx_error_counter == 0 && s.rx_error_counter == 0) {
        can_busoff_strikes = 0;
    }
}

/* ==================== bus enrollment (slave-side) ====================
 * HELM is the bus master; this board's fake nodes are slaves that must
 * be assigned an index before doing anything else. See can_protocol.h's
 * ENROLLMENT doc comment for the handshake and derive_sim_mac() above
 * for why one physical board needs 3 distinct MAC identities here. */

static void sim_engine_enroll_tick(struct sim_engine_t *s)
{
    if (s->enrolled) return;
    uint32_t now = millis();
    uint32_t retry_ms = s->last_assign_none ? ENROLL_RETRY_SLOW_MS : ENROLL_RETRY_MS;
    if (s->last_enroll_req_ms && now - s->last_enroll_req_ms < retry_ms) return;

    s->last_enroll_req_ms = now;
    uint8_t d[8] = {0};
    memcpy(d, s->mac, 6);
    d[6] = NODE_TYPE_ENGINE_CTRL;
    bus_send(MSG_ENROLL_REQUEST, d, 8);
}

static void sim_engine_handle_enroll_assign(struct sim_engine_t *s, const twai_message_t *m)
{
    if (s->enrolled || m->data_length_code < 7) return;
    if (!md_mac_eq(&m->data[0], s->mac)) return;

    if (m->data[6] == ASSIGNED_ID_NONE) {
        s->last_assign_none = true;
        Serial.println("Enroll: HELM has no free engine slot - retrying slower");
        return;
    }
    s->idx = m->data[6];
    s->enrolled = true;
    Serial.printf("Enroll: assigned engine index %d\n", s->idx);
}

static void alarmer_enroll_tick(void)
{
    if (alarmer_enrolled) return;
    uint32_t now = millis();
    uint32_t retry_ms = alarmer_last_assign_none ? ENROLL_RETRY_SLOW_MS : ENROLL_RETRY_MS;
    if (alarmer_last_enroll_req_ms && now - alarmer_last_enroll_req_ms < retry_ms) return;

    alarmer_last_enroll_req_ms = now;
    uint8_t d[8] = {0};
    memcpy(d, alarmer_mac, 6);
    d[6] = NODE_TYPE_ALARMER;
    bus_send(MSG_ENROLL_REQUEST, d, 8);
}

static void alarmer_handle_enroll_assign(const twai_message_t *m)
{
    if (alarmer_enrolled || m->data_length_code < 7) return;
    if (!md_mac_eq(&m->data[0], alarmer_mac)) return;

    if (m->data[6] == ASSIGNED_ID_NONE) {
        alarmer_last_assign_none = true;
        Serial.println("Enroll: HELM has no free alarmer slot - retrying slower");
        return;
    }
    alarmer_id = m->data[6];
    alarmer_enrolled = true;
    Serial.printf("Enroll: alarmer assigned id %d\n", alarmer_id);
}

/* ---- receive: dispatch to our own engines' command handling, and
 * feed the alarmer from EVERY engine's telemetry/silence on the bus ---- */

static void sim_engine_recv_cmd(struct sim_engine_t *s, const twai_message_t *m)
{
    if (!s->enabled || !s->enrolled) return;   /* powered down, or no index yet - can't hear commands either */

    uint32_t id = m->identifier;
    if (m->data_length_code < 2) return;
    uint8_t value = m->data[0];
    uint8_t source = m->data[1];

    if (id == MSG_CMD_IGNITION(s->idx)) {
        if ((s->caps & CAP_IGNITION) && source == NODE_ID_HELM) {   /* ignition: HELM authority only */
            s->ign_commanded = (value != 0);
            Serial.printf("Cmd: engine %d ignition -> %d (source=%d)\n", s->idx, value, source);
        } else {
            Serial.printf("Cmd: engine %d ignition REJECTED (has_cap=%d source=%d, want HELM=%d)\n",
                s->idx, !!(s->caps & CAP_IGNITION), source, NODE_ID_HELM);
        }
    } else if (id == MSG_CMD_GLOW_HELD(s->idx)) {
        if ((s->caps & CAP_GLOW) && (source == NODE_ID_HELM || source == NODE_ID_CYD)) {
            s->glow_held     = true;
            s->glow_last_rx  = millis();
            Serial.printf("Cmd: engine %d glow held (source=%d)\n", s->idx, source);
        } else {
            Serial.printf("Cmd: engine %d glow REJECTED (has_cap=%d source=%d)\n",
                s->idx, !!(s->caps & CAP_GLOW), source);
        }
    } else if (id == MSG_CMD_START_HELD(s->idx)) {
        if ((s->caps & CAP_START) && (source == NODE_ID_HELM || source == NODE_ID_CYD)) {
            s->start_held    = true;
            s->start_last_rx = millis();
            Serial.printf("Cmd: engine %d start held (source=%d)\n", s->idx, source);
        } else {
            Serial.printf("Cmd: engine %d start REJECTED (has_cap=%d source=%d)\n",
                s->idx, !!(s->caps & CAP_START), source);
        }
    } else if (id == MSG_CMD_STOP(s->idx)) {
        if ((s->caps & CAP_STOP) && source == NODE_ID_HELM) {   /* STOP: HELM only, CAP_STOP only */
            s->stop_held    = true;
            s->stop_last_rx = millis();
            Serial.printf("Cmd: engine %d stop held (source=%d)\n", s->idx, source);
        } else {
            Serial.printf("Cmd: engine %d stop REJECTED (has_cap=%d source=%d, want HELM=%d)\n",
                s->idx, !!(s->caps & CAP_STOP), source, NODE_ID_HELM);
        }
    }
}

/* Reassembles one MSG_OTA_START chunk. Only for an engine index that's
 * actually one of ours (has_mac/enrolled sanity - see md_is_unicast_
 * command()'s comment on why this matters more for the CAN path, where
 * the frame is broadcast-shaped on the wire and every node sees every
 * engine's traffic). chunk_idx==0 (re)starts a fresh reassembly, so a
 * retried burst (e.g. HELM never got the ACK) can't get stuck merged
 * with stale leftovers from an earlier attempt. Once every expected
 * chunk has arrived, splits the "ssid\0pass\0url\0md5\0" payload and
 * hands off to ota_tick() - does NOT touch WiFi here, see that
 * function's comment for why. */
static void ota_handle_start_chunk(int e, const twai_message_t *m)
{
    bool ours = false;
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        if (sims[i].enrolled && sims[i].idx == e) ours = true;
    if (!ours) {
        /* silent before - this is a real, easy-to-hit failure mode if
         * HELM's idea of this engine's index doesn't match what's
         * actually enrolled here (e.g. re-enrolled onto a different
         * slot since HELM last heard an ANNOUNCE) - every chunk gets
         * dropped with no visible reason at all otherwise. */
        Serial.printf("OTA: MSG_OTA_START chunk for engine %d ignored - not one of "
            "this board's enrolled engines\n", e);
        return;
    }
    if (m->data_length_code < 2) return;

    uint8_t total_len = m->data[0];
    uint8_t chunk_idx = m->data[1];
    if (total_len == 0 || total_len > OTA_START_MAX_LEN) return;

    if (chunk_idx == 0) {
        g_ota_total_len = total_len;
        g_ota_in_progress = true;
        memset(g_ota_chunk_seen, 0, sizeof(g_ota_chunk_seen));
        memset(g_ota_buf, 0, sizeof(g_ota_buf));
        Serial.printf("OTA: MSG_OTA_START reassembly started for engine %d (%d bytes total)\n",
            e, total_len);
    }
    if (!g_ota_in_progress || total_len != g_ota_total_len) return;

    int off = chunk_idx * OTA_START_CHUNK_BYTES;
    int avail = m->data_length_code - 2;
    for (int j = 0; j < avail && off + j < g_ota_total_len; j++)
        g_ota_buf[off + j] = m->data[2 + j];
    if (chunk_idx < OTA_MAX_CHUNKS) g_ota_chunk_seen[chunk_idx] = true;

    int expected_chunks = (g_ota_total_len + OTA_START_CHUNK_BYTES - 1) / OTA_START_CHUNK_BYTES;
    Serial.printf("OTA: chunk %d/%d received for engine %d\n",
        chunk_idx + 1, expected_chunks, e);
    for (int c = 0; c < expected_chunks; c++)
        if (!g_ota_chunk_seen[c]) return;   /* still waiting on at least one chunk */

    g_ota_in_progress = false;

    char *ssid = (char *)g_ota_buf;
    char *pass = ssid + strnlen(ssid, g_ota_total_len) + 1;
    char *url  = pass + strnlen(pass, g_ota_total_len - (int)(pass - ssid)) + 1;
    if ((uint8_t *)url >= g_ota_buf + g_ota_total_len) {
        Serial.println("OTA: malformed MSG_OTA_START payload (couldn't find url)");
        return;
    }
    /* md5 may legitimately be an empty string (just its own NUL) if the
     * manifest had none - only the pointer itself needs to land in
     * bounds, unlike url's check above there's no "must be non-empty"
     * requirement here. */
    char *md5 = url + strnlen(url, g_ota_total_len - (int)(url - ssid)) + 1;
    if ((uint8_t *)md5 > g_ota_buf + g_ota_total_len) {
        Serial.println("OTA: malformed MSG_OTA_START payload (couldn't find md5)");
        return;
    }
    strncpy(g_ota_ssid, ssid, sizeof(g_ota_ssid) - 1); g_ota_ssid[sizeof(g_ota_ssid) - 1] = 0;
    strncpy(g_ota_pass, pass, sizeof(g_ota_pass) - 1); g_ota_pass[sizeof(g_ota_pass) - 1] = 0;
    strncpy(g_ota_url,  url,  sizeof(g_ota_url)  - 1); g_ota_url[sizeof(g_ota_url)  - 1] = 0;
    strncpy(g_ota_md5,  md5,  sizeof(g_ota_md5)  - 1); g_ota_md5[sizeof(g_ota_md5)  - 1] = 0;
    g_ota_engine = e;
    g_ota_ready = true;
    Serial.printf("OTA: MSG_OTA_START reassembled for engine %d - ssid=\"%s\"\n", e, g_ota_ssid);
}

/* Reassembles one MSG_WIFI_JOIN chunk - same ownership check and
 * chunk_idx==0-restarts-reassembly discipline as ota_handle_start_
 * chunk() above, just for the simpler "ssid\0pass\0" payload (no url/
 * md5). Does NOT touch WiFi/NVS here - see wifi_join_tick(). */
static void wifi_join_handle_start_chunk(int e, const twai_message_t *m)
{
    bool ours = false;
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        if (sims[i].enrolled && sims[i].idx == e) ours = true;
    if (!ours) {
        Serial.printf("WiFi: MSG_WIFI_JOIN chunk for engine %d ignored - not one of "
            "this board's enrolled engines\n", e);
        return;
    }
    if (m->data_length_code < 2) return;

    uint8_t total_len = m->data[0];
    uint8_t chunk_idx = m->data[1];
    if (total_len == 0 || total_len > WIFI_JOIN_MAX_LEN) return;

    if (chunk_idx == 0) {
        g_wifi_join_total_len = total_len;
        g_wifi_join_in_progress = true;
        memset(g_wifi_join_chunk_seen, 0, sizeof(g_wifi_join_chunk_seen));
        memset(g_wifi_join_buf, 0, sizeof(g_wifi_join_buf));
    }
    if (!g_wifi_join_in_progress || total_len != g_wifi_join_total_len) return;

    int off = chunk_idx * WIFI_JOIN_CHUNK_BYTES;
    int avail = m->data_length_code - 2;
    for (int j = 0; j < avail && off + j < g_wifi_join_total_len; j++)
        g_wifi_join_buf[off + j] = m->data[2 + j];
    if (chunk_idx < WIFI_JOIN_MAX_CHUNKS) g_wifi_join_chunk_seen[chunk_idx] = true;

    int expected_chunks = (g_wifi_join_total_len + WIFI_JOIN_CHUNK_BYTES - 1) / WIFI_JOIN_CHUNK_BYTES;
    for (int c = 0; c < expected_chunks; c++)
        if (!g_wifi_join_chunk_seen[c]) return;   /* still waiting on at least one chunk */

    g_wifi_join_in_progress = false;

    char *ssid = (char *)g_wifi_join_buf;
    char *pass = ssid + strnlen(ssid, g_wifi_join_total_len) + 1;
    /* pass may legitimately point exactly at the end (empty password,
     * just its own NUL, for an open AP) - only reject if it overruns */
    if ((uint8_t *)pass > g_wifi_join_buf + g_wifi_join_total_len) {
        Serial.println("WiFi: malformed MSG_WIFI_JOIN payload (couldn't find password)");
        return;
    }
    strncpy(g_wifi_join_ssid, ssid, sizeof(g_wifi_join_ssid) - 1);
    g_wifi_join_ssid[sizeof(g_wifi_join_ssid) - 1] = 0;
    strncpy(g_wifi_join_pass, pass, sizeof(g_wifi_join_pass) - 1);
    g_wifi_join_pass[sizeof(g_wifi_join_pass) - 1] = 0;
    g_wifi_join_ready = true;
    Serial.printf("WiFi: MSG_WIFI_JOIN reassembled for engine %d - ssid=\"%s\"\n", e, g_wifi_join_ssid);
}

/* extracted so both the CAN receive loop and the ESP-NOW bus-frame path
 * (espnow_on_recv(), see below) can feed it a real or synthetic
 * twai_message_t - identical dispatch either way. */
static void bus_handle_rx(const twai_message_t *m)
{
    /* every inbound frame, whichever transport it arrived on - if you hold
     * glow/start on HELM and see NOTHING here, the frame never reached this
     * board at all (check HELM's own bus_send() routing / CAN wiring /
     * ESP-NOW pairing+transport, this board's radio isn't the problem). */
    int cmd_e;
    if (md_is_unicast_command(m->identifier, &cmd_e))
        Serial.printf("Rx: id=0x%03lX dlc=%d - unicast command for engine %d\n",
            (unsigned long)m->identifier, m->data_length_code, cmd_e);
    else if (m->identifier != MSG_HB_HELM)   /* the 1 Hz heartbeat would be a line a second */
        Serial.printf("Rx: id=0x%03lX dlc=%d\n", (unsigned long)m->identifier, m->data_length_code);

    if (m->identifier == MSG_ENROLL_ASSIGN) {
        for (int i = 0; i < MD_MAX_ENGINES; i++)
            sim_engine_handle_enroll_assign(&sims[i], m);
        alarmer_handle_enroll_assign(m);
        return;
    }

    if (m->identifier >= MSG_OTA_START(0) && m->identifier < MSG_OTA_START(0) + MD_MAX_ENGINES) {
        ota_handle_start_chunk((int)(m->identifier - MSG_OTA_START(0)), m);
        return;
    }

    if (m->identifier >= MSG_WIFI_JOIN(0) && m->identifier < MSG_WIFI_JOIN(0) + MD_MAX_ENGINES) {
        wifi_join_handle_start_chunk((int)(m->identifier - MSG_WIFI_JOIN(0)), m);
        return;
    }

    int te = md_engine_from_telem(m->identifier);
    if (te >= 0) alarmer_handle_telem(te, m);

    if (m->identifier >= MSG_CMD_ALARM_SILENCE(0) &&
        m->identifier <  MSG_CMD_ALARM_SILENCE(0) + MD_MAX_ENGINES) {
        alarmer_handle_silence((int)(m->identifier - MSG_CMD_ALARM_SILENCE(0)), m);
    }

    for (int i = 0; i < MD_MAX_ENGINES; i++)
        sim_engine_recv_cmd(&sims[i], m);
}

/* called from loop() - the one safe place to touch WiFi APIs on this
 * board (matches engine_display.ino's own wifi_setup() discipline,
 * "WiFi APIs may only be touched from the one proven place" - never from
 * bus_handle_rx()'s context, which can be an ESP-NOW receive callback).
 * Only acts once ota_handle_start_chunk() has fully reassembled a
 * command and set g_ota_ready. */
static void ota_tick(void)
{
    if (!g_ota_ready) return;
    g_ota_ready = false;

    uint8_t ack[8] = {0};
    ack[0] = OTA_ACK_OK;
    /* sent 3 times, not once - this is a one-shot unicast with no
     * retry/resend mechanism of its own (unlike ENROLL_REQUEST's
     * repeated broadcast or the dead-man commands' HOLD_RESEND_MS resend
     * loop), and a single lost packet here means HELM never finds out a
     * genuine success happened at all. Confirmed on real hardware: this
     * board completed a full update (visible via its own fresh
     * ENROLL_REQUEST after reboot) while HELM's UI still timed out
     * waiting for an ACK that evidently never arrived. */
    for (int i = 0; i < 3; i++) {
        bus_send(MSG_OTA_ACK(g_ota_engine), ack, 8);
        delay(50);
    }
    delay(50);   /* extra settle time before WiFi.begin() disrupts the current channel */

    Serial.println("OTA: joining WiFi to download update...");
    WiFi.mode(WIFI_STA);
    wifi_tx_cap();
    WiFi.begin(g_ota_ssid, g_ota_pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(500);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OTA: WiFi join failed, aborting update - restarting to resume normal operation");
        delay(500);
        ESP.restart();
        return;
    }

    /* https:// (GitHub releases) needs a TLS client; plain http:// (a
     * local test server) must NOT use one. Certificate chain is not
     * verified - same trade as the other OpenBoat firmwares, the MD5
     * below guards against a corrupted download. GitHub answers with a
     * redirect to its download host, so follow it. */
    WiFiClient       plain_client;
    WiFiClientSecure tls_client;
    tls_client.setInsecure();
    String real_url = ota_resolve_url(String(g_ota_url));
    NetworkClient &client = real_url.startsWith("https://")
        ? (NetworkClient &)tls_client : (NetworkClient &)plain_client;
    httpUpdate.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);   /* ota_resolve_url() already did */
    httpUpdate.rebootOnUpdate(true);   /* headless board, no UI to show a result on either way */
    /* checksum verification (if HELM sent one - see ota_handle_start_
     * chunk()/can_protocol.h's MSG_OTA_START comment): same setMD5sum()
     * mechanism as engine_display.ino's self-update, makes Update.end()
     * reject a corrupted/tampered image instead of flashing it. Skipped,
     * not failed, if empty. */
    if (g_ota_md5[0]) httpUpdate.setMD5sum(g_ota_md5);
    Serial.printf("OTA: downloading from %s\n", g_ota_url);
    t_httpUpdate_return ret = httpUpdate.update(client, real_url);
    Serial.printf("OTA: httpUpdate.update() returned %d (%s)\n",
        (int)ret, httpUpdate.getLastErrorString().c_str());

    /* only reach here if it wasn't OK (success already rebooted via
     * rebootOnUpdate(true)) - restart anyway to cleanly resume normal
     * ESP-NOW/CAN operation rather than staying in a half-joined state */
    delay(500);
    ESP.restart();
}

/* called from loop() - same "one safe place to touch WiFi/NVS-then-
 * reboot" discipline as ota_tick() above. save_wifi_and_reboot() (defined
 * further down, shared with the serial WIFI: command) does the actual
 * work - this just hands off once wifi_join_handle_start_chunk() has
 * fully reassembled a command and set g_wifi_join_ready. */
static void wifi_join_tick(void)
{
    if (!g_wifi_join_ready) return;
    g_wifi_join_ready = false;
    Serial.printf("WiFi: MSG_WIFI_JOIN received - saving ssid=\"%s\" and restarting to join\n",
        g_wifi_join_ssid);
    save_wifi_and_reboot(g_wifi_join_ssid, g_wifi_join_pass);
}

static void can_poll(void)
{
    if (!can_ok) return;

    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) {
        if (m.rtr) continue;
        bus_handle_rx(&m);
    }

    can_bus_health_tick();
}

/* ==================== per-engine simulation ==================== */

/* dead-man timeouts + CRANK_MAX_MS, same discipline a real CTRL board
 * enforces per can_protocol.h - a held message means "finger on the
 * button NOW", never a latch, and cranking cannot exceed CRANK_MAX_MS */
static void sim_engine_deadman_tick(struct sim_engine_t *s)
{
    uint32_t now = millis();

    if (s->glow_held && now - s->glow_last_rx > HOLD_TIMEOUT_MS)
        s->glow_held = false;

    if (s->start_held) {
        if (now - s->start_last_rx > HOLD_TIMEOUT_MS)
            s->start_held = false;
        else if (s->crank_start_ms && now - s->crank_start_ms > CRANK_MAX_MS)
            s->start_held = false;
    }

    if (s->stop_held && now - s->stop_last_rx > HOLD_TIMEOUT_MS)
        s->stop_held = false;
}

static void sim_engine_physics_tick(struct sim_engine_t *s, float dt_s)
{
    bool cranking = s->start_held && !s->running;

    if (cranking) {
        if (!s->crank_start_ms) s->crank_start_ms = millis();
        if (!s->force_fail_start && millis() - s->crank_start_ms >= SIM_CRANK_CATCH_MS) {
            /* fires: catches rough, then ramps up to idle below */
            s->running = true;
            s->rpm = SIM_IDLE_RPM * SIM_CATCH_RPM_FRAC;
        } else {
            /* starter motor turning the engine over - the tach twitters
             * with each compression stroke rather than sitting at a flat
             * 0, same "look" as a real diesel being cranked */
            if (millis() >= s->crank_jitter_retarget_ms) {
                s->crank_jitter_rpm = SIM_CRANK_RPM_BASE +
                    (float)random(-(long)SIM_CRANK_RPM_JITTER, (long)SIM_CRANK_RPM_JITTER);
                s->crank_jitter_retarget_ms = millis() + SIM_CRANK_JITTER_MS;
            }
            s->rpm = s->crank_jitter_rpm;
        }
    } else {
        s->crank_start_ms = 0;
        s->crank_jitter_retarget_ms = 0;
    }

    if (s->stop_held && s->running && (s->caps & CAP_STOP)) {
        /* fuel/ignition cut - let it decelerate naturally below rather
         * than snapping to zero, same as any other engine-off case */
        s->running = false;
    }

    if (s->running && !cranking) {
        float target_rpm = SIM_IDLE_RPM;

        if (s->throttle_mode == THROTTLE_RANDOM) {
            if (millis() >= s->random_retarget_ms) {
                float span = SIM_RANDOM_REV_MAX - SIM_RANDOM_REV_MIN;
                s->random_target_rpm = SIM_RANDOM_REV_MIN + (float)random(0, (long)span);
                s->random_retarget_ms = millis() +
                    random(SIM_RANDOM_RETARGET_MIN_MS, SIM_RANDOM_RETARGET_MAX_MS);
            }
            target_rpm = s->random_target_rpm;
        }

        float ramp = (s->throttle_mode == THROTTLE_RANDOM) ? SIM_RANDOM_REV_RAMP_PER_S : SIM_IDLE_SETTLE_PER_S;
        if (s->rpm < target_rpm)      s->rpm += ramp * dt_s;
        else if (s->rpm > target_rpm) s->rpm -= ramp * dt_s;
    } else if (!s->running && !cranking && s->rpm > 0) {
        s->rpm -= SIM_SPINDOWN_PER_S * dt_s;
        if (s->rpm < 0) s->rpm = 0;
    }

    float target_temp = s->running ? SIM_OPERATING_TEMP_C : SIM_AMBIENT_TEMP_C;
    s->temp_c += (target_temp - s->temp_c) * 0.02f;

    float target_oil = s->running ? (1.5f + s->rpm / 4000.0f * 2.5f) : 0.0f;
    s->oil_bar += (target_oil - s->oil_bar) * SIM_OIL_RAMP_FRAC;
}

static void sim_engine_send_announce(struct sim_engine_t *s)
{
    uint8_t d[8] = {0};
    d[0] = s->idx;
    md_pack_u16(&d[1], s->caps);
    d[3] = s->type;
    d[4] = MD_PROTO_VERSION;
    md_pack_u16(&d[5], FW_BUILD);   /* OTA - see can_protocol.h's ANNOUNCE comment */
    d[7] = HW_ID;
    bus_send(MSG_ANNOUNCE(s->idx), d, 8);
}

static void sim_engine_send_telem(struct sim_engine_t *s)
{
    /* each alarm: (do we even have the sensor?) && (forced OR physics-tripped).
     * charge/water have no physics model at all - the force checkbox is
     * the only way to ever raise them, which is the point (bench-testing
     * the display/alarmer's reaction to those two capabilities). */
    bool temp_alarm   = (s->caps & CAP_TEMP_ALARM)   &&
                         (s->force_temp_alarm  || s->temp_c >= SIM_TEMP_ALARM_C);
    bool press_alarm  = (s->caps & CAP_PRESS_ALARM)  &&
                         (s->force_press_alarm || (s->running && s->oil_bar < SIM_OIL_ALARM_BAR));
    bool charge_alarm = (s->caps & CAP_CHARGE_ALARM) && s->force_charge_alarm;
    bool water_alarm  = (s->caps & CAP_WATER_ALARM)  && s->force_water_alarm;

    if (!temp_alarm)   s->temp_silenced   = false;   /* re-arm on clear, same as the display */
    if (!press_alarm)  s->press_silenced  = false;
    if (!charge_alarm) s->charge_silenced = false;
    if (!water_alarm)  s->water_silenced  = false;

    uint8_t flags = 0;
    if (temp_alarm)                                 flags |= TFLAG_TEMP_ALARM;
    if (press_alarm)                                flags |= TFLAG_PRESS_ALARM;
    if (charge_alarm)                               flags |= TFLAG_CHARGE_ALARM;
    if (water_alarm)                                flags |= TFLAG_WATER_ALARM;
    if (s->ign_commanded)                           flags |= TFLAG_IGNITION_ON;
    if (s->glow_held)                               flags |= TFLAG_GLOW_ACTIVE;
    if (s->start_held && !s->running)               flags |= TFLAG_CRANK_ACTIVE;
    if ((s->caps & CAP_STOP) && s->stop_held)        flags |= TFLAG_STOP_ACTIVE;

    uint8_t d[8] = {0};
    md_pack_u16(&d[0], (uint16_t)(s->rpm < 0 ? 0 : s->rpm));
    md_pack_u16(&d[2], (uint16_t)(int16_t)(s->temp_c * 10.0f));
    md_pack_u16(&d[4], (uint16_t)(s->oil_bar * 100.0f));
    d[6] = flags;
    bus_send(MSG_TELEM_PRIMARY(s->idx), d, 8);
}

static void sim_engine_send_hours(struct sim_engine_t *s)
{
    if (!(s->caps & CAP_HOURS)) return;   /* no hour meter wired up */
    uint8_t d[8] = {0};
    md_pack_u32(d, s->hours_x10);
    bus_send(MSG_TELEM_HOURS(s->idx), d, 4);
}

static void sim_engine_send_name(struct sim_engine_t *s)
{
    uint8_t total_len = (uint8_t)strnlen(s->name, MD_ENGINE_NAME_MAXLEN);
    if (!total_len) return;
    uint8_t chunks = (total_len + NAME_CHUNK_BYTES - 1) / NAME_CHUNK_BYTES;

    for (uint8_t c = 0; c < chunks; c++) {
        uint8_t d[8] = {0};
        d[0] = total_len;
        d[1] = c;
        int off = c * NAME_CHUNK_BYTES;
        for (int j = 0; j < NAME_CHUNK_BYTES && off + j < total_len; j++)
            d[2 + j] = (uint8_t)s->name[off + j];
        bus_send(MSG_ENGINE_NAME(s->idx), d, 8);
    }
}

/* called when the debug page disables an engine - resets to a clean
 * "powered off" state so re-enabling later doesn't resume mid-crank or
 * carry over a stale held-button. Name/caps/throttle mode/force-alarm
 * toggles are user configuration, not power state - left untouched. */
static void sim_engine_reset(struct sim_engine_t *s)
{
    s->ign_commanded = s->glow_held = s->start_held = s->stop_held = false;
    s->crank_start_ms = 0;
    s->crank_jitter_retarget_ms = 0;
    s->rpm = 0;
    s->running = false;
    s->temp_c = SIM_AMBIENT_TEMP_C;
    s->oil_bar = 0;
    s->random_retarget_ms = 0;
}

static void sim_engine_tick(struct sim_engine_t *s, float dt_s)
{
    if (!s->enabled) return;   /* off the bus entirely - like powered down */

    if (!s->enrolled) {
        sim_engine_enroll_tick(s);
        return;   /* no assigned index yet - nothing else can go on the bus */
    }

    uint32_t now = millis();

    sim_engine_deadman_tick(s);
    sim_engine_physics_tick(s, dt_s);

    if (s->running) {
        /* a single tick's worth of runtime is a tiny fraction of one
         * "0.1h" unit - accumulate in float and only commit whole units,
         * or this would truncate to zero every iteration and hours would
         * never move */
        s->hours_accum_x10 += dt_s / 3600.0f * 10.0f;
        if (s->hours_accum_x10 >= 1.0f) {
            uint32_t whole = (uint32_t)s->hours_accum_x10;
            s->hours_x10 += whole;
            s->hours_accum_x10 -= (float)whole;
        }
    }

    if (now - s->last_announce_ms >= ANNOUNCE_PERIOD_MS) {
        s->last_announce_ms = now;
        sim_engine_send_announce(s);
    }
    if (now - s->last_telem_ms >= TELEM_PERIOD_MS) {
        s->last_telem_ms = now;
        sim_engine_send_telem(s);
    }
    if (now - s->last_hours_ms >= SIM_HOURS_SAVE_MS) {
        s->last_hours_ms = now;
        sim_engine_send_hours(s);
    }
    if (now - s->last_name_ms >= NAME_SEND_PERIOD_MS) {
        s->last_name_ms = now;
        sim_engine_send_name(s);
    }
}

/* ==================== WiFi debug page ==================== */

static WebServer server(80);
static char wifi_ip_text[64] = "WiFi starting...";

/* CAP_* checkbox ids, in the fixed order used by both the HTML template
 * and handle_set()/status JSON below - keeps the two in lockstep without
 * a giant hand-written if/else chain. */
struct cap_checkbox { const char *id; uint16_t bit; };
static const struct cap_checkbox CAP_CHECKBOXES[] = {
    { "rpm",   CAP_RPM },          { "temp",  CAP_COOLANT_TEMP },
    { "oil",   CAP_OIL_PRESS },    { "talm",  CAP_TEMP_ALARM },
    { "palm",  CAP_PRESS_ALARM },  { "glow",  CAP_GLOW },
    { "start", CAP_START },        { "ign",   CAP_IGNITION },
    { "hrs",   CAP_HOURS },        { "aud",   CAP_AUDIO },
    { "stop",  CAP_STOP },         { "chg",   CAP_CHARGE_ALARM },
    { "wtr",   CAP_WATER_ALARM },
};
#define NUM_CAP_CHECKBOXES (sizeof(CAP_CHECKBOXES) / sizeof(CAP_CHECKBOXES[0]))

static const char PAGE_HEAD[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width">
<title>CAN Sim Debug</title><style>
body{font-family:sans-serif;background:#0a1929;color:#eee;padding:16px;max-width:640px;margin:auto}
h2{color:#fff;font-size:20px} h3{margin:0 0 8px;color:#8fa3b8;font-size:1em}
em{color:#888;font-size:0.85em}
.card{background:#102438;border:1px solid #33475c;border-radius:8px;padding:14px;margin-top:16px}
.row{display:flex;flex-wrap:wrap;gap:4px 14px;margin:8px 0;align-items:center}
.row label{display:flex;align-items:center;gap:4px;font-size:13px;white-space:nowrap}
.cap-label{width:100%;font-size:12px;color:#8fa3b8;margin-top:4px}
.stat{font-family:monospace;font-size:0.9em;margin-right:14px;color:#6cf}
input[type=text]{background:#0a1929;color:#eee;border:1px solid #33475c;border-radius:4px;padding:4px 6px;width:170px}
select{background:#0a1929;color:#eee;border:1px solid #33475c;border-radius:4px;padding:3px}
</style></head><body>
<h2>CAN Bus Simulator &mdash; Debug</h2>
<em>Up to 4 fake engines + 1 alarmer. Physics run on-board (hours accumulate
in real time while running); this page defines what each engine IS (name,
capabilities), how its throttle behaves, and can force any alarm on for
testing regardless of the simulated temp/oil values.</em>
<div class="card"><h3>Board settings</h3>
<div class="row">
<label><input type="checkbox" id="canen" checked> CAN Enabled (uncheck to bench-test ESP-NOW with no transceiver wired)</label>
</div>
<div class="row">
<span class="stat">Wireless pairing: <span id="pairstat">--</span></span>
<span class="stat">Channel: <span id="chanstat">--</span></span>
<span class="stat">Wired link: <span id="wiredstat">--</span></span>
<button id="repairbtn">Clear Pairing</button>
</div>
</div>
)HTML";

/* '@' is replaced with the engine index digit (0-3) at request time -
 * simpler and less error-prone than an snprintf with ~25 repeated %d
 * args for one value. See append_engine_card(). */
static const char CARD_TEMPLATE[] PROGMEM = R"HTML(
<div class="card"><h3>Engine @</h3>
<div class="row">
<label><input type="checkbox" id="en@"> Enabled (on the bus)</label>
<label>Name <input type="text" id="nm@" maxlength="24"></label>
<label>Throttle <select id="md@"><option value="off">Off</option><option value="idle">Idle</option><option value="rand">Random rev</option></select></label>
</div>
<div class="row"><span class="cap-label">Capabilities</span>
<label><input type="checkbox" id="c@_rpm"> RPM</label>
<label><input type="checkbox" id="c@_temp"> Coolant temp</label>
<label><input type="checkbox" id="c@_oil"> Oil pressure</label>
<label><input type="checkbox" id="c@_talm"> Temp alarm</label>
<label><input type="checkbox" id="c@_palm"> Press alarm</label>
<label><input type="checkbox" id="c@_glow"> Glow</label>
<label><input type="checkbox" id="c@_start"> Start</label>
<label><input type="checkbox" id="c@_ign"> Ignition</label>
<label><input type="checkbox" id="c@_hrs"> Hours</label>
<label><input type="checkbox" id="c@_aud"> Audio</label>
<label><input type="checkbox" id="c@_stop"> Electric stop</label>
<label><input type="checkbox" id="c@_chg"> Charge alarm</label>
<label><input type="checkbox" id="c@_wtr"> Water-in-fuel alarm</label>
</div>
<div class="row"><span class="cap-label">Force alarm (test - ignores capability's natural trigger)</span>
<label><input type="checkbox" id="f@_temp"> Temp</label>
<label><input type="checkbox" id="f@_press"> Press</label>
<label><input type="checkbox" id="f@_chg"> Charge</label>
<label><input type="checkbox" id="f@_wtr"> Water</label>
</div>
<div class="row"><span class="cap-label">Starter test</span>
<label><input type="checkbox" id="f@_ffs"> Force fail to start</label>
</div>
<div class="row">
<span class="stat">state <span id="st@_state">--</span></span>
<span class="stat">rpm <span id="st@_rpm">--</span></span>
<span class="stat">temp <span id="st@_temp">--</span></span>
<span class="stat">oil <span id="st@_oil">--</span></span>
<span class="stat">hours <span id="st@_hours">--</span></span>
<span class="stat"><span id="st@_enr">--</span></span>
</div></div>
)HTML";

static const char PAGE_TAIL[] PROGMEM = R"HTML(
<script>
var CAPS = ['rpm','temp','oil','talm','palm','glow','start','ign','hrs','aud','stop','chg','wtr'];

function pushGlobal(){
  /* checkbox is "CAN Enabled" (positive sense) - wire format is still
     candis=1 meaning disabled, so invert at this boundary */
  fetch('/setg?candis='+(document.getElementById('canen').checked?0:1)).catch(function(){});
}
document.getElementById('canen').addEventListener('change', pushGlobal);
document.getElementById('repairbtn').addEventListener('click', function(){
  fetch('/setg?repair=1').catch(function(){});
});

function push(i){
  var q = 'i='+i+'&en='+(document.getElementById('en'+i).checked?1:0)
        +'&nm='+encodeURIComponent(document.getElementById('nm'+i).value)
        +'&md='+document.getElementById('md'+i).value
        +'&f_temp='+(document.getElementById('f'+i+'_temp').checked?1:0)
        +'&f_press='+(document.getElementById('f'+i+'_press').checked?1:0)
        +'&f_chg='+(document.getElementById('f'+i+'_chg').checked?1:0)
        +'&f_wtr='+(document.getElementById('f'+i+'_wtr').checked?1:0)
        +'&f_ffs='+(document.getElementById('f'+i+'_ffs').checked?1:0);
  CAPS.forEach(function(c){
    q += '&c_'+c+'='+(document.getElementById('c'+i+'_'+c).checked?1:0);
  });
  fetch('/set?'+q).catch(function(){});
}

/* element ids are known up front (i is just 0..3) - wire listeners
 * immediately rather than waiting on the first /status response, so the
 * page is interactive right away */
for (var wi = 0; wi < 4; wi++) (function(i){
  ['en'+i,'nm'+i,'md'+i,'f'+i+'_temp','f'+i+'_press','f'+i+'_chg','f'+i+'_wtr','f'+i+'_ffs']
    .forEach(function(id){ document.getElementById(id).addEventListener('change', function(){ push(i); }); });
  CAPS.forEach(function(c){ document.getElementById('c'+i+'_'+c).addEventListener('change', function(){ push(i); }); });
})(wi);

var inited = [false,false,false,false];
var globalInited = false;
function poll(){
  fetch('/status').then(function(r){ return r.json(); }).then(function(j){
    if (!globalInited) {
      document.getElementById('canen').checked = !j.can_disabled;
      globalInited = true;
    }
    document.getElementById('pairstat').textContent = j.paired ? 'paired' : 'not paired';
    document.getElementById('chanstat').textContent = j.channel;
    document.getElementById('wiredstat').textContent = j.wired_active ? 'receiving' : 'no signal';
    for (var i = 0; i < 4; i++) {
      var e = j['e'+i];
      if (!inited[i]) {
        document.getElementById('en'+i).checked = e.enabled;
        document.getElementById('nm'+i).value = e.name;
        document.getElementById('md'+i).value = e.mode;
        document.getElementById('f'+i+'_temp').checked = e.f_temp;
        document.getElementById('f'+i+'_press').checked = e.f_press;
        document.getElementById('f'+i+'_chg').checked = e.f_chg;
        document.getElementById('f'+i+'_wtr').checked = e.f_wtr;
        document.getElementById('f'+i+'_ffs').checked = e.f_ffs;
        CAPS.forEach(function(c){ document.getElementById('c'+i+'_'+c).checked = e['c_'+c]; });
        inited[i] = true;
      }
      document.getElementById('st'+i+'_state').textContent = e.cranking ? 'cranking' : (e.running ? 'running' : 'stopped');
      document.getElementById('st'+i+'_rpm').textContent = e.rpm;
      document.getElementById('st'+i+'_temp').textContent = e.temp + ' C';
      document.getElementById('st'+i+'_oil').textContent = e.oil + ' bar';
      document.getElementById('st'+i+'_hours').textContent = e.hours + ' h';
      document.getElementById('st'+i+'_enr').textContent = e.enrolled ? ('bus id ' + e.idx) : 'enrolling...';
    }
  }).catch(function(){});
}
setInterval(poll, 1000);
poll();
</script></body></html>
)HTML";

static void append_engine_card(String &html, int i)
{
    char tmpl[sizeof(CARD_TEMPLATE)];
    strcpy_P(tmpl, CARD_TEMPLATE);
    char digit = (char)('0' + i);
    for (char *p = tmpl; *p; p++) if (*p == '@') *p = digit;
    html += tmpl;
}

static void handle_root(void)
{
    String page;
    page.reserve(10000);
    page += FPSTR(PAGE_HEAD);
    for (int i = 0; i < MD_MAX_ENGINES; i++) append_engine_card(page, i);
    page += FPSTR(PAGE_TAIL);
    server.send(200, "text/html", page);
}

/* strip anything that isn't printable ASCII (and isn't '"' or '\\', to
 * keep the status JSON below trivially safe without full escaping) -
 * this is user-entered free text going out over CAN and back over JSON */
static void sanitize_name(char *dst, const char *src, size_t dstsize)
{
    size_t n = 0;
    for (size_t i = 0; src[i] && n < dstsize - 1; i++) {
        char c = src[i];
        dst[n++] = (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : ' ';
    }
    dst[n] = 0;
}

static void handle_set(void)
{
    if (!server.hasArg("i")) { server.send(400, "text/plain", "missing i"); return; }
    int i = server.arg("i").toInt();
    if (i < 0 || i >= MD_MAX_ENGINES) { server.send(400, "text/plain", "bad i"); return; }
    struct sim_engine_t *s = &sims[i];

    if (server.hasArg("en")) {
        bool en = server.arg("en") == "1";
        if (s->enabled && !en) sim_engine_reset(s);
        s->enabled = en;
    }
    if (server.hasArg("nm")) sanitize_name(s->name, server.arg("nm").c_str(), sizeof(s->name));
    if (server.hasArg("md")) {
        String md = server.arg("md");
        s->throttle_mode = (md == "rand") ? THROTTLE_RANDOM : (md == "idle") ? THROTTLE_IDLE : THROTTLE_OFF;
    }

    if (server.hasArg("f_temp"))  s->force_temp_alarm   = server.arg("f_temp")  == "1";
    if (server.hasArg("f_press")) s->force_press_alarm  = server.arg("f_press") == "1";
    if (server.hasArg("f_chg"))   s->force_charge_alarm = server.arg("f_chg")   == "1";
    if (server.hasArg("f_wtr"))   s->force_water_alarm  = server.arg("f_wtr")   == "1";
    if (server.hasArg("f_ffs"))   s->force_fail_start   = server.arg("f_ffs")   == "1";

    for (size_t c = 0; c < NUM_CAP_CHECKBOXES; c++) {
        String argname = String("c_") + CAP_CHECKBOXES[c].id;
        if (!server.hasArg(argname)) continue;
        if (server.arg(argname) == "1") s->caps |= CAP_CHECKBOXES[c].bit;
        else                            s->caps &= ~CAP_CHECKBOXES[c].bit;
    }

    server.send(200, "text/plain", "ok");
}

static void sim_engine_status_json(struct sim_engine_t *s, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen,
        "{\"enabled\":%s,\"name\":\"%s\",\"mode\":\"%s\",\"running\":%s,\"cranking\":%s,"
        "\"rpm\":%d,\"temp\":%.1f,\"oil\":%.2f,\"hours\":%.1f,"
        "\"enrolled\":%s,\"idx\":%d,"
        "\"f_temp\":%s,\"f_press\":%s,\"f_chg\":%s,\"f_wtr\":%s,\"f_ffs\":%s",
        s->enabled ? "true" : "false", s->name,
        (s->throttle_mode == THROTTLE_RANDOM) ? "rand" : (s->throttle_mode == THROTTLE_IDLE) ? "idle" : "off",
        s->running ? "true" : "false",
        (s->start_held && !s->running) ? "true" : "false",
        (int)s->rpm, s->temp_c, s->oil_bar, s->hours_x10 / 10.0f,
        s->enrolled ? "true" : "false", s->enrolled ? (int)s->idx : -1,
        s->force_temp_alarm ? "true" : "false", s->force_press_alarm ? "true" : "false",
        s->force_charge_alarm ? "true" : "false", s->force_water_alarm ? "true" : "false",
        s->force_fail_start ? "true" : "false");

    for (size_t c = 0; c < NUM_CAP_CHECKBOXES && n < (int)buflen; c++)
        n += snprintf(buf + n, buflen - n, ",\"c_%s\":%s", CAP_CHECKBOXES[c].id,
                      (s->caps & CAP_CHECKBOXES[c].bit) ? "true" : "false");
    if (n < (int)buflen) snprintf(buf + n, buflen - n, "}");
}

static void handle_status(void)
{
    char eng[MD_MAX_ENGINES][480];   /* worst case ~434 bytes: 24-char name + 13 cap booleans */
    String out = "{";
    out += "\"can_disabled\":"; out += (g_can_disabled ? "true" : "false"); out += ",";
    out += "\"paired\":"; out += (g_paired ? "true" : "false"); out += ",";
    /* live radio channel (WiFi.channel(), not just g_channel - the
     * hunt/candidate variable - so a mismatch between "what we think
     * we're on" and "what the radio's actually on" is visible too) */
    out += "\"channel\":"; out += WiFi.channel(); out += ",";
    /* "wired_active" - a good frame received in the last 3s, so the debug
     * page can show whether the direct-wire link (see wired_bus.h) is
     * actually hearing anything, same visibility reasoning as "channel"
     * above for ESP-NOW - added since there's no other way to confirm
     * the 3-wire hookup without a serial cable already attached. */
    out += "\"wired_active\":";
    out += (g_wired_last_rx_ms != 0 && millis() - g_wired_last_rx_ms < 3000) ? "true" : "false";
    out += ",";
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        sim_engine_status_json(&sims[i], eng[i], sizeof(eng[i]));
        out += "\"e"; out += i; out += "\":"; out += eng[i];
        if (i < MD_MAX_ENGINES - 1) out += ",";
    }
    out += "}";
    server.send(200, "application/json", out);
}

/* global (non-per-engine) settings - kept separate from handle_set(),
 * which is i-scoped and validates against MD_MAX_ENGINES. */
static void handle_set_global(void)
{
    if (server.hasArg("candis")) {
        g_can_disabled = server.arg("candis") == "1";
        if (prefs_ok) prefs.putBool("can_dis", g_can_disabled);
        Serial.printf("Debug page: CAN %s (bus_send will now use %s)\n",
            g_can_disabled ? "disabled" : "enabled",
            (!g_can_disabled && can_ok) ? "CAN" : "ESP-NOW");
    }
    if (server.hasArg("repair") && server.arg("repair") == "1") {
        espnow_forget_helm();
        Serial.println("ESP-NOW: pairing re-armed, broadcasting again");
    }
    server.send(200, "text/plain", "ok");
}

static void wifi_setup(void)
{
    /* credential resolution: compiled-in values win and are persisted;
     * empty compiled-in values fall back to what's saved in flash -
     * same rule as the HELM panel's wifi_setup(). */
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
     * radio in STA mode so ESP-NOW still works (it only needs WiFi.mode()
     * to have run, not an actual connection). The channel set here is
     * just a starting point - g_wifi_joined stays false, so
     * pairing_requester_tick() takes over from its very first call and
     * actively hunts for HELM's real channel rather than assuming this
     * one is right. WIFI:<ssid>,<password> over serial (works with no
     * network at all) is the only way this board ever starts using WiFi. */
    if (ssid.length() == 0) {
        WiFi.mode(WIFI_STA);
        wifi_tx_cap();
        esp_wifi_set_channel(ESPNOW_AP_FALLBACK_CHANNEL, WIFI_SECOND_CHAN_NONE);
        Serial.printf("WiFi: no credentials - staying off WiFi (ESP-NOW only, channel %d to start). "
                      "Send WIFI:<ssid>,<password> over serial to enable networking.\n",
                      ESPNOW_AP_FALLBACK_CHANNEL);
        snprintf(wifi_ip_text, sizeof(wifi_ip_text), "WiFi off (ESP-NOW only)");
        return;
    }

    Serial.printf("WiFi: connecting to \"%s\"", ssid.c_str());
    WiFi.mode(WIFI_STA);
    wifi_tx_cap();
    WiFi.setSleep(false);
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

    g_wifi_joined = true;
    Serial.print("WiFi: connected. Debug page: http://");
    Serial.println(WiFi.localIP());
    snprintf(wifi_ip_text, sizeof(wifi_ip_text),
             "Debug: http://%s", WiFi.localIP().toString().c_str());
    Serial.printf("WiFi: channel %d (ESP-NOW will use this)\n", WiFi.channel());

    server.on("/", handle_root);
    server.on("/set", handle_set);
    server.on("/setg", handle_set_global);
    server.on("/status", handle_status);
    server.begin();
}

/* ==================== serial console (WiFi creds, any time) ====================
 * No screen on this board, so this is really the primary way to
 * configure WiFi day-to-day (not just a bench convenience like it is on
 * the HELM panel). Send over USB serial at any time:
 *   WIFI:<ssid>,<password>\n     (password may be empty for open APs)
 * Only ever writes NVS + calls ESP.restart() - same reasoning as the
 * panel's identical feature: never touch WiFi.* directly outside
 * wifi_setup()'s proven boot-time path. */

static void print_serial_help(void)
{
    Serial.println("Serial commands (case-insensitive):");
    Serial.println("  HELP                    - show this list");
    Serial.println("  WIFI:<ssid>,<password>  - save WiFi creds and reboot (password may be empty)");
    Serial.println("  KEY <passphrase>        - set the shared ESP-NOW secret (12+ characters; the same on every board), then restart");
    Serial.println("  KEY?                    - is a key set? shows its fingerprint (same on every board with the same key)");
    Serial.println("  KEYCLEAR                - forget the key (ESP-NOW goes off), then restart");
    Serial.println("  REPAIR                  - forget HELM and look for it again");
    Serial.println("  PAIRSTATUS / STATUS     - show ESP-NOW link + CAN/transport status");
    Serial.println("  WEBMODE                 - restart into setup mode: a web page to set the key and WiFi (10 minutes)");
    Serial.println("  UPDATE                  - not on this board: the HELM updates it (HELM serial UPDATE, or Update All)");
    Serial.println("  REBOOT                  - restart");
    Serial.println("  CANON / CANOFF          - enable/disable CAN (bus_send falls back to ESP-NOW when off)");
}

/* Shared by the serial WIFI: command and wifi_join_tick() (MSG_WIFI_JOIN,
 * see can_protocol.h) - saves creds to NVS and reboots to connect, so the
 * join survives future reboots rather than being a one-off connection.
 * Only ever writes NVS + calls ESP.restart() - never touches WiFi.*
 * directly outside wifi_setup()'s proven boot-time path, same discipline
 * as everywhere else WiFi credentials get set on this board. */
static void save_wifi_and_reboot(const char *ssid, const char *pass)
{
    if (!ssid[0]) {
        Serial.println("WiFi: empty SSID, ignored");
        return;
    }
    if (!prefs_ok) {
        Serial.println("WiFi: NVS unavailable, can't save WiFi creds");
        return;
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    Serial.printf("WiFi: saved ssid=\"%s\" - restarting to connect...\n", ssid);
    Serial.flush();
    delay(200);
    ESP.restart();
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
        if (prefs_ok) prefs.putBool("paired", false);   /* any old link used the old key */
        Serial.printf("Serial: key saved, fingerprint %08lX (it must read the same on every board) - restarting\n",
            (unsigned long)fsec_fingerprint());
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "KEY?") == 0 || strcasecmp(line, "KEY") == 0) {
        if (g_fsec_have_key) Serial.printf("Serial: key is set, fingerprint %08lX\n", (unsigned long)fsec_fingerprint());
        else Serial.println("Serial: no key set - ESP-NOW is off. Type  KEY <passphrase>");
        return;
    }
    if (strcasecmp(line, "KEYCLEAR") == 0) {
        fsec_clear(prefs, prefs_ok);
        if (prefs_ok) prefs.putBool("paired", false);
        Serial.println("Serial: key cleared - restarting with ESP-NOW off");
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "WEBMODE") == 0) {
        if (prefs_ok) prefs.putBool("webmode", true);
        Serial.println("Serial: restarting into setup mode");
        Serial.flush();
        delay(300);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "UPDATE") == 0) {
        Serial.println("Serial: this board is updated by the HELM - use UPDATE on the HELM's serial menu, or Update All on its screen");
        return;
    }
    if (strcasecmp(line, "REBOOT") == 0) {
        Serial.println("Serial: restarting");
        Serial.flush();
        delay(200);
        ESP.restart();
        return;
    }
    if (strcasecmp(line, "REPAIR") == 0) {
        espnow_forget_helm();
        Serial.println("ESP-NOW: forgot HELM, looking for it again");
        return;
    }
    if (strcasecmp(line, "PAIRSTATUS") == 0 || strcasecmp(line, "STATUS") == 0) {
        Serial.printf("Firmware build %d, ESP-NOW key %s\n", FW_BUILD, g_fsec_have_key ? "set" : "NOT SET");
        Serial.printf("ESP-NOW: %s, CAN %s (%s) - bus_send uses %s, WiFi %s\n",
            g_paired ? "paired with HELM" : "not paired",
            g_can_disabled ? "disabled" : "enabled",
            can_ok ? "healthy" : "not healthy",
            (!g_can_disabled && can_ok) ? "CAN" : "ESP-NOW",
            g_wifi_joined ? "joined (owns channel)" : "not joined (channel-hunting)");
        if (g_paired)
            Serial.printf("  HELM MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                g_helm_mac[0], g_helm_mac[1], g_helm_mac[2],
                g_helm_mac[3], g_helm_mac[4], g_helm_mac[5]);
        if (!g_wifi_joined)
            Serial.printf("  channel: %d (%s)\n", g_channel,
                g_hunting ? "hunting" : "confirmed");
        return;
    }
    if (strcasecmp(line, "CANON") == 0 || strcasecmp(line, "CANOFF") == 0) {
        g_can_disabled = (strcasecmp(line, "CANOFF") == 0);
        if (prefs_ok) prefs.putBool("can_dis", g_can_disabled);
        Serial.printf("Serial: CAN %s (bus_send will now use %s)\n",
            g_can_disabled ? "disabled" : "enabled",
            (!g_can_disabled && can_ok) ? "CAN" : "ESP-NOW");
        return;
    }
    if (strncasecmp(line, "WIFI:", 5) != 0) {
        Serial.println("Serial: unrecognized command. Type HELP for the list.");
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
    save_wifi_and_reboot(ssid, pass);
}

static void serial_console_tick(void)
{
    static char line[128];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (len > 0) {
                line[len] = 0;
                handle_serial_line(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

/* ==================== arduino entry points ==================== */

/* ==================== setup mode (serial WEBMODE) ====================
 * Restarts into a mode that serves the setup page (ESP-NOW key + WiFi details) for a few minutes - see setup_web.h. */
static void webmode_key_changed(void)
{
    if (prefs_ok) prefs.putBool("paired", false);   /* any old link used the old key */
}

static void webmode_run(void)
{
    SetupWebCtx ctx = { &prefs, prefs_ok, "can_sim board", FW_BUILD, webmode_key_changed };
    sw_run_setup_mode(ctx, wifi_tx_cap, NULL);   /* never returns */
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("CAN bus simulator starting: up to 4 fake engines + alarmer");
    Serial.printf("Firmware build: %d\n", FW_BUILD);   /* previously only
        * visible indirectly via ANNOUNCE's fw_build byte on HELM's own
        * device list - printed here too since that's not always at hand
        * when eyeballing this board's own serial output right after an
        * OTA update to confirm it actually landed */

    prefs_ok = prefs.begin("cansim", false);
    if (!prefs_ok) Serial.println("NVS: prefs.begin failed - WiFi creds won't persist");
    fsec_begin(prefs, prefs_ok);   /* the shared ESP-NOW secret, if one was set */
    if (prefs_ok && prefs.getBool("webmode", false)) {   /* serial WEBMODE asked for the setup page */
        prefs.putBool("webmode", false);
        webmode_run();
    }
    if (prefs_ok) {
        g_can_disabled = prefs.getBool("can_dis", false);
        g_paired       = prefs.getBool("paired", false);
    }

    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);

    /* first 2 slots on by default (matches the old fixed SIM_A/SIM_B
     * bench setup out of the box); slots 2/3 start disabled - turn them
     * on from the debug page once you've given them a name/caps */
    static const char *default_names[MD_MAX_ENGINES] = {
        "SE1 - J1G1E2", "SE2 - J1G2E2", "SE3 - J4G8E4", "SE4 - J8G12E12"
    };
    for (int i = 0; i < MD_MAX_ENGINES; i++) {
        struct sim_engine_t *s = &sims[i];
        s->type    = ENGTYPE_GENERIC_DIESEL;
        s->caps    = SIM_DEFAULT_CAPS | (i == 1 ? CAP_STOP : 0);
        s->enabled = (i < 2);
        s->temp_c  = SIM_AMBIENT_TEMP_C;
        snprintf(s->name, sizeof(s->name), "%s", default_names[i]);
    }

    if (!g_can_disabled) can_setup();
    else Serial.println("CAN: disabled by user setting - skipping TWAI init");
    wired_setup();   /* always-on supplementary transport, see wired_bus.h */

    wifi_setup();   /* must come first - derive_sim_mac() needs the WiFi
                      * radio initialized to read the real MAC address */
    espnow_setup();

    for (int i = 0; i < MD_MAX_ENGINES; i++)
        derive_sim_mac(sims[i].mac, (uint8_t)(0xE0 + i));
    derive_sim_mac(alarmer_mac, 0xEA);

    Serial.println("Serial: send HELP at any time for the full command list");
    Serial.println("Enroll: unenrolled - broadcasting ENROLL_REQUEST until HELM assigns an index");

    /* OTA rollback safety - same reasoning as engine_display.ino's
     * identical call: an image flashed via Update.h/HTTPUpdate boots
     * into a PENDING_VERIFY state on a rollback-enabled partition table
     * and gets auto-rolled-back if never marked valid before the next
     * reboot. Requires this board's PartitionScheme to actually have the
     * ota_0/ota_1 pair - see can_sim/CLAUDE.md's OTA section. */
    esp_ota_mark_app_valid_cancel_rollback();

    Serial.println("setup() complete");
}

void loop()
{
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    float dt_s = (last_tick == 0) ? 0.0f : (now - last_tick) / 1000.0f;
    last_tick = now;

    server.handleClient();
    serial_console_tick();
    pairing_requester_tick();
    can_poll();
    wired_bus_tick();
    ota_tick();
    wifi_join_tick();

    alarmer_enroll_tick();
    for (int i = 0; i < MD_MAX_ENGINES; i++)
        sim_engine_tick(&sims[i], dt_s);

    buzzer_tick();

    static uint32_t last_alive = 0;
    if (now - last_alive > 2000) {
        last_alive = now;
        char line[256];
        int  len = 0;
        for (int i = 0; i < MD_MAX_ENGINES; i++) {
            struct sim_engine_t *s = &sims[i];
            len += snprintf(line + len, sizeof(line) - len,
                "%s\"%s\"(en=%d enr=%d/%d rpm=%d run=%d)",
                i ? " | " : "alive: ", s->name, s->enabled, s->enrolled,
                s->idx, (int)s->rpm, s->running);
        }
        Serial.printf("%s | alarm=%d\n", line, alarmer_should_sound());
    }

    delay(2);
}
