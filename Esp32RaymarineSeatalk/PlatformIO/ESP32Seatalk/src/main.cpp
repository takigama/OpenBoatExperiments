#include <Arduino.h>

#include "ota_manager.h"
#include "web_config.h"
#include "wifi_manager.h"

// Check for an OTA update once, ~30s after a STA-mode boot (WiFi/DNS/etc
// need a moment to settle) - convenient during dev when "push a build,
// walk away, it updates itself" matters more than fine-grained scheduling.
// A real periodic re-check interval (EngineControl's HELM does ~24h) can
// come later once this loop has more than one thing to schedule.
constexpr uint32_t kBootCheckDelayMs = 30000;
bool s_bootCheckDone = false;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("ESP32Seatalk build %d booted\n", FW_BUILD);

    WifiManager::begin();
    WebConfig::begin();
}

void loop() {
    WebConfig::handleClient();

    if (!s_bootCheckDone && WifiManager::currentMode() == WifiManager::Mode::STA &&
        millis() > kBootCheckDelayMs) {
        s_bootCheckDone = true;
        OtaManager::checkForUpdate();  // logged only for now; web UI drives the actual apply step
    }

    // Periodic liveness line - startup-only logging is useless for anyone
    // attaching a serial monitor after the fact (native USB CDC doesn't
    // buffer for a not-yet-attached host, those lines are just gone).
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus >= 5000) {
        lastStatus = millis();
        const char *mode = WifiManager::currentMode() == WifiManager::Mode::STA ? "STA" : "AP";
        Serial.printf("status: mode=%s heap=%u uptime=%lus\n", mode, ESP.getFreeHeap(), millis() / 1000);
    }
}
