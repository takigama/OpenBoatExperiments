/**
 * can_protocol.h  (protocol v3 - multi-engine + dynamic enrollment)
 *
 * COPY of ../engine_display/can_protocol.h (the canonical one). Arduino
 * sketches can't share files across folders, so this copy must be
 * updated by hand any time the original changes. Keep both copies'
 * content identical; diff them if unsure which changed last.
 *
 * Shared CAN (TWAI) protocol for the engine panel system.
 * Include this SAME file on every node:
 *   - HELM  : 7" display (VIEWE). Full authority, and always the bus
 *             MASTER (see ENROLLMENT below) - never any other node.
 *   - CYD   : small cockpit display. Glow/start/silence only.
 *   - CTRL  : one control board PER ENGINE. Relays, senders, audio.
 *   - ALARMER: standalone speaker/buzzer unit. Listens to every
 *             engine's alarm flags, honors ALARM_SILENCE, no controls.
 *
 * Bus: CAN 2.0 @ 250 kbps, 11-bit IDs, twisted pair, 120R at both ends.
 *
 * ====================== ENROLLMENT (dynamic addressing) ======================
 *
 * HELM is always the bus MASTER; every other node is a SLAVE that gets
 * its working id handed out at runtime rather than hardcoded/jumpered.
 * A slave identifies itself by its MAC address (ESP32s all have a
 * globally-unique one burned in), not by a manually-configured engine
 * index - this is what lets you swap/replace a CTRL board without
 * reshuffling every other engine's slot.
 *
 * Boot sequence for any non-HELM node:
 *   1. Broadcast ENROLL_REQUEST every ENROLL_RETRY_MS: [MAC(6), node_type,
 *      reserved]. Keep sending until an ENROLL_ASSIGN naming this MAC
 *      arrives - the request is the only thing an unenrolled node may
 *      transmit (no ANNOUNCE/telemetry with a not-yet-assigned index).
 *   2. HELM looks the MAC up in its enrollment table (persisted in NVS,
 *      keyed by MAC, so a re-plugged/rebooted board gets the SAME index
 *      back, not a new one). Unknown MAC -> first free slot for that
 *      node_type is assigned and persisted. No free slot -> ASSIGNED_ID_NONE
 *      (0xFF), meaning "no room, stop retrying so fast."
 *   3. HELM broadcasts ENROLL_ASSIGN: [MAC(6), assigned_id, reserved].
 *      Every node hears every ENROLL_ASSIGN and ignores ones for a
 *      different MAC - there's no unicast on this bus.
 *   4. The matching node adopts assigned_id (an engine index e 0..3 for
 *      NODE_TYPE_ENGINE_CTRL, an alarmer index 0..MD_MAX_ALARMERS-1 for
 *      NODE_TYPE_ALARMER) and starts normal operation (ANNOUNCE/telemetry/
 *      hours on that index's ID block, for an engine controller).
 *
 * An enrolled engine controller also broadcasts its human-readable name
 * (MSG_ENGINE_NAME, e.g. "Volvo Penta MD2030C") in NAME_CHUNK_BYTES-byte
 * chunks, since a real product name doesn't fit an 8-byte CAN frame -
 * see that message's comment below for the chunking layout. This is
 * SEPARATE from ANNOUNCE (which stays fixed-size/unchanged for the
 * capability bitmask + engine type enum), sent less often, and purely
 * cosmetic - engine TYPE (ENGTYPE_*) is still what capability/behavior
 * decisions key off, the name is just what a human reads in the UI.
 *
 * Only HELM ever sends ENROLL_ASSIGN. A secondary display (CYD role on
 * this same sketch, see ../engine_display/CLAUDE.md) is a bus listener like
 * any other display and never enrolls devices, matching its lack of
 * ignition/stop authority elsewhere in this protocol.
 *
 * ====================== MULTI-ENGINE MODEL ======================
 *
 * Up to MD_MAX_ENGINES control boards share the bus, each with a
 * unique engine index e (0..3) assigned per ENROLLMENT above. All
 * engine-related message IDs are BASE + e. Displays DISCOVER engines by
 * listening for ANNOUNCE messages, which each control board broadcasts
 * at 1 Hz and which carry a capability bitmask. A display shows only
 * the widgets the selected engine's capabilities include, and
 * addresses its commands to that engine's ID block.
 *
 * ====================== AUTHORITY ======================
 *
 * Commands carry the sender's node id in payload byte 1. The control
 * board honors:
 *   - CMD_IGNITION / CMD_STOP  only from NODE_ID_HELM
 *   - CMD_GLOW_HELD / CMD_START_HELD / CMD_ALARM_SILENCE from
 *     NODE_ID_HELM or NODE_ID_CYD
 * (Private wired bus: payload-based source is the appropriate rigor.)
 *
 * ====================== DEAD-MAN (glow, start & stop) ======================
 *
 * Displays send *_HELD every HOLD_RESEND_MS while the button is
 * physically held. The control board de-energizes the relay if no
 * message arrives for HOLD_TIMEOUT_MS and enforces CRANK_MAX_MS
 * absolutely. A held message is "finger is on the button NOW",
 * never a latch.
 *
 * IGNITION is a latched desired-state: stored by the control board,
 * never timed out (a dead display must not drop a running engine's
 * instrument/alternator circuit).
 *
 * STOP is CAPABILITY-GATED (CAP_STOP) and NOT universal. Most engines
 * on this bus - the MD2030 specifically - can ONLY be stopped
 * mechanically; nothing on this bus may claim to stop them, and
 * CAP_STOP must never be set for one. CAP_STOP exists for a future
 * control board wired to an actual electric fuel/ignition cutoff on an
 * engine that supports it. A display must only show a stop control
 * when the SELECTED engine's ANNOUNCE included CAP_STOP - never as a
 * generic/always-available action. Sent CMD_STOP(e) as dead-man HELD,
 * same as glow/start (deliberate sustained press, not a single tap).
 *
 * ====================== ALARMS ======================
 *
 * The control board raises alarm flags in telemetry and sounds its
 * own audio. CMD_ALARM_SILENCE acknowledges the CURRENTLY ACTIVE
 * alarms: the control board mutes audio for those conditions until
 * they clear; a newly-raised alarm sounds again. Displays mirror the
 * same rule visually (steady instead of flashing once silenced).
 *
 * Alarm conditions beyond coolant temp / oil pressure (TFLAG_TEMP_ALARM,
 * TFLAG_PRESS_ALARM) - both capability-gated, same as everything else,
 * since not every control board wires up every sensor:
 *   - TFLAG_CHARGE_ALARM (CAP_CHARGE_ALARM): alternator/D+ not charging
 *     while running - the classic "battery" idiot light.
 *   - TFLAG_WATER_ALARM (CAP_WATER_ALARM): water-in-fuel sensor tripped
 *     (common on Volvo Penta fuel filters/separators).
 * Over-speed/over-rev is deliberately NOT a wire flag: redline is a
 * shared constant the display already knows (RPM_RED_V), so it's
 * derived client-side from the rpm telemetry already being sent,
 * rather than spending one of the last free flag bits on it.
 */

