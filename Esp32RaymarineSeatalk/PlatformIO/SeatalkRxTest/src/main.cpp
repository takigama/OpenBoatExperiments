// SeatalkRxTest - the least firmware that can answer one question: with nothing else running (no WiFi, no MQTT,
// no web, no OTA, no libraries), does the SeaTalk receive on this board still lose or mangle messages?
//
// It listens on the SeaTalk pin (GPIO4, through the BSS138 shifter) with TWO receivers at once, on the same edges,
// so any difference between them is the method, not the bus:
//
//   A "timer": exactly how ESP32Seatalk receives today - a falling-edge interrupt starts an esp_timer, and a timer
//              callback samples each bit and schedules the next one RELATIVE to when it ran (so any lateness
//              accumulates over the 10 samples of a byte).
//   D "edges": the interrupt only records the time of every edge; the bits are worked out afterwards from those
//              times, so nothing depends on how promptly a timer runs.
//
// Reading the result:
//   - A and D agree and both are clean            -> the board's circuit is fine; the real firmware's load is the issue
//   - A loses / mangles messages, D does not      -> the timer method is the problem (lateness numbers show it)
//   - both lose / mangle the same ones            -> the signal itself (the circuit) is the problem
//
// Prints one line per datagram (microsecond start time, receiver, bytes) and a summary every 30 s. 115200 on the
// USB port. Transmits nothing.

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>

