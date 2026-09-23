#pragma once

#include <Arduino.h>

// Once this board is actually plugged into a real SeaTalk bus, USB is
// gone entirely - the board's 3.3V comes from the bus itself, and that
// fights with USB's own 3.3V regulator if both are connected at once
// (see the schematic notes). The web UI is the only diagnostic surface
// that'll ever exist in the field, so every module's log output needs to
// land here, not just be Serial.printf'd into the void - a fixed-size
// ring buffer of recent lines, rendered by web_config.cpp.
namespace DebugLog {

// printf-style. Always also goes to Serial (harmless/free when nothing's
// listening on native USB CDC) - this doesn't replace Serial output, it
// adds a second destination that survives when USB isn't there at all.
void logf(const char *fmt, ...);

// Rendered oldest-to-newest, one per line, HTML-escaped by the caller
// (web_config.cpp) - this just returns raw text.
String recentLines();

}  // namespace DebugLog
