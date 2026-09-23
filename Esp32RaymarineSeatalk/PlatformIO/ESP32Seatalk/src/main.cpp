#include <Arduino.h>
#include <math.h>

#include "debug_log.h"
#include "demo_mode.h"
#include "mqtt_manager.h"
#include "n2k_manager.h"
#include "ota_manager.h"
#include "route_config.h"
#include "seatalk_bus.h"
#include "seatalk_decode.h"
#include "signalk_manager.h"
#include "web_config.h"
#include "wifi_manager.h"

// Check for an OTA update once, ~30s after a STA-mode boot (WiFi/DNS/etc
// need a moment to settle) - convenient during dev when "push a build,
// walk away, it updates itself" matters more than fine-grained scheduling.
// A real periodic re-check interval (EngineControl's HELM does ~24h) can
// come later once this loop has more than one thing to schedule.
constexpr uint32_t kBootCheckDelayMs = 30000;
bool s_bootCheckDone = false;

constexpr int kSeatalkPin = 4;  // LV_SEATALK, per the schematic - GPIO4 through the BSS138 shifter

String hexDump(const SeatalkBus::Datagram &dg) {
    String out;
    for (int i = 0; i < dg.length; i++) {
        if (dg.bytes[i] < 0x10) out += '0';
        out += String(dg.bytes[i], HEX);
        out += ' ';
    }
    return out;
}

// No live SeaTalk bus connected yet (see seatalk_bus.h) - this is the only
// validation the RX path has had so far: transmit a known depth datagram
// and confirm our own decoder reconstructs exactly the value we sent,
// looped back over the same open-drain wire. Runs once, a couple seconds
// after boot.
bool s_loopbackTestDone = false;
constexpr uint32_t kLoopbackTestDelayMs = 3000;

void runLoopbackTest() {
    // Depth below transducer: "00 02 YZ XX XX" (see seatalk_decode.cpp) -
    // dataBytes here is everything after the command byte, so it needs
    // the attribute byte (0x02) itself, not just the payload after it.
    // 12.3m -> feet*10 = 12.3/0.3048*10 = 403 = 0x0193
    uint16_t raw = (uint16_t)(12.3 / 0.3048 * 10.0 + 0.5);
    uint8_t data[] = {0x02, 0x00, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
    SeatalkBus::send(0x00, data, sizeof(data));

    uint32_t start = millis();
    SeatalkBus::Datagram dg;
    while (millis() - start < 200) {
        if (SeatalkBus::poll(&dg)) {
            String rawHex = hexDump(dg);
            SeatalkDecode::Event ev;
            if (SeatalkDecode::decode(dg, &ev) && ev.type == SeatalkDecode::Type::Depth &&
                fabs(ev.value - 12.3) < 0.05) {
                DebugLog::logf("seatalk: loopback test PASSED (sent 12.3m, decoded %.2fm, raw: %s)",
                                ev.value, rawHex.c_str());
            } else {
                DebugLog::logf("seatalk: loopback test FAILED - raw bytes: %s (len %d)", rawHex.c_str(),
                                dg.length);
            }
            return;
        }
        delay(1);
    }
    DebugLog::logf("seatalk: loopback test FAILED - nothing came back within 200ms");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::logf("ESP32Seatalk build %d booted", FW_BUILD);

    WifiManager::begin();
    WebConfig::begin();
    SeatalkBus::begin(kSeatalkPin);
    MqttManager::begin();
    SignalKManager::begin();
    N2kManager::begin();
    RouteConfig::begin();
}

void loop() {
    WebConfig::handleClient();
    WebConfig::tick();
    DemoMode::tick();
    MqttManager::tick();
    SignalKManager::tick();
    N2kManager::tick();

    if (!s_loopbackTestDone && millis() > kLoopbackTestDelayMs) {
        s_loopbackTestDone = true;
        runLoopbackTest();
    }

    // Drain everything queued, not just one per iteration - demo mode
    // alone can enqueue 9 self-echoed datagrams in a single blocking
    // tick() call (the RX queue is only 8 deep), and a single poll()
    // here let a real backlog build up: the queue filled faster than it
    // drained, so what looked like "one tick's worth" of log lines was
    // actually a stale/current mix - caught because a speed reading
    // jumped 22.7kn -> 4.0kn between consecutive log lines, which the
    // 0.5kn/sec ramp makes physically impossible in one tick.
    SeatalkBus::Datagram dg;
    while (SeatalkBus::poll(&dg)) {
        // Every received datagram gets hex-dumped, decoded or not - once
        // this is on a real bus this is the only view into traffic we
        // don't (yet) have a decoder for, and worth having regardless
        // even for ones we do decode. Same story over MQTT: raw always
        // goes out on its own topic (see MqttManager::publishRawBus()),
        // decoded values additionally go to their SignalK-style path.
        String hex = hexDump(dg);
        MqttManager::publishRawBus("seatalk", dg.bytes, dg.length);
        SeatalkDecode::Event ev;
        if (SeatalkDecode::decode(dg, &ev)) {
            DebugLog::logf("seatalk: %s-> type=%d value=%.3f value2=%.3f", hex.c_str(), (int)ev.type,
                            ev.value, ev.value2);
            RouteConfig::relay(RouteConfig::Bus::SeaTalk, ev);
        } else {
            DebugLog::logf("seatalk: %s-> undecoded", hex.c_str());
        }
    }

    if (!s_bootCheckDone && WifiManager::currentMode() == WifiManager::Mode::STA &&
        millis() > kBootCheckDelayMs) {
        s_bootCheckDone = true;
        OtaManager::checkForUpdate();  // logged only for now; web UI drives the actual apply step
    }

    // Periodic liveness line - startup-only logging is useless for anyone
    // attaching a serial monitor after the fact (native USB CDC doesn't
    // buffer for a not-yet-attached host, those lines are just gone).
    // Fast cadence to Serial only (dev-time, USB attached); a much slower
    // one also into the ring buffer - at 5s intervals a heartbeat alone
    // would cycle the whole 80-line /log buffer in under 7 minutes,
    // crowding out the WiFi/OTA/SeaTalk events that buffer actually
    // exists for.
    static uint32_t lastStatus = 0;
    static uint32_t lastStatusLogged = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        const char *mode = WifiManager::currentMode() == WifiManager::Mode::STA ? "STA" : "AP";
        Serial.printf("status: mode=%s heap=%u uptime=%lus\n", mode, ESP.getFreeHeap(), millis() / 1000);
        if (millis() - lastStatusLogged >= 60000) {
            lastStatusLogged = millis();
            DebugLog::logf("status: mode=%s heap=%u uptime=%lus", mode, ESP.getFreeHeap(), millis() / 1000);
        }
    }
}
