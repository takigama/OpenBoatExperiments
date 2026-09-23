#include "seatalk_bus.h"

#include <esp_timer.h>

namespace SeatalkBus {

namespace {

constexpr uint32_t kBitUs = 208;  // 4800 baud -> ~208.33us/bit, same constant
                                   // as the proven AVR TX (seatalk_bruteforce.c)

int s_pin = -1;

// ---- RX assembly state (touched by both the timer callback and poll()/
// send(), so access is kept to simple flag/byte operations only - no
// allocation/locking needed since ESP32 byte/bool reads+writes are
// atomic and there's exactly one producer (the timer callback) and one
// consumer (poll(), called from the main loop)). ----
uint8_t s_rxByte;         // bit-shift accumulator for the byte currently being sampled
uint8_t s_rxBitIndex;     // 0..7 = data bits, 8 = command bit, 9 = stop bit
Datagram s_rxDatagram;    // datagram currently being assembled (not yet complete)
uint8_t s_rxExpectedLen;  // 0 = not yet known (still waiting on the attribute byte)

// Completed datagrams queue here until poll() drains them. This matters:
// an earlier version had only a single "ready" slot and didn't re-arm for
// the next start bit until poll() was called - meaning RX went
// completely deaf the moment one datagram completed, for as long as
// whatever called send() (or anything else) took to get back around to
// calling poll(). Caught for real testing this against the actual Rudder
// instrument: a web handler that sent 5 lamp commands with a blocking
// delay() between each only ever showed 1 of them in the log, because RX
// stopped listening after the first one and had nothing to re-arm it
// until the handler finally returned several seconds later. A proper
// queue plus always re-arming immediately (see the bottom of
// sampleBitCallback()) fixes this independent of whatever the consumer
// happens to be doing.
constexpr int kQueueSize = 8;
Datagram s_queue[kQueueSize];
uint8_t s_queueHead = 0;  // next slot poll() will read
uint8_t s_queueTail = 0;  // next slot the timer callback will write
uint8_t s_queueCount = 0;
uint32_t s_queueDrops = 0;  // completed datagrams lost because the queue was full

esp_timer_handle_t s_sampleTimer;

void resetRxDatagram() {
    s_rxDatagram.length = 0;
    s_rxExpectedLen = 0;
}

void IRAM_ATTR armForNextStartBit() {
    attachInterrupt(digitalPinToInterrupt(s_pin), []() IRAM_ATTR {
        detachInterrupt(digitalPinToInterrupt(s_pin));
        s_rxBitIndex = 0;
        s_rxByte = 0;
        esp_timer_start_once(s_sampleTimer, kBitUs + kBitUs / 2);  // land at 1st data bit's center
    }, FALLING);
}

// Runs once per bit, kBitUs after the previous edge/sample - see
// armForNextStartBit(). Task-context callback (esp_timer's default
// dispatch method), not a true ISR - simpler and safer to reason about
// than ISR-context code (no restrictions on what it can touch), at the
// cost of somewhat higher/more variable latency than a raw ISR would
// have. At 208us/bit and sampling at bit-center, that tradeoff has plenty
// of margin under normal load - see seatalk_bus.h's header comment on
// what could still go wrong under heavy concurrent activity (e.g. an OTA
// flash write).
void sampleBitCallback(void *) {
    bool level = digitalRead(s_pin);  // HIGH = mark = 1, LOW = space = 0

    if (s_rxBitIndex < 8) {
        s_rxByte |= (level ? 1 : 0) << s_rxBitIndex;
        s_rxBitIndex++;
        esp_timer_start_once(s_sampleTimer, kBitUs);
        return;
    }

    if (s_rxBitIndex == 8) {
        // Command/data marker bit.
        bool isCommand = level;
        s_rxBitIndex++;
        esp_timer_start_once(s_sampleTimer, kBitUs);  // one more sample: the stop bit

        if (isCommand && s_rxDatagram.length > 0) {
            // A fresh command byte arrived before the datagram we were
            // assembling was actually complete - drop what we had and
            // resync on this one rather than silently emit a truncated
            // datagram. See seatalk_bus.h on why this can legitimately
            // happen (dropped byte, bus noise, etc.), not just a bug.
            resetRxDatagram();
        }

        if (s_rxDatagram.length < sizeof(s_rxDatagram.bytes)) {
            s_rxDatagram.bytes[s_rxDatagram.length++] = s_rxByte;
        }

        if (s_rxDatagram.length == 2) {
            // Attribute byte just landed - now we know the total length
            // (see seatalk_bus.h / this session's earlier-confirmed
            // formula: length = (attribute & 0x0F) + 3).
            s_rxExpectedLen = (s_rxDatagram.bytes[1] & 0x0F) + 3;
        }
        return;
    }

    // Stop bit - not stored. Whether or not this completed a datagram,
    // always re-arm immediately for the next start bit - the two used to
    // be conditional on each other (see the queue comment above for why
    // that was wrong). Not sanity-checked against "should be mark" - a
    // real framing error here will simply produce a byte that fails the
    // length-driven resync logic above on the next byte, which is enough
    // for this v1.
    s_rxBitIndex = 0;
    s_rxByte = 0;

    if (s_rxExpectedLen && s_rxDatagram.length >= s_rxExpectedLen) {
        if (s_queueCount < kQueueSize) {
            s_queue[s_queueTail] = s_rxDatagram;
            s_queueTail = (s_queueTail + 1) % kQueueSize;
            s_queueCount++;
        } else {
            s_queueDrops++;
        }
        resetRxDatagram();
    }

    armForNextStartBit();
}

}  // namespace

void begin(int gpioPin) {
    s_pin = gpioPin;
    pinMode(s_pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(s_pin, HIGH);  // released - bus idles at mark via R1's pull-up

    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &sampleBitCallback;
    timerArgs.name = "seatalk_rx";
    esp_timer_create(&timerArgs, &s_sampleTimer);

    resetRxDatagram();
    armForNextStartBit();
}

bool poll(Datagram *out) {
    if (s_queueCount == 0) return false;
    *out = s_queue[s_queueHead];
    s_queueHead = (s_queueHead + 1) % kQueueSize;
    s_queueCount--;
    return true;
}

namespace {

inline void txBit(uint8_t bitval) {
    digitalWrite(s_pin, bitval ? HIGH : LOW);  // HIGH=released=mark=1, LOW=driven=space=0
    delayMicroseconds(kBitUs);
}

void txByte(uint8_t data, bool isCommand) {
    txBit(0);                                     // start bit
    for (uint8_t i = 0; i < 8; i++) txBit((data >> i) & 0x01);  // LSB first
    txBit(isCommand ? 1 : 0);                      // command/data marker
    txBit(1);                                      // stop bit
}

}  // namespace

void send(uint8_t command, const uint8_t *dataBytes, uint8_t dataLen) {
    // RX deliberately stays armed through our own TX - every device on a
    // shared open-drain bus hears its own transmission, that's inherent
    // to the medium, not something to suppress.
    txByte(command, true);
    for (uint8_t i = 0; i < dataLen; i++) txByte(dataBytes[i], false);
}

}  // namespace SeatalkBus
