#include "mqtt_manager.h"

#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "debug_log.h"
#include "route_config.h"

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

// Inbound commands live under their own "set/" prefix, entirely separate
// from the "{base}/{path}" tree our own publishValue() writes to - so
// subscribing to "{base}/set/#" can never pick up our own outbound
// messages and bounce them back in as if they were external commands.
String setTopicFilter() { return s_baseTopic + "/set/#"; }

// Same RX-side heading/rudder combining N2kManager does - MQTT keeps
// them as two separate topics (matching how they're published), but
// SeatalkDecode::Type::HeadingAndRudder needs both in one Event.
double s_rxHeading = 0, s_rxRudder = 0;
bool s_rxHasHeading = false, s_rxHasRudder = false;

void handleMessage(char *topic, uint8_t *payload, unsigned int length);

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
    s_client.setCallback(handleMessage);
}

// MQTT topics mirror SeatalkDecode::canonicalPath()'s SignalK-style
// dot-path, just with slashes instead of dots (that's what MQTT topics
// actually use) - see mqtt_manager.h.
String pathFor(SeatalkDecode::Type type) {
    const char *dotPath = SeatalkDecode::canonicalPath(type);
    if (!dotPath) return String();  // HeadingAndRudder, GnssDate - handled specially below
    String path(dotPath);
    path.replace('.', '/');
    return path;
}

String slashify(const char *dotPath) {
    String path(dotPath);
    path.replace('.', '/');
    return path;
}

void publishValue(const String &subPath, const String &payload) {
    if (!s_client.connected()) return;
    String topic = s_baseTopic + "/" + subPath;
    s_client.publish(topic.c_str(), payload.c_str());
}

void handleMessage(char *topic, uint8_t *payload, unsigned int length) {
    String topicStr(topic);
    String prefix = s_baseTopic + "/set/";
    if (!topicStr.startsWith(prefix)) return;
    String subPath = topicStr.substring(prefix.length());  // e.g. "navigation/speedThroughWater"

    String payloadStr;
    payloadStr.reserve(length);
    for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];
    double value = payloadStr.toDouble();

    if (subPath == slashify(SeatalkDecode::kPathHeadingMagnetic)) {
        s_rxHeading = value;
        s_rxHasHeading = true;
        if (s_rxHasRudder) {
            SeatalkDecode::Event ev;
            ev.type = SeatalkDecode::Type::HeadingAndRudder;
            ev.value = s_rxHeading;
            ev.value2 = s_rxRudder;
            DebugLog::logf("mqtt: rx %s = %.3f", topic, value);
            RouteConfig::relay(RouteConfig::Bus::Mqtt, ev);
        }
        return;
    }
    if (subPath == slashify(SeatalkDecode::kPathRudderAngle)) {
        s_rxRudder = value;
        s_rxHasRudder = true;
        if (s_rxHasHeading) {
            SeatalkDecode::Event ev;
            ev.type = SeatalkDecode::Type::HeadingAndRudder;
            ev.value = s_rxHeading;
            ev.value2 = s_rxRudder;
            DebugLog::logf("mqtt: rx %s = %.3f", topic, value);
            RouteConfig::relay(RouteConfig::Bus::Mqtt, ev);
        }
        return;
    }
    if (subPath == slashify(SeatalkDecode::kPathDatetimeDate)) {
        int year, month, day;
        if (sscanf(payloadStr.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
            SeatalkDecode::Event ev;
            ev.type = SeatalkDecode::Type::GnssDate;
            ev.year = year;
            ev.month = month;
            ev.day = day;
            DebugLog::logf("mqtt: rx %s = %s", topic, payloadStr.c_str());
            RouteConfig::relay(RouteConfig::Bus::Mqtt, ev);
        }
        return;
    }

    String dotPath = subPath;
    dotPath.replace('/', '.');
    SeatalkDecode::Type type;
    if (!SeatalkDecode::typeForCanonicalPath(dotPath, &type)) return;  // not one we recognize - ignored

    SeatalkDecode::Event ev;
    ev.type = type;
    ev.value = value;
    DebugLog::logf("mqtt: rx %s = %.3f", topic, value);
    RouteConfig::relay(RouteConfig::Bus::Mqtt, ev);
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
        s_client.subscribe(setTopicFilter().c_str());
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

void publishDecoded(const SeatalkDecode::Event &ev) {
    if (ev.type == SeatalkDecode::Type::HeadingAndRudder) {
        publishValue(slashify(SeatalkDecode::kPathHeadingMagnetic), String(ev.value, 4));
        publishValue(slashify(SeatalkDecode::kPathRudderAngle), String(ev.value2, 4));
        return;
    }
    if (ev.type == SeatalkDecode::Type::GnssDate) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ev.year, ev.month, ev.day);
        publishValue(slashify(SeatalkDecode::kPathDatetimeDate), buf);
        return;
    }
    String path = pathFor(ev.type);
    if (path.isEmpty()) return;
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
