/**
 * wired_bus.h - direct-wire supplementary transport
 *
 * COPY of ../engine_display/wired_bus.h (the canonical one). Arduino
 * sketches can't share files across folders, so this copy must be
 * updated by hand any time the original changes. Keep both copies'
 * content identical, same discipline as can_protocol.h/espnow_pairing.h/
 * espnow_bus.h.
 *
 * For when the display and one engine driver (or can_sim) are physically
 * right next to each other and you'd rather run 3 wires between them
 * than fit a CAN transceiver or rely on ESP-NOW - e.g. a bench setup, or
 * a CTRL board mounted right behind the panel. Strictly POINT-TO-POINT:
 * exactly two boards ever share one of these links, so there's no bus
 * arbitration to worry about (unlike CAN's multi-drop bus) - this is a
 * much simpler plain UART link, not a second CAN implementation.
 *
 * Wiring: 3 wires - TX, RX (cross-connected: this board's TX to the
 * other's RX, and vice versa), and a common GND. No transceiver chip, no
 * termination resistors, no differential signaling - straight 3.3V UART
 * logic levels, both boards being ESP32 family. See each sketch's
 * PIN_WIRED_TX/PIN_WIRED_RX for the actual GPIOs.
 *
 * Framing (WIRED_FRAME_LEN bytes/frame) mirrors a CAN/twai_message_t's
 * shape exactly, so it carries the SAME can_protocol.h messages
 * completely unchanged - the identical synthetic-twai_message_t trick
 * espnow_bus.h's envelope already uses for ESP-NOW:
 *   [0]     SYNC byte (WIRED_SYNC_BYTE) - lets a receiver that just
 *           powered up, or lost sync after a dropped/corrupted byte,
 *           resynchronize at the next frame boundary instead of reading
 *           garbage forever. Framing is fixed-length-after-sync, not a
 *           byte-stuffing scheme - simple and sufficient for a short,
 *           dedicated point-to-point wire.
 *   [1..2]  identifier, u16 LE (an 11-bit CAN-style ID, reused verbatim -
 *           every existing message in can_protocol.h just works)
 *   [3]     data_length_code (0-8)
 *   [4..11] data[8] (always sent; bytes past dlc are 0)
 *   [12]    checksum: XOR of bytes [1..11]
 *
 * No encryption (unlike ESP-NOW's unicast+encrypted bucket) - a direct
 * wire is inherently more physically secure than radio, same reasoning
 * CAN itself already goes out in plaintext for. No ACK/retry at this
 * layer either, again same as CAN - the protocol's own dead-man/ENROLL
 * resend loops are the real reliability backstop, not this transport;
 * a corrupted frame just fails its checksum and gets silently dropped,
 * self-recovering at the next successfully-received frame.
 *
 * Always-on, not a user-toggleable feature: both ends simply run
 * wired_setup()/wired_bus_tick() unconditionally (mirroring can_setup()'s
 * own "always try, note whether it worked" shape) - a floating/
 * unconnected RX pin reading noise just fails checksums harmlessly, the
 * same tolerance already accepted for CAN with no transceiver attached
 * (see engine_display/CLAUDE.md's CAN hard-constraint #5).
 *
 * WIRED_ACTIVE_TIMEOUT_MS is the shared "is a wired peer actually there
 * right now" window (a good frame heard within this long) - used by
 * can_sim.ino to know when to stop bothering with ESP-NOW pairing/
 * channel-hunting entirely (see that file's pairing_requester_tick()):
 * when two boards are directly wired together, there's no reason to
 * spend airtime hunting for a wireless peer they don't need. Detection
 * is near-immediate in practice - HELM's own 1Hz heartbeat and this
 * board's ENROLL_REQUEST retries already go out over the wire the
 * moment both boards are up (bus_send() dual/triple-emits broadcasts
 * onto every live transport), so this timeout is really just "how long
 * to wait before concluding the wire went away," not a startup delay.
 */

#ifndef WIRED_BUS_H
#define WIRED_BUS_H

#include <stdint.h>

#define WIRED_BAUD        250000   /* matches CAN's own bitrate - arbitrary
                                     * but a sensible, well-supported UART
                                     * speed at typical short bench-wiring
                                     * lengths */
#define WIRED_SYNC_BYTE   0xAA
#define WIRED_FRAME_LEN   13
#define WIRED_ACTIVE_TIMEOUT_MS   3000

#endif /* WIRED_BUS_H */