#ifndef CAN_PROTOCOL_H
#define CAN_PROTOCOL_H

#include <stdint.h>

#define MD_PROTO_VERSION      3
#define MD_MAX_ENGINES        4
#define MD_MAX_ALARMERS       2
#define MD_CAN_BITRATE_KBPS   250

/* ==================== timing rules (ms) ==================== */

#define HOLD_RESEND_MS        100
#define HOLD_TIMEOUT_MS       300
#define CRANK_MAX_MS        60000
#define TELEM_PERIOD_MS       100
#define IGNITION_RESEND_MS   1000
#define HEARTBEAT_MS         1000
#define ANNOUNCE_PERIOD_MS   1000
#define LINK_TIMEOUT_MS      1500   /* telemetry stale (selected engine) */
#define ENGINE_LOST_MS      10000   /* engine removed from discovery     */

#define ENROLL_RETRY_MS       2000  /* unenrolled node: resend ENROLL_REQUEST */
#define ENROLL_RETRY_SLOW_MS 10000  /* ...unless told ASSIGNED_ID_NONE (no room) */
#define NAME_SEND_PERIOD_MS   3000  /* enrolled engine: resend MSG_ENGINE_NAME */

/* ==================== node ids ==================== */

#define NODE_ID_HELM          0x01
#define NODE_ID_CYD           0x02
#define NODE_ID_CTRL_BASE     0x10   /* control board for engine e = 0x10+e */

/* ==================== node types (ENROLL_REQUEST) ==================== */

#define NODE_TYPE_UNKNOWN      0
#define NODE_TYPE_HELM         1   /* never enrolls - always master */
#define NODE_TYPE_CYD          2   /* currently still uses fixed NODE_ID_CYD */
#define NODE_TYPE_ENGINE_CTRL  3   /* assigned_id = engine index e (0..3) */
#define NODE_TYPE_ALARMER      4   /* assigned_id = alarmer index (0..MD_MAX_ALARMERS-1) */

#define ASSIGNED_ID_NONE      0xFF  /* HELM's enrollment table has no free slot */

