/**
 * fleet_security.h - the shared secret behind the ESP-NOW network.
 *
 * CANONICAL COPY. Also duplicated (by hand) at ../can_sim/fleet_security.h - Arduino sketches can't
 * share files across folders, so keep both copies byte-identical (cmp them if unsure).
 * Needs espnow_bus.h (the frame layout) included first.
 *
 * The idea: every board holds the SAME secret, typed in once as a passphrase (serial `KEY <phrase>`
 * or the setup web page) and kept in the board's own flash. NOTHING secret is compiled into the
 * firmware - the release images are public, so a compiled-in key would be public too.
 *
 *   passphrase --PBKDF2-HMAC-SHA256 (10000 rounds, fixed public salt)--> 32-byte master key
 *   master --HMAC--> pmk          ESP-NOW's network key (esp_now_set_pmk)
 *   master --HMAC--> hello key    proves a board knows the secret when it asks to join
 *   master --HMAC--> bcast key    signs every bus message (tag + counter, see the frame layout)
 *   master + both MACs --HMAC--> lmk   the AES key for ONE pair of boards (esp_now_add_peer)
 *
 * Because the per-pair key (lmk) is DERIVED from the secret and the two radio addresses, no key is ever
 * sent over the air - there is no pairing handshake to eavesdrop and no pairing window. A board that
 * knows the secret can join at any time; one that doesn't can neither get a valid reply, nor produce a
 * valid message tag, nor decrypt anything addressed to a member.
 *
 * Signed (not encrypted) broadcasts: ESP-NOW cannot encrypt a broadcast, so engine data that goes out to
 * everyone carries an 8-byte tag (HMAC over the sender's radio address and the whole frame), a random
 * per-boot session number and a counter. A forged frame has no valid tag; a recorded one is rejected
 * by the counter. Limit: a receiver that has never heard a given sender will accept the first valid
 * frame it sees from it, which could be an old recording (only that one, and only until the real
 * sender's counter takes over).
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include "mbedtls/md.h"

#define FSEC_MIN_PASSPHRASE          12
#define FSEC_PBKDF2_ITERS            10000UL
#define FSEC_REPLAY_SLOTS            8
#define FSEC_NEW_SESSION_MAX_COUNTER 100   /* a restarted sender starts counting again from 1 */

static uint8_t  g_fsec_master[32];
static uint8_t  g_fsec_hello_key[32];
static uint8_t  g_fsec_bcast_key[32];
static bool     g_fsec_have_key = false;
static uint32_t g_fsec_session = 0;   /* random per boot, carried in every frame */
static uint32_t g_fsec_counter = 0;

static void fsec_hmac(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, msg, mlen, out);
}

static void fsec_my_mac(uint8_t mac[6])
{
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

/* HMAC(master, label || extra) */
static void fsec_subkey(const char *label, const uint8_t *extra, size_t elen, uint8_t out[32])
{
    uint8_t buf[48];
    size_t n = strlen(label);
    if (n > 16) n = 16;
    memcpy(buf, label, n);
    if (elen > sizeof(buf) - n) elen = sizeof(buf) - n;
    if (elen) memcpy(buf + n, extra, elen);
    fsec_hmac(g_fsec_master, sizeof(g_fsec_master), buf, n + elen, out);
}

static void fsec_derive_all(void)
{
    fsec_subkey("hello", NULL, 0, g_fsec_hello_key);
    fsec_subkey("bcast", NULL, 0, g_fsec_bcast_key);
    g_fsec_session = esp_random();
    g_fsec_counter = 0;
}

static void fsec_pmk(uint8_t out[16])
{
    uint8_t k[32];
    fsec_subkey("pmk", NULL, 0, k);
    memcpy(out, k, 16);
}

/* the AES key for the pair (a, b): symmetric - both boards compute the same value */
static void fsec_lmk(const uint8_t *a, const uint8_t *b, uint8_t out[16])
{
    uint8_t pair[12];
    if (memcmp(a, b, 6) <= 0) { memcpy(pair, a, 6); memcpy(pair + 6, b, 6); }
    else                      { memcpy(pair, b, 6); memcpy(pair + 6, a, 6); }
    uint8_t k[32];
    fsec_subkey("lmk", pair, sizeof(pair), k);
    memcpy(out, k, 16);
}

/* a short code that is the same on every board holding the same secret - for comparing boards
 * without ever showing the secret */
static uint32_t fsec_fingerprint(void)
{
    uint8_t k[32];
    fsec_subkey("fp", NULL, 0, k);
    return ((uint32_t)k[0] << 24) | ((uint32_t)k[1] << 16) | ((uint32_t)k[2] << 8) | k[3];
}

static void fsec_pbkdf2(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                        uint32_t iters, uint8_t out[32])
{
    uint8_t first[64 + 4], u[32], t[32];
    if (slen > 64) slen = 64;
    memcpy(first, salt, slen);
    first[slen] = 0; first[slen + 1] = 0; first[slen + 2] = 0; first[slen + 3] = 1;   /* block index 1 */
    fsec_hmac(pw, pwlen, first, slen + 4, u);
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        fsec_hmac(pw, pwlen, u, 32, u);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 32);
}

static void fsec_to_hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) snprintf(&out[i * 2], 3, "%02X", b[i]);
}

static bool fsec_from_hex(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char *end;
        out[i] = (uint8_t)strtoul(b, &end, 16);
        if (*end) return false;
    }
    return true;
}

