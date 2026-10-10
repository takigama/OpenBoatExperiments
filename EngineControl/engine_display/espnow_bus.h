/**
 * espnow_bus.h - carries a real can_protocol.h message over ESP-NOW.
 *
 * CANONICAL COPY. Also duplicated (by hand) at ../can_sim/espnow_bus.h -
 * Arduino sketches can't share files across folders, so that copy must be
 * updated to match any time this one changes. Keep both copies' content
 * identical; diff them if unsure which changed last.
 *
 * Deliberately does NOT redesign can_protocol.h's message catalog - this
 * just tunnels the exact same (id, data, len) triple a CAN frame already
 * carries, so every existing dispatch/handler function keeps working
 * unchanged regardless of which wire actually delivered the frame.
 *
 * Requires espnow_pairing.h (FLEET_ID, ESPNOW_BROADCAST_MAC, and the
 * type-byte namespace PAIR_MSG_REQUEST=1/PAIR_MSG_ACK=2 already occupy -
 * new type values must be coordinated in that file, not duplicated here).
 */
#pragma once

#define ESPNOW_MSG_BUS_FRAME 3   /* next free type value after espnow_pairing.h's 1/2 */

/* Packed, fixed size - mirrors twai_message_t's essential fields
 * (identifier/data_length_code/data), not the whole struct (rtr/ss/extd
 * are CAN-controller-specific and never read by any dispatch/handler
 * function this frame ends up feeding). can_id is uint32_t to match
 * twai_message_t.identifier's actual field width, even though today's
 * protocol only uses 11 bits of it. sizeof() is 18 bytes -
 * sizeof(espnow_pair_msg_t) is 28, so the two never collide by length
 * alone; the leading type byte is the defense-in-depth discriminant. */
typedef struct __attribute__((packed)) {
    uint8_t  type;       /* ESPNOW_MSG_BUS_FRAME */
    uint32_t fleet_id;
    uint32_t can_id;     /* the can_protocol.h message ID */
    uint8_t  dlc;        /* 0-8, mirrors data_length_code */
    uint8_t  data[8];    /* zero-padded past dlc */
} espnow_bus_frame_t;
