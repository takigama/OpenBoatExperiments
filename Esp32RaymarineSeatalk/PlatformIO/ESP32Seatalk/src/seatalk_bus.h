#pragma once

#include <Arduino.h>

// Low-level SeaTalk1 bus I/O: 4800 baud, 9-bit datagrams (start + 8 data
// bits LSB-first + a command/data marker bit + stop bit), single
// open-drain wire through the board's BSS138 level shifter (GPIO4).
//
// No hardware UART involved - the ESP32's UART peripheral only supports
// computed even/odd parity, not a fixed mark/space bit, so it can't
// produce or read SeaTalk's command-bit framing directly. TX is a direct
// port of the bit-banged approach from RaymarineST50Code/firmware/
// seatalk_bruteforce.c's st_bit()/st_sendByte() (same 208us-per-bit
// timing, same command-bit-before-stop-bit framing) - that TX primitive
// has a real positive confirmation behind it (the lamp-intensity command
// got a repeatable, visible response from a real instrument, including a
// CODE-locked one, using this exact bit timing - the *PIN-guessing*
// attempts built on top of it never worked, but that's a failure of what
// bytes were sent, not this underlying mechanism). RX is genuinely new:
// a GPIO edge interrupt catches the start bit, then a hardware timer
// samples each subsequent bit at its center - the standard technique for
// bit-banged async RX under an RTOS where a polling loop can't be trusted
// not to get delayed by WiFi/other tasks.
//
// No live SeaTalk bus is connected to the dev setup as of this writing -
// RX has only been validated via self-loopback (this device's own TX
// looping back into its own RX on the shared wire), not against real bus
// traffic from an actual instrument. Treat decoded values with that in
// mind until it's been run against a real bus.
namespace SeatalkBus {

struct Datagram {
    uint8_t bytes[18];  // command + attribute + up to 16 data bytes (SeaTalk's max)
    uint8_t length;      // total bytes including command+attribute
};

void begin(int gpioPin);

// Non-blocking - returns true and fills *out if a complete datagram has
// been assembled since the last call. Includes our own transmitted
// datagrams (see send()) - every device on the bus hears its own TX,
// that's inherent to a shared open-drain wire, not filtered out here.
// Callers that care whether a given datagram was self-sent vs. genuinely
// received from the bus need to track that themselves (e.g. by comparing
// against what they just called send() with).
bool poll(Datagram *out);

// Blocks for the datagram's duration (~2ms/byte at 4800 baud) - fine for
// the short, infrequent datagrams SeaTalk actually sends, not meant for
// bulk transfer. dataBytes excludes the command byte itself. RX stays
// armed throughout (see seatalk_bus.cpp) - the datagram will come back
// through poll() same as any bus traffic would.
void send(uint8_t command, const uint8_t *dataBytes, uint8_t dataLen);

}  // namespace SeatalkBus
