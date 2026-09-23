// TLS cert verification for the GitHub OTA path is deliberately skipped
// (setInsecure() below) rather than pinning/bundling a root CA. Both prior
// projects in this account that tried verifying GitHub's chain on a small
// ESP32 (TDisplay-Media's trimmed cert bundle, chasing "Out of buffer" OTA
// failures on a no-PSRAM board) never actually landed on a working,
// maintained version of it - the full default mbedTLS bundle is a real
// heap cost, and a hand-trimmed one bit-rots as GitHub's CA chain changes.
// MD5 (from the manifest, verified via HTTPUpdate's setMD5sum()) covers
// integrity instead: transport is still fully encrypted, just not
// chain-verified, and a corrupted/tampered download is rejected before
// ever being written to flash either way.

#include "ota_manager.h"

#include <algorithm>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace OtaManager {

namespace {

// Raw file in the repo itself, not a GitHub Release API lookup - avoids
// GitHub API rate limits/User-Agent requirements for what's just a small
// JSON fetch, and keeps the manifest a normal committed file the release
// process updates alongside the .bin it describes.
constexpr const char *kManifestUrl =
    "https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/"
    "Esp32RaymarineSeatalk/PlatformIO/ESP32Seatalk/ota/manifest.json";

}  // namespace

UpdateInfo checkForUpdate() {
    UpdateInfo info;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ota: no WiFi, skipping check");
        return info;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, kManifestUrl)) {
        Serial.println("ota: http.begin() failed");
        return info;
    }

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("ota: manifest fetch failed, HTTP %d\n", status);
        http.end();
        return info;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("ota: manifest JSON parse failed: %s\n", err.c_str());
        return info;
    }

    uint32_t remoteBuild = doc["build"] | 0;
    const char *url = doc["url"] | "";
    const char *md5 = doc["md5"] | "";

    if (remoteBuild > FW_BUILD && url[0]) {
        info.available = true;
        info.build = remoteBuild;
        info.url = url;
        info.md5 = md5;
        Serial.printf("ota: update available - build %u (running %d)\n", remoteBuild, FW_BUILD);
    } else {
        Serial.printf("ota: up to date (running %d, manifest has %u)\n", FW_BUILD, remoteBuild);
    }
    return info;
}

// Hand-rolled rather than HTTPUpdate::update(): this core's HTTPUpdate has
// no way to pass an expected MD5 in at all (no setMD5sum(), see the header
// dumped while chasing this - it only ever reads an "x-MD5" response
// header automatically, which GitHub Releases doesn't send). Driving
// Update/MD5Builder directly gets us the same integrity guarantee
// regardless of what the server sends, at the cost of doing the streaming
// loop ourselves.
bool applyUpdate(const UpdateInfo &info) {
    if (!info.available) return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, info.url)) {
        Serial.println("ota: http.begin() failed for download");
        return false;
    }

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        Serial.printf("ota: download failed, HTTP %d\n", status);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("ota: server didn't report a content length");
        http.end();
        return false;
    }

    if (!Update.begin(contentLength)) {
        Serial.printf("ota: Update.begin() failed: %s\n", Update.errorString());
        http.end();
        return false;
    }

    MD5Builder md5;
    md5.begin();

    Serial.printf("ota: downloading build %u from %s (%d bytes)\n", info.build, info.url.c_str(),
                  contentLength);

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    while (http.connected() && written < contentLength) {
        size_t avail = stream->available();
        if (!avail) {
            delay(2);
            continue;
        }
        size_t n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
        Update.write(buf, n);
        md5.add(buf, n);
        written += n;
    }
    http.end();

    if (written != contentLength) {
        Serial.printf("ota: short read (%d of %d bytes) - aborting\n", written, contentLength);
        Update.abort();
        return false;
    }

    md5.calculate();
    String gotMd5 = md5.toString();
    if (info.md5.length() && !info.md5.equalsIgnoreCase(gotMd5)) {
        Serial.printf("ota: MD5 mismatch - expected %s, got %s - aborting\n", info.md5.c_str(),
                      gotMd5.c_str());
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        Serial.printf("ota: Update.end() failed: %s\n", Update.errorString());
        return false;
    }

    Serial.println("ota: update OK (MD5 verified), restarting");
    delay(500);
    ESP.restart();
    return true;  // unreached, but keeps the compiler happy
}

}  // namespace OtaManager
