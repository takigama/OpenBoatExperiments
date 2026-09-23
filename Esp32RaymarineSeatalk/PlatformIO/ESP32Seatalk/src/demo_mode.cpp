#include "demo_mode.h"

#include <math.h>

#include "debug_log.h"
#include "seatalk_bus.h"

namespace DemoMode {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool s_running = false;
bool s_enabled[(int)Object::Count] = {};
uint32_t s_lastTick = 0;
uint32_t s_secondsElapsed = 0;

// ---- Simulated vessel state - all in SeaTalk-native units (degrees,
// knots, feet) since that's what every encoder below wants directly; no
// benefit to routing through SI here the way seatalk_decode.cpp does for
// received data. ----
double s_lat = -33.8;   // arbitrary starting point, Sydney-ish
double s_lon = 151.2;
double s_headingDeg = 0;
double s_rudderDeg = 0;
double s_speedKn = 0;
double s_depthFt = 0;
double s_windAngleDeg = 0;
double s_windSpeedKn = 0;
double s_waterTempC = 18;

// Triangle wave: ramps 0->amplitude->0 over periodSec, starting at 0.
double triangle(uint32_t elapsedSec, double periodSec, double amplitude) {
    double phase = fmod(elapsedSec, periodSec) / periodSec;  // 0..1
    double tri = phase < 0.5 ? phase * 2 : 2 - phase * 2;     // 0..1..0
    return tri * amplitude;
}

void advanceSimulation() {
    s_secondsElapsed++;

    // Rudder: +-15deg, ~60s each way (see the earlier design discussion -
    // this is the "control input" driving heading, not just cosmetic).
    s_rudderDeg = triangle(s_secondsElapsed, 120.0, 30.0) - 15.0;

    // Heading integrates from rudder - gentle constant so it doesn't spin
    // unrealistically fast even at full simulated lock.
    s_headingDeg += s_rudderDeg * 0.1;
    s_headingDeg = fmod(s_headingDeg + 360.0, 360.0);

    // Speed: the original example pattern - 0->25kn at 0.5kn/sec, so a
    // full up+down period is 100s.
    s_speedKn = triangle(s_secondsElapsed, 100.0, 25.0);

    // Position: flat-earth dead reckoning from heading+speed, fine at
    // demo fidelity (see the design discussion - no need for great-circle
    // math here). distance in nm this tick = speed(kn) * dt(hours).
    double distanceNm = s_speedKn * (1.0 / 3600.0);
    double headingRad = s_headingDeg * kPi / 180.0;
    s_lat += distanceNm * cos(headingRad) / 60.0;                                    // 1 deg lat = 60nm
    s_lon += distanceNm * sin(headingRad) / (60.0 * cos(s_lat * kPi / 180.0));

    // Independent ramps - not coupled to vessel motion at all.
    s_depthFt = triangle(s_secondsElapsed, 60.0, 30.0);
    s_windAngleDeg = fmod(s_secondsElapsed * (360.0 / 45.0), 360.0);  // continuous sweep, 45s/rotation
    s_windSpeedKn = triangle(s_secondsElapsed, 80.0, 30.0);
    s_waterTempC = 10.0 + triangle(s_secondsElapsed, 120.0, 15.0);
}

void sendDepth() {
    uint16_t raw = (uint16_t)(s_depthFt * 10.0 + 0.5);
    uint8_t data[] = {0x02, 0x00, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x00, data, sizeof(data));
}

void sendSpeedThroughWater() {
    uint16_t raw = (uint16_t)(s_speedKn * 10.0 + 0.5);
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x20, data, sizeof(data));
}

void sendSpeedOverGround() {
    uint16_t raw = (uint16_t)(s_speedKn * 10.0 + 0.5);  // no current/leeway modeled - SOG=STW
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x52, data, sizeof(data));
}

void sendApparentWindAngle() {
    uint16_t raw = (uint16_t)(s_windAngleDeg * 2.0 + 0.5);
    uint8_t data[] = {0x01, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x10, data, sizeof(data));
}

void sendApparentWindSpeed() {
    uint8_t whole = (uint8_t)s_windSpeedKn & 0x7F;
    uint8_t tenths = (uint8_t)((s_windSpeedKn - (int)s_windSpeedKn) * 10.0) & 0x0F;
    uint8_t data[] = {0x01, whole, tenths};
    SeatalkBus::send(0x11, data, sizeof(data));
}

void sendWaterTemperature() {
    uint8_t data[] = {0x01, (uint8_t)(int8_t)lround(s_waterTempC),
                       (uint8_t)(int8_t)lround(s_waterTempC * 9.0 / 5.0 + 32.0)};
    SeatalkBus::send(0x23, data, sizeof(data));
}

