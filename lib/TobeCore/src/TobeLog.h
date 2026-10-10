/**
 * TobeLog.h - the console (serial port) and a copy of recent output for web pages.
 *
 *   tobe::console      use this instead of `Serial`. Behaves like Serial but
 *                        - ends every line with CR LF (a bare LF makes picocom/minicom show a staircase),
 *                        - keeps the last few KB of what was printed, for the web "log" page.
 *   tobe::logf(...)    a timestamped line: "[   12345] wifi: joined". Always prefer this for events.
 *   tobe::logText()    the recent output, oldest first (what the web log page shows).
 *
 * Code that already prints with Serial.printf can keep doing so: put `#define TOBE_REDIRECT_SERIAL` before
 * including this header and every later use of the name `Serial` in that file means tobe::console. (The
 * libraries never rely on that - they call tobe::console directly - so include order does not matter for them.)
 *
 * Output is safe from any task: the ring is guarded by a spinlock, and ESP-NOW / WiFi callbacks do print.
 *
 * Size of the kept output: TOBE_LOG_SIZE bytes (build flag, default 4096).
 */
#pragma once

#include <Arduino.h>

#ifndef TOBE_LOG_SIZE
#define TOBE_LOG_SIZE 4096
#endif

namespace tobe {

class Console : public Stream {
public:
    /** Opens the serial port (Serial.begin). */
    void begin(unsigned long baud = 115200);

    int available() override;
    int read() override;
    int peek() override;
    void flush() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t n) override;
    using Print::write;

    int availableForWrite();
    operator bool();

    /** Pause the copy kept for the web page (e.g. while dumping the log itself). */
    void setCapture(bool on) { capture_ = on; }

private:
    uint8_t last_ = 0;
    bool capture_ = true;
};

extern Console console;

/** printf-style event line with a "[millis]" prefix, to the console and the kept output. */
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Copies the kept output (oldest first) into out, NUL terminated. Returns the length. No heap use. */
size_t logCopy(char *out, size_t cap);

/** The kept output as a String. */
String logText();

}  // namespace tobe

#ifdef TOBE_REDIRECT_SERIAL
#ifdef Serial
#undef Serial   /* the core defines it (HWCDCSerial) on boards with native USB */
#endif
#define Serial tobe::console
#endif
