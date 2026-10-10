#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include <Tobe.h>
#include <TobeLog.h>
#include <TobeWeb.h>
#include <TobeWifi.h>

#include "op_mode.h"

namespace WebConfig {

namespace {

WebServer server(80);

void handleRoot() {
    // The sonar display is not running in WiFi mode (see op_mode.h) -
    // just the way back. WiFi setup, firmware update, log and restart are TobeWeb's system section.
    String body = "<p>WiFi mode. Address <b>" + tobe::wifi::ip() + "</b></p>";
    body += "<form method=GET action=/mode/ble><button>Back to sonar display</button></form>";
    body += tobe::web::systemSection();
    server.send(200, "text/html", tobe::web::page("Status", body));
}

void handleModeBle() {
    server.send(200, "text/html", tobe::web::page("Switching", tobe::web::note("Switching to the sonar display, restarting...")));
    OpMode::switchTo(OpMode::Mode::Ble);  // does not return
}

}  // namespace

void begin() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/mode/ble", HTTP_GET, handleModeBle);
    tobe::web::attach(server);
    server.begin();
    tobe::logf("web: config server listening on port 80");
}

void handleClient() { server.handleClient(); }

}  // namespace WebConfig
