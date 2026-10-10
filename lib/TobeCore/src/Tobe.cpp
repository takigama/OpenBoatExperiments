#include "Tobe.h"

#include <esp_mac.h>

namespace tobe {

String title() { return String("TOBE ") + TOBE_FW_NAME; }

String titleWithVersion() { return title() + " v" + String(FW_BUILD); }

String macSuffix() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);   // the eFuse address: needs no radio
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

String apName() {
    String suffix = macSuffix();
    String name = String("TOBE-") + TOBE_FW_NAME;
    const size_t room = 32 - 1 - suffix.length();   // "-" + suffix must fit in 32 characters
    if (name.length() > room) name.remove(room);
    return name + "-" + suffix;
}

void restart(uint32_t delay_ms) {
    Serial.flush();
    delay(delay_ms);
    ESP.restart();
}

}  // namespace tobe
