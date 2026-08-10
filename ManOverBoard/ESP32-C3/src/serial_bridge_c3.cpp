// ESP32-C3 counterpart of ../../ESP32/src/serial_bridge.cpp - same job (scan
// for FMDN broadcasts, print one machine-parseable line per sighting for a
// Pi-side script to forward into /sightings), same BLE scanning logic
// (BLEDevice/BLEScan/BLEAdvertisedDevice - arduino-esp32's classic
// Bluedroid-based BLE library, which the C3 supports fine despite being
// single-core/BLE-only - scanning runs in its own FreeRTOS task regardless
// of core count, same as the dual-core ESP32). Not just the ESP32 build
// retargeted at a different board id: this chip has no separate USB-UART
// bridge chip like the CYD's CH340 - it talks Serial over its own native
// USB-CDC peripheral instead (see platformio.ini's ARDUINO_USB_MODE=1 /
// ARDUINO_USB_CDC_ON_BOOT=1 - without those, Serial doesn't come up over
// the USB-C port at all on this chip). Practical effect on the Pi side:
// this enumerates as /dev/ttyACM0 (CDC ACM), not /dev/ttyUSB0 like the
// CYD's CH340 - see serial_bridge.py's KNOWN_USB_SERIAL_IDS, which matches
// Espressif's native-USB VID (0x303A) for exactly this reason.
//
// Purpose (see the 2026-07-23 3-scanner comparison test): this is the third
// independent vantage point, on the theory that gaps are complementary
// across scanners - if the Pi's onboard Bluetooth or the CYD misses a
// given beacon, hopefully this one catches it, so the *combined* worst-case
// gap keeps shrinking as more independent scanners are added, even though
// no single scanner's own gap is perfect.
//
// CRASH FIX (2026-07-23): an earlier version of this file used the
// library's high-level accessors (haveServiceData()/getServiceDataUUID()/
// getServiceData(), with the default shouldParse=true) exactly like the
// ESP32/CYD build. That crashed this chip within ~30-40s in a real RF
// environment: "Failed to allocate N bytes for payload in copy
// constructor" (BLEAdvertisedDevice.cpp), then abort()/reboot. Root cause,
// confirmed by reading the actual library source
// (framework-arduinoespressif32's BLEScan.cpp/BLEAdvertisedDevice.cpp):
//   - onResult() is called with a BLEAdvertisedDevice *by value* for
//     EVERY advertisement in range (not just FMDN ones) - our own RF
//     environment alone showed ~50+ raw advertisements/sec from many
//     unrelated nearby devices (confirmed via a btmon HCI capture on the
//     Pi during this same test). Each one triggers BLEAdvertisedDevice's
//     copy constructor, which mallocs a fresh buffer to deep-copy the raw
//     payload (correct behavior, not a bug in itself).
//   - With the default shouldParse=true, parseAdvertisement() ALSO builds
//     a std::vector<String> of every service-data entry, plus Strings for
//     name/manufacturer data/etc., for every AD field in every packet -
//     multiple extra heap allocations per advertisement, on top of the
//     payload copy, for fields we don't even use.
//   - The ESP32-C3 has meaningfully less free heap than the classic
//     dual-core ESP32 (this project's CYD board) after the BLE stack,
//     Arduino runtime, and native-USB CDC driver take their share - so
//     the same non-stop allocate/free churn that (so far) hasn't visibly
//     crashed the CYD fragments this chip's smaller heap fast enough to
//     start failing allocations within under a minute.
// Fix: shouldParse=false (skips all that per-field parsing/allocation),
// and manually scan the raw advertisement bytes ourselves for just the one
// AD structure we care about (Service Data, 16-bit UUID 0xFEAA) - see
// extract_fmdn_eid() below. This can't eliminate the one copy-constructor
// payload malloc per advertisement (that happens before our code ever
// runs, dictated by the library's callback signature), but removes the
// much larger amount of per-field parsing overhead layered on top of it,
// which was the dominant source of allocation churn.

#include <Arduino.h>
#include <string.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// Scans a raw BLE advertisement payload for a Service Data (AD type 0x16)
// structure carrying UUID 0xFEAA, without any of BLEAdvertisedDevice's
// per-field parsing/allocation (see file header for why that matters
// here). Standard BLE AD structure: [length][type][data...], repeated
// until the payload is exhausted; length counts everything after itself
// (i.e. includes the type byte).
static bool extract_fmdn_eid(const uint8_t *payload, size_t len, uint8_t eid_out[20], uint8_t *frame_type_out) {
    size_t i = 0;
    while (i + 1 < len) {
        uint8_t ad_len = payload[i];
        if (ad_len == 0 || i + 1 + ad_len > len) break;
        uint8_t ad_type = payload[i + 1];
        // Service Data - 16-bit UUID: [0x16][uuid_lo][uuid_hi][service data...]
        // Need ad_len >= 1(type) + 2(uuid) + 21(frame type + 20-byte EID).
        if (ad_type == 0x16 && ad_len >= 24 && payload[i + 2] == 0xAA && payload[i + 3] == 0xFE) {
            uint8_t frame_type = payload[i + 4];
            if (frame_type == 0x40 || frame_type == 0x41) {
                memcpy(eid_out, &payload[i + 5], 20);
                *frame_type_out = frame_type;
                return true;
            }
        }
        i += 1 + ad_len;
    }
    return false;
}

class FmdnAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        uint8_t eid[20];
        uint8_t frame_type;
        if (!extract_fmdn_eid(advertisedDevice.getPayload(), advertisedDevice.getPayloadLength(), eid, &frame_type)) {
            return;
        }

        // Same fixed format as the ESP32/CYD build - "SEEN <hex> <rssi>",
        // nothing else - so the Pi-side parser doesn't care which board
        // sent it, only which serial port (and therefore which --source
        // tag) it came in on.
        Serial.print("SEEN ");
        for (int i = 0; i < 20; i++) {
            if (eid[i] < 0x10) Serial.print('0');
            Serial.print(eid[i], HEX);
        }
        Serial.print(' ');
        Serial.println(advertisedDevice.getRSSI());
    }
};

void setup() {
    Serial.begin(115200);
    // Native USB-CDC takes noticeably longer than a real UART to enumerate
    // and be ready for the host to open - 2000ms (same delay the CYD build
    // uses) wasn't reliably enough in testing on other C3 boards' native
    // USB; give it more margin here since there's no separate UART bridge
    // chip masking the timing difference.
    delay(3000);

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    // wantDuplicates=true (unchanged - we want every rotation, not just
    // first-seen-per-address), shouldParse=false (the actual crash fix -
    // see file header).
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true, false);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    pBLEScan->start(0, nullptr, false);

    Serial.println("READY serial_bridge_c3");
}

void loop() {
    // Nothing to do here - onResult() fires asynchronously from the BLE
    // stack's own task and prints directly; loop() is intentionally empty.
}
