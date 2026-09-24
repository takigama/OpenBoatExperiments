// TLS cert verification for the GitHub OTA path is deliberately skipped
// (setInsecure() below) rather than pinning/bundling a root CA - same
// reasoning as Esp32RaymarineSeatalk's ota_manager.cpp: verifying GitHub's
// chain on a small ESP32 has never landed cleanly across prior projects in
// this account, and the full default mbedTLS bundle is a real heap cost.
// MD5 (from the manifest, verified via a hand-rolled Update/MD5Builder
// loop) covers integrity instead: transport is still fully encrypted, just
// not chain-verified.

#include "ota_manager.h"

#include <algorithm>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "debug_log.h"

namespace OtaManager {

namespace {

// Raw file in the repo itself, not a GitHub Release API lookup - avoids
// GitHub API rate limits/User-Agent requirements for what's just a small
// JSON fetch, and keeps the manifest a normal committed file the release
// process updates alongside the .bin it describes.
constexpr const char *kManifestUrl =
    "https://raw.githubusercontent.com/takigama/OpenBoatExperiments/master/"
    "FishFinderProBluetooth/PlatformIO/FishFinderProBluetooth/ota/manifest.json";

}  // namespace

UpdateInfo checkForUpdate() {
    UpdateInfo info;

    if (WiFi.status() != WL_CONNECTED) {
        DebugLog::logf("ota: no WiFi, skipping check");
        return info;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, kManifestUrl)) {
        DebugLog::logf("ota: http.begin() failed");
        return info;
    }

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        DebugLog::logf("ota: manifest fetch failed, HTTP %d", status);
        http.end();
        return info;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        DebugLog::logf("ota: manifest JSON parse failed: %s", err.c_str());
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
        DebugLog::logf("ota: update available - build %u (running %d)", remoteBuild, FW_BUILD);
    } else {
        DebugLog::logf("ota: up to date (running %d, manifest has %u)", FW_BUILD, remoteBuild);
    }
    return info;
}

// Hand-rolled rather than HTTPUpdate::update() - this core's HTTPUpdate has
// no way to pass an expected MD5 in (no setMD5sum()); driving
// Update/MD5Builder directly gets the same integrity guarantee regardless
// of what the server sends.
bool applyUpdate(const UpdateInfo &info) {
    if (!info.available) return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, info.url)) {
        DebugLog::logf("ota: http.begin() failed for download");
        return false;
    }
    // GitHub Release asset URLs 302 to a signed release-assets.githubusercontent.com
    // blob URL - HTTPClient does NOT follow redirects by default.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int status = http.GET();
    if (status != HTTP_CODE_OK) {
        DebugLog::logf("ota: download failed, HTTP %d", status);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        DebugLog::logf("ota: server didn't report a content length");
        http.end();
        return false;
    }

    if (!Update.begin(contentLength)) {
        DebugLog::logf("ota: Update.begin() failed: %s", Update.errorString());
        http.end();
        return false;
    }

    MD5Builder md5;
    md5.begin();

    DebugLog::logf("ota: downloading build %u from %s (%d bytes)", info.build, info.url.c_str(),
                    contentLength);

    // No data at all for this long means the connection has effectively
    // stalled (seen in practice on marginal-RF boards under BLE+WiFi
    // coexistence: the download just goes silent, http.connected() stays
    // true, and the old unbounded loop spun forever with no way out -
    // needed a manual power cycle to recover). Bail out and let the
    // two-partition scheme's normal "update failed" path handle it
    // instead of hanging indefinitely.
    constexpr uint32_t kStallTimeoutMs = 20000;
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    uint32_t lastDataAt = millis();
    while (http.connected() && written < contentLength) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastDataAt > kStallTimeoutMs) {
                DebugLog::logf("ota: download stalled (%d of %d bytes) - aborting", written, contentLength);
                Update.abort();
                http.end();
                return false;
            }
            delay(2);
            continue;
        }
        size_t n = stream->readBytes(buf, std::min(avail, sizeof(buf)));
        Update.write(buf, n);
        md5.add(buf, n);
        written += n;
        lastDataAt = millis();
    }
    http.end();

    if (written != contentLength) {
        DebugLog::logf("ota: short read (%d of %d bytes) - aborting", written, contentLength);
        Update.abort();
        return false;
    }

    md5.calculate();
    String gotMd5 = md5.toString();
    if (info.md5.length() && !info.md5.equalsIgnoreCase(gotMd5)) {
        DebugLog::logf("ota: MD5 mismatch - expected %s, got %s - aborting", info.md5.c_str(),
                        gotMd5.c_str());
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        DebugLog::logf("ota: Update.end() failed: %s", Update.errorString());
        return false;
    }

    DebugLog::logf("ota: update OK (MD5 verified), restarting");
    delay(500);
    ESP.restart();
    return true;  // unreached, but keeps the compiler happy
}

}  // namespace OtaManager
