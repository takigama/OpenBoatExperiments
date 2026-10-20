#include "seatalk_bus.h"

#include <driver/gpio.h>
#include <esp_timer.h>

namespace SeatalkBus {

namespace {

constexpr uint32_t kBitUs = 208;     // TX: 4800 baud -> ~208.33us/bit, same constant as the proven AVR TX
                                      // (seatalk_bruteforce.c)
constexpr float kBitF = 208.333f;    // RX: the real bit time

int s_pin = -1;

// ---- RX: the interrupt only records WHEN the line changes; the bits are worked out later, from those times -----
//
// This used to sample each bit from an esp_timer callback that scheduled the next sample relative to when it
// had actually run, so every bit of lateness added up over the 10 samples of a byte (stop-bit sample 50-100 us
// late, the limit being 104 us) - a board doing nothing else still mangled ~3% of the messages, and with WiFi /
// MQTT competing for the CPU about a third of them were lost (measured: SeatalkRxTest, receiver A vs D). Working
// from the edge times instead means no timer has to run on time: the decode is exact however late loop() gets
// around to it.
constexpr uint16_t kEdgeN = 4096;  // power of two: ~70 s of a busy bus, so a blocked loop() (a TCP connect) is fine
uint32_t s_edgeT[kEdgeN];          // microseconds (low 32 bits of esp_timer_get_time())
uint8_t s_edgeL[kEdgeN];           // the level the line changed TO
volatile uint16_t s_edgeHead = 0;  // written by the interrupt
volatile uint16_t s_edgeTail = 0;  // written by poll()
volatile uint32_t s_edgeDrops = 0;

void IRAM_ATTR onEdge() {
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint8_t lvl = gpio_get_level((gpio_num_t)s_pin);
    uint16_t h = s_edgeHead;
    uint16_t nx = (h + 1) & (kEdgeN - 1);
    if (nx == s_edgeTail) {
        s_edgeDrops++;
        return;
    }
    s_edgeT[h] = now;
    s_edgeL[h] = lvl;
    s_edgeHead = nx;
}

uint32_t s_startGlitches = 0;  // a low pulse shorter than half a bit: noise, not a start bit
uint32_t s_stopErrors = 0;     // the stop bit was low
uint32_t s_resyncs = 0;        // a new command byte arrived before the datagram being assembled was complete

// Datagram assembler: a command byte (the 9th bit) starts one, its attribute byte gives the length.
Datagram s_rxDatagram;
uint8_t s_rxExpectedLen = 0;  // 0 = not yet known (still waiting on the attribute byte)

void resetRxDatagram() {
    s_rxDatagram.length = 0;
    s_rxExpectedLen = 0;
    s_rxDatagram.self = false;
}

// Completed datagrams wait here until poll() hands them out.
constexpr int kQueueSize = 20;
Datagram s_queue[kQueueSize];
uint8_t s_queueHead = 0;  // next slot poll() will read
uint8_t s_queueTail = 0;  // next slot to write
uint8_t s_queueCount = 0;
uint32_t s_queueDrops = 0;  // completed datagrams lost because the queue was full

// ---- our own transmissions come back as RX (every device on a shared open-drain wire hears itself). While
// setTxIsSelf(true) is in effect the time of everything we send is remembered as a window, and a datagram that
// STARTED inside a window is marked Datagram::self. ----
constexpr uint32_t kSelfGraceUs = 5000;
struct Window {
    uint32_t from, until;
};
Window s_windows[16];
uint8_t s_windowNext = 0;
uint32_t s_selfFrom = 0;
bool s_txIsSelf = false;

bool inSelfWindow(uint32_t t) {
    for (const Window &w : s_windows) {
        if ((uint32_t)(t - w.from) <= (uint32_t)(w.until - w.from)) return true;
    }
    return false;
}

void feedByte(uint8_t byte, bool isCommand, uint32_t startUs) {
    if (isCommand && s_rxDatagram.length > 0) {
        // A fresh command byte arrived before the datagram we were assembling was complete - drop what we had and
        // resync on this one rather than emit a truncated datagram.
        s_resyncs++;
        resetRxDatagram();
    }
    if (s_rxDatagram.length == 0) s_rxDatagram.self = inSelfWindow(startUs);
    if (s_rxDatagram.length < sizeof(s_rxDatagram.bytes)) s_rxDatagram.bytes[s_rxDatagram.length++] = byte;
    if (s_rxDatagram.length == 2) s_rxExpectedLen = (s_rxDatagram.bytes[1] & 0x0F) + 3;  // length = (attribute & 0x0F) + 3

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
}

// Turns the recorded edges into bytes: a falling edge is a start bit; the byte's bits are the line level 1.5, 2.5 ...
// 9.5 bit times later (8 data bits, then the command bit) and the stop bit at 10.5. Only a byte that is over (2.4 ms
// after its start) is decoded, so every one of its edges has been recorded.
void decodeEdges() {
    const uint16_t mask = kEdgeN - 1;
    for (;;) {
        uint32_t nowUs = (uint32_t)esp_timer_get_time();  // read before head: every edge before nowUs is in the log
        uint16_t head = s_edgeHead;
        uint16_t i = s_edgeTail;
        while (i != head && s_edgeL[i] != 0) i = (i + 1) & mask;  // skip rising edges: wait for a start bit
        if (i == head) {
            s_edgeTail = head;
            return;
        }
        uint32_t t0 = s_edgeT[i];
        if ((uint32_t)(nowUs - t0) < 2400) {  // the byte is not over yet
            s_edgeTail = i;
            return;
        }
        // the line level dt microseconds after the start edge: the level after the last edge at or before that time
        auto levelAt = [&](uint32_t dt) -> int {
            int lvl = 0;
            for (uint16_t j = i; j != head; j = (j + 1) & mask) {
                if ((uint32_t)(s_edgeT[j] - t0) <= dt) lvl = s_edgeL[j];
                else break;
            }
            return lvl;
        };
        // where to look for the next start bit: past the stop bit's start
        uint16_t next = i;
        while (next != head && (uint32_t)(s_edgeT[next] - t0) < (uint32_t)(kBitF * 10.0f)) next = (next + 1) & mask;

        if (levelAt((uint32_t)(kBitF * 0.5f)) != 0) {  // the "start bit" ended before its middle: a glitch, not a byte
            s_startGlitches++;
            s_edgeTail = (i + 1) & mask;
            continue;
        }
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) {
            if (levelAt((uint32_t)(kBitF * (1.5f + k)))) v |= 1 << k;
        }
        bool isCommand = levelAt((uint32_t)(kBitF * 9.5f)) != 0;
        if (levelAt((uint32_t)(kBitF * 10.5f)) == 0) s_stopErrors++;
        feedByte(v, isCommand, t0);
        s_edgeTail = next;
    }
}

}  // namespace

