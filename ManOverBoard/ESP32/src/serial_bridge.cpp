// Minimal BLE-scan-to-serial bridge: no crypto, no WiFi, no display - just
// listens for FMDN broadcasts and prints one machine-parseable line per
// sighting, meant to be read by a script on whatever host this CYD is wired
// to over USB (see ../../OpenPlotter/eid_resolver/serial_bridge.py), which
// forwards each line to the existing /sightings HTTP endpoint. Lets a CYD
// act as a scanning node without needing its own WiFi/network stack at all -
// same brain.py pipeline either way, just a different transport for raw
// sightings (see server.py's docstring on the hub-and-spoke design).
//
// Deliberately NOT continuous_scan_diagnostic.cpp reused as-is: that one's
// output is human-readable (timestamp prefix, periodic "[OTHER WORK] tick"
// demo lines) which would just be noise a parser has to filter back out.
// This one prints ONLY sighting lines, in a fixed format, nothing else -
// same proven BLE scanning logic (FmdnAdvertisedDeviceCallbacks), stripped
// down to just what a serial reader needs.
//
// CRASH FIX (2026-07-23, ported from ../../ESP32-C3/src/serial_bridge_c3.cpp):
// the ESP32-C3 counterpart of this file, using the library's high-level
// accessors (haveServiceData()/getServiceDataUUID()/getServiceData() with
// the default shouldParse=true), crashed within ~30-40s in a real RF
// environment: "Failed to allocate N bytes for payload in copy
// constructor" (BLEAdvertisedDevice.cpp), then abort()/reboot. Root cause,
// confirmed by reading the actual library source
// (framework-arduinoespressif32's BLEScan.cpp/BLEAdvertisedDevice.cpp):
// onResult() is called with a BLEAdvertisedDevice *by value* for EVERY
// advertisement in range (not just FMDN ones - a real environment showed
// ~50+ raw advertisements/sec from unrelated nearby devices, confirmed via
// a btmon HCI capture on the Pi), and the default shouldParse=true also
// builds a std::vector<String> of every service-data entry plus Strings
// for name/manufacturer data/etc. for every AD field in every packet -
// heavy heap churn on top of the one payload-copy malloc that happens
// regardless.
//
// This board (classic dual-core ESP32) didn't visibly crash over a 35-
// minute test run - it has meaningfully more free heap than the C3 after
// the BLE stack and Arduino runtime take their share - but the same
// allocate/free churn is happening here too, just with more margin before
// it would eventually fragment badly enough to fail. Applying the same
// fix here as cheap insurance for a real multi-hour/day boat deployment,
// not because this board has already shown the symptom.

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

        // Fixed format, one line per sighting, nothing else on this line:
        //   SEEN <40 hex chars> <signed rssi>
        // No MAC (rotates, unneeded - brain.record_sighting() only wants
        // eid/rssi/source), no timestamp (the Pi timestamps on receipt,
        // which is what actually matters for the missing-tag check).
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
    delay(2000);

    BLEDevice::init("");
    BLEScan *pBLEScan = BLEDevice::getScan();
    // wantDuplicates=true (unchanged - we want every rotation, not just
    // first-seen-per-address), shouldParse=false (the crash-prevention fix
    // - see file header).
    pBLEScan->setAdvertisedDeviceCallbacks(new FmdnAdvertisedDeviceCallbacks(), true, false);
    pBLEScan->setActiveScan(false);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    // Non-blocking, continuous scan started once - see
    // continuous_scan_diagnostic.cpp's setup() for the fuller explanation
    // of why this exact start() overload (duration=0, no wait) is used.
    pBLEScan->start(0, nullptr, false);

    Serial.println("READY serial_bridge");
}

void loop() {
    // Nothing to do here - onResult() fires asynchronously from the BLE
    // stack's own task and prints directly; loop() is intentionally empty.
}
