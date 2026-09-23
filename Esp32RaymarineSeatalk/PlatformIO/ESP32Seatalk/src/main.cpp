// Pipeline smoke test only: proves Docker build -> flash -> boot works end
// to end on real hardware before any WiFi/OTA/SeaTalk logic gets layered
// on top. Nothing here is meant to survive past that point.

#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("ESP32Seatalk build %d booted\n", FW_BUILD);
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        Serial.printf("alive, uptime %lus\n", millis() / 1000);
    }
}
