#include "debug_log.h"

#include <stdarg.h>

namespace DebugLog {

namespace {

constexpr int kLineCount = 80;
constexpr int kLineMax = 110;  // including the timestamp prefix

char s_lines[kLineCount][kLineMax];
int s_nextSlot = 0;
bool s_wrapped = false;

}  // namespace

void logf(const char *fmt, ...) {
    char msg[kLineMax];
    int prefixLen = snprintf(msg, sizeof(msg), "[%8lu] ", millis() / 1);

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg + prefixLen, sizeof(msg) - prefixLen, fmt, args);
    va_end(args);

    Serial.println(msg);

    strncpy(s_lines[s_nextSlot], msg, kLineMax - 1);
    s_lines[s_nextSlot][kLineMax - 1] = '\0';
    s_nextSlot++;
    if (s_nextSlot >= kLineCount) {
        s_nextSlot = 0;
        s_wrapped = true;
    }
}

String recentLines() {
    String out;
    out.reserve(kLineCount * kLineMax);

    int start = s_wrapped ? s_nextSlot : 0;
    int count = s_wrapped ? kLineCount : s_nextSlot;
    for (int i = 0; i < count; i++) {
        out += s_lines[(start + i) % kLineCount];
        out += '\n';
    }
    return out;
}

}  // namespace DebugLog