/* ==================== enrollment messages (broadcast, no engine index) === */

#define MSG_ENROLL_REQUEST     0x010  /* slave -> HELM, repeated until assigned */
/*  [0..5] MAC address (6 bytes)
 *  [6]    node_type (NODE_TYPE_*)
 *  [7]    reserved (0)
 */
#define MSG_ENROLL_ASSIGN      0x011  /* HELM -> everyone, only HELM sends this */
/*  [0..5] MAC address (6 bytes) - recipient matches this against its own MAC
 *  [6]    assigned_id, or ASSIGNED_ID_NONE if no slot was free
 *  [7]    reserved (0)
 */

/* ==================== message IDs (11-bit), e = engine 0..3 ========= */
/* Commands (displays -> engine e). Payload: [0]=value, [1]=source node */

#define MSG_CMD_IGNITION(e)      (0x080u + (e))  /* [0]=desired 0/1, latched */
#define MSG_CMD_GLOW_HELD(e)     (0x090u + (e))  /* [0]=1, dead-man          */
#define MSG_CMD_START_HELD(e)    (0x0A0u + (e))  /* [0]=1, dead-man          */
#define MSG_CMD_ALARM_SILENCE(e) (0x0B0u + (e))  /* [0]=1, ack active alarms */
#define MSG_CMD_STOP(e)          (0x0C0u + (e))  /* [0]=1, dead-man, CAP_STOP only */

/* Telemetry (engine e -> everyone) */

#define MSG_TELEM_PRIMARY(e)     (0x100u + (e))
/*  [0..1] rpm          uint16 LE
 *  [2..3] temp_c x10   int16  LE
 *  [4..5] oil_bar x100 uint16 LE
 *  [6]    flags        (TFLAG_*)
 *  [7]    reserved (0)
 */
#define MSG_TELEM_HOURS(e)       (0x110u + (e))
/*  [0..3] engine_hours x10  uint32 LE */

#define MSG_ANNOUNCE(e)          (0x120u + (e))
/*  [0] engine index e (sanity)
 *  [1..2] capability bits uint16 LE  (CAP_*)
 *  [3] engine type (ENGTYPE_*)
 *  [4] protocol version (MD_PROTO_VERSION)
 *  [5..6] fw_build uint16 LE (OTA - the sending board's own FW_BUILD;
 *         a receiver must check data_length_code >= 7 before reading
 *         this, since it's a backward-compatible extension of what used
 *         to be 3 reserved bytes)
 *  [7] hw_id (HW_*, below): which hardware build this board is, so HELM
 *      can pick the matching image from the OTA manifest. 0 = not
 *      reported (firmware older than this field); HELM treats that as
 *      HW_CANSIM_S3ZERO, the only board that existed then. A receiver
 *      must check data_length_code >= 8 before reading it.
 */

/* Hardware ids for ANNOUNCE[7]. The OTA manifest is keyed by device and
 * then by the short name md_hw_key() returns, e.g.
 *   {"can_sim": {"s3zero": {build,url,md5}, ...}}
 * To add a board: a new id here (and HW_COUNT), its key in md_hw_key(),
 * `#define HW_ID` for its chip/board in can_sim.ino, and a variant in
 * tools/devices.json. Keep the keys short - they are part of the release
 * URL, which has to fit OTA_START_MAX_LEN (see MSG_OTA_START). */
#define HW_UNKNOWN        0
#define HW_CANSIM_S3ZERO  1   /* Waveshare ESP32-S3-Zero */
#define HW_CANSIM_C3      2   /* ESP32-C3 (DevKitM-1 style) */
#define HW_COUNT          3   /* table size: highest id + 1 */

static inline const char *md_hw_key(uint8_t hw)
{
    switch (hw) {
    case HW_UNKNOWN:
    case HW_CANSIM_S3ZERO: return "s3zero";
    case HW_CANSIM_C3:     return "c3";
    default:               return NULL;
    }
}

#define MSG_ENGINE_NAME(e)       (0x130u + (e))
/* Human-readable engine name (e.g. "Volvo Penta MD2030C"), chunked
 * since it doesn't fit one 8-byte frame. Sent every NAME_SEND_PERIOD_MS
 * by an enrolled engine controller; a display assembles chunks into a
 * name[MD_ENGINE_NAME_MAXLEN+1] buffer keyed by chunk_index - order/
 * loss-tolerant since every chunk repeats total_len, and a display can
 * just re-render once total_len bytes have been filled in.
 *  [0] total_len (whole name length in bytes, <= MD_ENGINE_NAME_MAXLEN)
 *  [1] chunk_index (0-based; byte offset into the name = chunk_index * NAME_CHUNK_BYTES)
 *  [2..7] up to NAME_CHUNK_BYTES (6) bytes of ASCII name text, no
 *         terminator - the final chunk is padded with 0x00 past total_len
 */
