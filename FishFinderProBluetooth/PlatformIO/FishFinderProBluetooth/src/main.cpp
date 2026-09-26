#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <string.h>

#include "debug_log.h"
#include "op_mode.h"
#include "ota_manager.h"
#include "web_config.h"
#include "wifi_manager.h"

// GATT layout of the "Fish Helper Pro" castable sonar - see ../README.md.
// FFF1: subscribe, receive notifications, reassemble frames, decode
// depth/temperature to NMEA 0183 and print alongside the raw hex to
// Serial. FFF2: the command/settings channel - sensitivity and range are
// sent continuously (see sendSonarSettings()), mirroring the vendor
// app's own behavior exactly rather than a one-shot write. The
// device-information strings are still a next step.
constexpr const char *kDeviceName = "Fish Helper Pro";
constexpr const char *kServiceUuid = "0000fff0-0000-1000-8000-00805f9b34fb";
constexpr const char *kDataCharUuid = "0000fff1-0000-1000-8000-00805f9b34fb";
constexpr const char *kCmdCharUuid = "0000fff2-0000-1000-8000-00805f9b34fb";

// One frame: "SF" sync, 13-byte header (depth/fish/battery/temperature/
// range - see emitDecoded() below), checksum, a fixed 0x55 sentinel, 120
// amplitude bins (not decoded yet), AA 55 AA 55 AA trailer - see
// ../README.md's "Frame format" section for the full byte layout,
// reverse-engineered from the vendor app's decompiled source and
// confirmed against real captured frames.
constexpr size_t kFrameLen = 140;

// Check for an OTA update once, ~30s after boot (WiFi needs a moment to
// settle) - same pattern as Esp32RaymarineSeatalk. Logged only; applying
// still needs a web UI click.
constexpr uint32_t kBootCheckDelayMs = 30000;
bool s_bootCheckDone = false;

NimBLEClient *s_bleClient = nullptr;
// A plain address, not a pointer into the scan result: NimBLEAdvertisedDevice
// objects are owned/reused by the scan internals and can be gone or
// overwritten by the time loop() gets to act on s_doConnect - storing a raw
// pointer to one made every connect() attempt fail instantly (a dangling-
// pointer connect, not a real radio-level failure), confirmed via serial
// logs showing sub-millisecond "connect failed" turnaround on every try.
NimBLEAddress s_targetAddress;
bool s_doConnect = false;
bool s_bleConnected = false;
uint32_t s_frameCount = 0;
// Whether scanning/connection is currently wanted, independent of WiFi/BLE
// mode (see op_mode.h) - lets "stop"/"start" pause and resume streaming
// live, without a reboot, while staying in BLE mode. Distinct from
// s_bleConnected, which tracks whether a GATT connection actually exists
// right now.
bool s_bleActive = false;
NimBLERemoteCharacteristic *s_writeChar = nullptr;  // FFF2, null until connected

// Device-side sonar settings. The vendor app re-sends these on every
// single received frame (writeDataToBluetooth(), called unconditionally
// from its receive path) - tried mirroring that exactly here and it
// stalled the connection hard, frames dropping from ~4.5/sec to about 1
// every 10-15s, whether the write was issued from inside the BLE notify
// callback or deferred to loop() - a GATT write competing with the
// notification stream on this cheap peripheral's connection seems to
// cost far more than the app (running on a phone's much more capable
// BLE stack) ever lets on. So instead: send once right after connecting,
// and again only when "sens"/"range" actually changes something -
// there's no real need for a continuous re-sync the device doesn't ask
// for. Defaults are guesses; the sensitivity scale (0-100) is confirmed
// from the app's own slider range, but the range byte's real-world units
// aren't confirmed yet - adjust live via "sens <0-100>" / "range
// <0-255>" and watch the decoded Depth Range debug line to calibrate
// against a known distance.
uint8_t s_sensitivity = 50;
uint8_t s_noiseFilter = 0;
uint8_t s_range = 0;

// The actual GATT write happens from loop() (see sendSonarSettingsIfDue()
// below), not here, to keep it well clear of the BLE notify callback path.
bool s_settingsSendPending = false;
void requestSendSonarSettings() { s_settingsSendPending = true; }

void sendSonarSettingsIfDue() {
    if (!s_settingsSendPending) return;
    s_settingsSendPending = false;
    if (!s_writeChar) return;
    uint8_t buf[10] = {0x53, 0x46, 0x01, s_noiseFilter, s_sensitivity, 0x00, s_range, 0x00, 0x00, 0x55};
    uint16_t sum = 0;
    for (int i = 0; i < 8; i++) sum += buf[i];
    buf[8] = (uint8_t)sum;
    s_writeChar->writeValue(buf, sizeof(buf), false);  // write without response, matches the app
}

