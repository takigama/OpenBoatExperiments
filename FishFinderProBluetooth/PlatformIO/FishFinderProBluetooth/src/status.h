#pragma once

#include <Arduino.h>

// Small accessor surface so web_config.cpp can show live status without
// owning (or reaching into) main.cpp's BLE/MQTT state directly.
namespace Status {

bool bleConnected();
uint32_t frameCount();  // complete, trailer-valid frames published since boot

}  // namespace Status
