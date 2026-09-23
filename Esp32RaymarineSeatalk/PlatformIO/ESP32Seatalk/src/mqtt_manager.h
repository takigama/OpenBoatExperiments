#pragma once

#include <Arduino.h>

#include "seatalk_bus.h"
#include "seatalk_decode.h"

// Publishes decoded SeaTalk data to MQTT, SI units throughout (per the
// earlier design discussion - same units as SignalK, one conversion path
// rather than two). Every received datagram also gets published raw as
// hex, decoded or not - the only view into traffic we don't have a
// decoder for, mirroring what the debug log already does.
//
// Topics mirror SignalK's own dot-path convention with slashes, hung off
// a configurable base topic - e.g. base "boat/seatalk" ->
// "boat/seatalk/navigation/speedThroughWater". One mapping table (see
// mqtt_manager.cpp) drives this rather than maintaining a second naming
// scheme separately from the eventual SignalK layer.
namespace MqttManager {

void begin();

// Call every loop() iteration - pumps the MQTT client and handles
// reconnection (rate-limited, non-blocking).
void tick();

bool isConnected();

// Broker host/port/base-topic, persisted to NVS. Applied live (client
// reconnects with the new settings) - no reboot needed, unlike WiFi
// credentials.
void saveConfig(const String &host, uint16_t port, const String &baseTopic);
String configHost();
uint16_t configPort();
String configBaseTopic();

// Publishes a decoded event to its SignalK-style path, and the raw hex
// dump to the raw-message topic - called from main.cpp's RX loop for
// every datagram received, decoded or not.
void publishDecoded(const SeatalkBus::Datagram &dg, const SeatalkDecode::Event &ev);
void publishRaw(const SeatalkBus::Datagram &dg);

}  // namespace MqttManager
