/**
 * espnow_pairing.h - how a board joins the ESP-NOW network, and shared radio constants.
 *
 * Byte-identical copies live in engine_display/ and can_sim/ (Arduino sketches can't share files
 * across folders) - compare them with cmp if unsure which changed last.
 *
 * There is no pairing window and no key on the air any more. Every board that belongs to the network
 * holds the same SECRET (a passphrase typed in once, kept in its own flash - see fleet_security.h) and
 * joins at any time by proving it knows it:
 *
 *   joining board --- ESPNOW_MSG_HELLO (broadcast) ------------> HELM
 *                       "I am <mac>, a <node_type>", tagged with a value only a holder of the secret
 *                       can compute
 *   joining board <-- ESPNOW_MSG_HELLO_ACK (broadcast) --------- HELM
 *                       "I am <helm mac>, answering <your mac>", tagged the same way
 *
 * Both sides then derive the key for their pair from the secret and the two radio addresses
 * (fleet_security.h: fsec_lmk) and register each other as encrypted ESP-NOW peers. Neither message
 * contains a key. A board without the secret cannot make a valid tag, so it cannot join, and a stranger
 * cannot impersonate the HELM to a board.
 *
 * Message types share one byte at the start of every ESP-NOW payload and are told apart by length too:
 * 3 = a signed bus frame (espnow_bus.h, 30 bytes), 4 = HELLO (16), 5 = HELLO_ACK (22).
 */
#pragma once

#define ESPNOW_LMK_LEN 16   /* matches ESP-IDF's ESP_NOW_KEY_LEN */

#define ESPNOW_MSG_HELLO      4
#define ESPNOW_MSG_HELLO_ACK  5

/* node_type reuses NODE_TYPE_* from can_protocol.h; tag = first 8 bytes of
 * HMAC-SHA256(hello key, type || mac || node_type [|| peer_mac]) - see fsec_hello_tag() */
typedef struct __attribute__((packed)) {
    uint8_t type;           /* ESPNOW_MSG_HELLO */
    uint8_t mac[6];         /* the joining board's own radio address */
    uint8_t node_type;
    uint8_t tag[8];
} espnow_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t type;           /* ESPNOW_MSG_HELLO_ACK */
    uint8_t mac[6];         /* the HELM's radio address */
    uint8_t node_type;      /* NODE_TYPE_HELM */
    uint8_t peer_mac[6];    /* the board being answered - the ACK is only for that board */
    uint8_t tag[8];
} espnow_hello_ack_t;

static const uint8_t ESPNOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ESP-NOW is channel-locked to whatever WiFi is currently doing - both peers must be on the same
 * channel or packets go out into the void with no error (esp_now_send() reports OK regardless of
 * whether anyone's listening). A board with no WiFi credentials configured (or a failed STA join)
 * stays in WIFI_STA mode with no active connection and pins the radio to a channel via
 * esp_wifi_set_channel(), purely so ESP-NOW still works. Boards that are not on a real network hunt
 * for the HELM's channel (can_sim.ino's pairing_requester_tick(), engine_display.ino's
 * cyd_pairing_tick()); this is just where they start. */
#define ESPNOW_AP_FALLBACK_CHANNEL 1
