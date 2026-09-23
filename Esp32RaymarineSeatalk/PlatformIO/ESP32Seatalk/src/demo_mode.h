#pragma once

#include <Arduino.h>

// Simulated vessel broadcasting SeaTalk data - see the design discussion
// this was built from: rudder drives heading, heading+speed dead-reckon
// position and set COG (flat-earth approximation, fine at this scale);
// depth/wind/water-temp ramp independently, unrelated to vessel motion.
// Each object is independently checkbox-enabled; only enabled objects get
// transmitted, once per second, while demo mode is running.
namespace DemoMode {

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

bool isRunning();
void start();
void stop();

bool isEnabled(Object obj);
void setEnabled(Object obj, bool enabled);

const char *objectName(Object obj);

// Advances the simulation and transmits whatever's enabled - call every
// loop() iteration; internally rate-limits itself to 1/sec.
void tick();

}  // namespace DemoMode
