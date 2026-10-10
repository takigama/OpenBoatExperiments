#pragma once

#include <Arduino.h>

#include "seatalk_decode.h"

// A log of the messages this board has RECEIVED, for the web page /messages (web_config.cpp). Separate from the
// debug log (tobe::logf, the /sys/log page): that one is a few KB of events of every kind and a busy bus pushes
// everything else out of it within seconds.
//
// One ring per bus, so a chatty NMEA2000 bus cannot push the SeaTalk lines out: each bus keeps its own last
// kPerSource messages (more than the 100 asked for), and the page merges them back into time order.
// Memory: kSourceCount * kPerSource * sizeof(Entry) - about 38 KB of RAM; lower kPerSource / kTextMax to trade
// history for memory.
//
// Safe to call from any task (a spinlock guards the rings); add() formats outside the lock.
namespace RxLog {

enum class Source : uint8_t { SeaTalk = 0, Can, Mqtt, SignalK };
constexpr int kSourceCount = 4;

constexpr size_t kPerSource = 120;
constexpr size_t kTextMax = 72;  // including the NUL; longer lines are cut

struct Entry {
    uint32_t seq;  // 1, 2, 3... across all sources, in arrival order; 0 = unused slot
    uint32_t ms;   // millis() when it arrived
    Source src;
    char text[kTextMax];
};

const char *sourceName(Source s);

// Adds one received message. printf-style, one line, no trailing newline.
void add(Source s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// A decoded value in words with units, e.g. "Depth 3.4 m", "Heading 123 deg, rudder -2 deg".
void describe(const SeatalkDecode::Event &ev, char *out, size_t cap);

// add() of describe(ev); `note` (may be NULL) goes after it in [brackets] - a topic, a hex dump.
void addEvent(Source s, const SeatalkDecode::Event &ev, const char *note = nullptr);

// Space-separated hex of up to maxBytes bytes ("1A 2B"), "..." if cut. Writes into out.
void hex(const uint8_t *data, size_t len, size_t maxBytes, char *out, size_t cap);

// The newest message among the sources in srcMask (bit n = Source n) that is older than beforeSeq - i.e. walk the
// log newest-first by starting with beforeSeq = UINT32_MAX and passing each result's seq back in. false = no more.
bool findOlder(uint8_t srcMask, uint32_t beforeSeq, Entry *out);

// How many messages each source has delivered since boot (not just those still in the ring).
uint32_t total(Source s);

}  // namespace RxLog
