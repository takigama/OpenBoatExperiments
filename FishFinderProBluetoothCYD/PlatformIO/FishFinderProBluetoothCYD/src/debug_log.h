#pragma once

#include <Arduino.h>

// Now that WiFi mode exists (op_mode.h, entered via the on-screen "Check
// Update" button), USB isn't guaranteed attached while checking logs
// remotely might matter - a ring buffer of recent lines, rendered by
// web_config.cpp's /log page, same pattern as FishFinderProBluetooth.
namespace DebugLog {

// printf-style. Always also goes to Serial.
void logf(const char *fmt, ...);

// Rendered oldest-to-newest, one per line, HTML-escaped by the caller
// (web_config.cpp) - this just returns raw text.
String recentLines();

}  // namespace DebugLog