void sendPosition() {
    // Mirrors seatalk_decode.cpp's 0x50/0x51 formulas exactly, inverted.
    double latAbs = fabs(s_lat);
    uint8_t latDeg = (uint8_t)latAbs;
    uint16_t latMinRaw = (uint16_t)((latAbs - latDeg) * 60.0 * 100.0 + 0.5);
    if (s_lat < 0) latMinRaw |= 0x8000;  // south
    uint8_t latData[] = {0x02, latDeg, (uint8_t)(latMinRaw & 0xFF), (uint8_t)(latMinRaw >> 8)};
    SeatalkBus::send(0x50, latData, sizeof(latData));

    double lonAbs = fabs(s_lon);
    uint8_t lonDeg = (uint8_t)lonAbs;
    uint16_t lonMinRaw = (uint16_t)((lonAbs - lonDeg) * 60.0 * 100.0 + 0.5);
    if (s_lon >= 0) lonMinRaw |= 0x8000;  // east
    uint8_t lonData[] = {0x02, lonDeg, (uint8_t)(lonMinRaw & 0xFF), (uint8_t)(lonMinRaw >> 8)};
    SeatalkBus::send(0x51, lonData, sizeof(lonData));
}

void sendCourseOverGround() {
    // "53 U0 VW" = (U&0x3)*90 + (VW&0x3F)*2 + (U&0xC)/8 - the last term
    // (sub-2-degree refinement) is left at 0 here, not worth the fiddly
    // inverse math for demo-mode precision (COG=heading, no drift
    // modeled anyway - see the earlier design discussion).
    double cog = s_headingDeg;
    uint8_t uLow = (uint8_t)(cog / 90.0) & 0x3;
    uint8_t vw = (uint8_t)((cog - uLow * 90.0) / 2.0) & 0x3F;
    uint8_t data[] = {(uint8_t)(uLow << 4), vw};  // attribute byte (U in high nibble), then VW
    SeatalkBus::send(0x53, data, sizeof(data));
}

void sendHeadingAndRudder() {
    // "9C U1 VW RR" - same simplification as COG above (skip the
    // popcount sub-2-degree refinement term).
    double heading = s_headingDeg;
    uint8_t uLow = (uint8_t)(heading / 90.0) & 0x3;
    uint8_t vw = (uint8_t)((heading - uLow * 90.0) / 2.0) & 0x3F;
    uint8_t attribute = (uLow << 4) | 0x1;
    int8_t rudder = (int8_t)lround(s_rudderDeg);
    uint8_t data[] = {attribute, vw, (uint8_t)rudder};
    SeatalkBus::send(0x9C, data, sizeof(data));
}

}  // namespace

bool isRunning() { return s_running; }

void start() {
    s_running = true;
    s_secondsElapsed = 0;
    s_lastTick = 0;
    DebugLog::logf("demo: started");
}

void stop() {
    s_running = false;
    DebugLog::logf("demo: stopped");
}

bool isEnabled(Object obj) { return s_enabled[(int)obj]; }
void setEnabled(Object obj, bool enabled) { s_enabled[(int)obj] = enabled; }

const char *objectName(Object obj) {
    switch (obj) {
        case Object::Depth: return "Depth";
        case Object::SpeedThroughWater: return "Speed through water";
        case Object::ApparentWindAngle: return "Apparent wind angle";
        case Object::ApparentWindSpeed: return "Apparent wind speed";
        case Object::WaterTemperature: return "Water temperature";
        case Object::Position: return "Position (lat/lon)";
        case Object::CourseOverGround: return "Course over ground";
        case Object::SpeedOverGround: return "Speed over ground";
        case Object::HeadingAndRudder: return "Heading + rudder";
        default: return "?";
    }
}

void tick() {
    if (!s_running) return;
    if (millis() - s_lastTick < 1000) return;
    s_lastTick = millis();

    advanceSimulation();

    if (s_enabled[(int)Object::Depth]) sendDepth();
    if (s_enabled[(int)Object::SpeedThroughWater]) sendSpeedThroughWater();
    if (s_enabled[(int)Object::ApparentWindAngle]) sendApparentWindAngle();
    if (s_enabled[(int)Object::ApparentWindSpeed]) sendApparentWindSpeed();
    if (s_enabled[(int)Object::WaterTemperature]) sendWaterTemperature();
    if (s_enabled[(int)Object::Position]) sendPosition();
    if (s_enabled[(int)Object::CourseOverGround]) sendCourseOverGround();
    if (s_enabled[(int)Object::SpeedOverGround]) sendSpeedOverGround();
    if (s_enabled[(int)Object::HeadingAndRudder]) sendHeadingAndRudder();
}

}  // namespace DemoMode
