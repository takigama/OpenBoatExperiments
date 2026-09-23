#include "seatalk_encode.h"

#include <math.h>

#include "seatalk_bus.h"

namespace SeatalkEncode {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFeetToMeters = 0.3048;
constexpr double kKnotsToMs = 0.514444;
constexpr double kRadToDeg = 180.0 / kPi;

double s_lastLat = 0, s_lastLon = 0;
bool s_hasLat = false, s_hasLon = false;

}  // namespace

// Every encoder below mirrors seatalk_decode.cpp's formula for the same
// command, inverted - moved here unchanged from demo_mode.cpp, which now
// calls these instead of keeping its own private copies.

void sendDepth(double ft) {
    uint16_t raw = (uint16_t)(ft * 10.0 + 0.5);
    uint8_t data[] = {0x02, 0x00, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x00, data, sizeof(data));
}

void sendSpeedThroughWater(double kn) {
    uint16_t raw = (uint16_t)(kn * 10.0 + 0.5);
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x20, data, sizeof(data));
}

void sendSpeedOverGround(double kn) {
    uint16_t raw = (uint16_t)(kn * 10.0 + 0.5);
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x52, data, sizeof(data));
}

void sendApparentWindAngle(double deg) {
    uint16_t raw = (uint16_t)(deg * 2.0 + 0.5);
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x10, data, sizeof(data));
}

void sendApparentWindSpeed(double kn) {
    uint8_t whole = (uint8_t)kn & 0x7F;
    uint8_t tenths = (uint8_t)((kn - (int)kn) * 10.0) & 0x0F;
    uint8_t data[] = {0x01, whole, tenths};
    SeatalkBus::send(0x11, data, sizeof(data));
}

void sendWaterTemperature(double celsius) {
    uint8_t data[] = {0x01, (uint8_t)(int8_t)lround(celsius),
                       (uint8_t)(int8_t)lround(celsius * 9.0 / 5.0 + 32.0)};
    SeatalkBus::send(0x23, data, sizeof(data));
}

void sendPosition(double lat, double lon) {
    double latAbs = fabs(lat);
    uint8_t latDeg = (uint8_t)latAbs;
    uint16_t latMinRaw = (uint16_t)((latAbs - latDeg) * 60.0 * 100.0 + 0.5);
    if (lat < 0) latMinRaw |= 0x8000;  // south
    uint8_t latData[] = {0x02, latDeg, (uint8_t)(latMinRaw & 0xFF), (uint8_t)(latMinRaw >> 8)};
    SeatalkBus::send(0x50, latData, sizeof(latData));

    double lonAbs = fabs(lon);
    uint8_t lonDeg = (uint8_t)lonAbs;
    uint16_t lonMinRaw = (uint16_t)((lonAbs - lonDeg) * 60.0 * 100.0 + 0.5);
    if (lon >= 0) lonMinRaw |= 0x8000;  // east
    uint8_t lonData[] = {0x02, lonDeg, (uint8_t)(lonMinRaw & 0xFF), (uint8_t)(lonMinRaw >> 8)};
    SeatalkBus::send(0x51, lonData, sizeof(lonData));
}

void sendCourseOverGround(double cog) {
    uint8_t uLow = (uint8_t)(cog / 90.0) & 0x3;
    uint8_t vw = (uint8_t)((cog - uLow * 90.0) / 2.0) & 0x3F;
    uint8_t data[] = {(uint8_t)(uLow << 4), vw};
    SeatalkBus::send(0x53, data, sizeof(data));
}

void sendHeadingAndRudder(double headingDeg, double rudderDeg) {
    uint8_t uLow = (uint8_t)(headingDeg / 90.0) & 0x3;
    uint8_t vw = (uint8_t)((headingDeg - uLow * 90.0) / 2.0) & 0x3F;
    uint8_t attribute = (uLow << 4) | 0x1;
    int8_t rudder = (int8_t)lround(rudderDeg);
    uint8_t data[] = {attribute, vw, (uint8_t)rudder};
    SeatalkBus::send(0x9C, data, sizeof(data));
}

void encodeAndSend(const SeatalkDecode::Event &ev) {
    switch (ev.type) {
        case SeatalkDecode::Type::Depth:
            sendDepth(ev.value / kFeetToMeters);
            return;
        case SeatalkDecode::Type::SpeedThroughWater:
            sendSpeedThroughWater(ev.value / kKnotsToMs);
            return;
        case SeatalkDecode::Type::SpeedOverGround:
            sendSpeedOverGround(ev.value / kKnotsToMs);
            return;
        case SeatalkDecode::Type::ApparentWindAngle:
            sendApparentWindAngle(ev.value * kRadToDeg);
            return;
        case SeatalkDecode::Type::ApparentWindSpeed:
            sendApparentWindSpeed(ev.value / kKnotsToMs);
            return;
        case SeatalkDecode::Type::WaterTemperature:
            sendWaterTemperature(ev.value - 273.15);
            return;
        case SeatalkDecode::Type::Latitude:
            s_lastLat = ev.value;
            s_hasLat = true;
            if (s_hasLon) sendPosition(s_lastLat, s_lastLon);
            return;
        case SeatalkDecode::Type::Longitude:
            s_lastLon = ev.value;
            s_hasLon = true;
            if (s_hasLat) sendPosition(s_lastLat, s_lastLon);
            return;
        case SeatalkDecode::Type::CourseOverGround:
            sendCourseOverGround(ev.value * kRadToDeg);
            return;
        case SeatalkDecode::Type::HeadingAndRudder:
            sendHeadingAndRudder(ev.value * kRadToDeg, ev.value2 * kRadToDeg);
            return;
        default:
            return;  // TripLog, TotalLog, GnssTime, GnssDate, SatelliteCount, MagneticVariation - no SeaTalk encoding
    }
}

}  // namespace SeatalkEncode
