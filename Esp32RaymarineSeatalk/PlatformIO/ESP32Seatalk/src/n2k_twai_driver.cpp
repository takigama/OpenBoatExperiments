#include "n2k_twai_driver.h"

#include <driver/twai.h>
#include <string.h>

tNMEA2000_Twai::tNMEA2000_Twai(gpio_num_t txPin, gpio_num_t rxPin) : tNMEA2000(), txPin_(txPin), rxPin_(rxPin) {}

bool tNMEA2000_Twai::CANOpen() {
    if (open_) return true;

    twai_general_config_t gConfig = TWAI_GENERAL_CONFIG_DEFAULT(txPin_, rxPin_, TWAI_MODE_NORMAL);
    twai_timing_config_t tConfig = TWAI_TIMING_CONFIG_250KBITS();  // NMEA2000-mandated bit rate
    twai_filter_config_t fConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&gConfig, &tConfig, &fConfig) != ESP_OK) return false;
    if (twai_start() != ESP_OK) return false;
    open_ = true;
    return true;
}

bool tNMEA2000_Twai::CANSendFrame(unsigned long id, unsigned char len, const unsigned char *buf, bool wait_sent) {
    twai_message_t msg = {};
    msg.identifier = id;
    msg.extd = 1;  // NMEA2000/J1939 always uses 29-bit extended CAN IDs
    msg.data_length_code = len > 8 ? 8 : len;
    memcpy(msg.data, buf, msg.data_length_code);
    return twai_transmit(&msg, wait_sent ? pdMS_TO_TICKS(10) : 0) == ESP_OK;
}

bool tNMEA2000_Twai::CANGetFrame(unsigned long &id, unsigned char &len, unsigned char *buf) {
    twai_message_t msg;
    if (twai_receive(&msg, 0) != ESP_OK) return false;  // non-blocking - tick() polls every loop()
    id = msg.identifier;
    len = msg.data_length_code;
    memcpy(buf, msg.data, len);
    return true;
}
