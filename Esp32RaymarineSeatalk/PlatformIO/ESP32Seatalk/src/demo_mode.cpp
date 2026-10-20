#include "demo_mode.h"

#include <math.h>

#include <TobeLog.h>
#include "mqtt_manager.h"
#include "seatalk_bus.h"
#include "seatalk_encode.h"
#include "signalk_manager.h"

namespace DemoMode {

namespace {

constexpr double kPi = 3.14159265358979323846;

Mode s_mode = Mode::Off;
Destinations s_dest;
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

// Publishes one value, in the SI units a decoded SeaTalk reading has, to MQTT and / or SignalK (the same sinks
// RouteConfig::relay() feeds for real bus data).
void emit(SeatalkDecode::Type type, double value, double value2 = 0) {
    SeatalkDecode::Event ev;
    ev.type = type;
    ev.value = value;
    ev.value2 = value2;
    if (s_dest.mqtt) MqttManager::publishDecoded(ev);
    if (s_dest.signalk) SignalKManager::publishDecoded(ev);
}

constexpr double kFeetToMeters = 0.3048;
constexpr double kKnotsToMs = 0.514444;
constexpr double kDegToRad = kPi / 180.0;

// Sends one demo object (values in SeaTalk-native units) to every chosen destination.
void put(Object obj, double v1, double v2 = 0) {
    switch (obj) {
        case Object::Depth:
            if (s_dest.seatalk) sendDepth(v1);
            emit(SeatalkDecode::Type::Depth, v1 * kFeetToMeters);
            break;
        case Object::SpeedThroughWater:
            if (s_dest.seatalk) sendSpeedThroughWater(v1);
            emit(SeatalkDecode::Type::SpeedThroughWater, v1 * kKnotsToMs);
            break;
        case Object::ApparentWindAngle: {
            if (s_dest.seatalk) sendApparentWindAngle(v1);
            double deg = fmod(v1, 360.0);
            if (deg > 180.0) deg -= 360.0;  // signed, negative = port - what decode() publishes
            emit(SeatalkDecode::Type::ApparentWindAngle, deg * kDegToRad);
            break;
        }
        case Object::ApparentWindSpeed:
            if (s_dest.seatalk) sendApparentWindSpeed(v1);
            emit(SeatalkDecode::Type::ApparentWindSpeed, v1 * kKnotsToMs);
            break;
        case Object::WaterTemperature:
            if (s_dest.seatalk) sendWaterTemperature(v1);
            emit(SeatalkDecode::Type::WaterTemperature, v1 + 273.15);
            break;
        case Object::Position:
            if (s_dest.seatalk) sendPosition(v1, v2);
            emit(SeatalkDecode::Type::Latitude, v1);
            emit(SeatalkDecode::Type::Longitude, v2);
            break;
        case Object::CourseOverGround:
            if (s_dest.seatalk) sendCourseOverGround(v1);
            emit(SeatalkDecode::Type::CourseOverGround, v1 * kDegToRad);
            break;
        case Object::SpeedOverGround:
            if (s_dest.seatalk) sendSpeedOverGround(v1);
            emit(SeatalkDecode::Type::SpeedOverGround, v1 * kKnotsToMs);
            break;
        case Object::HeadingAndRudder:
            if (s_dest.seatalk) sendHeadingAndRudder(v1, v2);
            emit(SeatalkDecode::Type::HeadingAndRudder, v1 * kDegToRad, v2 * kDegToRad);
            break;
        default: break;
    }
}

void sendEnabledFromSimulation() {
    if (s_enabled[(int)Object::Depth]) put(Object::Depth, s_depthFt);
    if (s_enabled[(int)Object::SpeedThroughWater]) put(Object::SpeedThroughWater, s_speedKn);
    if (s_enabled[(int)Object::ApparentWindAngle]) put(Object::ApparentWindAngle, s_windAngleDeg);
    if (s_enabled[(int)Object::ApparentWindSpeed]) put(Object::ApparentWindSpeed, s_windSpeedKn);
    if (s_enabled[(int)Object::WaterTemperature]) put(Object::WaterTemperature, s_waterTempC);
    if (s_enabled[(int)Object::Position]) put(Object::Position, s_lat, s_lon);
    if (s_enabled[(int)Object::CourseOverGround]) put(Object::CourseOverGround, s_headingDeg);
    if (s_enabled[(int)Object::SpeedOverGround]) put(Object::SpeedOverGround, s_speedKn);
    if (s_enabled[(int)Object::HeadingAndRudder]) put(Object::HeadingAndRudder, s_headingDeg, s_rudderDeg);
}

void sendEnabledFromManual() {
    if (s_enabled[(int)Object::Depth]) put(Object::Depth, s_manualDepthFt);
    if (s_enabled[(int)Object::SpeedThroughWater]) put(Object::SpeedThroughWater, s_manualSpeedKn);
    if (s_enabled[(int)Object::ApparentWindAngle]) put(Object::ApparentWindAngle, s_manualWindAngleDeg);
    if (s_enabled[(int)Object::ApparentWindSpeed]) put(Object::ApparentWindSpeed, s_manualWindSpeedKn);
    if (s_enabled[(int)Object::WaterTemperature]) put(Object::WaterTemperature, s_manualWaterTempC);
    if (s_enabled[(int)Object::Position]) put(Object::Position, s_manualLat, s_manualLon);
    if (s_enabled[(int)Object::CourseOverGround]) put(Object::CourseOverGround, s_manualCogDeg);
    if (s_enabled[(int)Object::SpeedOverGround]) put(Object::SpeedOverGround, s_manualSogKn);
    if (s_enabled[(int)Object::HeadingAndRudder]) put(Object::HeadingAndRudder, s_manualHeadingDeg, s_manualRudderDeg);
}

}  // namespace

Mode currentMode() { return s_mode; }

void startCycling() {
    s_mode = Mode::Cycling;
    s_secondsElapsed = 0;
    s_lastTick = 0;
    tobe::logf("demo: started (cycling)");
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
    tobe::logf("demo: started (manual)");
}

void stop() {
    s_mode = Mode::Off;
    tobe::logf("demo: stopped");
}

Destinations destinations() { return s_dest; }
void setDestinations(bool seatalk, bool mqtt, bool signalk) {
    s_dest.seatalk = seatalk;
    s_dest.mqtt = mqtt;
    s_dest.signalk = signalk;
    tobe::logf("demo: destinations seatalk=%d mqtt=%d signalk=%d", seatalk, mqtt, signalk);
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

    // Our own transmissions come back on the bus; mark that echo so the main loop does not publish it again.
    SeatalkBus::setTxIsSelf(true);
    if (s_mode == Mode::Cycling) {
        advanceSimulation();
        sendEnabledFromSimulation();
    } else {
        sendEnabledFromManual();
    }
    SeatalkBus::setTxIsSelf(false);
}

}  // namespace DemoMode
