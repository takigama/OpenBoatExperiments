#include "mqtt_manager.h"

#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "debug_log.h"

namespace MqttManager {

namespace {

constexpr const char *kPrefsNamespace = "mqtt";
constexpr uint32_t kReconnectIntervalMs = 5000;

WiFiClient s_wifiClient;
PubSubClient s_client(s_wifiClient);

String s_host;
uint16_t s_port = 1883;
String s_baseTopic = "esp32seatalk";
uint32_t s_lastReconnectAttempt = 0;

Preferences prefs() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    return p;
}

void loadConfig() {
    Preferences p = prefs();
    s_host = p.getString("host", "");
    s_port = p.getUShort("port", 1883);
    s_baseTopic = p.getString("base", "esp32seatalk");
    p.end();
}

void applyConfig() {
    if (s_host.isEmpty()) return;
    s_client.setServer(s_host.c_str(), s_port);
}

// SignalK-style dot-path per object, written with slashes since that's
// what MQTT topics actually use - see mqtt_manager.h. Matches the
// mapping table worked out in the design discussion; a couple of types
// (HeadingAndRudder, GnssDate) need special handling in publishDecoded()
// below rather than a single path each.
const char *pathFor(SeatalkDecode::Type type) {
    switch (type) {
        case SeatalkDecode::Type::Depth: return "environment/depth/belowTransducer";
        case SeatalkDecode::Type::SpeedThroughWater: return "navigation/speedThroughWater";
        case SeatalkDecode::Type::TripLog: return "navigation/trip/log";
        case SeatalkDecode::Type::TotalLog: return "navigation/log";
        case SeatalkDecode::Type::ApparentWindAngle: return "environment/wind/angleApparent";
        case SeatalkDecode::Type::ApparentWindSpeed: return "environment/wind/speedApparent";
        case SeatalkDecode::Type::WaterTemperature: return "environment/water/temperature";
        case SeatalkDecode::Type::Latitude: return "navigation/position/latitude";
        case SeatalkDecode::Type::Longitude: return "navigation/position/longitude";
        case SeatalkDecode::Type::SpeedOverGround: return "navigation/speedOverGround";
        case SeatalkDecode::Type::CourseOverGround: return "navigation/courseOverGroundTrue";
        case SeatalkDecode::Type::GnssTime: return "navigation/datetime/secondsSinceMidnight";
        case SeatalkDecode::Type::SatelliteCount: return "navigation/gnss/satellites";
        case SeatalkDecode::Type::MagneticVariation: return "navigation/magneticVariation";
        default: return nullptr;  // HeadingAndRudder, GnssDate - handled specially below
    }
}

void publishValue(const String &subPath, const String &payload) {
    if (!s_client.connected()) return;
    String topic = s_baseTopic + "/" + subPath;
    s_client.publish(topic.c_str(), payload.c_str());
}

}  // namespace

void begin() {
    loadConfig();
    applyConfig();
}

void tick() {
    if (s_host.isEmpty()) return;  // not configured yet

    if (s_client.connected()) {
        s_client.loop();
        return;
    }

    if (millis() - s_lastReconnectAttempt < kReconnectIntervalMs) return;
    s_lastReconnectAttempt = millis();

    String clientId = "esp32seatalk-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (s_client.connect(clientId.c_str())) {
        DebugLog::logf("mqtt: connected to %s:%u", s_host.c_str(), s_port);
    } else {
        DebugLog::logf("mqtt: connect failed, state=%d", s_client.state());
    }
}

bool isConnected() { return s_client.connected(); }

void saveConfig(const String &host, uint16_t port, const String &baseTopic) {
    Preferences p = prefs();
    p.putString("host", host);
    p.putUShort("port", port);
    p.putString("base", baseTopic);
    p.end();

    s_host = host;
    s_port = port;
    s_baseTopic = baseTopic.isEmpty() ? "esp32seatalk" : baseTopic;
    s_client.disconnect();
    applyConfig();
    DebugLog::logf("mqtt: config saved (%s:%u, base \"%s\")", s_host.c_str(), s_port, s_baseTopic.c_str());
}

String configHost() { return s_host; }
uint16_t configPort() { return s_port; }
String configBaseTopic() { return s_baseTopic; }

void publishDecoded(const SeatalkBus::Datagram &dg, const SeatalkDecode::Event &ev) {
    if (ev.type == SeatalkDecode::Type::HeadingAndRudder) {
        publishValue("navigation/headingMagnetic", String(ev.value, 4));
        publishValue("steering/rudderAngle", String(ev.value2, 4));
        return;
    }
    if (ev.type == SeatalkDecode::Type::GnssDate) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ev.year, ev.month, ev.day);
        publishValue("navigation/datetime/date", buf);
        return;
    }
    const char *path = pathFor(ev.type);
    if (!path) return;
    publishValue(path, String(ev.value, 4));
}

void publishRaw(const SeatalkBus::Datagram &dg) {
    if (dg.length == 0) return;
    String hex;
    for (int i = 0; i < dg.length; i++) {
        if (dg.bytes[i] < 0x10) hex += '0';
        hex += String(dg.bytes[i], HEX);
        hex += ' ';
    }
    char cmdHex[3];
    snprintf(cmdHex, sizeof(cmdHex), "%02X", dg.bytes[0]);
    publishValue(String("raw/") + cmdHex, hex);
}

}  // namespace MqttManager
