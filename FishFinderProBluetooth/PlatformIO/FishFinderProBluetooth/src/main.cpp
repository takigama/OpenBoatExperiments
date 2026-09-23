#include <Arduino.h>
#include <NimBLEDevice.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <string.h>

#include "debug_log.h"
#include "net_config.h"
#include "ota_manager.h"
#include "status.h"
#include "web_config.h"
#include "wifi_manager.h"

// GATT layout of the "Fish Helper Pro" castable sonar - see ../README.md.
// FFF1 is the only thing this firmware touches so far: subscribe, receive
// notifications, reassemble frames, publish raw hex. FFF2 (probably the
// command/settings channel) and the device-information strings are next
// steps, not implemented here yet.
constexpr const char *kDeviceName = "Fish Helper Pro";
constexpr const char *kServiceUuid = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr const char *kDataCharUuid = "0000fff1-0000-1000-8000-00805f9b34fb";

// One frame: "SF" sync, header, 115 amplitude bins, AA 55 AA 55 AA
// trailer - see ../README.md's "Frame format" section for the byte
// layout. Nothing here decodes the header/bins yet - that's step 4 in the
// README, deliberately deferred until there's a real wet capture to
// decode against.
constexpr size_t kFrameLen = 140;

// Check for an OTA update once, ~30s after boot (WiFi needs a moment to
// settle) - same pattern as Esp32RaymarineSeatalk. Logged only; applying
// still needs a web UI click.
constexpr uint32_t kBootCheckDelayMs = 30000;
bool s_bootCheckDone = false;

WiFiClient s_wifiClient;
PubSubClient s_mqtt(s_wifiClient);
uint32_t s_lastMqttAttempt = 0;
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
bool s_mqttServerSet = false;

NimBLEClient *s_bleClient = nullptr;
const NimBLEAdvertisedDevice *s_targetDevice = nullptr;
bool s_doConnect = false;
bool s_bleConnected = false;
uint32_t s_frameCount = 0;

// Rolling reassembly buffer - BLE notifications arrive as 20-byte chunks
// with no framing of their own, so frames get pieced back together here
// and resynced on the "SF" marker whenever something doesn't line up
// (corruption, or the very first notification after subscribing landing
// mid-frame).
constexpr size_t kBufCap = 512;
uint8_t s_buf[kBufCap];
size_t s_bufLen = 0;

void publishRawFrame(const uint8_t *data, size_t len) {
    s_frameCount++;
    if (!s_mqtt.connected()) return;
    String hex;
    hex.reserve(len * 3);
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x10) hex += '0';
        hex += String(data[i], HEX);
        hex += ' ';
    }
    String topic = NetConfig::mqttBaseTopic() + "/raw";
    s_mqtt.publish(topic.c_str(), hex.c_str());
}

// Finds the next "SF" occurrence in s_buf starting at `from`, returns
// s_bufLen if there isn't one (i.e. nothing to resync to yet).
size_t findSync(size_t from) {
    if (from >= s_bufLen || s_bufLen < 2) return s_bufLen;
    for (size_t i = from; i + 1 < s_bufLen; i++) {
        if (s_buf[i] == 0x53 && s_buf[i + 1] == 0x46) return i;
    }
    return s_bufLen;
}

void discardTo(size_t i) {
    if (i == 0) return;
    memmove(s_buf, s_buf + i, s_bufLen - i);
    s_bufLen -= i;
}

// Drains as many complete, trailer-valid frames out of s_buf as it can.
void tryExtractFrames() {
    for (;;) {
        size_t sync = findSync(0);
        if (sync != 0) {
            discardTo(sync);
            if (s_bufLen < 2) return;  // nothing left worth searching further
        }
        if (s_bufLen < kFrameLen) return;  // have a sync, not a full frame yet

        bool trailerOk = s_buf[135] == 0xAA && s_buf[136] == 0x55 && s_buf[137] == 0xAA &&
                          s_buf[138] == 0x55 && s_buf[139] == 0xAA;
        if (trailerOk) {
            publishRawFrame(s_buf, kFrameLen);
            discardTo(kFrameLen);
            continue;
        }
        // Trailer didn't match - this "SF" wasn't a real frame start (or
        // the frame got corrupted mid-stream). Resync past it rather than
        // blindly dropping a fixed 140 bytes.
        size_t nextSync = findSync(1);
        if (nextSync >= s_bufLen) {
            s_bufLen = 0;
            return;
        }
        discardTo(nextSync);
    }
}

