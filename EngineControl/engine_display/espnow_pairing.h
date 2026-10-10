/**
 * espnow_pairing.h - ESP-NOW pairing infrastructure shared constants.
 *
 * CANONICAL COPY. Also duplicated (by hand) at ../can_sim/espnow_pairing.h -
 * Arduino sketches can't share files across folders, so that copy must be
 * updated to match any time this one changes. Keep both copies' content
 * identical; diff them if unsure which changed last.
 *
 * Infrastructure only (as of this writing): this header defines the
 * pairing handshake (who's allowed to talk to whom over ESP-NOW), NOT any
 * actual engine command/telemetry traffic - that still goes over CAN via
 * can_protocol.h. A paired peer is just a MAC address a board has agreed
 * to trust; nothing about glow/start/telemetry routing changes yet.
 */
#pragma once

/**
 * Must be identical across every board on YOUR boat (HELM, can_sim/CTRL,
 * alarmer, ...) - a pairing request whose fleet_id doesn't match is
 * ignored, even during an open pairing window. This is what keeps a
 * neighboring boat's identical firmware (e.g. at a marina) from being
 * able to pair with - or accidentally get paired by - your boards.
 * EDIT to a value unique to your boat before flashing anything.
 */
#define FLEET_ID  0xDEADBEEFUL

#define PAIR_MSG_REQUEST   1   /* requester -> broadcast: "here's my MAC, pair me" */
#define PAIR_MSG_ACK       2   /* acceptor -> unicast: "you're paired, here's your key" */

#define ESPNOW_LMK_LEN 16   /* matches ESP-IDF's ESP_NOW_KEY_LEN */

/* Raw ESP-NOW payload. Packed: this goes over the air byte-for-byte, no
 * padding. node_type reuses NODE_TYPE_* from can_protocol.h. lmk is only
 * meaningful on PAIR_MSG_ACK - see the "AES pairing" note below. */
typedef struct __attribute__((packed)) {
    uint8_t  type;        /* PAIR_MSG_REQUEST or PAIR_MSG_ACK */
    uint32_t fleet_id;
    uint8_t  mac[6];      /* sender's own MAC */
    uint8_t  node_type;   /* NODE_TYPE_* - meaningful on PAIR_MSG_REQUEST only */
    uint8_t  lmk[ESPNOW_LMK_LEN];   /* meaningful on PAIR_MSG_ACK only */
} espnow_pair_msg_t;

/**
 * AES pairing: HELM generates a fresh random 16-byte key (the peer's
 * "Local Master Key") the moment it accepts a new MAC, and sends it back
 * in the PAIR_MSG_ACK. From then on, both sides register that peer with
 * encrypt=true + this lmk, so ESP-NOW transparently encrypts/decrypts
 * traffic between them - a message claiming to be from a paired MAC but
 * not actually encrypted with the right key gets dropped by the radio,
 * not just ignored by application logic.
 *
 * Proportionate, not bulletproof: the ACK itself (which carries the LMK)
 * is unencrypted, since neither side has the key yet at that exact
 * instant - the bootstrap moment is inherently plaintext. This raises
 * the bar from "anyone who's read this protocol can spoof a paired
 * peer's traffic" to "an attacker has to be actively sniffing during the
 * few-second window right when you deliberately trigger a new pairing" -
 * a real improvement, not a cryptographic guarantee against a
 * sufficiently motivated/positioned attacker.
 *
 * ESPNOW_PMK is the device-wide Primary Master Key, set once via
 * esp_now_set_pmk() - used locally to protect each peer's LMK when it's
 * loaded into the radio's crypto engine. It does NOT need to match
 * between devices the way FLEET_ID and each peer's LMK do, but keeping
 * it consistent across your own boards costs nothing. EDIT this to your
 * own 16 bytes alongside FLEET_ID before flashing anything - the value
 * below is a placeholder, not something to ship as-is.
 */
static const uint8_t ESPNOW_PMK[ESPNOW_LMK_LEN] = {
    0x4D, 0x44, 0x32, 0x30, 0x33, 0x30, 0x2D, 0x50,
    0x4D, 0x4B, 0x2D, 0x45, 0x44, 0x49, 0x54, 0x21
};

/* requester retry timing is channel-hunt-driven now (can_sim.ino's
 * pairing_requester_tick() - CHANNEL_DWELL_MS/LOST_CONTACT_MS, defined
 * locally there since only can_sim is a requester today), not a fixed
 * fast/slow broadcast cadence - a requester with no WiFi credentials of
 * its own can no longer assume HELM is on a fixed known channel (HELM
 * might have joined a real network on any channel), so it sweeps
 * candidate channels instead of just retrying in place. See that
 * function's comment for the full mechanism.
 *
 * acceptor (HELM) timing: how long "Wireless Pairing" stays open once
 * tapped. Outside this window, unrecognized MACs are silently ignored -
 * only a MAC already on the allowlist (from a previous successful
 * pairing) is ever accepted. */
#define PAIR_WINDOW_MS      60000UL

static const uint8_t ESPNOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ESP-NOW is channel-locked to whatever WiFi is currently doing - both
 * peers must be on the same channel or packets go out into the void with
 * no error (esp_now_send() reports OK regardless of whether anyone's
 * listening). A board with no WiFi credentials configured (or a failed
 * STA join) deliberately does NOT fall back to broadcasting its own AP -
 * that's unrequested WiFi activity on a bench unit that's often run with
 * no WiFi at all - it stays in WIFI_STA mode with no active connection
 * and pins the radio to this fixed channel via esp_wifi_set_channel(),
 * purely so ESP-NOW still works. Every board doing this lands on the
 * same channel and can pair with zero WiFi configuration anywhere. Only
 * matters for the no-WiFi case - a board that actually joins a real
 * network uses whatever channel that router assigned instead, which is
 * why BOTH boards still need to join the SAME real network for pairing
 * to work once you do configure WiFi on both. */
#define ESPNOW_AP_FALLBACK_CHANNEL 1
