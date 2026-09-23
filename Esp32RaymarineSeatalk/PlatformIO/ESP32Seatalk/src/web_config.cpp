#include "web_config.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "debug_log.h"
#include "ota_manager.h"
#include "seatalk_bus.h"
#include "wifi_manager.h"

namespace WebConfig {

namespace {

WebServer server(80);
OtaManager::UpdateInfo s_lastCheck;  // result of the last /ota/check, consumed by /ota/apply

String htmlEscape(const String &s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    out.replace("\"", "&quot;");
    return out;
}

String pageWrap(const String &title, const String &body) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + title + "</title></head><body style='font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em'>";
    html += "<h2>ESP32Seatalk</h2>";
    html += body;
    html += "</body></html>";
    return html;
}

String wifiJoinForm() {
    // Synchronous scan - blocks a couple seconds, acceptable for a page
    // that's loaded once during setup rather than polled.
    int n = WiFi.scanNetworks();
    String options;
    for (int i = 0; i < n; i++) {
        options += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>" + htmlEscape(WiFi.SSID(i)) +
                   " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();

    String body = "<p>In setup mode - connected to <b>" + WifiManager::apSsid() +
                  "</b>. Pick a network to join:</p>";
    body += "<form method='POST' action='/wifi/save'>";
    body += "<select name='ssid' style='width:100%;padding:.5em;margin:.3em 0'>" + options + "</select>";
    body += "<input name='pass' type='password' placeholder='Password' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.5em'>Join &amp; restart</button>";
    body += "</form>";
    return body;
}

String otaSection() {
    String body = "<hr><h3>Firmware</h3><p>Running build " + String(FW_BUILD) + "</p>";
    if (s_lastCheck.available) {
        body += "<p>Update available: build " + String(s_lastCheck.build) + "</p>";
        body += "<a href='/ota/apply'><button style='width:100%;padding:.6em'>Update now</button></a>";
    } else {
        body += "<a href='/ota/check'><button style='width:100%;padding:.6em'>Check for updates</button></a>";
    }
    // Direct local upload, alongside the GitHub-manifest path above, not
    // instead of it - GitHub Releases stays the real distribution
    // mechanism (and the only one once this is out of dev and unattended
    // on a boat), this is purely a fast path for iterating during
    // development without a publish+CDN-cache round trip each time. No
    // MD5 check on this path - it's a direct, deliberate local upload
    // over the LAN, not a fetch from the open internet, so the same
    // integrity concern the GitHub path (see ota_manager.cpp) exists for
    // doesn't really apply here.
    body += "<form method='POST' action='/ota/upload' enctype='multipart/form-data' style='margin-top:.5em'>";
    body += "<input type='file' name='firmware' accept='.bin' style='width:100%'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.3em'>Upload firmware directly</button>";
    body += "</form>";
    return body;
}

void handleRoot() {
    String body;
    if (WifiManager::currentMode() == WifiManager::Mode::AP) {
        body = wifiJoinForm();
    } else {
        body = "<p>Joined WiFi. IP: <b>" + WiFi.localIP().toString() + "</b></p>";
        body += otaSection();
    }
    if (WifiManager::currentMode() == WifiManager::Mode::STA) {
        body += "<hr><p><a href='/seatalk/test-lamp'><button style='width:100%;padding:.6em'>"
                "Test: cycle instrument lamp</button></a></p>";
    }
    // No USB once this is plugged into a real SeaTalk bus (it shares 3.3V
    // with the bus itself) - this page is the only diagnostic surface
    // that'll exist in the field, so the log link belongs on every page,
    // not just once things go wrong.
    body += "<hr><p><a href='/log'>View debug log</a></p>";
    server.send(200, "text/html", pageWrap("ESP32Seatalk setup", body));
}

void handleLog() {
    server.send(200, "text/plain; charset=utf-8", DebugLog::recentLines());
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
        DebugLog::logf("seatalk: sent lamp level 0x%02X", level);
        delay(1200);
    }
}

void handleWifiSave() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/plain", "missing ssid");
        return;
    }
    String ssid = server.arg("ssid");
    String pass = server.hasArg("pass") ? server.arg("pass") : "";
    server.send(200, "text/html",
                pageWrap("Restarting...", "<p>Saved. Restarting to join <b>" + htmlEscape(ssid) + "</b>...</p>"));
    WifiManager::saveCredentialsAndReboot(ssid, pass);  // does not return
}

void handleOtaCheck() {
    s_lastCheck = OtaManager::checkForUpdate();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleOtaApply() {
    if (!s_lastCheck.available) {
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }
    server.send(200, "text/html", pageWrap("Updating...", "<p>Downloading and flashing build " +
                                                                String(s_lastCheck.build) +
                                                                "... device will restart on success.</p>"));
    OtaManager::applyUpdate(s_lastCheck);  // reboots on success; on failure, falls through
    s_lastCheck = OtaManager::UpdateInfo{};
}

// Runs once the whole upload request has been received - just reports
// what handleOtaUploadChunk() below already did and reboots on success.
void handleOtaUploadDone() {
    if (Update.hasError()) {
        server.send(200, "text/html",
                     pageWrap("Upload failed", "<p>Nothing was changed - still running build " +
                                                    String(FW_BUILD) + ".</p>"));
        return;
    }
    server.send(200, "text/html", pageWrap("Upload OK", "<p>Flashed OK, restarting...</p>"));
    delay(500);
    ESP.restart();
}

// Streams in as the upload arrives - WebServer's two-callback upload
// pattern (see begin()'s server.on() call below): this one fires
// repeatedly as chunks come in, handleOtaUploadDone() above fires once
// after the full request completes.
void handleOtaUploadChunk() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        DebugLog::logf("ota: direct upload starting: %s", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            DebugLog::logf("ota: Update.begin() failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            DebugLog::logf("ota: Update.write() failed: %s", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            DebugLog::logf("ota: direct upload OK, %u bytes, restarting", upload.totalSize);
        } else {
            DebugLog::logf("ota: Update.end() failed: %s", Update.errorString());
        }
    }
}

}  // namespace

void begin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/wifi/save", HTTP_POST, handleWifiSave);
    server.on("/ota/check", HTTP_GET, handleOtaCheck);
    server.on("/ota/apply", HTTP_GET, handleOtaApply);
    server.on("/ota/upload", HTTP_POST, handleOtaUploadDone, handleOtaUploadChunk);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/seatalk/test-lamp", HTTP_GET, handleTestLamp);
    server.begin();
    DebugLog::logf("web: config server listening on port 80");
}

void handleClient() { server.handleClient(); }

}  // namespace WebConfig
