#pragma once

#include <Arduino.h>

// MQTT broker host/port/base-topic, persisted to NVS - same reasoning as
// wifi_manager.h: nothing network-related gets compiled into the binary,
// since that binary is what gets published for OTA.
namespace NetConfig {

void begin();  // loads from NVS

String mqttHost();
uint16_t mqttPort();
String mqttBaseTopic();

void saveMqtt(const String &host, uint16_t port, const String &baseTopic);

}  // namespace NetConfig
