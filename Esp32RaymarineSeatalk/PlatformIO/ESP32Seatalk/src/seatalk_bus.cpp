#include "seatalk_bus.h"

#include <esp_timer.h>

namespace SeatalkBus {

namespace {

constexpr uint32_t kBitUs = 208;  // 4800 baud -> ~208.33us/bit, same constant
                                   // as the proven AVR TX (seatalk_bruteforce.c)

int s_pin = -1;

// ---- RX assembly state (touched by both the timer callback and poll(),
// so access is kept to simple flag/byte operations only - no
// allocation/locking needed since ESP32 byte/bool reads+writes are
// atomic and there's exactly one producer (the timer callback) and one
// consumer (poll(), called from the main loop)). ----
enum class RxState { WaitStart, Sampling, DatagramReady };
volatile RxState s_rxState = RxState::WaitStart;

uint8_t s_rxByte;         // bit-shift accumulator for the byte currently being sampled
uint8_t s_rxBitIndex;     // 0..7 = data bits, 8 = command bit, 9 = stop bit
Datagram s_rxDatagram;
uint8_t s_rxExpectedLen;  // 0 = not yet known (still waiting on the attribute byte)

esp_timer_handle_t s_sampleTimer;

void IRAM_ATTR IRAM_unused() {}  // placeholder to keep the ISR section grouped; see startBit ISR below

void resetRxDatagram() {
    s_rxDatagram.length = 0;
    s_rxExpectedLen = 0;
}

// Runs once per bit, kBitUs after the previous edge/sample - see the
// scheduling comment in onStartBitEdge(). Task-context callback (esp_timer's
// default dispatch method), not a true ISR - simpler and safer to reason
// about than ISR-context code (no restrictions on what it can touch), at
// the cost of somewhat higher/more variable latency than a raw ISR would
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

    // Stop bit - not stored, just re-arms for the next start-bit edge.
    // (Not sanity-checked against "should be mark" - a real framing error
    // here will simply produce a byte that fails the length-driven resync
    // logic above on the next byte, which is enough for this v1.)
    s_rxBitIndex = 0;
    s_rxByte = 0;

    if (s_rxExpectedLen && s_rxDatagram.length >= s_rxExpectedLen) {
        s_rxState = RxState::DatagramReady;  // poll() picks this up and resets state
    } else {
        s_rxState = RxState::WaitStart;
        attachInterrupt(digitalPinToInterrupt(s_pin), []() IRAM_ATTR {
            portDISABLE_INTERRUPTS();
            detachInterrupt(digitalPinToInterrupt(s_pin));
            s_rxState = RxState::Sampling;
            s_rxBitIndex = 0;
            s_rxByte = 0;
            esp_timer_start_once(s_sampleTimer, kBitUs + kBitUs / 2);  // land at 1st data bit's center
            portENABLE_INTERRUPTS();
        }, FALLING);
    }
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
    s_rxState = RxState::WaitStart;
    attachInterrupt(digitalPinToInterrupt(s_pin), []() IRAM_ATTR {
        detachInterrupt(digitalPinToInterrupt(s_pin));
        s_rxState = RxState::Sampling;
        s_rxBitIndex = 0;
        s_rxByte = 0;
        esp_timer_start_once(s_sampleTimer, kBitUs + kBitUs / 2);
    }, FALLING);
}

bool poll(Datagram *out) {
    if (s_rxState != RxState::DatagramReady) return false;

    *out = s_rxDatagram;
    resetRxDatagram();
    s_rxState = RxState::WaitStart;
    attachInterrupt(digitalPinToInterrupt(s_pin), []() IRAM_ATTR {
        detachInterrupt(digitalPinToInterrupt(s_pin));
        s_rxState = RxState::Sampling;
        s_rxBitIndex = 0;
        s_rxByte = 0;
        esp_timer_start_once(s_sampleTimer, kBitUs + kBitUs / 2);
    }, FALLING);
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
    // to the medium, not something to suppress. (An earlier version
    // detached RX here to avoid exactly that self-echo; the real problem
    // that caused was structural, not cosmetic - it meant RX was never
    // listening at the one moment data actually existed on the wire,
    // making a self-loopback test literally impossible to pass regardless
    // of whether RX itself worked. Caught by that test actually failing.)
    txByte(command, true);
    for (uint8_t i = 0; i < dataLen; i++) txByte(dataBytes[i], false);
}

}  // namespace SeatalkBus
