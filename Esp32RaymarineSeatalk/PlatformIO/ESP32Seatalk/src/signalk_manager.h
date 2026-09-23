#pragma once

#include <Arduino.h>

#include "seatalk_decode.h"

// Publishes decoded SeaTalk data to a SignalK server over its WebSocket
// delta stream (ws://<host>:<port>/signalk/v1/stream) - the officially
// documented mechanism for an external device to push data into a
// SignalK server's data model, distinct from MqttManager's flatter
// slash-topic publishing.
//
// Anonymous/unauthenticated writes only for now, verified against a
// security-disabled dev SignalK instance - a real boat server with
// security enabled (the common default) would need the device
// access-request token flow instead, deliberately not built yet since
// nothing here has exercised it against a real security-enabled server.
// Adding a stored token to the WS connection later is additive, not a
// rearchitecture.
//
// Also subscribes (subscribe=self) so it can receive deltas, not just
// send them - an incoming delta gets routed via RouteConfig::relay(),
// which decides (per the web UI's matrix) whether it goes on to SeaTalk
// and/or CAN. Deltas tagged with our own source label are ignored (the
// server rebroadcasts every delta to every subscriber, including the one
// that sent it, so without this filter our own outbound values would
// bounce straight back in as if an external client had sent them).
//
// Every raw incoming WS text frame - understood or not - also gets
// hex-dumped to MQTT's raw/signalk topic (see mqtt_manager.h), and
// sendRaw() is how a raw hex payload published to raw/signalk/send goes
// straight out over the WS connection as a text frame, no JSON building
// or RouteConfig gating involved.
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

// Sends `text` as a raw WS text frame - bypasses the delta-building
// helpers entirely, since a raw hex payload from MQTT is arbitrary bytes,
// not necessarily even valid SignalK JSON.
void sendRaw(const String &text);

}  // namespace SignalKManager