void begin(int gpioPin) {
    s_pin = gpioPin;
    pinMode(s_pin, OUTPUT_OPEN_DRAIN);
    digitalWrite(s_pin, HIGH);  // released - bus idles at mark via R1's pull-up

    resetRxDatagram();
    attachInterrupt(digitalPinToInterrupt(s_pin), onEdge, CHANGE);  // once; stays attached
}

uint32_t droppedDatagrams() { return s_queueDrops; }
uint32_t droppedEdges() { return s_edgeDrops; }
uint32_t startGlitches() { return s_startGlitches; }
uint32_t stopBitErrors() { return s_stopErrors; }
uint32_t truncatedDatagrams() { return s_resyncs; }

bool poll(Datagram *out) {
    if (s_queueCount == 0) decodeEdges();
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

void setTxIsSelf(bool on) {
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (on && !s_txIsSelf) {
        s_selfFrom = now;
    } else if (!on && s_txIsSelf) {
        s_windows[s_windowNext] = {s_selfFrom, now + kSelfGraceUs};
        s_windowNext = (s_windowNext + 1) % (sizeof(s_windows) / sizeof(s_windows[0]));
    }
    s_txIsSelf = on;
}

void send(uint8_t command, const uint8_t *dataBytes, uint8_t dataLen) {
    // RX deliberately stays armed through our own TX - every device on a
    // shared open-drain bus hears its own transmission, that's inherent
    // to the medium, not something to suppress.
    txByte(command, true);
    for (uint8_t i = 0; i < dataLen; i++) txByte(dataBytes[i], false);
}

}  // namespace SeatalkBus
