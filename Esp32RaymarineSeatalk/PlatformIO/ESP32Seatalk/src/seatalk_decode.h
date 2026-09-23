#pragma once

#include <Arduino.h>

#include "seatalk_bus.h"

// Decodes the priority telemetry object list (depth/speed/wind/temp/
// position/COG/SOG/time/date/heading/rudder/variation/log/sat-count) from
// raw SeaTalk datagrams, into SI units ready for the SignalK/MQTT layers.
//
// Every formula here is transcribed directly from Thomas Knauf's SeaTalk
// Technical Reference (thomasknauf.de/rap/seatalk2.htm, rev 3.23) rather
// than from memory - command bytes and bit-packing are exact quotes, not
// guesses. What's NOT included yet, deliberately deferred rather than
// guessed at: full autopilot state/target-course (cmd 84 - the simpler
// cmd 9C covers heading+rudder, which is what we actually need), and
// waypoint/XTE navigation (cmd 85) - both have bit-packing subtle enough
// that I'd want a real worked example or a live bus to check against
// before trusting them, and this module has had neither yet (see
// seatalk_bus.h - no live bus connected, self-loopback only so far).
namespace SeatalkDecode {

enum class Type {
    Depth,             // meters below transducer
    SpeedThroughWater,  // m/s
    TripLog,           // meters
    TotalLog,          // meters
    ApparentWindAngle,  // radians, positive = starboard of bow
    ApparentWindSpeed,  // m/s
    WaterTemperature,   // Kelvin
    // Lat/lon arrive as two independent datagrams (commands 50/51), never
    // atomically paired - decode() is stateless, so these come out as two
    // separate event types rather than one Position event. Combining them
    // into navigation.position is an application-layer concern (the
    // SignalK glue), not this module's.
    Latitude,          // decimal degrees, positive = North
    Longitude,         // decimal degrees, positive = East
    SpeedOverGround,    // m/s
    CourseOverGround,   // radians
    GnssTime,          // seconds since midnight UTC (combine with GnssDate for a full timestamp)
    GnssDate,          // year/month/day
    SatelliteCount,
    HeadingAndRudder,   // radians heading, radians rudder angle (see command 9C)
    MagneticVariation,  // radians, SignalK sign convention (positive = East)
};

struct Event {
    Type type;

    double value = 0;       // primary scalar (meters, m/s, radians, Kelvin - per Type)
    double value2 = 0;      // secondary scalar where two values travel together (lon, rudder, ...)
    bool flagA = false;     // per-Type meaning - see decode() call sites
    bool flagB = false;
    int year = 0, month = 0, day = 0;  // GnssDate only
};

// Returns false if dg's command byte isn't one we decode (yet), or the
// datagram's actual length doesn't match what the command expects (a
// framing error slipping through, or a command we don't fully understand).
bool decode(const SeatalkBus::Datagram &dg, Event *out);

// SignalK's own dot-path convention (e.g. "navigation.speedThroughWater") -
// the single source of truth both MqttManager (converting dots to slashes
// for its topic names) and SignalKManager (using them as delta paths
// directly) publish under, so the mapping only exists in one place.
// Returns nullptr for HeadingAndRudder and GnssDate, which each need
// their own multi-path handling at the publisher (one Event, two or more
// destination paths) rather than a single 1:1 mapping.
const char *canonicalPath(Type type);

// The reverse of canonicalPath() - given a dot-path, finds the Type it
// maps to (linear scan over canonicalPath()'s table; called rarely enough
// - once per inbound MQTT/SignalK message - that this isn't worth a
// lookup table). Returns false for paths with no 1:1 Type (HeadingAndRudder,
// GnssDate - see kPathHeadingMagnetic etc. below) or that canonicalPath()
// doesn't produce at all.
bool typeForCanonicalPath(const String &path, Type *out);

// HeadingAndRudder and GnssDate each need multiple/combined paths, so they
// fall outside canonicalPath()'s 1:1 mapping - these constants are the
// shared literal paths for them, so MqttManager and SignalKManager agree
// without duplicating the strings.
constexpr const char *kPathHeadingMagnetic = "navigation.headingMagnetic";
constexpr const char *kPathRudderAngle = "steering.rudderAngle";
constexpr const char *kPathDatetimeDate = "navigation.datetime.date";

}  // namespace SeatalkDecode
