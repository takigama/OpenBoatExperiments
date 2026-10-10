#include "TobeLog.h"

#include <stdarg.h>

// (This library is compiled on its own, so `Serial` here is always the real port - the project's
// TOBE_REDIRECT_SERIAL only affects the project's own files.)

namespace tobe {

namespace {

char s_ring[TOBE_LOG_SIZE];
size_t s_pos = 0;
bool s_wrapped = false;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void ringAdd(const uint8_t *buf, size_t n) {
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        s_ring[s_pos++] = (char)buf[i];
        if (s_pos >= TOBE_LOG_SIZE) {
            s_pos = 0;
            s_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

}  // namespace

Console console;

// USB serial (the C3 / S3 native USB, ARDUINO_USB_CDC_ON_BOOT): arduino-esp32 3.x BLOCKS a write for up to
// 20 x TX-timeout (2 s by default) when USB is plugged in but nothing is reading the port - a board on a bench
// with the serial monitor closed, or plugged into a PC that is not looking. Printing from the main loop then
// starves the whole firmware (found with ESP32Seatalk's demo mode: the web page went unreachable for minutes).
// So: a short timeout, a bigger buffer, and the console DROPS a message when the buffer has no room instead of
// waiting. (A real UART console is unaffected: writes there only ever wait for the wire.)
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#define TOBE_USB_CONSOLE 1
#endif

void Console::begin(unsigned long baud) {
#ifdef TOBE_USB_CONSOLE
    Serial.setTxBufferSize(1024);   // must be before begin()
    Serial.begin(baud);
    Serial.setTxTimeoutMs(10);      // data written straight to Serial (a sonar stream) waits at most this per chunk
#else
    Serial.begin(baud);
#endif
}
int Console::available() { return Serial.available(); }
int Console::read() { return Serial.read(); }
int Console::peek() { return Serial.peek(); }
void Console::flush() { Serial.flush(); }
int Console::availableForWrite() { return Serial.availableForWrite(); }
Console::operator bool() { return (bool)Serial; }

// bytes that will actually be put on the wire for buf[0..n): LF -> CR LF where the CR is missing
static size_t wireLength(const uint8_t *buf, size_t n, uint8_t last) {
    size_t extra = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == 0x0A && last != 0x0D) extra++;   // an LF with no CR before it
        last = buf[i];
    }
    return n + extra;
}

static inline bool usbHasRoom(size_t bytes) {
#ifdef TOBE_USB_CONSOLE
    return (size_t)Serial.availableForWrite() >= bytes;
#else
    (void)bytes;
    return true;
#endif
}

size_t Console::write(uint8_t c) {
    if (capture_) ringAdd(&c, 1);
    if (!usbHasRoom(2)) {   // nobody is draining the USB port: drop, never wait
        last_ = c;
        return 1;
    }
    // a bare LF goes out as CR LF; an LF that already follows a CR is left alone
    if (c == '\n' && last_ != '\r') Serial.write((uint8_t)'\r');
    last_ = c;
    return Serial.write(c);
}

size_t Console::write(const uint8_t *buf, size_t n) {
    if (capture_) ringAdd(buf, n);   // one lock for the whole run - printf output arrives in bursts
    if (!usbHasRoom(wireLength(buf, n, last_))) {   // nobody is draining the USB port: drop, never wait
        if (n) last_ = buf[n - 1];
        return n;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n' && last_ != '\r') {
            if (i > start) Serial.write(buf + start, i - start);
            Serial.write((uint8_t)'\r');
            start = i;
        }
        last_ = buf[i];
    }
    if (n > start) Serial.write(buf + start, n - start);
    return n;
}

void logf(const char *fmt, ...) {
    char msg[256];
    int prefix = snprintf(msg, sizeof(msg), "[%8lu] ", (unsigned long)millis());
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg + prefix, sizeof(msg) - prefix, fmt, args);
    va_end(args);
    console.println(msg);
}

size_t logCopy(char *out, size_t cap) {
    if (!cap) return 0;
    size_t n;
    portENTER_CRITICAL(&s_mux);
    if (s_wrapped) {
        size_t tail = TOBE_LOG_SIZE - s_pos;   // oldest bytes are from s_pos to the end of the ring
        size_t total = TOBE_LOG_SIZE;
        if (total > cap - 1) total = cap - 1;
        size_t skip = (TOBE_LOG_SIZE - total);   // when the caller's buffer is smaller, keep the newest bytes
        // linear view of the ring: [s_pos .. end) then [0 .. s_pos); copy its last `total` bytes
        size_t copied = 0;
        for (size_t i = skip; i < TOBE_LOG_SIZE && copied < total; i++) {
            size_t idx = (i < tail) ? (s_pos + i) : (i - tail);
            out[copied++] = s_ring[idx];
        }
        n = copied;
    } else {
        n = s_pos;
        if (n > cap - 1) n = cap - 1;
        memcpy(out, s_ring + (s_pos - n), n);
    }
    portEXIT_CRITICAL(&s_mux);
    out[n] = 0;
    return n;
}

String logText() {
    String out;
    out.reserve(TOBE_LOG_SIZE + 1);
    char *tmp = (char *)malloc(TOBE_LOG_SIZE + 1);
    if (!tmp) return out;
    logCopy(tmp, TOBE_LOG_SIZE + 1);
    out = tmp;
    free(tmp);
    return out;
}

}  // namespace tobe
