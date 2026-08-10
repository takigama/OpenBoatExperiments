// Minimal diagnostic build: listen for FMDN broadcasts and print them raw -
// no crypto matching, no stats, no web server, no ring/button-press testing.
// The full-featured version is full_featured.cpp in this same directory -
// each builds via its own PlatformIO environment (`-e <name>`, see
// platformio.ini), no file renaming needed to switch between them.
//
// Purpose: get a clean baseline of how often FMDN frames actually arrive at
// this ESP32 with zero processing overhead, since the full-featured build's
// own crypto-matching cost (up to ~62s of processing per ~5s scan window)
// was almost certainly the real cause of the "20-60s gaps" that looked like
// a tag/RF problem but wasn't.

#include <Arduino.h>
#include <string.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Prefixes every printed line with "[t=<ms since boot>]" - see
// full_featured.cpp for the fuller explanation of how/why this
// works (it's a Print subclass swapped in via #define Serial).
class TimestampedSerial : public Print {
public:
    void begin(unsigned long baud) { Serial.begin(baud); }

    size_t write(uint8_t c) override {
        if (at_line_start) {
            at_line_start = false;
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "[t=%8lu] ", millis());
            Serial.write((const uint8_t *)buf, n);
        }
        size_t ret = Serial.write(c);
        if (c == '\n') at_line_start = true;
        return ret;
    }

private:
    bool at_line_start = true;
};

static TimestampedSerial TSerial;
#define Serial TSerial

static const BLEUUID FMDN_SERVICE_UUID((uint16_t)0xFEAA);

class FmdnAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (!advertisedDevice.haveServiceData()) return;
        if (!advertisedDevice.getServiceDataUUID().equals(FMDN_SERVICE_UUID)) return;

        String data = advertisedDevice.getServiceData();
        if (data.length() < 21) return; // frame type (1) + EID (20)

        uint8_t frame_type = (uint8_t)data[0];
        if (frame_type != 0x40 && frame_type != 0x41) return;

        // No expensive work here at all now - safe to print directly from
        // the BLE stack's own task, unlike when this callback used to queue
        // work for ~2.1s-per-candidate crypto checks in loop().
        Serial.printf("[SEEN] addr=%s rssi=%d frameType=0x%02x eid=",
                      advertisedDevice.getAddress().toString().c_str(),
                      advertisedDevice.getRSSI(), frame_type);
        for (int i = 0; i < 20; i++) Serial.printf("%02x", (uint8_t)data[1 + i]);
        if (data.length() >= 22) {
            Serial.printf(" hashedFlags=0x%02x", (uint8_t)data[21]);
        }
        Serial.println();
    }
};

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== Minimal FMDN listener - no processing, just printing ===");

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    // Non-blocking, continuous scan started ONCE here - the previous
    // version called the *other* start() overload (no callback argument)
    // repeatedly from loop(), which explicitly blocks on a semaphore until
    // each scan window finishes (see BLEScan.cpp - that overload is a
    // convenience wrapper around this one). duration=0 here means "scan
    // forever until explicitly stopped" (per esp_ble_gap_start_scanning's
    // own doc comment) - the actual radio scanning now runs autonomously in
    // the BLE stack's own task, firing onResult() asynchronously whenever it
    // hears something. loop() never needs to wait on it at all.
    pBLEScan->start(0, nullptr, false);

    Serial.println("Scanning continuously in the BACKGROUND - loop() below never blocks on it.");
}

void loop() {
    // Standing in for the GPS/10DOF sensor work the real hub/sensor boards
    // need to do more than once a second - proves loop() is genuinely free
    // to do other time-sensitive work now, not stuck waiting on BLE.
    static unsigned long last_tick = 0;
    if (millis() - last_tick >= 500) {
        last_tick = millis();
        Serial.println("[OTHER WORK] tick (stand-in for GPS/10DOF sensor reads)");
    }
}
