#pragma once

// Compiled in place of secrets.h when that file doesn't exist (see
// main.cpp's __has_include check) - e.g. a fresh clone. Same pattern as
// ManOverBoard/ESP32/src/no_secrets.h: placeholders here, real values in
// secrets.h (gitignored - see ../.gitignore), never committed.
//
// To set this up: copy this file to secrets.h and fill in real values.

constexpr const char *kWifiSsid = "";
constexpr const char *kWifiPassword = "";
constexpr const char *kMqttHost = "";
constexpr uint16_t kMqttPort = 1883;
constexpr const char *kMqttBaseTopic = "fishfinder";
