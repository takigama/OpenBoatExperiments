#include "route_config.h"

#include <Preferences.h>

#include "mqtt_manager.h"
#include "n2k_manager.h"
#include "seatalk_encode.h"
#include "signalk_manager.h"

namespace RouteConfig {

namespace {

constexpr const char *kPrefsNamespace = "route";

// One bitmask per configurable pair, bit index = (int)SeatalkDecode::Type.
// Default: everything off - injecting synthetic data onto a real
// instrument network is opt-in, not a surprise default.
uint32_t s_masks[6] = {};

int pairIndex(Bus source, Bus dest) {
    if (source == Bus::SeaTalk && dest == Bus::Can) return 0;
    if (source == Bus::Can && dest == Bus::SeaTalk) return 1;
    if (source == Bus::Mqtt && dest == Bus::SeaTalk) return 2;
    if (source == Bus::Mqtt && dest == Bus::Can) return 3;
    if (source == Bus::SignalK && dest == Bus::SeaTalk) return 4;
    if (source == Bus::SignalK && dest == Bus::Can) return 5;
    return -1;
}

}  // namespace

void begin() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/true);
    for (int i = 0; i < 6; i++) {
        char key[4];
        snprintf(key, sizeof(key), "m%d", i);
        s_masks[i] = p.getUInt(key, 0);
    }
    p.end();
}

bool isConfigurable(Bus source, Bus dest) { return pairIndex(source, dest) >= 0; }

bool isAllowed(Bus source, Bus dest, SeatalkDecode::Type type) {
    int idx = pairIndex(source, dest);
    if (idx < 0) return false;
    return (s_masks[idx] & (1UL << (int)type)) != 0;
}

void setAllowed(Bus source, Bus dest, SeatalkDecode::Type type, bool allowed) {
    int idx = pairIndex(source, dest);
    if (idx < 0) return;  // not a configurable pair - silently ignored, matches isAllowed() always returning false for it
    if (allowed) {
        s_masks[idx] |= (1UL << (int)type);
    } else {
        s_masks[idx] &= ~(1UL << (int)type);
    }
}

void persist() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    for (int i = 0; i < 6; i++) {
        char key[4];
        snprintf(key, sizeof(key), "m%d", i);
        p.putUInt(key, s_masks[i]);
    }
    p.end();
}

void relay(Bus source, const SeatalkDecode::Event &ev) {
    if (source == Bus::SeaTalk || source == Bus::Can) {
        MqttManager::publishDecoded(ev);
        SignalKManager::publishDecoded(ev);
    }
    if (source != Bus::SeaTalk && isAllowed(source, Bus::SeaTalk, ev.type)) {
        SeatalkEncode::encodeAndSend(ev);
    }
    if (source != Bus::Can && isAllowed(source, Bus::Can, ev.type)) {
        N2kManager::publishDecoded(ev);
    }
}

}  // namespace RouteConfig