#define NAME_CHUNK_BYTES         6
#define MD_ENGINE_NAME_MAXLEN    24  /* 4 chunks; fits "Volvo Penta MD2030C" (20) */

/* ==================== OTA (remote update trigger) ====================
 * HELM -> engine e: hand it WiFi credentials + a firmware download URL,
 * chunked exactly like MSG_ENGINE_NAME above (same reasoning - doesn't
 * fit one 8-byte frame). Deliberately in the UNICAST+ENCRYPTED bucket
 * (see md_is_unicast_command() below) so a WiFi password gets the same
 * AES protection as the dead-man commands when routed over ESP-NOW; over
 * plain CAN it goes out unencrypted, an accepted tradeoff for a CAN-only
 * device that has no other way to receive it. */
#define MSG_OTA_START(e)         (0x140u + (e))
/*  [0] total_len (of the whole concatenated payload, <= OTA_START_MAX_LEN)
 *  [1] chunk_index (0-based; byte offset = chunk_index * OTA_START_CHUNK_BYTES)
 *  [2..7] up to OTA_START_CHUNK_BYTES (6) bytes of payload, no padding
 *         except the final chunk past total_len
 * payload (once fully reassembled) =
 * "<ssid>\0<password>\0<url>\0<md5>\0", four NUL-terminated strings
 * concatenated - the device joins WiFi with ssid/password, then
 * HTTP(S)-downloads from url and flashes, verifying the written image
 * against md5 (32 lowercase hex chars, from the OTA manifest's own "md5"
 * field - see check_for_update_tick()/tools/release.sh) via HTTPUpdate's
 * built-in setMD5sum()/Update.end() check before it's trusted. md5 may
 * be an empty string (just the NUL) if the manifest didn't have one for
 * this entry - the receiver treats that as "no checksum available",
 * not as a failure, and updates without verifying. */
#define OTA_START_CHUNK_BYTES     6
#define OTA_START_MAX_LEN         250   /* fits ssid(32)+pass(64)+md5(32)+url(118)+4 NULs exactly */

/* engine e -> everyone (broadcast, like all of an engine controller's
 * other outbound traffic - this board never unicasts anything, see
 * bus_send()'s comment). Sent once the full MSG_OTA_START burst has been
 * reassembled and validated, before actually attempting the WiFi
 * join/download - lets HELM's UI show "acknowledged, updating..."
 * instead of guessing whether the command even arrived. */
#define MSG_OTA_ACK(e)            (0x150u + (e))
/*  [0] status: OTA_ACK_OK
 *  [1..7] reserved (0)
 */
#define OTA_ACK_OK  1

/* ==================== WiFi join (debug/admin trigger) ====================
 * HELM -> engine e: hand it WiFi credentials and tell it to join and
 * PERSIST that join - same effect as can_sim's own serial WIFI:<ssid>,
 * <password> command (saves to NVS, reboots to connect, survives future
 * reboots), just triggered wirelessly from HELM's debug page instead of
 * over a USB cable. Chunked exactly like MSG_ENGINE_NAME/MSG_OTA_START
 * above. Deliberately in the UNICAST+ENCRYPTED bucket (see md_is_unicast_
 * command() below), same reasoning as MSG_OTA_START - a WiFi password
 * deserves AES protection when routed over ESP-NOW. */
#define MSG_WIFI_JOIN(e)          (0x160u + (e))
/*  [0] total_len (of the whole concatenated payload, <= WIFI_JOIN_MAX_LEN)
 *  [1] chunk_index (0-based; byte offset = chunk_index * WIFI_JOIN_CHUNK_BYTES)
 *  [2..7] up to WIFI_JOIN_CHUNK_BYTES (6) bytes of payload, no padding
 *         except the final chunk past total_len
 * payload (once fully reassembled) = "<ssid>\0<password>\0", two
 * NUL-terminated strings concatenated (password may be empty, just its
 * own NUL, for an open AP). */
#define WIFI_JOIN_CHUNK_BYTES     6
#define WIFI_JOIN_MAX_LEN         98    /* fits ssid(32)+pass(64)+2 NULs exactly */

