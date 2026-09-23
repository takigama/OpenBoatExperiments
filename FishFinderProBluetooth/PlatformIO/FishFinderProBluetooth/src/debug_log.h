#pragma once

#include <Arduino.h>

// Once OTA is the normal update path, USB won't always be attached (board
// off doing a wet capture, mounted somewhere awkward, etc.) - the web UI's
// /log page is the fallback diagnostic surface, same pattern as
// Esp32RaymarineSeatalk. A fixed-size ring buffer of recent lines,
// rendered by web_config.cpp.
namespace DebugLog {

// printf-style. Always also goes to Serial (harmless/free when nothing's
// listening) - this doesn't replace Serial output, it adds a second
// destination that survives when USB isn't there at all.
void logf(const char *fmt, ...);

// Rendered oldest-to-newest, one per line, HTML-escaped by the caller
// (web_config.cpp) - this just returns raw text.
String recentLines();

}  // namespace DebugLog
