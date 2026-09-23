#include "signalk_manager.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <WiFi.h>

#include "debug_log.h"

namespace SignalKManager {

namespace {

constexpr const char *kPrefsNamespace = "signalk";
constexpr uint32_t kReconnectIntervalMs = 5000;

WebSocketsClient s_ws;
String s_host;
uint16_t s_port = 3000;
bool s_connected = false;

double s_lastLat = 0, s_lastLon = 0;
bool s_hasLat = false, s_hasLon = false;

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
        default:
            break;  // not interested in incoming deltas/errors for now
    }
}

void applyConfig() {
    if (s_host.isEmpty()) return;
    s_ws.begin(s_host, s_port, "/signalk/v1/stream?subscribe=none");
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
