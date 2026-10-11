#!/usr/bin/env bash
# Host test of the SeaTalk decoder / encoder against datagrams captured from a REAL bus.
# No board, no PlatformIO: the two source files are compiled on the PC with small stand-ins for Arduino.h and the
# bus driver (in this folder). Run from anywhere:   Esp32RaymarineSeatalk/PlatformIO/ESP32Seatalk/tools/host-test/run.sh
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/../../src"
out="$(mktemp -d)"
g++ -std=c++17 -Wall -I"$here" -I"$src" \
    "$src/seatalk_decode.cpp" "$src/seatalk_encode.cpp" "$here/test_decode.cpp" -o "$out/test_decode"
"$out/test_decode"
rm -rf "$out"