// Rolling reassembly buffer - BLE notifications arrive as 20-byte chunks
// with no framing of their own, so frames get pieced back together here
// and resynced on the "SF" marker whenever something doesn't line up
// (corruption, or the very first notification after subscribing landing
// mid-frame).
constexpr size_t kBufCap = 512;
uint8_t s_buf[kBufCap];
size_t s_bufLen = 0;

// One line per frame, space-separated hex bytes - kept alongside the
// decoded NMEA output below since the 120 amplitude bins (offsets
// 15-134) aren't decoded yet. DebugLog's lines always start with "["
// (a bracketed timestamp); neither hex nor NMEA lines do, which is
// enough for anything reading this stream to tell log lines apart from
// data lines without a dedicated marker.
void emitRawHex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
}

// NMEA 0183 checksum is the XOR of every character between (not
// including) the leading '$' and trailing '*'. `body` is everything
// after the '$' and before the '*', e.g. "SDDPT,2.9,0.0".
void emitNmeaSentence(const String &body) {
    uint8_t cs = 0;
    for (size_t i = 0; i < body.length(); i++) cs ^= (uint8_t)body[i];
    Serial.print('$');
    Serial.print(body);
    Serial.print('*');
    if (cs < 0x10) Serial.print('0');
    Serial.println(cs, HEX);
}

// Frame layout and every formula here (units, checksum, valid-range
// gating) were pulled directly from the vendor app's decompiled source
// (com.xudaxin.sonarhelper, MainInterfaceActivity.bluetoothReceiveDataAnalysis()/
// mainTimerTask()) and cross-checked against real captured frames: the
// checksum formula reproduces our own captured checksum byte exactly,
// and the temperature formula reproduces the exact 25.2C the vendor app
// showed live against the same raw bytes. Depth/temperature are raw
// tenths-of-a-foot / tenths-of-a-Fahrenheit-degree on the wire
// regardless of the app's own display unit setting.
void emitDecoded(const uint8_t *data) {
    uint16_t sum = 0;
    for (int i = 0; i < 13; i++) sum += data[i];
    if ((uint8_t)sum != data[13]) return;  // corrupt header - raw hex line still has the bytes

    uint16_t rawDepthTenthsFt = (data[3] << 8) | data[4];
    uint16_t rawTempTenthsF = (data[9] << 8) | data[10];

    if (rawDepthTenthsFt >= 20 && rawDepthTenthsFt <= 2000) {
        float depthM = (rawDepthTenthsFt / 10.0f) * 0.3048f;
        char buf[16];
        dtostrf(depthM, 0, 1, buf);
        emitNmeaSentence(String("SDDPT,") + buf + ",0.0");
    }
    if (rawTempTenthsF > 0 && rawTempTenthsF <= 2000) {
        float tempC = ((int)rawTempTenthsF - 320) / 18.0f;
        char buf[16];
        dtostrf(tempC, 0, 1, buf);
        emitNmeaSentence(String("YXMTW,") + buf + ",C");
    }

    // Depth Range's real-world units aren't confirmed yet (see
    // sendSonarSettings()) - logged only on change, to help calibrate
    // what a given "range <n>" command actually does to this field,
    // without spamming a line for every single frame.
    static int16_t lastDepthRange = -1;
    if (data[12] != lastDepthRange) {
        lastDepthRange = data[12];
        DebugLog::logf("sonar: device depth range now %d", data[12]);
    }
}

void emitFrame(const uint8_t *data, size_t len) {
    s_frameCount++;
    emitRawHex(data, len);
    emitDecoded(data);
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
            emitFrame(s_buf, kFrameLen);
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
        // but don't corrupt memory if something stalls Serial output and
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
        s_targetAddress = device->getAddress();
        s_doConnect = true;
    }
};

class ConnCallbacks : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *) override {
        s_bleConnected = false;
        s_writeChar = nullptr;
        s_bufLen = 0;  // don't try to stitch a frame across a reconnect
        // A "stop" command disconnects deliberately, which fires this same
        // callback - only auto-rescan for an unexpected drop, not our own
        // requested stop, or "stop" would just immediately undo itself.
        if (s_bleActive) {
            DebugLog::logf("ble: disconnected, rescanning");
            NimBLEDevice::getScan()->start(0, nullptr, false);
        } else {
            DebugLog::logf("ble: disconnected (stopped)");
        }
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
    s_bleActive = true;
    NimBLEScan *scan = NimBLEDevice::getScan();
    static ScanCallbacks scanCallbacks;
    scan->setAdvertisedDeviceCallbacks(&scanCallbacks);
    scan->setInterval(45);
    scan->setWindow(15);
    scan->setActiveScan(true);
    scan->start(0, onScanComplete, false);
    DebugLog::logf("ble: scanning...");
}

