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
//
// Also subscribes to "{base}/set/#" for inbound commands - a value
// published there gets picked up and routed via RouteConfig::relay(),
// which decides (per the web UI's matrix) whether it goes on to SeaTalk
// and/or CAN. Deliberately a separate topic tree from our own outbound
// publishes, so subscribing can never pick up and re-process our own
// published values as if they were external commands.
//
// Separately, "{base}/raw/{seatalk,can,signalk}" carries a plain hex dump
// of every message seen on that bus/connection, decoded or not - the
// only view into traffic none of the structured Event/PGN/delta handling
// understands. "{base}/raw/{source}/send" is the reverse: publish hex
// there and it gets injected straight onto that bus/connection, bypassing
// SeatalkDecode/N2K-PGN/SignalK-delta parsing and RouteConfig entirely -
// a low-level debug/replay channel, not a source RouteConfig's matrix
// knows about.
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

// Publishes a decoded event to its SignalK-style path - takes just the
// Event (no Datagram) so any source can feed it, not only SeaTalk RX; see
// N2kManager, which relays parsed N2K PGNs through this same sink.
void publishDecoded(const SeatalkDecode::Event &ev);

// Hex-dumps `len` bytes from `data` to "{base}/raw/{source}" - `source`
// should be one of "seatalk", "can", "signalk" (matches the topics
// handleMessage() listens for on the send side). Called from main.cpp
// (SeaTalk RX), N2kManager (every parsed N2K message), and SignalKManager
// (every raw incoming WS text frame).
void publishRawBus(const char *source, const uint8_t *data, size_t len);

}  // namespace MqttManager
