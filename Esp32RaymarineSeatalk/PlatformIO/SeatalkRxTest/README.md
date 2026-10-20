# SeatalkRxTest

The least firmware that can answer one question: with nothing else running (no WiFi, no MQTT, no web, no OTA,
no libraries), does the SeaTalk receive on the ESP32Seatalk board still lose or mangle messages?

It listens on GPIO4 with two receivers on the same edges, so any difference between them is the method, not the bus:

- **A "timer"** - how ESP32Seatalk used to receive: an edge interrupt starts an `esp_timer`, a timer callback
  samples each bit and schedules the next sample *relative to when it ran*, so lateness adds up over the byte.
- **D "edges"** - the interrupt only records the time of every edge; the bits are worked out afterwards from those
  times (what ESP32Seatalk uses now).

It prints one line per datagram (`seconds receiver bytes`) and a summary every 30 s (115200, USB). It transmits nothing.
Flash it over USB; it is not released over the air. Restore ESP32Seatalk the same way (write the app at `0x10000` and
blank `0xe000..0x10000` so the board boots the app slot; leave the rest alone and the saved WiFi survives).

## What it showed (2026-10-21, ST50 Tridata on the bus, 220 s, board doing nothing else)

| | D (edges) | A (timer) |
|---|---|---|
| datagrams | 259 | 252 |
| mangled | 0 | 1 (`20 90 00` for `20 41 00 00`) |
| stop-bit errors | 0 | 1 |

Receiver A's stop-bit sample ran 50-100 us late on every byte (the limit is half a bit, 104 us), so even an idle
board lost about 3%; with WiFi / MQTT competing it was about a third. The circuit and the bus were clean (D: no
glitches, no framing errors, a steady 2.56 s cadence per message). ESP32Seatalk now uses method D.
