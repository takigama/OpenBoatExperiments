#include "sonar_ble.h"

#include <NimBLEDevice.h>
#include <string.h>

#include "debug_log.h"

namespace SonarBle {

namespace {

constexpr const char *kDeviceName = "Fish Helper Pro";
constexpr const char *kServiceUuid = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr const char *kDataCharUuid = "0000fff1-0000-1000-8000-00805f9b34fb";
constexpr size_t kFrameLen = 140;

NimBLEClient *s_bleClient = nullptr;
// A plain address, not a pointer into the scan result - see
// FishFinderProBluetooth/src/main.cpp's identical comment for why
// (NimBLEAdvertisedDevice objects can be gone/overwritten by the time
// loop() acts on s_doConnect).
NimBLEAddress s_targetAddress;
bool s_doConnect = false;
bool s_bleConnected = false;

Reading s_reading;

constexpr size_t kBufCap = 512;
uint8_t s_buf[kBufCap];
size_t s_bufLen = 0;

// Same checksum/unit-conversion formulas as FishFinderProBluetooth's
// emitDecoded() - see that project's README "Frame format" section for
// the full derivation (pulled from the vendor app's decompiled source,
// cross-checked against real captured frames). Status bitfield (byte 2)
// and battery (byte 8) weren't needed there (Serial/NMEA output only)
// but are worth showing on a screen, so they're decoded here too.
void decodeFrame(const uint8_t *data) {
    uint16_t sum = 0;
    for (int i = 0; i < 13; i++) sum += data[i];
    if ((uint8_t)sum != data[13]) return;  // corrupt header, skip

    uint16_t rawDepthTenthsFt = (data[3] << 8) | data[4];
    uint16_t rawFishDepthTenthsFt = (data[5] << 8) | data[6];
    uint16_t rawTempTenthsF = (data[9] << 8) | data[10];

    s_reading.outOfWater = data[2] & 0x08;
    s_reading.isCharging = data[2] & 0x80;
    s_reading.battery = data[8];
    s_reading.depthRangeIdx = data[12];
    s_reading.frameCount++;

    if (rawDepthTenthsFt >= 20 && rawDepthTenthsFt <= 2000) {
        s_reading.depthM = (rawDepthTenthsFt / 10.0f) * 0.3048f;
        s_reading.valid = true;
    }
    // 0 means "no fish detected" (or fish depth >= water depth) - not a
    // range-gated field like water depth above, just a plain sentinel.
    s_reading.fishValid = rawFishDepthTenthsFt != 0;
    if (s_reading.fishValid) {
        s_reading.fishDepthM = (rawFishDepthTenthsFt / 10.0f) * 0.3048f;
    }
    if (rawTempTenthsF > 0 && rawTempTenthsF <= 2000) {
        s_reading.tempC = ((int)rawTempTenthsF - 320) / 18.0f;
    }
}

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

void tryExtractFrames() {
    for (;;) {
        size_t sync = findSync(0);
        if (sync != 0) {
            discardTo(sync);
            if (s_bufLen < 2) return;
        }
        if (s_bufLen < kFrameLen) return;

        bool trailerOk = s_buf[135] == 0xAA && s_buf[136] == 0x55 && s_buf[137] == 0xAA &&
                          s_buf[138] == 0x55 && s_buf[139] == 0xAA;
        if (trailerOk) {
            decodeFrame(s_buf);
            discardTo(kFrameLen);
            continue;
        }
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
        s_bufLen = 0;  // overflow guard, shouldn't happen at this data rate
    }
    memcpy(s_buf + s_bufLen, data, length);
    s_bufLen += length;
    tryExtractFrames();
}

void startScan();  // fwd decl, needed by onScanComplete/ConnCallbacks below

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *device) override {
        if (device->getName() != kDeviceName) return;
        DebugLog::logf("ble: found %s (%s)", kDeviceName, device->getAddress().toString().c_str());
        NimBLEDevice::getScan()->stop();
        s_targetAddress = device->getAddress();
        s_doConnect = true;
    }
};

class ConnCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *) override {
        s_bleConnected = false;
        s_bufLen = 0;
        DebugLog::logf("ble: disconnected, rescanning");
        startScan();
    }
};

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
    if (!s_bleClient->connect(s_targetAddress)) {
        DebugLog::logf("ble: connect failed, rescanning");
        startScan();
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

}  // namespace

void begin() {
    NimBLEDevice::init("");
    startScan();
}

void loop() {
    if (s_doConnect) connectToFishFinder();

    // Polled rather than event-driven - NimBLEClient::getRssi() is a
    // synchronous host-level query (ble_gap_conn_rssi), not free, so it's
    // only worth doing periodically rather than every loop() iteration.
    static uint32_t lastRssiPoll = 0;
    if (s_bleConnected && millis() - lastRssiPoll >= 1000) {
        lastRssiPoll = millis();
        s_reading.rssi = s_bleClient->getRssi();
    }
}

bool connected() { return s_bleConnected; }
const Reading &latest() { return s_reading; }

}  // namespace SonarBle