namespace {

constexpr int kPin = 4;            // LV_SEATALK
constexpr uint32_t kBitUs = 208;   // what ESP32Seatalk uses
constexpr float kBitF = 208.333f;  // the real 1/4800 s
constexpr uint32_t kSummaryMs = 30000;

// ---- one datagram, as received -------------------------------------------------------------------------------
struct Dg {
    uint32_t t;  // start of its first byte, microseconds since boot (low 32 bits)
    uint8_t len;
    uint8_t b[18];
    char who;
};

// ---- per receiver: counters, the datagram assembler, and which datagrams were seen ---------------------------
struct Pat {
    uint8_t len;
    uint8_t b[8];
    uint32_t n;
};
struct Stats {
    char who;
    uint32_t bytes, datagrams, resync, stopErr, stray, glitch;
    Pat pats[16];
    uint8_t npat;
    uint32_t otherPat;
    uint8_t a[18];
    uint8_t alen, aexp;
};
Stats SA = {'A'};
Stats SD = {'D'};

// Same rules ESP32Seatalk uses: a command byte starts a datagram, the attribute byte gives the length.
bool feed(Stats &s, uint8_t byte, bool cmd, uint32_t t, Dg *out) {
    s.bytes++;
    if (cmd && s.alen > 0) {  // a new command before the last datagram finished: it was truncated
        s.resync++;
        s.alen = 0;
        s.aexp = 0;
    }
    if (!cmd && s.alen == 0) s.stray++;  // a data byte with no command in front of it
    if (s.alen < sizeof(s.a)) s.a[s.alen++] = byte;
    if (s.alen == 2) s.aexp = (s.a[1] & 0x0F) + 3;
    if (s.aexp && s.alen >= s.aexp) {
        out->t = t;
        out->len = s.alen;
        memcpy(out->b, s.a, s.alen);
        out->who = s.who;
        s.alen = 0;
        s.aexp = 0;
        return true;
    }
    return false;
}

void record(Stats &s, const Dg &d) {
    s.datagrams++;
    Serial.printf("%11.6f %c ", d.t / 1e6, d.who);
    for (uint8_t i = 0; i < d.len; i++) Serial.printf("%02x ", d.b[i]);
    Serial.println();
    for (uint8_t i = 0; i < s.npat; i++) {
        if (s.pats[i].len == d.len && memcmp(s.pats[i].b, d.b, d.len < 8 ? d.len : 8) == 0) {
            s.pats[i].n++;
            return;
        }
    }
    if (s.npat < 16) {
        Pat &p = s.pats[s.npat++];
        p.len = d.len;
        memcpy(p.b, d.b, d.len < 8 ? d.len : 8);
        p.n = 1;
    } else {
        s.otherPat++;
    }
}

// ---- the edge log (interrupt -> loop) -----------------------------------------------------------------------
constexpr uint16_t kE = 2048;  // power of two
uint32_t eT[kE];
uint8_t eL[kE];
volatile uint16_t eHead = 0, eTail = 0;
volatile uint32_t eDrops = 0;

// ---- receiver A: timer sampling, as ESP32Seatalk does it ----------------------------------------------------
constexpr int kQ = 32;  // datagrams finished by A's timer task, waiting for loop() to print them
Dg aq[kQ];
volatile uint8_t aqHead = 0, aqTail = 0;

esp_timer_handle_t aTimer;
volatile bool aArmed = true;
int64_t aEdge;     // when the start bit's falling edge happened
uint8_t aN;        // which sample is next: 0-7 data bits, 8 command bit, 9 stop bit
uint8_t aByte;
bool aCmd;
uint32_t lateHist[5], stopLateHist[5];  // how late the samples ran: <25, <50, <100, <200, >=200 us
int32_t lateMax;

void bucket(uint32_t *h, int32_t late) {
    if (late < 25) h[0]++;
    else if (late < 50) h[1]++;
    else if (late < 100) h[2]++;
    else if (late < 200) h[3]++;
    else h[4]++;
}

void aSample(void *) {
    int64_t now = esp_timer_get_time();
    // the first sample is due 1.5 bits after the edge, each later one a bit after that
    int32_t late = (int32_t)(now - (aEdge + kBitUs + kBitUs / 2 + (int64_t)aN * kBitUs));
    if (late < 0) late = 0;
    bool level = gpio_get_level((gpio_num_t)kPin);
    bucket(lateHist, late);
    if (late > lateMax) lateMax = late;

    if (aN < 8) {
        if (level) aByte |= 1 << aN;
        aN++;
        esp_timer_start_once(aTimer, kBitUs);  // relative to NOW, as ESP32Seatalk does
        return;
    }
    if (aN == 8) {
        aCmd = level;
        aN++;
        esp_timer_start_once(aTimer, kBitUs);
        return;
    }
    bucket(stopLateHist, late);  // the last sample of the byte: all the lateness so far has added up in it
    if (!level) SA.stopErr++;
    Dg d;
    if (feed(SA, aByte, aCmd, (uint32_t)aEdge, &d)) {
        uint8_t nx = (aqHead + 1) % kQ;
        if (nx != aqTail) {
            aq[aqHead] = d;
            aqHead = nx;
        }
    }
    aN = 0;
    aByte = 0;
    aArmed = true;
}

// One interrupt serves both receivers: log the edge for D, and start A's timer on a start bit.
void IRAM_ATTR onEdge() {
    int64_t now64 = esp_timer_get_time();
    uint8_t lvl = gpio_get_level((gpio_num_t)kPin);
    uint16_t h = eHead, nx = (h + 1) & (kE - 1);
    if (nx != eTail) {
        eT[h] = (uint32_t)now64;
        eL[h] = lvl;
        eHead = nx;
    } else {
        eDrops++;
    }
    if (!lvl && aArmed) {
        aArmed = false;
        aN = 0;
        aByte = 0;
        aEdge = now64;
        esp_timer_start_once(aTimer, kBitUs + kBitUs / 2);
    }
}

// ---- receiver D: work the bits out from the recorded edge times ---------------------------------------------
void decodeEdges() {
    const uint16_t mask = kE - 1;
    for (;;) {
        uint16_t head = eHead;
        uint16_t i = eTail;
        while (i != head && eL[i] != 0) i = (i + 1) & mask;  // skip rising edges: wait for a start bit
        if (i == head) {
            eTail = head;
            return;
        }
        uint32_t t0 = eT[i];
        if ((uint32_t)((uint32_t)esp_timer_get_time() - t0) < 2400) {  // the byte is not over yet
            eTail = i;
            return;
        }
        // level dt microseconds after the start edge = level after the last edge at or before that time
        auto levelAt = [&](uint32_t dt) -> int {
            int lvl = 0;
            for (uint16_t j = i; j != head; j = (j + 1) & mask) {
                if ((uint32_t)(eT[j] - t0) <= dt) lvl = eL[j];
                else break;
            }
            return lvl;
        };
        uint16_t next = i;  // where to look for the next start bit: past the stop bit's centre
        while (next != head && (uint32_t)(eT[next] - t0) < (uint32_t)(kBitF * 10.0f)) next = (next + 1) & mask;
        if (levelAt((uint32_t)(kBitF * 0.5f)) != 0) {  // the start bit ended before its middle: a glitch, not a byte
            SD.glitch++;
            eTail = (i + 1) & mask;
            continue;
        }
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) {
            if (levelAt((uint32_t)(kBitF * (1.5f + k)))) v |= 1 << k;
        }
        bool cmd = levelAt((uint32_t)(kBitF * 9.5f)) != 0;
        bool stop = levelAt((uint32_t)(kBitF * 10.5f)) != 0;
        if (!stop) SD.stopErr++;
        Dg d;
        if (feed(SD, v, cmd, t0, &d)) record(SD, d);
        eTail = next;
    }
}

