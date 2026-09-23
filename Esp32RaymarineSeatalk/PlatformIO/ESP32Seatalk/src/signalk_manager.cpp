#include "signalk_manager.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "debug_log.h"
#include "mqtt_manager.h"
#include "route_config.h"

namespace SignalKManager {

namespace {

constexpr const char *kPrefsNamespace = "signalk";
constexpr uint32_t kReconnectIntervalMs = 5000;
constexpr const char *kOwnSourceLabel = "esp32seatalk";

WebSocketsClient s_ws;
String s_host;
uint16_t s_port = 3000;
bool s_connected = false;

double s_lastLat = 0, s_lastLon = 0;
bool s_hasLat = false, s_hasLon = false;

// RX-side heading/rudder combining, same reasoning as MqttManager/
// N2kManager - SignalK keeps them as two separate delta paths.
double s_rxHeading = 0, s_rxRudder = 0;
bool s_rxHasHeading = false, s_rxHasRudder = false;

Preferences prefs() {
    Preferences p;
    p.begin(kPrefsNamespace, /*readOnly=*/false);
    return p;
}

void loadConfig() {
    Preferences p = prefs();
    s_host = p.getString("host", "");
    s_port = p.getUShort("port", 3000);
    p.end();
}

// One value from an incoming delta, resolved to a Type - routes it
// through RouteConfig::relay() same as every other source. Heading and
// rudder are cached and combined before relaying (see the s_rx* fields
// above); everything else relays immediately.
void relayValue(SeatalkDecode::Type type, double value) {
    if (type == SeatalkDecode::Type::HeadingAndRudder) return;  // never resolved directly - see caller
    SeatalkDecode::Event ev;
    ev.type = type;
    ev.value = value;
    DebugLog::logf("signalk: rx type=%d value=%.3f", (int)type, value);
    RouteConfig::relay(RouteConfig::Bus::SignalK, ev);
}

void relayHeadingAndRudder() {
    SeatalkDecode::Event ev;
    ev.type = SeatalkDecode::Type::HeadingAndRudder;
    ev.value = s_rxHeading;
    ev.value2 = s_rxRudder;
    DebugLog::logf("signalk: rx heading=%.3f rudder=%.3f", s_rxHeading, s_rxRudder);
    RouteConfig::relay(RouteConfig::Bus::SignalK, ev);
}

// Parses one incoming delta message and relays whatever it understands.
// Deltas whose source is our own label are skipped - the server
// rebroadcasts every delta (including our own outbound ones) to every
// subscriber of a context, this connection included, so without this
// check our own published values would come straight back in as if an
// external client had sent them (an immediate feedback loop).
void handleDelta(uint8_t *payload, size_t length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) return;

    for (JsonVariant upd : doc["updates"].as<JsonArray>()) {
        const char *sourceLabel = upd["source"]["label"] | "";
        if (String(sourceLabel) == kOwnSourceLabel) continue;

        for (JsonVariant v : upd["values"].as<JsonArray>()) {
            const char *path = v["path"] | "";
            if (!path[0]) continue;
            String pathStr(path);

            if (pathStr == "navigation.position") {
                JsonVariant val = v["value"];
                if (val["latitude"].is<double>()) relayValue(SeatalkDecode::Type::Latitude, val["latitude"].as<double>());
                if (val["longitude"].is<double>()) relayValue(SeatalkDecode::Type::Longitude, val["longitude"].as<double>());
                continue;
            }
            if (pathStr == SeatalkDecode::kPathDatetimeDate) {
                const char *dateStr = v["value"] | "";
                int year, month, day;
                if (sscanf(dateStr, "%d-%d-%d", &year, &month, &day) == 3) {
                    SeatalkDecode::Event ev;
                    ev.type = SeatalkDecode::Type::GnssDate;
                    ev.year = year;
                    ev.month = month;
                    ev.day = day;
                    DebugLog::logf("signalk: rx date = %s", dateStr);
                    RouteConfig::relay(RouteConfig::Bus::SignalK, ev);
                }
                continue;
            }
            if (!v["value"].is<double>()) continue;  // skip other non-numeric values
            double value = v["value"].as<double>();

            if (pathStr == SeatalkDecode::kPathHeadingMagnetic) {
                s_rxHeading = value;
                s_rxHasHeading = true;
                if (s_rxHasRudder) relayHeadingAndRudder();
                continue;
            }
            if (pathStr == SeatalkDecode::kPathRudderAngle) {
                s_rxRudder = value;
                s_rxHasRudder = true;
                if (s_rxHasHeading) relayHeadingAndRudder();
                continue;
            }

            SeatalkDecode::Type type;
            if (SeatalkDecode::typeForCanonicalPath(pathStr, &type)) relayValue(type, value);
        }
    }
}

void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            s_connected = true;
            DebugLog::logf("signalk: connected to %s:%u", s_host.c_str(), s_port);
            break;
        case WStype_DISCONNECTED:
            s_connected = false;
            DebugLog::logf("signalk: disconnected");
            break;
        case WStype_TEXT:
            MqttManager::publishRawBus("signalk", payload, length);
            handleDelta(payload, length);
            break;
        default:
            break;
    }
}

