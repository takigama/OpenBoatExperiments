#pragma once

#include <Arduino.h>

#include "seatalk_decode.h"

// Publishes decoded SeaTalk data to a SignalK server over its WebSocket
// delta stream (ws://<host>:<port>/signalk/v1/stream) - the officially
// documented mechanism for an external device to push data into a
// SignalK server's data model, distinct from MqttManager's flatter
// slash-topic publishing. No raw/undecoded publishing here (unlike MQTT) -
// SignalK's structured data model has no equivalent of "dump the
// undecoded hex somewhere", so only decode()'d events get sent.
//
// Anonymous/unauthenticated writes only for now, verified against a
// security-disabled dev SignalK instance - a real boat server with
// security enabled (the common default) would need the device
// access-request token flow instead, deliberately not built yet since
// nothing here has exercised it against a real security-enabled server.
// Adding a stored token to the WS connection later is additive, not a
// rearchitecture.
namespace SignalKManager {

void begin();

// Call every loop() iteration - pumps the WebSocket client (including its
// own built-in reconnect timer).
void tick();

bool isConnected();

// Server host/port, persisted to NVS. Applied live - no reboot needed.
void saveConfig(const String &host, uint16_t port);
String configHost();
uint16_t configPort();

// Publishes one decoded event as a SignalK delta. Latitude/Longitude
// arrive as two independent SeaTalk datagrams (see seatalk_decode.h) but
// SignalK's navigation.position path wants both together as one
// {latitude, longitude} value - the most recently seen one of each is
// cached internally and a combined delta goes out whenever either
// updates, once both are known at least once.
void publishDecoded(const SeatalkDecode::Event &ev);

}  // namespace SignalKManager
