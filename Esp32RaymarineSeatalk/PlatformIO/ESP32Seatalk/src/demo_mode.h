#pragma once

#include <Arduino.h>

// Two independent ways to broadcast synthetic SeaTalk data, sharing the
// same per-object encoders and the same enabled/disabled checkbox set:
//
// - Cycling: simulated vessel (see the design discussion this was built
//   from) - rudder drives heading, heading+speed dead-reckon position and
//   set COG (flat-earth approximation, fine at this scale); depth/wind/
//   water-temp ramp independently.
// - Manual: fixed, user-entered values, sent unchanged every second until
//   stopped or replaced - for pinning an instrument to one exact value
//   rather than watching it ramp.
//
// Only one mode runs at a time. Only enabled objects get transmitted,
// once per second, while a mode is running.
namespace DemoMode {

enum class Mode { Off, Cycling, Manual };

enum class Object {
    Depth,
    SpeedThroughWater,
    ApparentWindAngle,
    ApparentWindSpeed,
    WaterTemperature,
    Position,  // lat + lon together (two datagrams, sent back to back)
    CourseOverGround,
    SpeedOverGround,
    HeadingAndRudder,
    Count  // not a real object - array sizing/iteration bound
};

Mode currentMode();
void startCycling();
// Sets the fixed value(s) manual mode will send for this object - v2 is
// only used for the two-value objects (Position: lat,lon in decimal
// degrees; HeadingAndRudder: heading,rudder in degrees). Native SeaTalk
// units throughout (feet, knots, degrees, Celsius), same as the cycling
// simulation - see demo_mode.cpp.
void setManualValue(Object obj, double v1, double v2 = 0);
void startManual();
void stop();

bool isEnabled(Object obj);
void setEnabled(Object obj, bool enabled);
int valueCount(Object obj);  // 1 or 2 - how many manual value fields this object needs

const char *objectName(Object obj);

// Advances the simulation (Cycling) or just re-sends the fixed values
// (Manual) and transmits whatever's enabled - call every loop()
// iteration; internally rate-limits itself to 1/sec.
void tick();

}  // namespace DemoMode