// ---- the summary -----------------------------------------------------------------------------------------------
void printPats(const Stats &s) {
    // most frequent first
    bool used[16] = {};
    for (int shown = 0; shown < s.npat; shown++) {
        int best = -1;
        for (int i = 0; i < s.npat; i++) {
            if (!used[i] && (best < 0 || s.pats[i].n > s.pats[best].n)) best = i;
        }
        used[best] = true;
        Serial.printf("      %5lux ", (unsigned long)s.pats[best].n);
        for (uint8_t k = 0; k < s.pats[best].len && k < 8; k++) Serial.printf("%02x ", s.pats[best].b[k]);
        Serial.println();
    }
    if (s.otherPat) Serial.printf("      %5lux (other patterns)\n", (unsigned long)s.otherPat);
}

void summary() {
    Serial.printf("\n==== %lu s ====  edges dropped (ring full): %lu\n", (unsigned long)(millis() / 1000), (unsigned long)eDrops);
    Serial.printf("A timer : bytes=%lu datagrams=%lu truncated=%lu stopbit-low=%lu stray-data=%lu\n", (unsigned long)SA.bytes,
                  (unsigned long)SA.datagrams, (unsigned long)SA.resync, (unsigned long)SA.stopErr, (unsigned long)SA.stray);
    Serial.printf("   sample lateness us  <25:%lu <50:%lu <100:%lu <200:%lu >=200:%lu  max=%ld\n", (unsigned long)lateHist[0],
                  (unsigned long)lateHist[1], (unsigned long)lateHist[2], (unsigned long)lateHist[3], (unsigned long)lateHist[4],
                  (long)lateMax);
    Serial.printf("   stop-bit sample     <25:%lu <50:%lu <100:%lu <200:%lu >=200:%lu   (a bit is 208; half a bit, 104, is the limit)\n",
                  (unsigned long)stopLateHist[0], (unsigned long)stopLateHist[1], (unsigned long)stopLateHist[2],
                  (unsigned long)stopLateHist[3], (unsigned long)stopLateHist[4]);
    printPats(SA);
    Serial.printf("D edges : bytes=%lu datagrams=%lu truncated=%lu stopbit-low=%lu stray-data=%lu start-glitches=%lu\n",
                  (unsigned long)SD.bytes, (unsigned long)SD.datagrams, (unsigned long)SD.resync, (unsigned long)SD.stopErr,
                  (unsigned long)SD.stray, (unsigned long)SD.glitch);
    printPats(SD);
    Serial.println();
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\nSeatalkRxTest: GPIO4 receive only - A (timer, as ESP32Seatalk) and D (edge times) side by side");

    pinMode(kPin, INPUT);  // released; the bus idles high through the board's pull-up

    esp_timer_create_args_t args = {};
    args.callback = &aSample;
    args.name = "rxA";
    esp_timer_create(&args, &aTimer);
    attachInterrupt(digitalPinToInterrupt(kPin), onEdge, CHANGE);
}

void loop() {
    while (aqTail != aqHead) {  // datagrams A finished
        Dg d = aq[aqTail];
        aqTail = (aqTail + 1) % kQ;
        record(SA, d);
    }
    decodeEdges();

    static uint32_t lastSummary = 0;
    if (millis() - lastSummary >= kSummaryMs) {
        lastSummary = millis();
        summary();
    }
    delay(1);
}
