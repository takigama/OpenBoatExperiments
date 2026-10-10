#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include <Tobe.h>
#include <TobeLog.h>
#include <TobeWeb.h>
#include <TobeWifi.h>

#include "demo_mode.h"
#include "mqtt_manager.h"
#include "route_config.h"
#include "rx_log.h"
#include "seatalk_bus.h"
#include "seatalk_decode.h"
#include "signalk_manager.h"

namespace WebConfig {

namespace {

WebServer server(80);
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

// The page frame (dark theme, "TOBE ESP32Seatalk" heading) and escaping are TobeWeb's.
String htmlEscape(const String &s) { return tobe::web::escape(s); }

String pageWrap(const String &title, const String &body) { return tobe::web::page(title, body); }

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
    body += "<p style='font-size:.85em;color:#999'>SeaTalk and CAN always relay to MQTT/SignalK when "
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

// ---- received-messages page ------------------------------------------------------------------------------------
// /messages shows what each bus has delivered (see rx_log.h): newest first, refreshed every 2 s, filterable by bus.
//   /messages                 everything, newest 200
//   /messages?src=can         one bus: seatalk | can | mqtt | signalk
//   /messages?n=400           show more (up to RxLog::kPerSource per bus)
//   /messages?pause=1         stop the automatic refresh (to read it, or copy from it)
//   /messages.txt             the same as plain text (curl, a log file)
// The page is streamed in chunks, so it does not need the whole table in memory at once.
uint8_t messagesSourceMask() {
    String want = server.arg("src");
    for (int i = 0; i < RxLog::kSourceCount; i++) {
        if (want.equalsIgnoreCase(RxLog::sourceName((RxLog::Source)i))) return (uint8_t)(1 << i);
    }
    return (uint8_t)((1 << RxLog::kSourceCount) - 1);
}

void streamMessages(bool plain) {
    const uint8_t mask = messagesSourceMask();
    int limit = server.hasArg("n") ? server.arg("n").toInt() : 200;
    if (limit < 1) limit = 1;
    if (limit > (int)(RxLog::kPerSource * RxLog::kSourceCount)) limit = RxLog::kPerSource * RxLog::kSourceCount;

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, plain ? "text/plain; charset=utf-8" : "text/html", "");

    RxLog::Entry e;
    uint32_t before = UINT32_MAX;
    int shown = 0;

    if (plain) {
        char line[RxLog::kTextMax + 40];
        while (shown < limit && RxLog::findOlder(mask, before, &e)) {
            before = e.seq;
            snprintf(line, sizeof(line), "%lu.%03lu %-7s %s\n", (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000),
                     RxLog::sourceName(e.src), e.text);
            server.sendContent(line);
            shown++;
        }
        server.sendContent("");
        return;
    }

    const bool pause = server.hasArg("pause");
    // The links keep the current choice of bus; "n" is kept too.
    String srcArg = server.arg("src");
    String keep = (srcArg.length() ? "src=" + srcArg + "&" : String()) + (server.hasArg("n") ? "n=" + server.arg("n") + "&" : String());

    // Refreshed by a script timer (so Copy can stop it); a browser without scripts falls back to a meta refresh.
    String head = tobe::web::pageHead("Received messages",
                                      pause ? String() : String("<noscript><meta http-equiv='refresh' content='2'></noscript>"));
    head += "<p>";
    head += "<a href='/messages'>All</a>";
    for (int i = 0; i < RxLog::kSourceCount; i++) {
        const char *name = RxLog::sourceName((RxLog::Source)i);
        head += String(" &middot; <a href='/messages?src=") + name + "'>" + name + "</a>";
    }
    head += String(" &middot; ") + (pause ? "<a href='/messages?" + keep + "'>Resume</a>" : "<a href='/messages?" + keep + "pause=1'>Pause</a>");
    head += " &middot; <a href='/messages.txt?" + keep + "'>text</a> &middot; <a href='/'>back</a></p>";
    head += "<p><small>Since boot:";
    for (int i = 0; i < RxLog::kSourceCount; i++) {
        head += String(i ? "," : "") + " " + RxLog::sourceName((RxLog::Source)i) + " " + String((unsigned long)RxLog::total((RxLog::Source)i));
    }
    head += ". Newest first; each bus keeps its last " + String((unsigned)RxLog::kPerSource) + ". Times are seconds since boot.</small></p>";
    head += "<p><button id=cp style='width:auto;padding:.5em 1.2em'>Copy these messages</button></p>";
    head += "<table id=m style='font-family:monospace;font-size:.85em;width:100%'><tr><th>time<th>bus<th>message</tr>";
    server.sendContent(head);

    String chunk;
    chunk.reserve(1800);
    while (shown < limit && RxLog::findOlder(mask, before, &e)) {
        before = e.seq;
        char t[24];
        snprintf(t, sizeof(t), "%lu.%03lu", (unsigned long)(e.ms / 1000), (unsigned long)(e.ms % 1000));
        chunk += String("<tr><td>") + t + "<td>" + RxLog::sourceName(e.src) + "<td>" + htmlEscape(String(e.text)) + "</tr>";
        shown++;
        if (chunk.length() > 1500) {
            server.sendContent(chunk);
            chunk = "";
        }
    }
    chunk += "</table>";
    if (shown == 0) chunk += "<p>Nothing received yet.</p>";