// Live pause, no reboot - stops scanning/disconnects but stays in BLE mode
// (NimBLE itself stays initialized), so "start" can resume without paying
// for a full mode-switch reboot.
void stopBle() {
    if (!s_bleActive) {
        DebugLog::logf("ble: already stopped");
        return;
    }
    s_bleActive = false;
    NimBLEDevice::getScan()->stop();
    if (s_bleClient && s_bleClient->isConnected()) {
        s_bleClient->disconnect();  // fires ConnCallbacks::onDisconnect(), which checks s_bleActive
    } else {
        DebugLog::logf("ble: stopped");
    }
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
    s_writeChar = service->getCharacteristic(kCmdCharUuid);
    if (!s_writeChar) {
        DebugLog::logf("ble: characteristic fff2 not found - settings won't be sent");
    } else {
        requestSendSonarSettings();  // establish current sensitivity/range once, on connect
    }
    s_bleConnected = true;
    DebugLog::logf("ble: connected, subscribed to fff1");
}

OpMode::Mode s_opMode = OpMode::Mode::Ble;

// Reads a line typed into the serial monitor. "wifi"/"ble" switch modes
// (see op_mode.h) - the one channel guaranteed to work regardless of which
// radio is currently up, which matters most for BLE->WiFi since there's no
// web UI reachable while WiFi is off. "start"/"stop" control streaming
// live within BLE mode, no reboot.
void handleSerialCommand() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "wifi") {
        OpMode::switchTo(OpMode::Mode::Wifi);  // does not return
    } else if (cmd == "ble") {
        OpMode::switchTo(OpMode::Mode::Ble);  // does not return
    } else if (cmd == "start" || cmd == "stop") {
        if (s_opMode != OpMode::Mode::Ble) {
            DebugLog::logf("ble: not available in WiFi mode (try \"ble\" first)");
        } else if (cmd == "start") {
            if (s_bleActive) {
                DebugLog::logf("ble: already running");
            } else {
                startScan();
            }
        } else {
            stopBle();
        }
    } else if (cmd.startsWith("sens ") || cmd.startsWith("range ")) {
        if (s_opMode != OpMode::Mode::Ble) {
            DebugLog::logf("ble: not available in WiFi mode (try \"ble\" first)");
            return;
        }
        bool isSens = cmd.startsWith("sens ");
        int v = cmd.substring(isSens ? 5 : 6).toInt();
        // Range is an index into the device's preset list (0=auto,
        // 1..8=10/20/30/60/90/120/150/200ft), not a raw distance - confirmed
        // by testing (index 5 -> device reports 90ft).
        int maxV = isSens ? 100 : 8;
        if (v < 0) v = 0;
        if (v > maxV) v = maxV;
        if (isSens) {
            s_sensitivity = (uint8_t)v;
            DebugLog::logf("ble: sensitivity set to %d, sending", s_sensitivity);
        } else {
            s_range = (uint8_t)v;
            DebugLog::logf("ble: range set to %d, sending", s_range);
        }
        requestSendSonarSettings();
    } else if (cmd.length()) {
        DebugLog::logf(
            "unknown serial command \"%s\" (try \"wifi\", \"ble\", \"start\", \"stop\", \"sens <0-100>\", or "
            "\"range <0-8>\")",
            cmd.c_str());
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    DebugLog::logf("FishFinderProBluetooth build %d booted", FW_BUILD);

    // WiFi and BLE are mutually exclusive per boot, not live-switched - see
    // op_mode.h for why. Only the radio subsystem the current mode actually
    // needs ever gets initialized this boot.
    s_opMode = OpMode::current();
    if (s_opMode == OpMode::Mode::Ble) {
        // Idle, not streaming, until a "start" command - see handleSerialCommand().
        NimBLEDevice::init("");
        DebugLog::logf("ble: idle, waiting for \"start\"");
    } else {
        WifiManager::begin();
        WebConfig::begin();
    }
}

void loop() {
    handleSerialCommand();

    if (s_opMode == OpMode::Mode::Wifi) {
        WebConfig::handleClient();
        if (!s_bootCheckDone && WifiManager::currentMode() == WifiManager::Mode::STA &&
            millis() > kBootCheckDelayMs) {
            s_bootCheckDone = true;
            OtaManager::checkForUpdate();  // logged only for now; web UI drives the actual apply step
        }
    } else {
        if (s_doConnect) connectToFishFinder();
        sendSonarSettingsIfDue();
    }

    // Periodic liveness line - a board that's silently stuck (or just has
    // nothing else to report) still shows something within a few seconds
    // of any serial connection being opened, rather than looking dead.
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        DebugLog::logf("status: mode=%s wifi=%d ble_active=%d ble_connected=%d frames=%u heap=%u",
                        s_opMode == OpMode::Mode::Wifi ? "wifi" : "ble", WiFi.status(), s_bleActive,
                        s_bleConnected, s_frameCount, ESP.getFreeHeap());
    }
}
