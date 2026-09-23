#include "web_config.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "debug_log.h"
#include "demo_mode.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "route_config.h"
#include "seatalk_bus.h"
#include "seatalk_decode.h"
#include "signalk_manager.h"
#include "wifi_manager.h"

namespace WebConfig {

namespace {

WebServer server(80);
OtaManager::UpdateInfo s_lastCheck;  // result of the last /ota/check, consumed by /ota/apply
bool s_navCycling = false;           // see sendNavTestValues()/tick()
uint32_t s_lastNavSend = 0;

// The routing matrix's columns (the 6 configurable RouteConfig pairs) and
// rows (the object types SeatalkEncode can actually put onto SeaTalk -
// same list as DemoMode::Object, "Position" standing in for Latitude+
// Longitude together - see routeSection()/handleRouteSave()).
struct RouteColumn {
    RouteConfig::Bus source;
    RouteConfig::Bus dest;
    const char *label;
};
const RouteColumn kRouteColumns[] = {
    {RouteConfig::Bus::Mqtt, RouteConfig::Bus::SeaTalk, "MQTT&rarr;SeaTalk"},
    {RouteConfig::Bus::SignalK, RouteConfig::Bus::SeaTalk, "SignalK&rarr;SeaTalk"},
    {RouteConfig::Bus::Can, RouteConfig::Bus::SeaTalk, "CAN&rarr;SeaTalk"},
    {RouteConfig::Bus::SeaTalk, RouteConfig::Bus::Can, "SeaTalk&rarr;CAN"},
    {RouteConfig::Bus::Mqtt, RouteConfig::Bus::Can, "MQTT&rarr;CAN"},
    {RouteConfig::Bus::SignalK, RouteConfig::Bus::Can, "SignalK&rarr;CAN"},
};
constexpr int kRouteColumnCount = sizeof(kRouteColumns) / sizeof(kRouteColumns[0]);

struct RouteRow {
    const char *label;
    SeatalkDecode::Type type;
    bool isPosition;  // also toggles Longitude alongside Latitude - see handleRouteSave()
};
const RouteRow kRouteRows[] = {
    {"Depth", SeatalkDecode::Type::Depth, false},
    {"Speed through water", SeatalkDecode::Type::SpeedThroughWater, false},
    {"Apparent wind angle", SeatalkDecode::Type::ApparentWindAngle, false},
    {"Apparent wind speed", SeatalkDecode::Type::ApparentWindSpeed, false},
    {"Water temperature", SeatalkDecode::Type::WaterTemperature, false},
    {"Position (lat/lon)", SeatalkDecode::Type::Latitude, true},
    {"Course over ground", SeatalkDecode::Type::CourseOverGround, false},
    {"Speed over ground", SeatalkDecode::Type::SpeedOverGround, false},
    {"Heading + rudder", SeatalkDecode::Type::HeadingAndRudder, false},
    {"GNSS time (UTC)", SeatalkDecode::Type::GnssTime, false},
    {"GNSS date (UTC)", SeatalkDecode::Type::GnssDate, false},
};
constexpr int kRouteRowCount = sizeof(kRouteRows) / sizeof(kRouteRows[0]);

String htmlEscape(const String &s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    out.replace("\"", "&quot;");
    return out;
}

String pageWrap(const String &title, const String &body) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + title + "</title></head><body style='font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em'>";
    html += "<h2>ESP32Seatalk</h2>";
    html += body;
    html += "</body></html>";
    return html;
}

String wifiJoinForm() {
    // Synchronous scan - blocks a couple seconds, acceptable for a page
    // that's loaded once during setup rather than polled.
    int n = WiFi.scanNetworks();
    String options;
    for (int i = 0; i < n; i++) {
        options += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>" + htmlEscape(WiFi.SSID(i)) +
                   " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();

    String body = "<p>In setup mode - connected to <b>" + WifiManager::apSsid() +
                  "</b>. Pick a network to join:</p>";
    body += "<form method='POST' action='/wifi/save'>";
    body += "<select name='ssid' style='width:100%;padding:.5em;margin:.3em 0'>" + options + "</select>";
    body += "<input name='pass' type='password' placeholder='Password' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.5em'>Join &amp; restart</button>";
    body += "</form>";
    return body;
}

String otaSection() {
    String body = "<hr><h3>Firmware</h3><p>Running build " + String(FW_BUILD) + "</p>";
    if (s_lastCheck.available) {
        body += "<p>Update available: build " + String(s_lastCheck.build) + "</p>";
        body += "<a href='/ota/apply'><button style='width:100%;padding:.6em'>Update now</button></a>";
    } else {
        body += "<a href='/ota/check'><button style='width:100%;padding:.6em'>Check for updates</button></a>";
    }
    // Direct local upload, alongside the GitHub-manifest path above, not
    // instead of it - GitHub Releases stays the real distribution
    // mechanism (and the only one once this is out of dev and unattended
    // on a boat), this is purely a fast path for iterating during
    // development without a publish+CDN-cache round trip each time. No
    // MD5 check on this path - it's a direct, deliberate local upload
    // over the LAN, not a fetch from the open internet, so the same
    // integrity concern the GitHub path (see ota_manager.cpp) exists for
    // doesn't really apply here.
    body += "<form method='POST' action='/ota/upload' enctype='multipart/form-data' style='margin-top:.5em'>";
    body += "<input type='file' name='firmware' accept='.bin' style='width:100%'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.3em'>Upload firmware directly</button>";
    body += "</form>";
    return body;
}

String mqttSection() {
    String body = "<hr><h3>MQTT</h3>";
    if (!MqttManager::configHost().isEmpty()) {
        body += "<p>" + MqttManager::configHost() + ":" + String(MqttManager::configPort()) + ", base topic \"" +
                htmlEscape(MqttManager::configBaseTopic()) + "\" - " +
                (MqttManager::isConnected() ? "<b>connected</b>" : "not connected") + "</p>";
    } else {
        body += "<p>Not configured.</p>";
    }
    body += "<form method='POST' action='/mqtt/save'>";
    body += "<input name='host' placeholder='Broker host/IP' value='" + htmlEscape(MqttManager::configHost()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<input name='port' type='number' placeholder='Port' value='" + String(MqttManager::configPort()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<input name='base' placeholder='Base topic' value='" + htmlEscape(MqttManager::configBaseTopic()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em'>Save</button>";
    body += "</form>";
    return body;
}

String signalkSection() {
    String body = "<hr><h3>SignalK</h3>";
    if (!SignalKManager::configHost().isEmpty()) {
        body += "<p>" + SignalKManager::configHost() + ":" + String(SignalKManager::configPort()) + " - " +
                (SignalKManager::isConnected() ? "<b>connected</b>" : "not connected") + "</p>";
    } else {
        body += "<p>Not configured.</p>";
    }
    body += "<form method='POST' action='/signalk/save'>";
    body += "<input name='host' placeholder='Server host/IP' value='" + htmlEscape(SignalKManager::configHost()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<input name='port' type='number' placeholder='Port' value='" + String(SignalKManager::configPort()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em'>Save</button>";
    body += "</form>";
    return body;
}

String routeSection() {
    String body = "<hr><h3>Routing</h3>";
    body += "<p style='font-size:.85em;color:#666'>SeaTalk and CAN always relay to MQTT/SignalK when "
            "connected. Check a box below to also inject that data onto SeaTalk and/or CAN, from a given "
            "source.</p>";
    body += "<form method='POST' action='/route/save'>";
    body += "<div style='overflow-x:auto'><table style='border-collapse:collapse;font-size:.8em;width:100%'>";
    body += "<tr><th style='text-align:left;padding:.3em'>Object</th>";
    for (int c = 0; c < kRouteColumnCount; c++) {
        body += "<th style='padding:.3em'>" + String(kRouteColumns[c].label) + "</th>";
    }
    body += "</tr>";
    for (int r = 0; r < kRouteRowCount; r++) {
        body += "<tr><td style='padding:.3em;border-top:1px solid #ddd'>" + String(kRouteRows[r].label) + "</td>";
        for (int c = 0; c < kRouteColumnCount; c++) {
            bool checked = RouteConfig::isAllowed(kRouteColumns[c].source, kRouteColumns[c].dest, kRouteRows[r].type);
            String name = "r" + String(r) + "_" + String(c);
            body += "<td style='text-align:center;padding:.3em;border-top:1px solid #ddd'><input "
                    "type='checkbox' name='" +
                    name + "'" + (checked ? " checked" : "") + "></td>";
        }
        body += "</tr>";
    }
    body += "</table></div>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.5em'>Save routing</button>";
    body += "</form>";
    return body;
}

String demoSection() {
    String body = "<hr><h3>Demo mode</h3>";
    DemoMode::Mode mode = DemoMode::currentMode();

    if (mode != DemoMode::Mode::Off) {
        String label = mode == DemoMode::Mode::Cycling ? "Cycling" : "Manual";
        body += "<p>Running (" + label + ") - sending checked objects once/sec.</p>";
        body += "<a href='/demo/stop'><button style='width:100%;padding:.6em'>Stop demo</button></a>";
        return body;
    }

    // Cycling: checkboxes only, values come from the simulation.
    body += "<form method='POST' action='/demo/start-cycling'>";
    for (int i = 0; i < (int)DemoMode::Object::Count; i++) {
        auto obj = (DemoMode::Object)i;
        String name = "obj" + String(i);
        body += "<label style='display:block;margin:.2em 0'><input type='checkbox' name='" + name + "'" +
                (DemoMode::isEnabled(obj) ? " checked" : "") + "> " + DemoMode::objectName(obj) + "</label>";
    }
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.3em'>Start cycling</button>";
    body += "</form>";

    // Manual: same checkbox set, but each enabled object also gets 1 or 2
    // number fields for the fixed value(s) it'll send every second.
    body += "<h4 style='margin-top:1em'>Or send fixed values</h4>";
    body += "<form method='POST' action='/demo/start-manual'>";
    for (int i = 0; i < (int)DemoMode::Object::Count; i++) {
        auto obj = (DemoMode::Object)i;
        String base = "manual_obj" + String(i);
        body += "<div style='margin:.4em 0'><label><input type='checkbox' name='" + base + "'" +
                (DemoMode::isEnabled(obj) ? " checked" : "") + "> " + DemoMode::objectName(obj) + "</label> ";
        body += "<input type='number' step='0.1' name='" + base + "_v0' style='width:5em'>";
        if (DemoMode::valueCount(obj) == 2) {
            body += " <input type='number' step='0.1' name='" + base + "_v1' style='width:5em'>";
        }
        body += "</div>";
    }
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.3em'>Start sending fixed values</button>";
    body += "</form>";
    return body;
}

void handleRoot() {
    String body;
    if (WifiManager::currentMode() == WifiManager::Mode::AP) {
        body = wifiJoinForm();
    } else {
        body = "<p>Joined WiFi. IP: <b>" + WiFi.localIP().toString() + "</b></p>";
        body += otaSection();
        body += mqttSection();
        body += signalkSection();
        body += routeSection();
    }
    if (WifiManager::currentMode() == WifiManager::Mode::STA) {
        body += "<hr><p><a href='/seatalk/test-lamp'><button style='width:100%;padding:.6em'>"
                "Test: cycle instrument lamp</button></a></p>";
        if (s_navCycling) {
            body += "<p><a href='/seatalk/test-nav-data/stop'><button style='width:100%;padding:.6em'>"
                    "Stop: sending wind/speed/depth</button></a></p>";
        } else {
            body += "<p><a href='/seatalk/test-nav-data/start'><button style='width:100%;padding:.6em'>"
                    "Test: cycle wind/speed/depth</button></a></p>";
        }
        body += demoSection();
    }
    // No USB once this is plugged into a real SeaTalk bus (it shares 3.3V
    // with the bus itself) - this page is the only diagnostic surface
    // that'll exist in the field, so the log link belongs on every page,
    // not just once things go wrong.
    body += "<hr><p><a href='/log'>View debug log</a></p>";
    server.send(200, "text/html", pageWrap("ESP32Seatalk setup", body));
}

void handleLog() {
    server.send(200, "text/plain; charset=utf-8", DebugLog::recentLines());
}

// Quick physical-confirmation trigger for testing TX against a real
// instrument: cycles the lamp through off/1/2/3 with pauses, so a visible
// change happens regardless of whatever level it started at. Command 0x30
// "Set Lamp Intensity" - see seatalk_decode.cpp's reference-doc comment
// convention; this one isn't decoded (it's a command we send, not receive)
// so it's not in that module, just sent directly here.
void handleTestLamp() {
    server.send(200, "text/html",
                pageWrap("Testing lamp", "<p>Cycling lamp levels - watch the instrument...</p>"));
    const uint8_t levels[] = {0x00, 0x04, 0x08, 0x0C, 0x00};
    for (uint8_t level : levels) {
        uint8_t data[] = {0x00, level};
        SeatalkBus::send(0x30, data, sizeof(data));
        DebugLog::logf("seatalk: sent lamp level 0x%02X", level);
        delay(1200);
    }
}

// Injects fixed, easy-to-recognize values for the 4 readings the Wind/
// Tridata units on the bus right now have no transducer for, so their
// displays have nothing of their own to show - if these numbers show up
// there, it confirms both our TX encoding *and* the reference formulas
// against a real second implementation (not just our own decoder talking
// to itself, which self-loopback can't tell apart from "we encoded and
// decoded the same wrong thing"). Sent as a continuous ~1/sec cycle (see
// tick()) rather than a one-shot burst - real transducers stream
// continuously, and a lone datagram may just get timed out by the
// display before it's even noticed (a single send showed wind speed but
// not the other three, first time this ran).
void sendNavTestValues() {
    // Apparent wind angle 45.0deg: "10 01 XX YY", XXYY/2 - raw=90=0x005A
    uint8_t wind_angle[] = {0x01, 0x5A, 0x00};
    SeatalkBus::send(0x10, wind_angle, sizeof(wind_angle));

    // Apparent wind speed 12.5kn: "11 01 XX 0Y", (XX&0x7F)+Y/10
    uint8_t wind_speed[] = {0x01, 0x0C, 0x05};
    SeatalkBus::send(0x11, wind_speed, sizeof(wind_speed));

    // Speed through water 6.5kn: "20 01 XX XX", XXXX/10 - raw=65=0x0041
    uint8_t boat_speed[] = {0x01, 0x41, 0x00};
    SeatalkBus::send(0x20, boat_speed, sizeof(boat_speed));

    // Depth below transducer 15.5ft: "00 02 YZ XX XX", XXXX/10 - raw=155=0x009B
    uint8_t depth[] = {0x02, 0x00, 0x9B, 0x00};
    SeatalkBus::send(0x00, depth, sizeof(depth));

    DebugLog::logf("seatalk: sent nav test cycle (wind 45.0deg/12.5kn, speed 6.5kn, depth 15.5ft)");
}

void handleTestNavDataStart() {
    s_navCycling = true;
    s_lastNavSend = 0;  // fire immediately rather than waiting a full interval
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleTestNavDataStop() {
    s_navCycling = false;
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleDemoStartCycling() {
    for (int i = 0; i < (int)DemoMode::Object::Count; i++) {
        DemoMode::setEnabled((DemoMode::Object)i, server.hasArg("obj" + String(i)));
    }
    DemoMode::startCycling();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleDemoStartManual() {
    for (int i = 0; i < (int)DemoMode::Object::Count; i++) {
        auto obj = (DemoMode::Object)i;
        String base = "manual_obj" + String(i);
        DemoMode::setEnabled(obj, server.hasArg(base));
        double v0 = server.hasArg(base + "_v0") ? server.arg(base + "_v0").toDouble() : 0;
        double v1 = server.hasArg(base + "_v1") ? server.arg(base + "_v1").toDouble() : 0;
        DemoMode::setManualValue(obj, v0, v1);
    }
    DemoMode::startManual();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleDemoStop() {
    DemoMode::stop();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleMqttSave() {
    String host = server.hasArg("host") ? server.arg("host") : "";
    uint16_t port = server.hasArg("port") ? (uint16_t)server.arg("port").toInt() : 1883;
    String base = server.hasArg("base") ? server.arg("base") : "";
    MqttManager::saveConfig(host, port, base);
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleSignalkSave() {
    String host = server.hasArg("host") ? server.arg("host") : "";
    uint16_t port = server.hasArg("port") ? (uint16_t)server.arg("port").toInt() : 3000;
    SignalKManager::saveConfig(host, port);
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleRouteSave() {
    for (int r = 0; r < kRouteRowCount; r++) {
        for (int c = 0; c < kRouteColumnCount; c++) {
            String name = "r" + String(r) + "_" + String(c);
            bool checked = server.hasArg(name);
            RouteConfig::setAllowed(kRouteColumns[c].source, kRouteColumns[c].dest, kRouteRows[r].type, checked);
            if (kRouteRows[r].isPosition) {
                RouteConfig::setAllowed(kRouteColumns[c].source, kRouteColumns[c].dest, SeatalkDecode::Type::Longitude,
                                         checked);
            }
        }
    }
    RouteConfig::persist();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleWifiSave() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/plain", "missing ssid");
        return;
    }
    String ssid = server.arg("ssid");
    String pass = server.hasArg("pass") ? server.arg("pass") : "";
    server.send(200, "text/html",
                pageWrap("Restarting...", "<p>Saved. Restarting to join <b>" + htmlEscape(ssid) + "</b>...</p>"));
    WifiManager::saveCredentialsAndReboot(ssid, pass);  // does not return
}

void handleOtaCheck() {
    s_lastCheck = OtaManager::checkForUpdate();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleOtaApply() {
    if (!s_lastCheck.available) {
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }
    server.send(200, "text/html", pageWrap("Updating...", "<p>Downloading and flashing build " +
                                                                String(s_lastCheck.build) +
                                                                "... device will restart on success.</p>"));
    OtaManager::applyUpdate(s_lastCheck);  // reboots on success; on failure, falls through
    s_lastCheck = OtaManager::UpdateInfo{};
}

// Runs once the whole upload request has been received - just reports
// what handleOtaUploadChunk() below already did and reboots on success.
void handleOtaUploadDone() {
    if (Update.hasError()) {
        server.send(200, "text/html",
                     pageWrap("Upload failed", "<p>Nothing was changed - still running build " +
                                                    String(FW_BUILD) + ".</p>"));
        return;
    }
    server.send(200, "text/html", pageWrap("Upload OK", "<p>Flashed OK, restarting...</p>"));
    delay(500);
    ESP.restart();
}

// Streams in as the upload arrives - WebServer's two-callback upload
// pattern (see begin()'s server.on() call below): this one fires
// repeatedly as chunks come in, handleOtaUploadDone() above fires once
// after the full request completes.
void handleOtaUploadChunk() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        DebugLog::logf("ota: direct upload starting: %s", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            DebugLog::logf("ota: Update.begin() failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            DebugLog::logf("ota: Update.write() failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            DebugLog::logf("ota: direct upload OK, %u bytes, restarting", upload.totalSize);
        } else {
            DebugLog::logf("ota: Update.end() failed: %s", Update.errorString());
        }
    }
}

}  // namespace

void begin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/wifi/save", HTTP_POST, handleWifiSave);
    server.on("/mqtt/save", HTTP_POST, handleMqttSave);
    server.on("/signalk/save", HTTP_POST, handleSignalkSave);
    server.on("/route/save", HTTP_POST, handleRouteSave);
    server.on("/ota/check", HTTP_GET, handleOtaCheck);
    server.on("/ota/apply", HTTP_GET, handleOtaApply);
    server.on("/ota/upload", HTTP_POST, handleOtaUploadDone, handleOtaUploadChunk);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/seatalk/test-lamp", HTTP_GET, handleTestLamp);
    server.on("/seatalk/test-nav-data/start", HTTP_GET, handleTestNavDataStart);
    server.on("/seatalk/test-nav-data/stop", HTTP_GET, handleTestNavDataStop);
    server.on("/demo/start-cycling", HTTP_POST, handleDemoStartCycling);
    server.on("/demo/start-manual", HTTP_POST, handleDemoStartManual);
    server.on("/demo/stop", HTTP_GET, handleDemoStop);
    server.begin();
    DebugLog::logf("web: config server listening on port 80");
}

void handleClient() { server.handleClient(); }

void tick() {
    if (!s_navCycling) return;
    if (millis() - s_lastNavSend < 1000) return;
    s_lastNavSend = millis();
    sendNavTestValues();
}

}  // namespace WebConfig