    // Copy: the rows on screen (whatever bus filter / count is showing) as plain text, one per line, to the clipboard
    // of the device the page is open on. The board is reached over plain http://<ip>/, where browsers withhold
    // navigator.clipboard (secure contexts only), so there is an execCommand('copy') fallback through a hidden
    // textarea. Copying also stops the 2 s refresh, so the "Copied" note stays and the page holds still.
    static const char kCopyScript[] = R"JS(<script>
var t=%REFRESH%;
document.getElementById('cp').onclick=function(){
 var rows=document.querySelectorAll('#m tr'),out=['# '+document.title+' ('+(rows.length-1)+' messages, newest first)'],i,b=this;
 for(i=1;i<rows.length;i++){var c=rows[i].cells;out.push(c[0].textContent+' '+c[1].textContent+' '+c[2].textContent);}
 var txt=out.join('\n'),n=rows.length-1;
 function done(ok){if(t)clearTimeout(t);b.textContent=ok?('Copied '+n+' messages - refresh paused (reload to resume)'):'Copy failed - select the table text by hand';}
 function fallback(){
  var a=document.createElement('textarea');a.value=txt;a.style.position='fixed';a.style.top='0';a.style.opacity='0';
  document.body.appendChild(a);a.focus();a.select();
  var ok=false;try{ok=document.execCommand('copy');}catch(e){}
  document.body.removeChild(a);done(ok);
 }
 if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(txt).then(function(){done(true);},fallback);}
 else{fallback();}
};
</script>)JS";
    String js = kCopyScript;
    js.replace("%REFRESH%", pause ? String("0") : String("setTimeout(function(){location.reload();},2000)"));
    chunk += js;
    chunk += tobe::web::pageTail();
    server.sendContent(chunk);
    server.sendContent("");
}

void handleMessages() { streamMessages(false); }
void handleMessagesText() { streamMessages(true); }

void handleRoot() {
    String body;
    if (tobe::wifi::mode() == tobe::wifi::Mode::AP) {
        body = "<p>Setup network <b>" + tobe::wifi::apSsid() + "</b>. Join a WiFi network below to get the rest.</p>";
        body += tobe::web::systemSection();
    } else {
        body = "<p>Joined WiFi. IP: <b>" + WiFi.localIP().toString() + "</b></p>";
        body += "<p><a href='/messages'><button style='width:100%;padding:.6em'>Received messages (live)</button></a></p>";
        body += mqttSection();
        body += signalkSection();
        body += routeSection();
    }
    if (tobe::wifi::mode() == tobe::wifi::Mode::STA) {
        body += "<hr><h3>Test/Debug SeaTalk</h3>";
        body += "<p><a href='/seatalk/test-lamp'><button style='width:100%;padding:.6em'>"
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
    // No USB once this is plugged into a real SeaTalk bus (it shares 3.3V with the bus itself) - this page is
    // the only diagnostic surface that'll exist in the field, so firmware update, WiFi, the log and restart
    // (TobeWeb's system section) belong on the page in STA mode too, not just once things go wrong.
    if (tobe::wifi::mode() == tobe::wifi::Mode::STA) body += tobe::web::systemSection();
    server.send(200, "text/html", pageWrap("Status", body));
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
        tobe::logf("seatalk: sent lamp level 0x%02X", level);
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

    tobe::logf("seatalk: sent nav test cycle (wind 45.0deg/12.5kn, speed 6.5kn, depth 15.5ft)");
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

}  // namespace

void begin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/messages", HTTP_GET, handleMessages);
    server.on("/messages.txt", HTTP_GET, handleMessagesText);
    server.on("/mqtt/save", HTTP_POST, handleMqttSave);
    server.on("/signalk/save", HTTP_POST, handleSignalkSave);
    server.on("/route/save", HTTP_POST, handleRouteSave);
    server.on("/seatalk/test-lamp", HTTP_GET, handleTestLamp);
    server.on("/seatalk/test-nav-data/start", HTTP_GET, handleTestNavDataStart);
    server.on("/seatalk/test-nav-data/stop", HTTP_GET, handleTestNavDataStop);
    server.on("/demo/start-cycling", HTTP_POST, handleDemoStartCycling);
    server.on("/demo/start-manual", HTTP_POST, handleDemoStartManual);
    server.on("/demo/stop", HTTP_GET, handleDemoStop);
    tobe::web::attach(server);
    server.begin();
    tobe::logf("web: config server listening on port 80");
}

void handleClient() { server.handleClient(); }

void tick() {
    if (!s_navCycling) return;
    if (millis() - s_lastNavSend < 1000) return;
    s_lastNavSend = millis();
    sendNavTestValues();
}

}  // namespace WebConfig
