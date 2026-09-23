#include "web_config.h"

#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "debug_log.h"
#include "net_config.h"
#include "ota_manager.h"
#include "status.h"
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

// Permanent dark mode, no toggle - same as Esp32RaymarineSeatalk's
// kDarkStyle, copied rather than shared since these are separate
// firmware projects with no common library between them.
constexpr const char *kDarkStyle =
    "<style>"
    "body{background:#000;color:#e6e6e6}"
    "a{color:#6ab0ff}"
    "input,select,textarea{background:#1a1a1a;color:#e6e6e6;border:1px solid #444}"
    "button{background:#1e1e1e;color:#e6e6e6;border:1px solid #555}"
    "button:hover{background:#2a2a2a}"
    "hr{border-color:#333}"
    "h1,h2,h3,h4{color:#fff}"
    "</style>";

String pageWrap(const String &title, const String &body) {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + title + "</title>";
    html += kDarkStyle;
    html += "</head><body style='font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em'>";
    html += "<h2>FishFinderProBluetooth</h2>";
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
        options += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>" + htmlEscape(WiFi.SSID(i)) + " (" +
                    String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();

    String body =
        "<p>In setup mode - connected to <b>" + WifiManager::apSsid() + "</b>. Pick a network to join:</p>";
    body += "<form method='POST' action='/wifi/save'>";
    body += "<select name='ssid' style='width:100%;padding:.5em;margin:.3em 0'>" + options + "</select>";
    body += "<input name='pass' type='password' placeholder='Password' "
            "style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.5em'>Join &amp; restart</button>";
    body += "</form>";
    return body;
}

String mqttSection() {
    String body = "<hr><h3>MQTT</h3>";
    if (!NetConfig::mqttHost().isEmpty()) {
        body += "<p>" + NetConfig::mqttHost() + ":" + String(NetConfig::mqttPort()) + ", base topic \"" +
                htmlEscape(NetConfig::mqttBaseTopic()) + "\"</p>";
    } else {
        body += "<p>Not configured.</p>";
    }
    body += "<form method='POST' action='/mqtt/save'>";
    body += "<input name='host' placeholder='Broker host/IP' value='" + htmlEscape(NetConfig::mqttHost()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<input name='port' type='number' placeholder='Port' value='" + String(NetConfig::mqttPort()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<input name='base' placeholder='Base topic' value='" + htmlEscape(NetConfig::mqttBaseTopic()) +
            "' style='width:100%;padding:.5em;margin:.3em 0;box-sizing:border-box'>";
    body += "<button type='submit' style='width:100%;padding:.6em'>Save</button>";
    body += "</form>";
    return body;
}

String otaSection() {
    String body = "<hr><h3>Firmware</h3><p>Running build " + String(FW_BUILD) + "</p>";
    if (s_lastCheck.available) {
        body += "<p>Update available: build " + String(s_lastCheck.build) + "</p>";
        body += "<a href='/ota/apply'><button style='width:100%;padding:.6em'>Update now</button></a>";
    } else {
        body += "<a href='/ota/check'><button style='width:100%;padding:.6em'>Check for updates (github)</button></a>";
    }
    body += "<h4 style='margin-top:1em'>Manual update</h4>";
    body += "<form method='POST' action='/ota/upload' enctype='multipart/form-data'>";
    body += "<input type='file' name='firmware' accept='.bin' style='width:100%'>";
    body += "<button type='submit' style='width:100%;padding:.6em;margin-top:.3em'>Perform local update</button>";
    body += "</form>";
    return body;
}

void handleRoot() {
    String body;
    if (WifiManager::currentMode() == WifiManager::Mode::AP) {
        body = wifiJoinForm();
    } else {
        body = "<p>WiFi: <b>" + WiFi.localIP().toString() + "</b></p>";
        body += "<p>BLE: " + String(Status::bleConnected() ? "<b>connected</b>" : "scanning") + "</p>";
        body += "<p>Frames published: " + String(Status::frameCount()) + "</p>";
        body += mqttSection();
        body += otaSection();
    }
    body += "<hr><p><a href='/log'>View debug log</a></p>";
    server.send(200, "text/html", pageWrap("FishFinderProBluetooth", body));
}

void handleLog() { server.send(200, "text/plain; charset=utf-8", DebugLog::recentLines()); }

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

void handleMqttSave() {
    String host = server.hasArg("host") ? server.arg("host") : "";
    uint16_t port = server.hasArg("port") ? (uint16_t)server.arg("port").toInt() : 1883;
    String base = server.hasArg("base") ? server.arg("base") : "";
    NetConfig::saveMqtt(host, port, base);
    server.sendHeader("Location", "/");
    server.send(303);
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
    server.send(200, "text/html",
                pageWrap("Updating...", "<p>Downloading and flashing build " + String(s_lastCheck.build) +
                                             "... device will restart on success.</p>"));
    OtaManager::applyUpdate(s_lastCheck);  // reboots on success; on failure, falls through
    s_lastCheck = OtaManager::UpdateInfo{};
}

void handleOtaUploadDone() {
    if (Update.hasError()) {
        server.send(200, "text/html",
                     pageWrap("Upload failed",
                              "<p>Nothing was changed - still running build " + String(FW_BUILD) + ".</p>"));
        return;
    }
    server.send(200, "text/html", pageWrap("Upload OK", "<p>Flashed OK, restarting...</p>"));
    delay(500);
    ESP.restart();
}

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
    server.on("/mqtt/save", HTTP_POST, handleMqttSave);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/ota/check", HTTP_GET, handleOtaCheck);
    server.on("/ota/apply", HTTP_GET, handleOtaApply);
    server.on("/ota/upload", HTTP_POST, handleOtaUploadDone, handleOtaUploadChunk);
    server.begin();
    DebugLog::logf("web: config server listening on port 80");
}

void handleClient() { server.handleClient(); }

}  // namespace WebConfig
