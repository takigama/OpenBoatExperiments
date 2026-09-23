#pragma once

#include <NMEA2000.h>
#include <driver/gpio.h>

// Minimal tNMEA2000 CAN driver built on ESP-IDF's portable TWAI driver
// (driver/twai.h). ttlappalainen's own NMEA2000_esp32 companion library
// doesn't work here - it pokes the classic ESP32's SJA1000-style CAN
// peripheral registers directly (#include <soc/dport_reg.h>, MODULE_CAN->
// register struct), which is specific to the original ESP32 silicon and
// doesn't even compile on the C3 (a different, newer CAN peripheral, no
// DPORT on this chip - it predates the C3, which didn't exist when that
// library was last touched in 2020). TWAI is the ESP-IDF driver that
// actually supports the C3's CAN peripheral.
//
// Implements just the three pure virtual methods tNMEA2000 requires -
// see NMEA2000.h's own doc comments on CANOpen()/CANSendFrame()/
// CANGetFrame() for the contract each one has to satisfy.
class tNMEA2000_Twai : public tNMEA2000 {
public:
    tNMEA2000_Twai(gpio_num_t txPin, gpio_num_t rxPin);

protected:
    bool CANOpen() override;
    bool CANSendFrame(unsigned long id, unsigned char len, const unsigned char *buf, bool wait_sent) override;
    bool CANGetFrame(unsigned long &id, unsigned char &len, unsigned char *buf) override;

private:
    gpio_num_t txPin_;
    gpio_num_t rxPin_;
    bool open_ = false;
};
