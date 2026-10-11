#pragma once
#include <cstdint>
#include <string>
typedef std::string String;
namespace SeatalkBus {
struct Datagram { uint8_t bytes[18]; uint8_t length; };
void send(uint8_t command, const uint8_t *data, uint8_t len);
}
