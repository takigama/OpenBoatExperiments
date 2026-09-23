#pragma once

#include <Arduino.h>

#include "seatalk_decode.h"

// Bridges the board's NMEA2000/CAN interface (GPIO7 TX, GPIO10 RX, via an
// external transceiver on header J4 - see the hardware project) to the
// same decoded-event pipeline SeaTalk/MQTT/SignalK already share, using
// ttlappalainen's NMEA2000 library (the de facto standard stack for
// custom ESP32 N2K devices) for PGN encode/decode, at the
// NMEA2000-mandated 250kbit/s. The CAN bus driver itself is a small
// custom adapter (n2k_twai_driver.h) built on ESP-IDF's TWAI API, not
// ttlappalainen's own NMEA2000_esp32 companion library - that library
// only supports the classic ESP32's CAN peripheral (predates the C3).
//
// Two independent directions, deliberately not looped into each other:
//  - publishDecoded(): called by RouteConfig for any source (SeaTalk,
//    MQTT, SignalK) whose routing matrix allows it - re-encodes the
//    event as the matching N2K PGN and sends it, so N2K plotters/
//    instruments can see that data.
//  - tick()'s internal message handler: incoming N2K PGNs we understand
//    get parsed into the same SeatalkDecode::Event model and passed to
//    RouteConfig::relay(), which always forwards to MQTT/SignalK and -
//    per the web UI's matrix - optionally re-encodes onto the physical
//    SeaTalk bus too (see SeatalkEncode). Never re-sent back out as N2K
//    (would just echo a device's own data back at it).
//
// UNTESTED: there is no CAN transceiver or NMEA2000 bus available to
// verify any of this against, unlike every other module in this project
// (which all got real-hardware or real-server verification before being
// considered done). This compiles and follows the library's documented
// API and PGN definitions, but the actual on-wire behavior - bit timing,
// address claiming against other real devices, whether a real instrument
// accepts these PGNs - has not been confirmed.
namespace N2kManager {

void begin();

// Call every loop() iteration - pumps CAN RX/TX and dispatches incoming
// PGNs to the internal message handler.
void tick();

bool isOpen();

// Re-encodes one decoded SeaTalk event as the matching N2K PGN and sends
// it. Latitude/Longitude, Heading/MagneticVariation, and GnssDate/
// GnssTime are each cached and combined the same way SignalKManager
// combines lat/lon, since SeaTalk delivers them as separate datagrams but
// their N2K PGNs want them together (position -> PGN 129025, heading+
// variation -> PGN 127250 rather than a separate PGN 127258, date+time ->
// PGN 126992's System Time, which is also where GnssDate/GnssTime's own
// days-since-1970 timestamp comes from on the receive side - see
// daysSince1970()/its inverse in the .cpp). Not every SeatalkDecode::Type
// has a PGN mapped (trip/total log, satellite count are deliberately
// skipped - see .cpp) so this silently no-ops for those rather than
// guessing at an encoding.
void publishDecoded(const SeatalkDecode::Event &ev);

}  // namespace N2kManager
