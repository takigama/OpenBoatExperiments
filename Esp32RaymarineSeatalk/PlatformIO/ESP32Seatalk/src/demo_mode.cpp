#include "demo_mode.h"

#include <math.h>

#include "debug_log.h"
#include "seatalk_encode.h"

namespace DemoMode {

namespace {

constexpr double kPi = 3.14159265358979323846;

Mode s_mode = Mode::Off;
bool s_enabled[(int)Object::Count] = {};
uint32_t s_lastTick = 0;
uint32_t s_secondsElapsed = 0;

// ---- Simulated vessel state (Cycling mode) - all in SeaTalk-native
// units (degrees, knots, feet) since that's what every encoder below
// wants directly; no benefit to routing through SI here the way
// seatalk_decode.cpp does for received data. ----
double s_lat = -33.8;  // arbitrary starting point, Sydney-ish
double s_lon = 151.2;
double s_headingDeg = 0;
double s_rudderDeg = 0;
double s_speedKn = 0;
double s_depthFt = 0;
double s_windAngleDeg = 0;
double s_windSpeedKn = 0;
double s_waterTempC = 18;

// ---- Manual mode state - deliberately separate from the simulation
// state above rather than reusing it: e.g. STW and SOG share one
// variable in Cycling (SOG=STW, no drift modeled - see the design
// discussion), but manual mode should let each object be pinned
// independently, including STW != SOG if that's what's being tested. ----
double s_manualDepthFt = 0;
double s_manualSpeedKn = 0;
double s_manualWindAngleDeg = 0;
double s_manualWindSpeedKn = 0;
double s_manualWaterTempC = 0;
double s_manualLat = 0, s_manualLon = 0;
double s_manualCogDeg = 0;
double s_manualSogKn = 0;
double s_manualHeadingDeg = 0, s_manualRudderDeg = 0;

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

// ---- Encoders now live in seatalk_encode.h, shared with RouteConfig for
// bridging real CAN/MQTT/SignalK data onto SeaTalk - pulled in via a set
// of using-declarations so the call sites below don't need touching. ----
using SeatalkEncode::sendApparentWindAngle;
using SeatalkEncode::sendApparentWindSpeed;
using SeatalkEncode::sendCourseOverGround;
using SeatalkEncode::sendDepth;
using SeatalkEncode::sendHeadingAndRudder;
using SeatalkEncode::sendPosition;
using SeatalkEncode::sendSpeedOverGround;
using SeatalkEncode::sendSpeedThroughWater;
using SeatalkEncode::sendWaterTemperature;

void sendEnabledFromSimulation() {
    if (s_enabled[(int)Object::Depth]) sendDepth(s_depthFt);
    if (s_enabled[(int)Object::SpeedThroughWater]) sendSpeedThroughWater(s_speedKn);
    if (s_enabled[(int)Object::ApparentWindAngle]) sendApparentWindAngle(s_windAngleDeg);
    if (s_enabled[(int)Object::ApparentWindSpeed]) sendApparentWindSpeed(s_windSpeedKn);
    if (s_enabled[(int)Object::WaterTemperature]) sendWaterTemperature(s_waterTempC);
    if (s_enabled[(int)Object::Position]) sendPosition(s_lat, s_lon);
    if (s_enabled[(int)Object::CourseOverGround]) sendCourseOverGround(s_headingDeg);
    if (s_enabled[(int)Object::SpeedOverGround]) sendSpeedOverGround(s_speedKn);
    if (s_enabled[(int)Object::HeadingAndRudder]) sendHeadingAndRudder(s_headingDeg, s_rudderDeg);
}

void sendEnabledFromManual() {
    if (s_enabled[(int)Object::Depth]) sendDepth(s_manualDepthFt);
    if (s_enabled[(int)Object::SpeedThroughWater]) sendSpeedThroughWater(s_manualSpeedKn);
    if (s_enabled[(int)Object::ApparentWindAngle]) sendApparentWindAngle(s_manualWindAngleDeg);
    if (s_enabled[(int)Object::ApparentWindSpeed]) sendApparentWindSpeed(s_manualWindSpeedKn);
    if (s_enabled[(int)Object::WaterTemperature]) sendWaterTemperature(s_manualWaterTempC);
    if (s_enabled[(int)Object::Position]) sendPosition(s_manualLat, s_manualLon);
    if (s_enabled[(int)Object::CourseOverGround]) sendCourseOverGround(s_manualCogDeg);
    if (s_enabled[(int)Object::SpeedOverGround]) sendSpeedOverGround(s_manualSogKn);
    if (s_enabled[(int)Object::HeadingAndRudder]) sendHeadingAndRudder(s_manualHeadingDeg, s_manualRudderDeg);
}

}  // namespace

Mode currentMode() { return s_mode; }

void startCycling() {
    s_mode = Mode::Cycling;
    s_secondsElapsed = 0;
    s_lastTick = 0;
    DebugLog::logf("demo: started (cycling)");
}

void setManualValue(Object obj, double v1, double v2) {
    switch (obj) {
        case Object::Depth: s_manualDepthFt = v1; break;
        case Object::SpeedThroughWater: s_manualSpeedKn = v1; break;
        case Object::ApparentWindAngle: s_manualWindAngleDeg = v1; break;
        case Object::ApparentWindSpeed: s_manualWindSpeedKn = v1; break;
        case Object::WaterTemperature: s_manualWaterTempC = v1; break;
        case Object::Position: s_manualLat = v1; s_manualLon = v2; break;
        case Object::CourseOverGround: s_manualCogDeg = v1; break;
        case Object::SpeedOverGround: s_manualSogKn = v1; break;
        case Object::HeadingAndRudder: s_manualHeadingDeg = v1; s_manualRudderDeg = v2; break;
        default: break;
    }
}

void startManual() {
    s_mode = Mode::Manual;
    s_lastTick = 0;
    DebugLog::logf("demo: started (manual)");
}

void stop() {
    s_mode = Mode::Off;
    DebugLog::logf("demo: stopped");
}

bool isEnabled(Object obj) { return s_enabled[(int)obj]; }
void setEnabled(Object obj, bool enabled) { s_enabled[(int)obj] = enabled; }

int valueCount(Object obj) {
    return (obj == Object::Position || obj == Object::HeadingAndRudder) ? 2 : 1;
}

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
    if (s_mode == Mode::Off) return;
    if (millis() - s_lastTick < 1000) return;
    s_lastTick = millis();

    if (s_mode == Mode::Cycling) {
        advanceSimulation();
        sendEnabledFromSimulation();
    } else {
        sendEnabledFromManual();
    }
}

}  // namespace DemoMode
