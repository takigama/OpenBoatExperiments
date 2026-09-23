#include "seatalk_decode.h"

namespace SeatalkDecode {

namespace {

constexpr double kFeetToMeters = 0.3048;
constexpr double kKnotsToMs = 0.514444;
constexpr double kNmToMeters = 1852.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// Bits 2-3 of a nibble, "number of bits set" - the quirky extra-resolution
// term commands 84/89/9C use (NOT the same as command 53's COG formula,
// which divides those same two bits by 8 instead - easy to conflate, kept
// as two separate helpers on purpose rather than one "clever" shared one.
int headingExtraDegrees(uint8_t nibble) {
    uint8_t bits = nibble & 0xC;
    if (bits == 0) return 0;
    if (bits == 0xC) return 2;
    return 1;
}

}  // namespace

bool decode(const SeatalkBus::Datagram &dg, Event *out) {
    const uint8_t *b = dg.bytes;
    uint8_t cmd = b[0];
    uint8_t attrHigh = b[1] >> 4;

    switch (cmd) {
        case 0x00: {  // Depth below transducer: "00 02 YZ XX XX", XXXX/10 feet
            if (dg.length != 5) return false;
            uint16_t raw = b[3] | (b[4] << 8);
            out->type = Type::Depth;
            out->value = (raw / 10.0) * kFeetToMeters;
            out->flagA = (b[2] & 0x04) != 0;  // transducer defective
            return true;
        }
        case 0x20: {  // Speed through water: "20 01 XX XX", XXXX/10 knots
            if (dg.length != 4) return false;
            uint16_t raw = b[2] | (b[3] << 8);
            out->type = Type::SpeedThroughWater;
            out->value = (raw / 10.0) * kKnotsToMs;
            return true;
        }
        case 0x21: {  // Trip log: "21 02 XX XX 0X", XXXXX/100 nm
            if (dg.length != 5) return false;
            uint32_t raw = b[2] | (b[3] << 8) | ((b[4] & 0x0F) << 16);
            out->type = Type::TripLog;
            out->value = (raw / 100.0) * kNmToMeters;
            return true;
        }
        case 0x22: {  // Total log: "22 02 XX XX 00", XXXX/10 nm
            if (dg.length != 5) return false;
            uint16_t raw = b[2] | (b[3] << 8);
            out->type = Type::TotalLog;
            out->value = (raw / 10.0) * kNmToMeters;
            return true;
        }
        case 0x10: {  // Apparent wind angle: "10 01 XX YY", XXYY/2 degrees right of bow
            if (dg.length != 4) return false;
            uint16_t raw = b[2] | (b[3] << 8);
            double deg = raw / 2.0;
            if (deg > 180.0) deg -= 360.0;  // -> signed, negative = port (SignalK convention)
            out->type = Type::ApparentWindAngle;
            out->value = deg * kDegToRad;
            return true;
        }
        case 0x11: {  // Apparent wind speed: "11 01 XX 0Y"
            if (dg.length != 4) return false;
            double raw = (b[2] & 0x7F) + (b[3] & 0x0F) / 10.0;
            bool alreadyMetersPerSec = (b[2] & 0x80) != 0;
            out->type = Type::ApparentWindSpeed;
            out->value = alreadyMetersPerSec ? raw : raw * kKnotsToMs;
            return true;
        }
        case 0x23: {  // Water temp (ST50): "23 Z1 XX YY", XX = signed degC directly
            if (dg.length != 4) return false;
            out->type = Type::WaterTemperature;
            out->value = (int8_t)b[2] + 273.15;
            out->flagA = (attrHigh & 0x04) != 0;  // sensor defective/not connected
            return true;
        }
        case 0x50: {  // Latitude: "50 Z2 XX YY YY", XX deg, (YYYY&0x7FFF)/100 min, bit15=South
            if (dg.length != 5) return false;
            uint16_t yyyy = b[3] | (b[4] << 8);
            double deg = b[2] + (yyyy & 0x7FFF) / 100.0 / 60.0;
            out->type = Type::Latitude;
            out->value = (yyyy & 0x8000) ? -deg : deg;
            return true;
        }
        case 0x51: {  // Longitude: same shape as 0x50, bit15=East (opposite sign polarity to LAT)
            if (dg.length != 5) return false;
            uint16_t yyyy = b[3] | (b[4] << 8);
            double deg = b[2] + (yyyy & 0x7FFF) / 100.0 / 60.0;
            out->type = Type::Longitude;
            out->value = (yyyy & 0x8000) ? deg : -deg;
            return true;
        }
        case 0x52: {  // Speed over ground: "52 01 XX XX", XXXX/10 knots
            if (dg.length != 4) return false;
            uint16_t raw = b[2] | (b[3] << 8);
            out->type = Type::SpeedOverGround;
            out->value = (raw / 10.0) * kKnotsToMs;
            return true;
        }
        case 0x53: {  // COG: "53 U0 VW" - (U&0x3)*90 + (VW&0x3F)*2 + (U&0xC)/8
            if (dg.length != 3) return false;
            uint8_t u = attrHigh;
            uint8_t vw = b[2];
            double deg = (u & 0x3) * 90.0 + (vw & 0x3F) * 2.0 + (u & 0xC) / 8.0;
            out->type = Type::CourseOverGround;
            out->value = deg * kDegToRad;
            return true;
        }
        case 0x54: {  // GMT time: "54 T1 RS HH" - minutes=(RS&0xFC)/4, seconds=((RS&0xF)<<4|T)&0x3F
            if (dg.length != 4) return false;
            uint8_t t = attrHigh;
            uint8_t rs = b[2];
            uint8_t hours = b[3];
            uint8_t minutes = (rs & 0xFC) >> 2;
            uint8_t seconds = (((rs & 0x0F) << 4) | t) & 0x3F;
            out->type = Type::GnssTime;
            out->value = hours * 3600.0 + minutes * 60.0 + seconds;
            return true;
        }
        case 0x56: {  // Date: "56 M1 DD YY" - M=month (attribute high nibble), DD=day, YY=2-digit year
            if (dg.length != 4) return false;
            out->type = Type::GnssDate;
            out->month = attrHigh;
            out->day = b[2];
            out->year = 2000 + b[3];
            return true;
        }
        case 0x57: {  // Sat info: "57 S0 DD" - S=sat count (attribute high nibble)
            if (dg.length != 3) return false;
            out->type = Type::SatelliteCount;
            out->value = attrHigh;
            return true;
        }
        case 0x9C: {  // Heading+rudder: "9C U1 VW RR"
            if (dg.length != 4) return false;
            uint8_t u = attrHigh;
            uint8_t vw = b[2];
            double headingDeg = (u & 0x3) * 90.0 + (vw & 0x3F) * 2.0 + headingExtraDegrees(u);
            out->type = Type::HeadingAndRudder;
            out->value = headingDeg * kDegToRad;
            out->value2 = (int8_t)b[3] * kDegToRad;  // rudder: +right, -left
            out->flagA = (u & 0x8) != 0;             // turning right if set, left if clear
            return true;
        }
        case 0x99: {  // Variation: "99 00 XX" - signed, POSITIVE=West, NEGATIVE=East (raw SeaTalk
                       // convention - caller should check this against navigation.magneticVariation's
                       // actual sign convention before publishing, not verified against SignalK's spec here)
            if (dg.length != 3) return false;
            out->type = Type::MagneticVariation;
            out->value = (int8_t)b[2] * kDegToRad;
            return true;
        }
        default:
            return false;
    }
}

const char *canonicalPath(Type type) {
    switch (type) {
        case Type::Depth: return "environment.depth.belowTransducer";
        case Type::SpeedThroughWater: return "navigation.speedThroughWater";
        case Type::TripLog: return "navigation.trip.log";
        case Type::TotalLog: return "navigation.log";
        case Type::ApparentWindAngle: return "environment.wind.angleApparent";
        case Type::ApparentWindSpeed: return "environment.wind.speedApparent";
        case Type::WaterTemperature: return "environment.water.temperature";
        case Type::Latitude: return "navigation.position.latitude";
        case Type::Longitude: return "navigation.position.longitude";
        case Type::SpeedOverGround: return "navigation.speedOverGround";
        case Type::CourseOverGround: return "navigation.courseOverGroundTrue";
        case Type::GnssTime: return "navigation.datetime.secondsSinceMidnight";
        case Type::SatelliteCount: return "navigation.gnss.satellites";
        case Type::MagneticVariation: return "navigation.magneticVariation";
        default: return nullptr;  // HeadingAndRudder, GnssDate - see header comment
    }
}

}  // namespace SeatalkDecode
