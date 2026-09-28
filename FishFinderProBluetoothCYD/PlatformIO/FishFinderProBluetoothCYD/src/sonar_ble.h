#pragma once

#include <Arduino.h>

// BLE central for the "Fish Helper Pro" castable sonar - same GATT
// service/frame protocol as FishFinderProBluetooth (see that project's
// README for the full reverse-engineered frame format writeup and how it
// was derived). This module owns the connection/reassembly/decode; it
// doesn't touch the display - main.cpp wires latest() into Display::update().
namespace SonarBle {

struct Reading {
    bool valid = false;  // false until at least one checksum-valid frame decoded
    float depthM = 0;
    bool fishValid = false;  // false when the device reports "no fish" (raw value 0)
    float fishDepthM = 0;
    float tempC = 0;
    uint8_t battery = 0;  // raw 0-6, see README's "Frame format" for what each level means
    uint8_t depthRangeIdx = 0;
    bool outOfWater = false;
    bool isCharging = false;
    uint32_t frameCount = 0;
    int rssi = 0;  // dBm, 0 until first successful read - see loop()
};

// Inits NimBLE and starts scanning. Call once from setup().
void begin();

// Pumps connect-state handling. Call every loop() iteration.
void loop();

bool connected();
const Reading &latest();

}  // namespace SonarBle
