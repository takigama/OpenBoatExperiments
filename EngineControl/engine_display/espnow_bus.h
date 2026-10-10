/**
 * espnow_bus.h - carries a real can_protocol.h message over ESP-NOW, signed.
 *
 * Byte-identical copies live in engine_display/ and can_sim/ (Arduino sketches can't share files
 * across folders) - compare them with cmp if unsure which changed last.
 *
 * Deliberately does NOT redesign can_protocol.h's message catalog - this just tunnels the exact same
 * (id, data, len) triple a CAN frame already carries, so every existing dispatch/handler function keeps
 * working unchanged regardless of which wire actually delivered the frame.
 *
 * Every frame is SIGNED (fleet_security.h): `tag` is the first 8 bytes of an HMAC over the sender's radio
 * address and every field before it, keyed by the shared secret. `session` is a random number chosen
 * when the sender boots and `counter` goes up by one per frame, so a recorded frame cannot be replayed.
 * ESP-NOW cannot encrypt a broadcast, so a broadcast frame is authenticated but readable; a frame
 * addressed to one board is also encrypted by the radio with that pair's key.
 */
#pragma once

#define ESPNOW_MSG_BUS_FRAME 3   /* next type value after espnow_pairing.h's 4/5 are separate: see there */

/* Packed, fixed size (30 bytes - no other ESP-NOW message has that length). can_id is uint32_t to match
 * twai_message_t.identifier's field width, even though today's protocol only uses 11 bits of it. */
typedef struct __attribute__((packed)) {
    uint8_t  type;       /* ESPNOW_MSG_BUS_FRAME */
    uint32_t session;    /* random per boot of the sender */
    uint32_t counter;    /* +1 per frame sent */
    uint32_t can_id;     /* the can_protocol.h message ID */
    uint8_t  dlc;        /* 0-8, mirrors data_length_code */
    uint8_t  data[8];    /* zero-padded past dlc */
    uint8_t  tag[8];     /* HMAC-SHA256(bcast key, sender mac || fields above), first 8 bytes */
} espnow_bus_frame_t;
