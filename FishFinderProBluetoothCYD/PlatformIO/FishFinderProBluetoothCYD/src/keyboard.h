#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// A minimal on-screen QWERTY keyboard for text entry (WiFi SSID/password)
// where there's no physical keyboard. Blocking/modal by design - runs its
// own draw+touch loop until the user taps Done, then returns the entered
// text. Simpler than threading text entry through the caller's own
// non-blocking state machine, and text entry is inherently a "the user is
// focused on this one thing" interaction anyway.
namespace Keyboard {

// prompt is shown above the text field. initial pre-fills it (e.g. for
// editing). masked shows each character as a dot instead of itself, for
// password entry.
String run(TFT_eSPI &tft, const String &prompt, const String &initial, bool masked);

}  // namespace Keyboard