void applyConfig() {
    if (s_host.isEmpty()) return;
    // subscribe=self - only our own vessel's context, not every AIS
    // target the server might be tracking. Needed for the inbound
    // direction (see handleDelta()); the module worked fine without it
    // when it only ever pushed data, but never received any.
    s_ws.begin(s_host, s_port, "/signalk/v1/stream?subscribe=self");
    s_ws.onEvent(onWsEvent);
    s_ws.setReconnectInterval(kReconnectIntervalMs);
}

void send(String json) {
    if (!s_connected) return;
    s_ws.sendTXT(json);
}

// Every delta shares this same envelope - context + one update with our
// source label - just the "values" array contents differ per call site
// below, so each of these builds its own doc rather than sharing a
// half-built one (JsonDocument isn't cheap to pass around mid-build, and
// these are small/infrequent enough that it doesn't matter).
void sendNumeric(const char *path, double value) {
    JsonDocument doc;
    doc["context"] = "vessels.self";
    JsonArray updates = doc["updates"].to<JsonArray>();
    JsonObject upd = updates.add<JsonObject>();
    upd["source"]["label"] = "esp32seatalk";
    JsonArray vals = upd["values"].to<JsonArray>();
    JsonObject v = vals.add<JsonObject>();
    v["path"] = path;
    v["value"] = value;
    String out;
    serializeJson(doc, out);
    send(out);
}

void sendString(const char *path, const String &value) {
    JsonDocument doc;
    doc["context"] = "vessels.self";
    JsonArray updates = doc["updates"].to<JsonArray>();
    JsonObject upd = updates.add<JsonObject>();
    upd["source"]["label"] = "esp32seatalk";
    JsonArray vals = upd["values"].to<JsonArray>();
    JsonObject v = vals.add<JsonObject>();
    v["path"] = path;
    v["value"] = value;
    String out;
    serializeJson(doc, out);
    send(out);
}

void sendPosition(double lat, double lon) {
    JsonDocument doc;
    doc["context"] = "vessels.self";
    JsonArray updates = doc["updates"].to<JsonArray>();
    JsonObject upd = updates.add<JsonObject>();
    upd["source"]["label"] = "esp32seatalk";
    JsonArray vals = upd["values"].to<JsonArray>();
    JsonObject v = vals.add<JsonObject>();
    v["path"] = "navigation.position";
    JsonObject pos = v["value"].to<JsonObject>();
    pos["latitude"] = lat;
    pos["longitude"] = lon;
    String out;
    serializeJson(doc, out);
    send(out);
}

void sendHeadingAndRudder(double headingRad, double rudderRad) {
    JsonDocument doc;
    doc["context"] = "vessels.self";
    JsonArray updates = doc["updates"].to<JsonArray>();
    JsonObject upd = updates.add<JsonObject>();
    upd["source"]["label"] = "esp32seatalk";
    JsonArray vals = upd["values"].to<JsonArray>();
    JsonObject v1 = vals.add<JsonObject>();
    v1["path"] = SeatalkDecode::kPathHeadingMagnetic;
    v1["value"] = headingRad;
    JsonObject v2 = vals.add<JsonObject>();
    v2["path"] = SeatalkDecode::kPathRudderAngle;
    v2["value"] = rudderRad;
    String out;
    serializeJson(doc, out);
    send(out);
}

}  // namespace

void begin() {
    loadConfig();
    applyConfig();
}

void tick() {
    if (s_host.isEmpty()) return;  // not configured yet
    s_ws.loop();
}

bool isConnected() { return s_connected; }

void sendRaw(const String &text) { send(text); }

void saveConfig(const String &host, uint16_t port) {
    Preferences p = prefs();
    p.putString("host", host);
    p.putUShort("port", port);
    p.end();

    s_host = host;
    s_port = port;
    s_connected = false;
    applyConfig();
    DebugLog::logf("signalk: config saved (%s:%u)", s_host.c_str(), s_port);
}

String configHost() { return s_host; }
uint16_t configPort() { return s_port; }

void publishDecoded(const SeatalkDecode::Event &ev) {
    switch (ev.type) {
        case SeatalkDecode::Type::Latitude:
            s_lastLat = ev.value;
            s_hasLat = true;
            if (s_hasLon) sendPosition(s_lastLat, s_lastLon);
            return;
        case SeatalkDecode::Type::Longitude:
            s_lastLon = ev.value;
            s_hasLon = true;
            if (s_hasLat) sendPosition(s_lastLat, s_lastLon);
            return;
        case SeatalkDecode::Type::HeadingAndRudder:
            sendHeadingAndRudder(ev.value, ev.value2);
            return;
        case SeatalkDecode::Type::GnssDate: {
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", ev.year, ev.month, ev.day);
            sendString(SeatalkDecode::kPathDatetimeDate, buf);
            return;
        }
        default: {
            const char *path = SeatalkDecode::canonicalPath(ev.type);
            if (!path) return;
            sendNumeric(path, ev.value);
            return;
        }
    }
}

}  // namespace SignalKManager