/* load the stored master key (NVS "fkey"); call once at boot */
static void fsec_begin(Preferences &p, bool prefs_ok)
{
    g_fsec_have_key = false;
    if (!prefs_ok) return;
    String hex = p.getString("fkey", "");
    if (hex.length() == 64 && fsec_from_hex(hex.c_str(), g_fsec_master, 32)) {
        g_fsec_have_key = true;
        fsec_derive_all();
    }
}

/* turn a typed passphrase into the stored master key; false if it is too short or flash is unavailable */
static bool fsec_set_passphrase(Preferences &p, bool prefs_ok, const char *phrase)
{
    size_t n = strlen(phrase);
    if (!prefs_ok || n < FSEC_MIN_PASSPHRASE) return false;
    static const uint8_t salt[] = "EngineControl-ESPNOW-v1";
    fsec_pbkdf2((const uint8_t *)phrase, n, salt, sizeof(salt) - 1, FSEC_PBKDF2_ITERS, g_fsec_master);
    char hex[65];
    fsec_to_hex(g_fsec_master, 32, hex);
    p.putString("fkey", hex);
    g_fsec_have_key = true;
    fsec_derive_all();
    return true;
}

static void fsec_clear(Preferences &p, bool prefs_ok)
{
    if (prefs_ok) p.remove("fkey");
    memset(g_fsec_master, 0, sizeof(g_fsec_master));
    g_fsec_have_key = false;
}

/* ---------------------------------------------------------------- hello: "I know the secret"
 * A board that wants to join broadcasts a HELLO carrying a tag only a holder of the secret can make; the
 * HELM answers with a HELLO_ACK carrying its own tag (so the board knows the HELM is genuine too).
 * Both then derive the pair key locally. Layouts are in espnow_pairing.h. */
static void fsec_hello_tag(uint8_t type, const uint8_t *mac, uint8_t node_type, const uint8_t *peer_mac,
                           uint8_t out[8])
{
    uint8_t m[16], full[32];
    size_t n = 0;
    m[n++] = type;
    memcpy(m + n, mac, 6); n += 6;
    m[n++] = node_type;
    if (peer_mac) { memcpy(m + n, peer_mac, 6); n += 6; }
    fsec_hmac(g_fsec_hello_key, sizeof(g_fsec_hello_key), m, n, full);
    memcpy(out, full, 8);
}

static bool fsec_tag_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* ---------------------------------------------------------------- signed bus frames */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint32_t session;
    uint32_t last_counter;
} fsec_replay_t;
static fsec_replay_t g_fsec_replay[FSEC_REPLAY_SLOTS];

#ifdef ESPNOW_MSG_BUS_FRAME
static void fsec_frame_tag(const espnow_bus_frame_t *f, const uint8_t *sender_mac, uint8_t out[8])
{
    uint8_t m[6 + sizeof(espnow_bus_frame_t)], full[32];
    memcpy(m, sender_mac, 6);
    memcpy(m + 6, f, offsetof(espnow_bus_frame_t, tag));
    fsec_hmac(g_fsec_bcast_key, sizeof(g_fsec_bcast_key), m, 6 + offsetof(espnow_bus_frame_t, tag), full);
    memcpy(out, full, 8);
}

/* build a signed frame for message `id` */
static void fsec_frame_build(espnow_bus_frame_t *f, uint32_t id, const uint8_t *data, uint8_t len)
{
    memset(f, 0, sizeof(*f));
    f->type = ESPNOW_MSG_BUS_FRAME;
    f->session = g_fsec_session;
    f->counter = ++g_fsec_counter;
    f->can_id = id;
    f->dlc = len > 8 ? 8 : len;
    if (f->dlc) memcpy(f->data, data, f->dlc);
    uint8_t mac[6];
    fsec_my_mac(mac);
    fsec_frame_tag(f, mac, f->tag);
}

/* true only for a frame from `sender_mac` that carries a valid tag and is not a replay */
static bool fsec_frame_verify(const espnow_bus_frame_t *f, const uint8_t *sender_mac)
{
    if (!g_fsec_have_key || f->type != ESPNOW_MSG_BUS_FRAME || f->dlc > 8) return false;
    uint8_t want[8];
    fsec_frame_tag(f, sender_mac, want);
    if (!fsec_tag_equal(want, f->tag, 8)) return false;

    fsec_replay_t *slot = NULL, *free_slot = NULL;
    for (int i = 0; i < FSEC_REPLAY_SLOTS; i++) {
        if (g_fsec_replay[i].used && memcmp(g_fsec_replay[i].mac, sender_mac, 6) == 0) { slot = &g_fsec_replay[i]; break; }
        if (!g_fsec_replay[i].used && !free_slot) free_slot = &g_fsec_replay[i];
    }
    if (!slot) {   /* first time we hear this sender */
        slot = free_slot ? free_slot : &g_fsec_replay[0];
        slot->used = true;
        memcpy(slot->mac, sender_mac, 6);
        slot->session = f->session;
        slot->last_counter = f->counter;
        return true;
    }
    if (f->session == slot->session) {
        if (f->counter <= slot->last_counter) return false;   /* replay or duplicate */
        slot->last_counter = f->counter;
        return true;
    }
    /* a different session: the sender restarted - only believe it if it is near the start of its count */
    if (f->counter > FSEC_NEW_SESSION_MAX_COUNTER) return false;
    slot->session = f->session;
    slot->last_counter = f->counter;
    return true;
}
#endif /* ESPNOW_MSG_BUS_FRAME */
