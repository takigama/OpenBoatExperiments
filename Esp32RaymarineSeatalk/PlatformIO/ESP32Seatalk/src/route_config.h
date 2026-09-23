#pragma once

#include "seatalk_decode.h"

// Central routing matrix: for each (source bus, destination bus, object
// type), should an event just received on `source` also go out on
// `dest`? SeaTalk and CAN - the physical instrument networks - route to
// MQTT/SignalK unconditionally (see relay()), matching the project's
// original "unconditionally relay everything received" design; that's
// not user-configurable. Data flowing the other way - from MQTT/SignalK,
// or directly between the two physical buses - onto SeaTalk or CAN is
// opt-in per object type via isAllowed()/setAllowed(), since injecting
// synthetic data onto a real instrument network deserves an explicit
// choice, unlike relaying everything out to a log/dashboard. Every other
// (source, dest) pair - a bus to itself, or MQTT<->SignalK - is simply
// never sent: not configurable, not shown in the web UI, isAllowed()
// always false for it.
namespace RouteConfig {

enum class Bus { SeaTalk, Can, Mqtt, SignalK };

void begin();  // loads persisted config from NVS

// True for the 6 configurable (source, dest) pairs - (SeaTalk,Can),
// (Can,SeaTalk), (Mqtt,SeaTalk), (Mqtt,Can), (SignalK,SeaTalk),
// (SignalK,Can) - false for everything else (see header comment).
bool isConfigurable(Bus source, Bus dest);

bool isAllowed(Bus source, Bus dest, SeatalkDecode::Type type);
// Updates the in-memory config only - call persist() after a batch of
// these (e.g. the web UI's save handler covers many checkboxes in one
// POST) rather than writing NVS on every single call.
void setAllowed(Bus source, Bus dest, SeatalkDecode::Type type, bool allowed);
void persist();

// Central dispatch - call this once for every event received on
// `source`, instead of calling MqttManager/SignalKManager/N2kManager/
// SeatalkEncode directly. Decides everything downstream: the
// unconditional legs plus whatever setAllowed() has enabled.
void relay(Bus source, const SeatalkDecode::Event &ev);

}  // namespace RouteConfig
