#pragma once

#include "seatalk_decode.h"

// Encodes values back onto the physical SeaTalk bus - the inverse of
// seatalk_decode.cpp's formulas. The per-object functions take
// SeaTalk-native units (degrees/knots/feet/Celsius) exactly as
// demo_mode.cpp's own simulated/manual state already does, so it can call
// these directly with no unit conversion at the call site.
// encodeAndSend() is the SI-unit entry point for real decoded data
// bridged in from CAN/MQTT/SignalK (see RouteConfig) - it does the SI ->
// SeaTalk-native conversion (the inverse of seatalk_decode.cpp's
// constants) before calling the matching function below.
namespace SeatalkEncode {

void sendDepth(double ft);
void sendSpeedThroughWater(double kn);
void sendSpeedOverGround(double kn);
void sendApparentWindAngle(double deg);
void sendApparentWindSpeed(double kn);
void sendWaterTemperature(double celsius);
void sendPosition(double lat, double lon);  // decimal degrees, no conversion needed
void sendCourseOverGround(double cog);      // degrees
void sendHeadingAndRudder(double headingDeg, double rudderDeg);

// Latitude/Longitude are cached and combined into one sendPosition() call
// once both are known - SeaTalk needs them together as two datagrams, but
// they arrive as separate Events (same reasoning as SignalKManager's and
// N2kManager's position caching). Silently no-ops for event types with no
// SeaTalk encoding (TripLog, TotalLog, GnssTime, GnssDate, SatelliteCount,
// MagneticVariation - none of these have a standalone SeaTalk command
// implemented here, matching demo_mode.cpp's existing Object list).
void encodeAndSend(const SeatalkDecode::Event &ev);

}  // namespace SeatalkEncode