/* Reverse ID -> engine index helpers (return <0 if not in range) */
static inline int md_engine_from_telem(uint32_t id)
{
    if (id >= 0x100u && id < 0x100u + MD_MAX_ENGINES) return (int)(id - 0x100u);
    return -1;
}
static inline int md_engine_from_hours(uint32_t id)
{
    if (id >= 0x110u && id < 0x110u + MD_MAX_ENGINES) return (int)(id - 0x110u);
    return -1;
}
static inline int md_engine_from_announce(uint32_t id)
{
    if (id >= 0x120u && id < 0x120u + MD_MAX_ENGINES) return (int)(id - 0x120u);
    return -1;
}
static inline int md_engine_from_name(uint32_t id)
{
    if (id >= 0x130u && id < 0x130u + MD_MAX_ENGINES) return (int)(id - 0x130u);
    return -1;
}

/* true + *e_out set if id is one of the unicast dead-man/ignition/OTA/
 * WiFi-join commands (IGNITION, GLOW_HELD, START_HELD, STOP, OTA_START,
 * WIFI_JOIN) - deliberately NOT ALARM_SILENCE, which shares this same
 * BASE+e shape but is overheard by a standalone alarmer the same way
 * telemetry is, so it must stay broadcast rather than routed
 * point-to-point over an alternate transport (see espnow_bus.h's routing
 * notes). OTA_START and WIFI_JOIN joined this bucket specifically so a
 * WiFi password gets AES protection when routed over ESP-NOW - see their
 * own comments above. */
static inline bool md_is_unicast_command(uint32_t id, int *e_out)
{
    uint32_t bases[6] = { 0x080u, 0x090u, 0x0A0u, 0x0C0u, 0x140u, 0x160u };
    for (int i = 0; i < 6; i++) {
        if (id >= bases[i] && id < bases[i] + MD_MAX_ENGINES) {
            *e_out = (int)(id - bases[i]);
            return true;
        }
    }
    return false;
}

/* Heartbeats (displays only): [0]=node id, [1]=uptime s & 0xFF */
#define MSG_HB_HELM           0x700
#define MSG_HB_CYD            0x701

/* ==================== telemetry flag bits ==================== */

#define TFLAG_TEMP_ALARM      (1 << 0)
#define TFLAG_PRESS_ALARM     (1 << 1)
#define TFLAG_IGNITION_ON     (1 << 2)
#define TFLAG_GLOW_ACTIVE     (1 << 3)
#define TFLAG_CRANK_ACTIVE    (1 << 4)
#define TFLAG_STOP_ACTIVE     (1 << 5)   /* CTRL confirms the stop relay is cutting */
#define TFLAG_CHARGE_ALARM    (1 << 6)   /* alternator/D+ not charging while running */
#define TFLAG_WATER_ALARM     (1 << 7)   /* water-in-fuel sensor tripped */

/* ==================== capability bits (ANNOUNCE) ==================== */

#define CAP_RPM               (1 << 0)
#define CAP_COOLANT_TEMP      (1 << 1)
#define CAP_OIL_PRESS         (1 << 2)
#define CAP_TEMP_ALARM        (1 << 3)
#define CAP_PRESS_ALARM       (1 << 4)
#define CAP_GLOW              (1 << 5)
#define CAP_START             (1 << 6)
#define CAP_IGNITION          (1 << 7)
#define CAP_HOURS             (1 << 8)
#define CAP_AUDIO             (1 << 9)
#define CAP_STOP              (1 << 10)  /* electric stop exists on THIS engine's
                                           * control board - never set for the
                                           * MD2030 or any mechanical-only engine */
#define CAP_CHARGE_ALARM      (1 << 11)  /* alternator/D+ charge sensing wired up */
#define CAP_WATER_ALARM       (1 << 12)  /* water-in-fuel sensor wired up */

#define CAP_ALL_DEFAULT       (CAP_RPM | CAP_COOLANT_TEMP | CAP_OIL_PRESS | \
                               CAP_TEMP_ALARM | CAP_PRESS_ALARM | CAP_GLOW | \
                               CAP_START | CAP_IGNITION | CAP_HOURS)

/* ==================== engine types ==================== */

#define ENGTYPE_UNKNOWN       0
#define ENGTYPE_MD2030        1
#define ENGTYPE_MD2040        2
#define ENGTYPE_GENERIC_DIESEL 3
#define ENGTYPE_GENERIC_PETROL 4
#define ENGTYPE_MD2030C       5
#define ENGTYPE_YANMAR_2YM15  6
#define ENGTYPE_YANMAR_4JH80  7
#define ENGTYPE_NANNI         8

/* ==================== pack/unpack helpers ==================== */

static inline void md_pack_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
}

static inline uint16_t md_unpack_u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static inline void md_pack_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t md_unpack_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline bool md_mac_eq(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
    return true;
}

#endif /* CAN_PROTOCOL_H */