void onNotify(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
    if (s_bufLen + length > kBufCap) {
        // Overflow guard - shouldn't happen at this data rate (~630B/s),
        // but don't corrupt memory if something stalls MQTT/publish and
        // notifications keep arriving.
        s_bufLen = 0;
    }
    memcpy(s_buf + s_bufLen, data, length);
    s_bufLen += length;
    tryExtractFrames();
}

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *device) override {
        if (device->getName() != kDeviceName) return;
        DebugLog::logf("ble: found %s (%s)", kDeviceName, device->getAddress().toString().c_str());
        NimBLEDevice::getScan()->stop();
        s_targetDevice = device;
        s_doConnect = true;
    }
};

class ConnCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *) override {
        DebugLog::logf("ble: disconnected, rescanning");
        s_bleConnected = false;
        s_bufLen = 0;  // don't try to stitch a frame across a reconnect
        NimBLEDevice::getScan()->start(0, nullptr, false);
    }
};

// Fires if a scan ever actually completes (it shouldn't - duration 0
// means unlimited) - not expected in normal operation, just here so the
// 3-arg async overload of start() is the one that gets called. See the
// header comment on why this matters: the 2-arg overload looks almost
// identical but blocks forever, which hung the whole board during BLE
// bring-up until this was caught via isolation testing on real hardware.
void onScanComplete(NimBLEScanResults) { DebugLog::logf("ble: scan completed unexpectedly, restarting"); }

void startScan() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    static ScanCallbacks scanCallbacks;
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks);
    scan->setInterval(45);
    scan->setWindow(15);
    scan->setActiveScan(true);
    scan->start(0, onScanComplete, false);
    DebugLog::logf("ble: scanning...");
}

void connectToFishFinder() {
    s_doConnect = false;
    if (!s_bleClient) {
        s_bleClient = NimBLEDevice::createClient();
        static ConnCallbacks connCallbacks;
        s_bleClient->setClientCallbacks(&connCallbacks, false);
    }
    DebugLog::logf("ble: connecting...");
    if (!s_bleClient->connect(s_targetDevice)) {
        DebugLog::logf("ble: connect failed, rescanning");
        NimBLEDevice::getScan()->start(0, onScanComplete, false);
        return;
    }
    NimBLERemoteService *service = s_bleClient->getService(kServiceUuid);
    if (!service) {
        DebugLog::logf("ble: service fff0 not found");
        s_bleClient->disconnect();
        return;
    }
    NimBLERemoteCharacteristic *chr = service->getCharacteristic(kDataCharUuid);
    if (!chr || !chr->canNotify()) {
        DebugLog::logf("ble: characteristic fff1 not notifiable");
        s_bleClient->disconnect();
        return;
    }
    chr->subscribe(true, onNotify);
    s_bleConnected = true;
    DebugLog::logf("ble: connected, subscribed to fff1");
}

void mqttTick() {
    if (NetConfig::mqttHost().isEmpty()) return;  // not configured yet
    if (!s_mqttServerSet) {
        s_mqtt.setServer(NetConfig::mqttHost().c_str(), NetConfig::mqttPort());
        s_mqttServerSet = true;
    }
    if (s_mqtt.connected()) {
        s_mqtt.loop();
        return;
    }
    if (millis() - s_lastMqttAttempt < kMqttReconnectIntervalMs) return;
    s_lastMqttAttempt = millis();

    String clientId = "fishfinder-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (s_mqtt.connect(clientId.c_str())) {
        DebugLog::logf("mqtt: connected");
    } else {
        DebugLog::logf("mqtt: connect failed, state=%d", s_mqtt.state());
    }
}

namespace Status {
bool bleConnected() { return s_bleConnected; }
uint32_t frameCount() { return s_frameCount; }
}  // namespace Status

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::logf("FishFinderProBluetooth build %d booted", FW_BUILD);

    // BLE before WiFi - see WifiManager/wifi_manager.cpp's own comment on
    // the setSleep(false) fix; init order here doesn't matter for that
    // specific bug, but there's no reason to reorder now that it's fixed.
    NimBLEDevice::init("");
    startScan();

    WifiManager::begin();
    NetConfig::begin();

    WebConfig::begin();
}

void loop() {
    WebConfig::handleClient();
    mqttTick();
    if (s_doConnect) connectToFishFinder();

    if (!s_bootCheckDone && WifiManager::currentMode() == WifiManager::Mode::STA &&
        millis() > kBootCheckDelayMs) {
        s_bootCheckDone = true;
        OtaManager::checkForUpdate();  // logged only for now; web UI drives the actual apply step
    }

    // Periodic liveness line - a board that's silently stuck (or just has
    // nothing else to report) still shows something within a few seconds
    // of any serial connection being opened, rather than looking dead.
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        DebugLog::logf("status: wifi=%d ble=%d mqtt=%d frames=%u heap=%u", WiFi.status(), s_bleConnected,
                        s_mqtt.connected(), s_frameCount, ESP.getFreeHeap());
    }
}
