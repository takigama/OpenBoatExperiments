# FishFinderProBluetoothCYD

Standalone display unit for the same "Fish Helper Pro" castable BLE sonar
as [FishFinderProBluetooth](../FishFinderProBluetooth/) - see that
project's README for the full reverse-engineered protocol writeup (GATT
layout, frame format, unit conversions, command format). This project
reuses the same BLE-central/frame-decode logic, but targets a "CYD"
(Cheap Yellow Display) board instead of a bare ESP32-C3, so the depth/
temp/battery reading shows live on a built-in screen instead of (or
alongside) Serial/NMEA output - no laptop or RPi tether needed to see a
reading, which matters for wet-testing at real depth away from a desk.

## Board

- Sold as "CYD" (Cheap Yellow Display) - widely available under many
  reseller names, most commonly as **ESP32-2432S028R**.
- Confirmed via `esptool` against a real unit: **ESP32-D0WD-V3** (classic
  WROOM-32 die), 4MB flash, WiFi + Classic BT + BLE, MAC
  `5c:01:3b:33:b3:38`. Same single-radio WiFi/BLE coexistence caveat as
  FishFinderProBluetooth's C3 applies here too, if WiFi/OTA gets added
  later (see "Next steps").
- 2.8" 240x320 SPI TFT, ILI9341, XPT2046 resistive touch, CH340
  USB-serial, micro SD slot. Touch is confirmed on its own **separate**
  hardware SPI bus (SCK=25, MOSI=32, MISO=39, CS=33 - checked against
  `../EngineControl`'s config for this same board), not sharing the
  display's MISO/MOSI/SCLK. SD's pins aren't confirmed yet. Neither touch
  nor SD are wired up yet (v1 is a read-only display).
- Full 320x240 landscape works correctly - both the "panel is really only
  240x something" conclusion and the "there's an unfixable corruption
  artifact" conclusion that briefly lived in this file were wrong. Root
  cause of both: this project's `platformio.ini` was missing
  `USE_HSPI_PORT` and running the SPI clock far slower (27-40MHz) than
  this panel needs, and using the plain `ILI9341_DRIVER` instead of
  `ILI9341_2_DRIVER`. Found by compiling and flashing the community's own
  [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
  example repo unmodified onto this exact physical unit - its `cyd2usb`
  build variant (`ILI9341_2_DRIVER` + `USE_HSPI_PORT` + 55MHz SPI +
  `TFT_INVERSION_ON`) worked perfectly first try, full screen, no
  corruption, correct colors. That combination is now what this project's
  `platformio.ini` uses too.
- Pin mapping is the standard ESP32-2432S028R reference layout (see
  `platformio.ini`'s `build_flags` for the exact TFT_eSPI config - no
  library-source edits needed, configured entirely via build flags so the
  Docker build stays reproducible).

## Firmware

`PlatformIO/FishFinderProBluetoothCYD/` - three small modules:

- `sonar_ble.h/.cpp` - BLE central: scans for "Fish Helper Pro", connects,
  subscribes to FFF1, reassembles/decodes frames into a `Reading` struct
  (depth, temp, battery, out-of-water/charging flags, frame count).
  Same checksum/unit-conversion formulas as FishFinderProBluetooth, plus
  the status bitfield (byte 2) and battery (byte 8) decode, which that
  project didn't need for Serial/NMEA output but are worth showing on a
  screen.
- `display.h/.cpp` - TFT_eSPI UI, redrawn from `SonarBle::latest()` every
  loop() iteration. Big depth readout, temp, raw battery level (0-6 -
  exact %-mapping not confirmed yet), connection-status dot, out-of-
  water/charging flags, frame counter as a cheap liveness indicator.
- `main.cpp` - wires the two together, plus the same periodic Serial
  liveness line as FishFinderProBluetooth.

Deliberately left out of this first version, to keep it small and testable
quickly:
- **No WiFi/OTA/op-mode switching** - unlike FishFinderProBluetooth, this
  is BLE-only for now. Flash over USB (CH340, shows up as a normal
  `/dev/ttyUSB0` once passed through to the build container/WSL2, unlike
  the C3's native-USB `/dev/ttyACM0`).
- **No FFF2 sensitivity/range writes** - this is a display, not a
  configurator; add if it turns out useful.
- **No touch, no SD logging** - the two obvious next features (see below).

Build/flash via the Dockerfile in that directory - same reproducible-image
pattern as the rest of the repo; see that file's header comment for the
exact commands.

## Next steps

1. ~~Confirm the display driver assumption~~ - done, ILI9341 confirmed,
   working config is `ILI9341_2_DRIVER` + `USE_HSPI_PORT` + 55MHz SPI +
   `TFT_INVERSION_ON` (see "Board" above).
2. ~~Confirm screen orientation~~ - done, landscape (`setRotation(1)`)
   works correctly at full 320x240; flip to `3` if the board ends up
   mounted upside down.
3. SD card logging - the original motivation for considering a CYD at
   all: log raw frames (or decoded NMEA) to the SD card so a wet-test at
   real depth doesn't need a laptop/RPi tether at all, solving the "no
   easy way to test past 0.8m locally" problem from FishFinderProBluetooth.
   Needs careful SPI chip-select handling since SD shares the bus with
   the display.
4. Touch UI for live sensitivity/range control (mirrors
   FishFinderProBluetooth's `sens`/`range` serial commands, but on-device
   instead of over USB).
5. Decode the 120-byte amplitude bin block once real bottom-echo data is
   available - shared with FishFinderProBluetooth, not CYD-specific.
